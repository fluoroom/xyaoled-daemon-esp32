// Package xyaoled provides a native Go implementation of the XYAOLED BLE LED display protocol.
// This is a professional reverse-engineering port from the Node.js leo library.
package xyaoled

import (
	"bytes"
	"errors"
	"fmt"
	"image/gif"
	"strings"
	"time"

	"tinygo.org/x/bluetooth"
)

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

	// 88ff Chunked Protocol Constants
	// Based on analysis of lonelygifsmall.txt bluetooth dump
	protocol88ffHeader = 0x88ff      // Extended data transfer protocol
	protocol99aaHeader = 0x99aa      // Standard command protocol
	dataChunkSize      = 20          // Bytes per 88ff data chunk
	cmdSubInit         = 0x0f        // Initialize/connection setup
	cmdSubFrameHeader  = 0x1f        // Frame header with metadata
	cmdSubFrameControl = 0x11        // Frame control/transition
	cmdSubFinalize     = 0x0d        // Finalize/end marker
	chunkDelay88ff     = 2 * time.Millisecond // Delay between 88ff chunks
)

// ChunkLimits controls whether the sender obeys ATT-sized chunking, pacing and
// periodic pauses when uploading GIF packets. When false, the code will attempt
// to send the full packet in a single write for animated GIFs.
var ChunkLimits = true

// SetChunkLimits sets the package-level ChunkLimits flag.
func SetChunkLimits(v bool) {
	ChunkLimits = v
}

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
	if err != nil {
		// Mark wrapper as disconnected so connection monitor can pick this up
		d.isConnected = false
	}
	return err
}

