package xyaoled

import (
	"bytes"
	"errors"
	"fmt"
	"image"
	"image/gif"
	"time"

	"tinygo.org/x/bluetooth"
)

// MainConfigProvider is a function that returns the current ChunkLimits config (for injection from main)
var MainConfigProvider func() bool

// MainDivideGif returns frames-per-part (0 = disabled)
var MainDivideGif func() int

const (
	// BLE Service and Characteristic UUIDs
	serviceUUID    = "ae00"
	writeCharUUID  = "ae01"
	notifyCharUUID = "ae02"

	// Protocol constants
	maxChunkSize  = 20
	defaultSlot   = 0x07
	staticSlot    = 0x04
	maxBrightness = 125
	minBrightness = 0

	// Timing constants
	initDelay        = 300 * time.Millisecond
	commandDelay     = 30 * time.Millisecond
	smallGifDelay    = 2 * time.Millisecond
	largeGifDelay    = 4 * time.Millisecond
	smallGifFinish   = 50 * time.Millisecond
	largeGifFinish   = 100 * time.Millisecond
	gifSizeThreshold = 300
	burstPaceBytes   = 1024
	burstPause       = 12 * time.Millisecond
)

// clearPlaybackSequence returns a command that stops/clears current playback
// Reuses the initialization stop command observed to reset playback state.
func clearPlaybackSequence() []byte {
	return []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x0f, 0x00, 0x0a, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00}
}

// Device represents a connection to an XYAOLED display
type Device struct {
	adapter       *bluetooth.Adapter
	device        bluetooth.Device
	writeChar     bluetooth.DeviceCharacteristic
	notifyChar    bluetooth.DeviceCharacteristic
	isConnected   bool
	seqNum        uint8
	currentBright uint8
	slot          uint8
	isBusy        bool
	debug         bool
}

// SendOptions controls how GIF payloads are written to the device.
// SkipClear avoids the pre-send playback clear (useful for refresh/keepalive
// where the slot already contains the same content). BestEffortClear allows
// proceeding even if the clear command fails (e.g., when the characteristic
// write method temporarily reports unsupported), which prevents refresh loops
// from failing while still attempting to stop prior playback.
type SendOptions struct {
	SkipClear       bool
	BestEffortClear bool
	ForceReinit     bool
	StaticHeader    bool
	LogHeader       bool
	SlotOverride    *uint8
}

// NewDevice creates a new XYAOLED device instance
func NewDevice(debug bool) (*Device, error) {
	if debug {
		fmt.Println("[BLE] Creating new XYAOLED device instance")
	}

	adapter := bluetooth.DefaultAdapter
	if debug {
		fmt.Println("[BLE] Enabling BLE adapter...")
	}

	err := adapter.Enable()
	if err != nil {
		return nil, fmt.Errorf("failed to enable BLE adapter: %w", err)
	}

	if debug {
		fmt.Println("[BLE] ✓ BLE adapter enabled successfully")
	}

	return &Device{
		adapter:       adapter,
		seqNum:        1,
		currentBright: 100,
		slot:          defaultSlot,
		isConnected:   false,
		isBusy:        false,
		debug:         debug,
	}, nil
}

