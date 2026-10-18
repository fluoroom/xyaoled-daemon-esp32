// Package xyaoled provides a native Go implementation of the XYAOLED BLE LED display protocol.
// This is a professional reverse-engineering port from the Node.js leo library.
package xyaoled

import (
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"

	"tinygo.org/x/bluetooth"
)

const (
	// BLE Service and Characteristic UUIDs
	serviceUUID    = "ae00"
	writeCharUUID  = "ae01"
	notifyCharUUID = "ae02"

	// Protocol constants
	// defaultProgram is the program (playlist position) uploads replace.
	defaultProgram = 0x02
	maxBrightness  = 125
	minBrightness  = 0

	// Timing constants
	initDelay    = 300 * time.Millisecond
	commandDelay = 30 * time.Millisecond
)

// powerSequence returns the command that turns the panel itself on or off
// (type 0x0011), documented in github.com/samcgn/xyaoled-control. This is a
// hardware power state distinct from brightness-based "off" (dimming to 5).
func powerSequence(on bool, seq byte) []byte {
	state := byte(0x02)
	if on {
		state = 0x01
	}
	return []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x0e, 0x00, 0x11, 0x00, seq, state, 0x01, 0x01}
}

// clearPlaylistSequence returns the command that clears the screen and wipes
// the on-device playlist entirely (type 0x0005), documented in
// github.com/samcgn/xyaoled-control. Sent before every upload so a new
// program replaces the playlist instead of being appended to it.
func clearPlaylistSequence(seq byte) []byte {
	return []byte{0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x11, 0x00, 0x05, 0x00, seq, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00}
}

// nextSeq returns the next command sequence number. Every command carries
// its own number, as the vendor app does.
func (d *Device) nextSeq() byte {
	seq := d.seqNum
	d.seqNum++
	return seq
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
	program       uint8
	debug         bool
	notifyChan    chan []byte // Channel for receiving notifications

	// txMu serializes everything written to the device: a command written
	// in the middle of an upload corrupts the packet being reassembled.
	txMu           sync.Mutex
	lastAckCtr     byte
	haveLastAckCtr bool
}

// SendOptions controls how GIF payloads are written to the device.
// ForceReinit reruns the connection handshake before the upload; LogHeader
// selects the header used for static images; Program overrides the program
// (playlist position) the upload replaces.
type SendOptions struct {
	ForceReinit bool
	LogHeader   bool
	Program     *uint8
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
		program:       defaultProgram,
		isConnected:   false,
		debug:         debug,
		notifyChan:    make(chan []byte, 64), // Buffered channel for notifications
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

			// Send to channel for flow control (non-blocking)
			select {
			case d.notifyChan <- buf:
			default:
				// Channel full, skip (we only care about recent notifications)
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

// Ping detects broken connections by reading the link MTU over D-Bus. It
// writes nothing: stray bytes on ae01 land in the device's packet parser.
func (d *Device) Ping() error {
	if !d.isConnected {
		return errors.New("device not connected")
	}
	if _, err := d.writeChar.GetMTU(); err != nil {
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
	d.txMu.Lock()
	defer d.txMu.Unlock()

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
		fmt.Printf("[BLE] Setting brightness: level=%d, thirdByte=%d\n", level, thirdByte)
	}

	// Build brightness command with sequence number
	cmd := []byte{
		0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88,
		0x10, 0x00,
		0x12, 0x00,
		d.nextSeq(),
		level,
		thirdByte,
		0x00, 0x00, 0x00,
	}

	err := d.send(cmd)
	if err == nil && d.debug {
		fmt.Println("[BLE] ✓ Brightness set successfully")
	}
	return err
}

// Power turns the panel's own power state on or off. This is a hardware
// state distinct from SetBrightness: brightness 0 still leaves the panel
// "on" (just dark), while this actually powers it down.
func (d *Device) Power(on bool) error {
	if !d.isConnected {
		return errors.New("device not connected")
	}
	d.txMu.Lock()
	defer d.txMu.Unlock()
	return d.send(powerSequence(on, d.nextSeq()))
}

// ClearPlaylist clears the screen and wipes the on-device playlist, so
// previously uploaded programs no longer loop.
func (d *Device) ClearPlaylist() error {
	if !d.isConnected {
		return errors.New("device not connected")
	}
	d.txMu.Lock()
	defer d.txMu.Unlock()
	return d.send(clearPlaylistSequence(d.nextSeq()))
}

// firstPacketTrailer returns the 8 bytes that follow the packet count in the
// first upload packet. The device's meaning for them is not recovered; these
// are the values each header mode has always sent.
func firstPacketTrailer(opts SendOptions) [8]byte {
	switch {
	case opts.LogHeader:
		return [8]byte{0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00}
	default:
		return [8]byte{0x00, 0x01, 0x20, 0x03, 0x06, 0x00, 0x01, 0x00}
	}
}

// SendGif sends a GIF buffer to the display using default options
func (d *Device) SendGif(gifBuffer []byte) error {
	return d.SendGifWithOptions(gifBuffer, SendOptions{})
}

// SendGifWithOptions sends a GIF buffer with configurable behavior
func (d *Device) SendGifWithOptions(gifBuffer []byte, opts SendOptions) error {
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
		if err.Error() == "device not connected" || errors.Is(err, ErrPanelBusy) {
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

	d.txMu.Lock()
	defer d.txMu.Unlock()

	if d.debug {
		fmt.Printf("[BLE] Sending GIF: %d bytes\n", len(gifBuffer))
	}

	// Optionally rerun init handshake to flush any queued playlist/state
	if opts.ForceReinit {
		if d.debug {
			fmt.Println("[BLE] Re-initializing device before send...")
		}
		if err := d.initialize(); err != nil {
			return fmt.Errorf("failed to reinitialize device: %w", err)
		}
	}

	// Wipe the on-device playlist (best effort) so this upload replaces it
	// instead of being appended alongside whatever was already looping.
	if d.debug {
		fmt.Println("[BLE] Clearing playlist...")
	}
	// The panel never acks this command, so don't wait for one; instead give
	// it time to actually apply the clear before the next command lands,
	// otherwise the upload that follows races it and gets appended instead
	// of replacing the playlist.
	d.drainNotifications()
	if err := d.writePacket(clearPlaylistSequence(d.nextSeq())); err != nil && d.debug {
		fmt.Printf("[BLE] Clear playlist failed (continuing): %v\n", err)
	}
	time.Sleep(clearPlaylistSettleDelay)

	program := d.program
	if opts.Program != nil {
		program = *opts.Program
	}
	seq := d.nextSeq()
	packets := buildUploadPackets(gifBuffer, program, seq, firstPacketTrailer(opts))

	// A panel that answers data with 88ff05 has dropped out of receive mode:
	// select the program again and restart the upload.
	for attempt := 1; ; attempt++ {
		if d.debug {
			fmt.Printf("[BLE] Selecting program %d (seq %d)...\n", program, seq)
		}
		if err := d.selectProgram(program, seq); err != nil {
			return fmt.Errorf("failed to select program: %w", err)
		}
		if d.debug {
			fmt.Printf("[BLE] Uploading %d bytes in %d packet(s), %d-byte writes\n", len(gifBuffer), len(packets), d.attPayloadSize())
		}
		err := d.uploadPackets(packets)
		if err == nil {
			break
		}
		if !errors.Is(err, errNotReady) || attempt == maxPacketResends {
			return err
		}
	}

	if d.debug {
		fmt.Println("[BLE] ✓ GIF sent and acknowledged")
	}
	d.waitUploadApplied()
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