// Ping performs a lightweight write to detect broken connections. It does
// not alter any device state except marking the device disconnected on
// failure. Returns nil on success or an error on failure.
func (d *Device) Ping() error {
	if !d.isConnected {
		return errors.New("device not connected")
	}
	// A zero-length write may not be allowed on all stacks; use a single
	// zero byte which is unlikely to have side effects.
	_, err := d.writeChar.WriteWithoutResponse([]byte{0x00})
	if err != nil {
		d.isConnected = false
		return err
	}
	return nil
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

		// If the error indicates the device is not connected, reinitialization
		// failed, or a DBus/WriteValue signature issue, do not retry the image
		// upload — return immediately so callers get the real error and can
		// decide what to do (we'll trigger reconnect elsewhere).
		if err.Error() == "device not connected" || err.Error() == "device is busy" {
			return err
		}
		if strings.Contains(err.Error(), "failed to reinitialize") || strings.Contains(err.Error(), "Not connected") || strings.Contains(err.Error(), "WriteValue") {
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

	// Determine chunk delay based on GIF size (short, vendor-like pacing)
	var chunkDelay time.Duration
	if len(gifBuffer) > gifSizeThreshold {
		chunkDelay = largeGifDelay
	} else {
		chunkDelay = smallGifDelay
	}

	// If chunk limits are disabled and this is an animated GIF (we have meta),
	// attempt to send the full packet in a single write and skip ATT-sized chunking
	if !ChunkLimits && metaErr == nil && meta.frames > 1 {
		if d.debug {
			fmt.Printf("[BLE] Chunk limits disabled: sending full packet in one write (%d bytes)\n", len(packet))
		}
		if err := d.send(packet); err != nil {
			return fmt.Errorf("failed to send full packet: %w", err)
		}
	} else {
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

// ========================================================================
// 88ff Chunked Protocol Implementation
// ========================================================================
// Based on bluetooth dump analysis from lonelygifsmall.txt
// The vendor app uses two protocols:
// - 99aa: Command/control protocol (init, headers, transitions)
// - 88ff: Data transfer protocol (frame pixel data in chunks)
//
// This allows unlimited frames by chunking data into 20-byte writes
// instead of trying to send everything in one large write.

// Send88ffChunk sends a single data chunk using the 88ff protocol.
// frameId: Frame number (0-based)
// chunkId: Chunk number within the frame (0-based)
// data: Payload data (typically 20 bytes or less for final chunk)
func (d *Device) Send88ffChunk(frameId, chunkId byte, data []byte) error {
	if !d.isConnected {
		return errors.New("device not connected")
	}

	// Build 88ff protocol packet: [0x88, 0xff, frameId, chunkId, ...data]
	packet := make([]byte, 0, 4+len(data))
	packet = append(packet, 0x88, 0xff)
	packet = append(packet, frameId, chunkId)
	packet = append(packet, data...)

	if d.debug {
		fmt.Printf("[BLE] Sending 88ff chunk: frame=%d chunk=%d size=%d\n", frameId, chunkId, len(data))
	}

	_, err := d.writeChar.WriteWithoutResponse(packet)
	if err != nil {
		return fmt.Errorf("failed to write 88ff chunk: %w", err)
	}

	// Small delay between chunks to avoid overwhelming device
	time.Sleep(chunkDelay88ff)
	return nil
}

// SendFrameCommand sends a 99aa protocol command.
// subcmd: Subcommand byte (0x0f=init, 0x1f=frame header, 0x11=control, 0x0d=finalize)
// params: Command-specific parameters
func (d *Device) SendFrameCommand(subcmd byte, params []byte) error {
	if !d.isConnected {
		return errors.New("device not connected")
	}

	// Build 99aa protocol packet: [0x99, 0xaa, length_low, length_high, 0xff, 0x88, subcmd, ...params]
	payloadLen := 4 + len(params) // ff88 + subcmd + params
	packet := make([]byte, 0, 4+payloadLen)
	packet = append(packet, 0x99, 0xaa)
	packet = append(packet, byte(payloadLen&0xff), byte((payloadLen>>8)&0xff)) // Little-endian length
	packet = append(packet, 0xff, 0x88, subcmd)
	packet = append(packet, params...)

	if d.debug {
		fmt.Printf("[BLE] Sending 99aa command: subcmd=0x%02x params_len=%d\n", subcmd, len(params))
	}

	_, err := d.writeChar.WriteWithoutResponse(packet)
	if err != nil {
		return fmt.Errorf("failed to write frame command: %w", err)
	}

	time.Sleep(commandDelay)
	return nil
}

// chunkData splits data into chunks of specified size
func chunkData(data []byte, chunkSize int) [][]byte {
	if chunkSize <= 0 {
		chunkSize = dataChunkSize
	}

	var chunks [][]byte
	for i := 0; i < len(data); i += chunkSize {
		end := i + chunkSize
		if end > len(data) {
			end = len(data)
		}
		chunks = append(chunks, data[i:end])
	}
	return chunks
}

// SendGifWithChunkedProtocol sends an animated GIF using the 88ff chunked protocol.
// This allows sending GIFs with 18+ frames by chunking the data transfer.
//
// Protocol sequence per frame:
//  1. Send 99aa init command (first frame only)
//  2. Send 99aa frame header with timing/display params
//  3. Send frame pixel data in 20-byte chunks via 88ff
//  4. Send 99aa frame control command
//  5. Repeat for each frame
//  6. Send 99aa finalize command
func (d *Device) SendGifWithChunkedProtocol(gifData []byte, frameCount int) error {
	if !d.isConnected {
		return errors.New("device not connected")
	}

	if d.debug {
		fmt.Printf("[BLE] Starting chunked protocol send: %d bytes, %d frames\n", len(gifData), frameCount)
	}

	// Step 1: Send initialization command (0x0f)
	// Format observed: 0f 00 0a 00 01 00 00 00 00
	initParams := []byte{0x00, 0x0a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}
	if err := d.SendFrameCommand(cmdSubInit, initParams); err != nil {
		return fmt.Errorf("init command failed: %w", err)
	}

	// Parse GIF to extract individual frames
	g, err := gif.DecodeAll(bytes.NewReader(gifData))
	if err != nil {
		return fmt.Errorf("failed to decode GIF: %w", err)
	}

	if d.debug {
		fmt.Printf("[BLE] Decoded GIF: %d frames\n", len(g.Image))
	}

	// Step 2-5: Process each frame
	for frameIdx, img := range g.Image {
		if d.debug {
			fmt.Printf("[BLE] Processing frame %d/%d\n", frameIdx+1, len(g.Image))
		}

		// Get frame delay (in centiseconds, convert to useful format)
		delay := uint16(0x0100) // Default delay
		if frameIdx < len(g.Delay) && g.Delay[frameIdx] > 0 {
			delay = uint16(g.Delay[frameIdx])
		}

		// Send frame header command (0x1f)
		// Format observed: 1f 00 00 00 02 00 1a 01 03 11 15 1c 06 01 00 09 20 71 86 8e bf 00 00 00 00
		// This contains timing, dimensions, and display parameters
		frameHeaderParams := []byte{
			0x00, 0x00, 0x00, byte(frameIdx + 1), // Frame sequence
			0x00, 0x1a, 0x01, 0x03,               // Display parameters
			0x11, 0x15, 0x1c, 0x06,               // More parameters
			0x01, 0x00,                           // Flags
			byte(delay & 0xff), byte((delay >> 8) & 0xff), // Delay
			0x71, 0x86, 0x8e, 0xbf, // Additional params
			0x00, 0x00, 0x00, 0x00, // Padding
		}
		if err := d.SendFrameCommand(cmdSubFrameHeader, frameHeaderParams); err != nil {
			return fmt.Errorf("frame %d header failed: %w", frameIdx, err)
		}

		// Convert frame image to raw pixel data
		bounds := img.Bounds()
		frameData := make([]byte, 0, bounds.Dx()*bounds.Dy()*3) // RGB data
		for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
			for x := bounds.Min.X; x < bounds.Max.X; x++ {
				r, g, b, _ := img.At(x, y).RGBA()
				frameData = append(frameData, byte(r>>8), byte(g>>8), byte(b>>8))
			}
		}

		// Send frame data in chunks via 88ff protocol
		chunks := chunkData(frameData, dataChunkSize)
		for chunkIdx, chunk := range chunks {
			if err := d.Send88ffChunk(byte(frameIdx), byte(chunkIdx), chunk); err != nil {
				return fmt.Errorf("frame %d chunk %d failed: %w", frameIdx, chunkIdx, err)
			}
		}

		// Send frame control command (0x11)
		// Format observed: 11 00 05 00 03 01 01 00 00 00 00
		frameControlParams := []byte{
			0x00, 0x05, 0x00, byte(frameIdx + 1),
			0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
		}
		if err := d.SendFrameCommand(cmdSubFrameControl, frameControlParams); err != nil {
			return fmt.Errorf("frame %d control failed: %w", frameIdx, err)
		}
	}

	// Step 6: Send finalize command (0x0d)
	// Format observed: 0d 00 04 02 04 ff 00
	finalizeParams := []byte{0x00, 0x04, 0x02, 0x04, 0xff, 0x00}
	if err := d.SendFrameCommand(cmdSubFinalize, finalizeParams); err != nil {
		return fmt.Errorf("finalize command failed: %w", err)
	}

	// Wait for device to process all frames
	totalDelay := time.Duration(len(g.Image)) * 100 * time.Millisecond
	if d.debug {
		fmt.Printf("[BLE] Waiting %v for device to process %d frames...\n", totalDelay, len(g.Image))
	}
	time.Sleep(totalDelay)

	if d.debug {
		fmt.Println("[BLE] ✓ Chunked protocol send complete!")
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