// Connect scans for and connects to an XYAOLED device
func (d *Device) Connect(timeout time.Duration) error {
	if d.isConnected {
		return errors.New("already connected")
	}

	if d.debug {
		fmt.Printf("[BLE] Starting device scan (timeout: %v)\n", timeout)
	}

	deviceFound := make(chan bluetooth.ScanResult, 1)
	scanTimeout := time.After(timeout)

	// Start scanning
	if d.debug {
		fmt.Println("[BLE] Initiating BLE scan...")
	}

	err := d.adapter.Scan(func(adapter *bluetooth.Adapter, result bluetooth.ScanResult) {
		name := result.LocalName()
		if d.debug {
			fmt.Printf("[BLE] Found device: '%s' (address: %s)\n", name, result.Address.String())
		}

		if name == "" {
			return
		}

		// Look for XYAOLED device (case-insensitive)
		if len(name) >= 7 {
			// Check if name contains "xyaoled" (case-insensitive)
			nameUpper := ""
			for _, c := range name {
				if c >= 'a' && c <= 'z' {
					nameUpper += string(c - 32)
				} else {
					nameUpper += string(c)
				}
			}
			if contains(nameUpper, "XYAO") {
				if d.debug {
					fmt.Printf("[BLE] ✓ XYAOLED device found: '%s'\n", name)
				}
				select {
				case deviceFound <- result:
				default:
				}
				adapter.StopScan()
			}
		}
	})

	if err != nil {
		return fmt.Errorf("failed to start scan: %w", err)
	}

	// Wait for device or timeout
	if d.debug {
		fmt.Println("[BLE] Waiting for XYAOLED device...")
	}

	var result bluetooth.ScanResult
	select {
	case result = <-deviceFound:
		if d.debug {
			fmt.Printf("[BLE] ✓ Device selected: %s\n", result.LocalName())
		}
	case <-scanTimeout:
		d.adapter.StopScan()
		if d.debug {
			fmt.Println("[BLE] ✗ Scan timeout reached")
		}
		return errors.New("scan timeout: no XYAOLED device found")
	}

	// Connect to device
	if d.debug {
		fmt.Printf("[BLE] Connecting to device at address: %s\n", result.Address.String())
	}

	device, err := d.adapter.Connect(result.Address, bluetooth.ConnectionParams{})
	if err != nil {
		return fmt.Errorf("failed to connect: %w", err)
	}
	d.device = device

	if d.debug {
		fmt.Println("[BLE] ✓ Connected to device")
	}

	// Discover services
	if d.debug {
		fmt.Println("[BLE] Discovering services (UUID: ae00)...")
	}

	services, err := device.DiscoverServices([]bluetooth.UUID{
		bluetooth.NewUUID([16]byte{0x00, 0x00, 0xae, 0x00, 0x00, 0x00, 0x10, 0x00,
			0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}),
	})
	if err != nil {
		return fmt.Errorf("failed to discover services: %w", err)
	}

	if len(services) == 0 {
		return errors.New("service ae00 not found")
	}

	if d.debug {
		fmt.Printf("[BLE] ✓ Found %d service(s)\n", len(services))
	}

	// Discover characteristics
	if d.debug {
		fmt.Println("[BLE] Discovering characteristics (ae01, ae02)...")
	}

	chars, err := services[0].DiscoverCharacteristics([]bluetooth.UUID{
		bluetooth.NewUUID([16]byte{0x00, 0x00, 0xae, 0x01, 0x00, 0x00, 0x10, 0x00,
			0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}),
		bluetooth.NewUUID([16]byte{0x00, 0x00, 0xae, 0x02, 0x00, 0x00, 0x10, 0x00,
			0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}),
	})
	if err != nil {
		return fmt.Errorf("failed to discover characteristics: %w", err)
	}

	if d.debug {
		fmt.Printf("[BLE] ✓ Found %d characteristic(s)\n", len(chars))
	}

	// Assign characteristics
	for _, char := range chars {
		uuid := char.UUID().String()
		if d.debug {
			fmt.Printf("[BLE]   - Characteristic: %s\n", uuid)
		}
		if uuid == "0000ae01-0000-1000-8000-00805f9b34fb" {
			d.writeChar = char
			if d.debug {
				fmt.Println("[BLE]     ✓ Write characteristic (ae01) assigned")
			}
		} else if uuid == "0000ae02-0000-1000-8000-00805f9b34fb" {
			d.notifyChar = char
			if d.debug {
				fmt.Println("[BLE]     ✓ Notify characteristic (ae02) assigned")
			}
		}
	}

	if d.writeChar.UUID().String() == "" {
		return errors.New("write characteristic ae01 not found")
	}

	// Enable notifications if available
	if d.notifyChar.UUID().String() != "" {
		if d.debug {
			fmt.Println("[BLE] Enabling notifications on ae02...")
		}
		d.notifyChar.EnableNotifications(func(buf []byte) {
			if d.debug {
				fmt.Printf("[BLE] <-- Notification received: %d bytes: %x\n", len(buf), buf)
			}
		})
		if d.debug {
			fmt.Println("[BLE] ✓ Notifications enabled")
		}
	}

	d.isConnected = true

	// Initialize device with protocol handshake
	if d.debug {
		fmt.Println("[BLE] Starting device initialization sequence...")
	}

	if err := d.initialize(); err != nil {
		d.Disconnect()
		return fmt.Errorf("failed to initialize device: %w", err)
	}

	return nil
}

// ScanAndList scans for all Bluetooth devices and prints them to console
func (d *Device) ScanAndList(timeout time.Duration) error {
	fmt.Printf("Starting BLE scan for %v...\n", timeout)
	fmt.Println("Press Ctrl+C to stop")
	fmt.Println()

	devicesSeen := make(map[string]bool)
	deviceCount := 0
	scanTimeout := time.After(timeout)
	done := make(chan bool)

	// Start scanning
	err := d.adapter.Scan(func(adapter *bluetooth.Adapter, result bluetooth.ScanResult) {
		address := result.Address.String()

		// Only print each device once
		if devicesSeen[address] {
			return
		}
		devicesSeen[address] = true
		deviceCount++

		name := result.LocalName()
		if name == "" {
			name = "(no name)"
		}

		// Calculate signal strength indicator
		rssi := result.RSSI
		signalBars := "?"
		if rssi != 0 {
			if rssi > -50 {
				signalBars = "▂▄▆█" // Excellent
			} else if rssi > -60 {
				signalBars = "▂▄▆_" // Good
			} else if rssi > -70 {
				signalBars = "▂▄__" // Fair
			} else if rssi > -80 {
				signalBars = "▂___" // Weak
			} else {
				signalBars = "____" // Very weak
			}
		}

		// Check if it's an XYAOLED device
		isXYAOLED := ""
		nameUpper := ""
		for _, c := range name {
			if c >= 'a' && c <= 'z' {
				nameUpper += string(c - 32)
			} else {
				nameUpper += string(c)
			}
		}
		if contains(nameUpper, "XYAOLED") {
			isXYAOLED = " ← XYAOLED DEVICE!"
		}

		// Print device info
		fmt.Printf("%3d. [%s] %s  %s", deviceCount, signalBars, address, name)
		if rssi != 0 {
			fmt.Printf("  (%d dBm)", rssi)
		}
		fmt.Printf("%s\n", isXYAOLED)
	})

	if err != nil {
		return fmt.Errorf("failed to start scan: %w", err)
	}

	// Wait for timeout
	go func() {
		<-scanTimeout
		d.adapter.StopScan()
		done <- true
	}()

	<-done

	fmt.Printf("\nScan complete. Found %d device(s).\n", deviceCount)

	if deviceCount == 0 {
		fmt.Println("\nNo devices found. Make sure:")
		fmt.Println("  1. Bluetooth is enabled on this device")
		fmt.Println("  2. Target devices are powered on")
		fmt.Println("  3. You have sufficient permissions")
	}

	return nil
}

// initialize performs the device initialization sequence
// Ported from Node.js _init() method
func (d *Device) initialize() error {
	// Command 1: Initial handshake
	if d.debug {
		fmt.Println("[BLE] Sending init command 1/5 (initial handshake)...")
	}
	cmd1 := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x1f, 0x00, 0x00, 0x00, 0x02, 0x00,
		0x19, 0x0c, 0x1b, 0x00, 0x16, 0x29, 0x06, 0x01, 0x00, 0xc5, 0x7c, 0x82, 0x2e, 0x07,
		0x11, 0x00, 0x00, 0x00, 0x00}
	if err := d.send(cmd1); err != nil {
		return err
	}
	if d.debug {
		fmt.Printf("[BLE] Waiting %v...\n", initDelay)
	}
	time.Sleep(initDelay)

	// Command 2
	if d.debug {
		fmt.Println("[BLE] Sending init command 2/5...")
	}
	cmd2 := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x12, 0x00, 0x11, 0x00, 0x03, 0x02,
		0x01, 0x01, 0x00, 0x00, 0x00, 0x00}
	if err := d.send(cmd2); err != nil {
		return err
	}
	time.Sleep(commandDelay)

	// Command 3
	if d.debug {
		fmt.Println("[BLE] Sending init command 3/5...")
	}
	cmd3 := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x12, 0x00, 0x11, 0x00, 0x04, 0x01,
		0x01, 0x01, 0x00, 0x00, 0x00, 0x00}
	if err := d.send(cmd3); err != nil {
		return err
	}
	time.Sleep(commandDelay)

	// Command 4
	if d.debug {
		fmt.Println("[BLE] Sending init command 4/5...")
	}
	cmd4 := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x0f, 0x00, 0x0a, 0x00, 0x05, 0x00,
		0x00, 0x00, 0x00}
	if err := d.send(cmd4); err != nil {
		return err
	}
	time.Sleep(commandDelay)

	// Command 5
	if d.debug {
		fmt.Println("[BLE] Sending init command 5/5...")
	}
	cmd5 := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x11, 0x00, 0x05, 0x00, 0x06, 0x01,
		0x01, 0x00, 0x00, 0x00, 0x00}
	if err := d.send(cmd5); err != nil {
		return err
	}
	time.Sleep(commandDelay)

	if d.debug {
		fmt.Println("[BLE] ✓ Initialization sequence complete!")
	}

	return nil
}

// send writes data to the device via BLE characteristic
func (d *Device) send(data []byte) error {
	if !d.isConnected {
		return errors.New("device not connected")
	}

	if d.debug {
		fmt.Printf("[BLE] --> Sending %d bytes: %x\n", len(data), data)
	}

	_, err := d.writeChar.WriteWithoutResponse(data)
	if err != nil && d.debug {
		fmt.Printf("[BLE] ✗ Send failed: %v\n", err)
	}
	return err
}

// SetBrightness sets the display brightness (0-125 observed)
// Ported from Node.js setBrightness() method
func (d *Device) SetBrightness(level uint8) error {
	if !d.isConnected {
		return errors.New("device not connected")
	}

	// Clamp brightness to valid range
	originalLevel := level
	if level < minBrightness {
		level = minBrightness
	}
	if level > maxBrightness {
		level = maxBrightness
	}

	if d.debug && originalLevel != level {
		fmt.Printf("[BLE] Brightness clamped from %d to %d\n", originalLevel, level)
	}

	d.currentBright = level

	// Calculate third byte (level * 0.8) per observed protocol
	thirdByte := uint8(float32(level) * 0.8)

	if d.debug {
		fmt.Printf("[BLE] Setting brightness: level=%d, thirdByte=%d, seq=%d\n", level, thirdByte, d.seqNum)
	}

	// Build brightness command with sequence number
	cmd := []byte{
		0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88,
		0x10, 0x00,
		0x12, 0x00,
		d.seqNum,
		level,
		thirdByte,
		0x00, 0x00, 0x00,
	}

	d.seqNum++

	err := d.send(cmd)
	if err == nil && d.debug {
		fmt.Println("[BLE] ✓ Brightness set successfully")
	}
	return err
}

// buildGifPacket constructs the complete packet for GIF data
// Ported from Node.js buildGifPacket() method
func (d *Device) buildGifPacket(gifData []byte) []byte {
	headerSize := 24
	totalSize := headerSize + len(gifData)

	packet := make([]byte, 0, totalSize)

	// Protocol header (6 bytes)
	packet = append(packet, 0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88)

	// Total size (2 bytes, little-endian)
	packet = append(packet, uint8(totalSize&0xFF), uint8((totalSize>>8)&0xFF))

	// Command bytes
	packet = append(packet, 0x07, 0x02, d.slot)

	// Conditional header based on GIF size
	if len(gifData) <= gifSizeThreshold {
		// Small GIF header
		packet = append(packet, 0x0c, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0xff, 0xe0)
	} else {
		// Large GIF header
		gifSize := len(gifData)
		packet = append(packet, uint8(gifSize&0xFF), uint8((gifSize>>8)&0xFF))
		packet = append(packet, 0x00, 0x02, 0x00, 0x00, 0x01, 0x20, 0x03, 0x06, 0x00, 0x01, 0x00)
	}

	// Append GIF data
	packet = append(packet, gifData...)

	return packet
}

// buildStaticGifPacket constructs a packet using the static-image header observed in btlog3
// Header (after length): 07 02 <slot> 88 30 00 03 00 00 01 70 03 20 00 02 00
func (d *Device) buildStaticGifPacket(gifData []byte, slot byte) []byte {
	header := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88}

	// Total size = header (6 + 2 len + 16 payload header) + gif data
	headerFields := []byte{0x07, 0x02, slot, 0x88, 0x30, 0x00, 0x03, 0x00, 0x00, 0x01, 0x70, 0x03, 0x20, 0x00, 0x02, 0x00}
	headerSize := len(header) + 2 + len(headerFields)
	totalSize := headerSize + len(gifData)

	packet := make([]byte, 0, totalSize)
	packet = append(packet, header...)
	packet = append(packet, uint8(totalSize&0xff), uint8((totalSize>>8)&0xff))
	packet = append(packet, headerFields...)
	packet = append(packet, gifData...)
	return packet
}

// buildLogCompatPacket constructs a packet using the header observed in singleslog.txt
// Header bytes (after length): 52 09 07 02 <slot> 3a 09 00 02 00 01 01 00 00 01 00 08 00
func (d *Device) buildLogCompatPacket(gifData []byte, slot byte) []byte {
	header := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88}
	headFields := []byte{0x52, 0x09, 0x07, 0x02, slot, 0x3a, 0x09, 0x00, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00}

	headerSize := len(header) + 2 + len(headFields)
	totalSize := headerSize + len(gifData)

	packet := make([]byte, 0, totalSize)
	packet = append(packet, header...)
	packet = append(packet, uint8(totalSize&0xff), uint8((totalSize>>8)&0xff))
	packet = append(packet, headFields...)
	packet = append(packet, gifData...)
	return packet
}

// gifMeta holds basic metadata for pacing animated uploads
type gifMeta struct {
	frames   int
	duration time.Duration
}

// parseGifMeta extracts frame count and total duration from a GIF buffer
func parseGifMeta(buf []byte) (gifMeta, error) {
	g, err := gif.DecodeAll(bytes.NewReader(buf))
	if err != nil {
		return gifMeta{}, err
	}

	meta := gifMeta{frames: len(g.Image)}
	for _, d := range g.Delay {
		meta.duration += time.Duration(d) * 10 * time.Millisecond // GIF delay units are 1/100s
	}

	// Fallback duration if delays were zeroed
	if meta.duration == 0 && meta.frames > 0 {
		meta.duration = time.Duration(meta.frames) * 100 * time.Millisecond
	}

	return meta, nil
}

// SendGif sends a GIF buffer to the display using default options
func (d *Device) SendGif(gifBuffer []byte) error {
	return d.SendGifWithOptions(gifBuffer, SendOptions{})
}

// SendGifWithOptions sends a GIF buffer with configurable behavior
func (d *Device) SendGifWithOptions(gifBuffer []byte, opts SendOptions) error {
	// Defaults
	if !opts.BestEffortClear {
		opts.BestEffortClear = true
	}

	// Check divide config (frames per part). If set (>0) and this is an animated GIF,
	// split into parts of that many frames and send each sequentially without reinit.
	divide := 0
	if MainDivideGif != nil {
		divide = MainDivideGif()
	}
	if divide > 0 {
		// Try to decode GIF; if decode fails, fall through to normal send/ retry logic
		g, err := gif.DecodeAll(bytes.NewReader(gifBuffer))
		if err == nil && len(g.Image) > 1 {
			frames := len(g.Image)
			for start := 0; start < frames; start += divide {
				end := start + divide
				if end > frames {
					end = frames
				}

				// Build sub-gif
				sub := &gif.GIF{
					Image:     make([]*image.Paletted, 0, end-start),
					Delay:     make([]int, 0, end-start),
					LoopCount: g.LoopCount,
				}
				for i := start; i < end; i++ {
					sub.Image = append(sub.Image, g.Image[i])
					if i < len(g.Delay) {
						sub.Delay = append(sub.Delay, g.Delay[i])
					} else {
						sub.Delay = append(sub.Delay, 10) // fallback
					}
				}

				var buf bytes.Buffer
				if err := gif.EncodeAll(&buf, sub); err != nil {
					return fmt.Errorf("failed to encode gif part: %w", err)
				}
				partBuf := buf.Bytes()

				// For subsequent parts, avoid reinit/clear to maintain continuous playback
				partOpts := opts
				if start > 0 {
					partOpts.ForceReinit = false
					partOpts.SkipClear = true
				}

				// Send with retry logic per part
				maxRetries := 3
				var lastErr error
				for attempt := 1; attempt <= maxRetries; attempt++ {
					err := d.sendGifAttempt(partBuf, partOpts)
					if err == nil {
						lastErr = nil
						break
					}
					lastErr = err
					if d.debug {
						fmt.Printf("[BLE] Send part attempt %d/%d failed: %v\n", attempt, maxRetries, err)
					}
					if err.Error() == "device not connected" || err.Error() == "device is busy" {
						return err
					}
					if attempt < maxRetries {
						time.Sleep(time.Duration(attempt*50) * time.Millisecond)
					}
				}
				if lastErr != nil {
					return fmt.Errorf("failed sending gif part starting at frame %d: %w", start, lastErr)
				}
			}
			return nil
		}
	}

	// Fallback: send whole GIF with retry logic as before
	maxRetries := 3
	var lastErr error
	for attempt := 1; attempt <= maxRetries; attempt++ {
		err := d.sendGifAttempt(gifBuffer, opts)
		if err == nil {
			return nil
		}

		lastErr = err
		if d.debug {
			fmt.Printf("[BLE] Send attempt %d/%d failed: %v\n", attempt, maxRetries, err)
		}

		// Don't retry if device is not connected or is busy
		if err.Error() == "device not connected" || err.Error() == "device is busy" {
			return err
		}

		// Wait before retry with exponential backoff
		if attempt < maxRetries {
			waitTime := time.Duration(attempt*50) * time.Millisecond
			if d.debug {
				fmt.Printf("[BLE] Retrying in %v...\n", waitTime)
			}
			time.Sleep(waitTime)
		}
	}

	return fmt.Errorf("failed after %d attempts: %w", maxRetries, lastErr)
}

// sendGifAttempt performs a single attempt to send the GIF
func (d *Device) sendGifAttempt(gifBuffer []byte, opts SendOptions) error {
	// Check for chunk limits config
	chunkLimits := true
	if MainConfigProvider != nil {
		chunkLimits = MainConfigProvider()
	}
	if !d.isConnected {
		return errors.New("device not connected")
	}

	if d.isBusy {
		if d.debug {
			fmt.Println("[BLE] Device is busy, cannot send GIF")
		}
		return errors.New("device is busy")
	}

	meta, metaErr := parseGifMeta(gifBuffer)
	if d.debug {
		if metaErr == nil {
			fmt.Printf("[BLE] Sending GIF: %d bytes, frames=%d, duration=%v (slot: 0x%02x)\n", len(gifBuffer), meta.frames, meta.duration, d.slot)
		} else {
			fmt.Printf("[BLE] Sending GIF: %d bytes (slot: 0x%02x), meta error: %v\n", len(gifBuffer), d.slot, metaErr)
		}
	}

	d.isBusy = true
	defer func() {
		d.isBusy = false
	}()

	// Optionally rerun init handshake to flush any queued playlist/state
	if opts.ForceReinit {
		if d.debug {
			fmt.Println("[BLE] Re-initializing device before send...")
		}
		if err := d.initialize(); err != nil {
			return fmt.Errorf("failed to reinitialize device: %w", err)
		}
	}

	// Clear/stop any previous playback to avoid old playlists continuing
	if !opts.SkipClear {
		if d.debug {
			fmt.Println("[BLE] Clearing previous playback state...")
		}
		if err := d.send(clearPlaybackSequence()); err != nil {
			if opts.BestEffortClear {
				if d.debug {
					fmt.Printf("[BLE] Clear playback failed (continuing): %v\n", err)
				}
			} else {
				return fmt.Errorf("failed to clear playback: %w", err)
			}
		} else {
			time.Sleep(commandDelay)
		}
	}

	// Select slot command (use dedicated static slot when requested, allow override)
	slot := d.slot
	if opts.StaticHeader {
		slot = staticSlot
	}
	if opts.SlotOverride != nil {
		slot = *opts.SlotOverride
	}
	if d.debug {
		fmt.Printf("[BLE] Selecting slot 0x%02x...\n", slot)
	}
	selectSlot := []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x0d, 0x00, 0x04, 0x02, slot, 0xff, 0x00}
	if err := d.send(selectSlot); err != nil {
		return fmt.Errorf("failed to select slot: %w", err)
	}
	time.Sleep(commandDelay)

	// Build complete packet (log-compat, static, or animated/header style)
	var packet []byte
	if opts.LogHeader {
		packet = d.buildLogCompatPacket(gifBuffer, slot)
	} else if opts.StaticHeader {
		packet = d.buildStaticGifPacket(gifBuffer, slot)
	} else {
		packet = d.buildGifPacket(gifBuffer)
	}

	if d.debug {
		fmt.Printf("[BLE] Built packet: %d bytes total (header: 24, data: %d)\n", len(packet), len(gifBuffer))
	}

	if !chunkLimits {
		// Send all at once, no chunking or pacing
		if d.debug {
			fmt.Println("[BLE] Sending full packet in one go (CHUNK_LIMITS=false)")
		}
		if err := d.send(packet); err != nil {
			return fmt.Errorf("failed to send packet: %w", err)
		}
	} else {
		// Determine chunk delay based on GIF size (short, vendor-like pacing)
		var chunkDelay time.Duration
		if len(gifBuffer) > gifSizeThreshold {
			chunkDelay = largeGifDelay
		} else {
			chunkDelay = smallGifDelay
		}

		// Send packet in smaller ATT-sized chunks with periodic pauses to mimic vendor pacing
		offset := 0
		chunkNum := 0
		totalChunks := (len(packet) + maxChunkSize - 1) / maxChunkSize
		bytesSincePause := 0

		for offset < len(packet) {
			end := offset + maxChunkSize
			if end > len(packet) {
				end = len(packet)
			}

			chunk := packet[offset:end]
			chunkNum++

			if d.debug {
				fmt.Printf("[BLE] Sending chunk %d/%d (%d bytes)...\n", chunkNum, totalChunks, len(chunk))
			}

			if err := d.send(chunk); err != nil {
				return fmt.Errorf("failed to send chunk: %w", err)
			}

			offset += maxChunkSize
			bytesSincePause += len(chunk)

			time.Sleep(chunkDelay)

			if bytesSincePause >= burstPaceBytes {
				if d.debug {
					fmt.Printf("[BLE] Pausing %v after %d bytes to match vendor pacing...\n", burstPause, bytesSincePause)
				}
				time.Sleep(burstPause)
				bytesSincePause = 0
			}
		}
	}

	// Wait for device to finish processing
	if d.debug {
		fmt.Println("[BLE] All chunks sent, waiting for device to process...")
	}

	finishWait := smallGifFinish
	if len(gifBuffer) > 500 {
		finishWait = largeGifFinish
	}
	if metaErr == nil && meta.frames > 1 {
		// Ensure the device has time to stage the full animation; add a small buffer
		finishWait = meta.duration + 150*time.Millisecond
	}

	if d.debug {
		fmt.Printf("[BLE] Waiting %v for device processing...\n", finishWait)
	}
	time.Sleep(finishWait)

	if d.debug {
		fmt.Println("[BLE] ✓ GIF sent successfully!")
	}

	return nil
}

// Disconnect closes the connection to the device
func (d *Device) Disconnect() error {
	if !d.isConnected {
		return nil
	}

	if d.debug {
		fmt.Println("[BLE] Disconnecting from device...")
	}

	err := d.device.Disconnect()
	d.isConnected = false

	if d.debug && err == nil {
		fmt.Println("[BLE] ✓ Disconnected successfully")
	}

	return err
}

// IsConnected returns the connection status
func (d *Device) IsConnected() bool {
	return d.isConnected
}

// GetBrightness returns the current brightness level
func (d *Device) GetBrightness() uint8 {
	return d.currentBright
}

// Helper function for string contains (case-insensitive)
func contains(s, substr string) bool {
	if len(substr) > len(s) {
		return false
	}
	for i := 0; i <= len(s)-len(substr); i++ {
		match := true
		for j := 0; j < len(substr); j++ {
			if s[i+j] != substr[j] {
				match = false
				break
			}
		}
		if match {
			return true
		}
	}
	return false
}
