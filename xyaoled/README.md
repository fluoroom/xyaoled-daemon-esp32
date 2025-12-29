# XYAOLED Package

Professional Go implementation of the XYAOLED BLE LED display protocol, reverse-engineered from the [leo Node.js library](https://github.com/user/leo).

## Overview

This package provides native Go/TinyGo support for controlling XYAOLED Bluetooth LED displays without external dependencies. The implementation is a complete, professional port of the Node.js protocol implementation.

## Protocol Details

### BLE Characteristics
- **Service UUID**: `ae00`
- **Write Characteristic**: `ae01` - For sending commands and data
- **Notify Characteristic**: `ae02` - For receiving device notifications

### Communication Protocol

#### Initialization Sequence
The device requires a 5-command initialization handshake after connection:
1. Initial handshake with device parameters
2. Configuration command (0x03)
3. Configuration command (0x04)
4. Configuration command (0x05)
5. Final configuration command (0x06)

Each command is followed by specific delays (300ms for initial, 30ms for others).

#### Brightness Control
Brightness packets follow this structure:
```
[0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x10, 0x00, 0x12, 0x00, seq, level, level*0.8, 0x00, 0x00, 0x00]
```
- `seq`: Incrementing sequence number
- `level`: Brightness 1-100
- Third brightness byte is calculated as `level * 0.8`

#### GIF Transmission
GIFs are sent in 80-byte chunks with a 24-byte header:

**Header Structure (24 bytes):**
- Bytes 0-5: Protocol magic `[0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88]`
- Bytes 6-7: Total packet size (little-endian uint16)
- Bytes 8-10: Command `[0x07, 0x02, slot]`
- Bytes 11-23: Size-dependent parameters (small vs large GIF)

**Small GIFs (≤300 bytes):**
```
[0x0c, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0xff, 0xe0]
```

**Large GIFs (>300 bytes):**
```
[gifSize(LE), 0x00, 0x02, 0x00, 0x00, 0x01, 0x20, 0x03, 0x06, 0x00, 0x01, 0x00]
```

### Timing Constraints
- **Init delay**: 300ms after first command
- **Command delay**: 30ms between commands
- **Small GIF chunk delay**: 10ms
- **Large GIF chunk delay**: 15ms
- **Small GIF finish**: 50ms post-transmission
- **Large GIF finish**: 100ms post-transmission

## Usage

### Basic Connection

```go
import "travelsign/xyaoled"

// Create device
device, err := xyaoled.NewDevice()
if err != nil {
    log.Fatal(err)
}

// Connect with 30-second timeout
err = device.Connect(30 * time.Second)
if err != nil {
    log.Fatal(err)
}
defer device.Disconnect()
```

### Setting Brightness

```go
// Set brightness to 80% (valid range: 1-100)
err = device.SetBrightness(80)
if err != nil {
    log.Printf("Failed to set brightness: %v", err)
}
```

### Sending Images

```go
// Encode your image as GIF
var buf bytes.Buffer
err = gif.Encode(&buf, myImage, &gif.Options{NumColors: 2})
if err != nil {
    log.Fatal(err)
}

// Send to display
err = device.SendGif(buf.Bytes())
if err != nil {
    log.Printf("Failed to send GIF: %v", err)
}
```

### Complete Example

```go
package main

import (
    "bytes"
    "image"
    "image/color"
    "image/gif"
    "log"
    "time"
    "travelsign/xyaoled"
)

func main() {
    // Create and connect
    device, err := xyaoled.NewDevice()
    if err != nil {
        log.Fatal(err)
    }
    
    log.Println("Scanning for XYAOLED device...")
    err = device.Connect(30 * time.Second)
    if err != nil {
        log.Fatal(err)
    }
    defer device.Disconnect()
    
    log.Println("Connected!")
    
    // Set brightness
    device.SetBrightness(70)
    
    // Create a simple test image (96x16)
    img := image.NewPaletted(
        image.Rect(0, 0, 96, 16),
        color.Palette{color.Black, color.RGBA{0, 255, 0, 255}},
    )
    
    // Draw something
    for x := 0; x < 96; x++ {
        img.SetColorIndex(x, 8, 1)
    }
    
    // Encode as GIF
    var buf bytes.Buffer
    gif.Encode(&buf, img, &gif.Options{NumColors: 2})
    
    // Send to display
    err = device.SendGif(buf.Bytes())
    if err != nil {
        log.Printf("Failed to send: %v", err)
    } else {
        log.Println("Image sent successfully!")
    }
}
```

## API Reference

### Types

#### `Device`
Represents a connection to an XYAOLED display.

### Functions

#### `NewDevice() (*Device, error)`
Creates a new device instance and enables the BLE adapter.

#### `(d *Device) Connect(timeout time.Duration) error`
Scans for and connects to an XYAOLED device. Returns error if timeout is reached.

#### `(d *Device) Disconnect() error`
Closes the BLE connection.

#### `(d *Device) SetBrightness(level uint8) error`
Sets display brightness (1-100). Values are automatically clamped to valid range.

#### `(d *Device) SendGif(gifBuffer []byte) error`
Sends a GIF buffer to the display. Handles packet building and chunking automatically.
Returns error if device is busy or disconnected.

#### `(d *Device) IsConnected() bool`
Returns current connection status.

#### `(d *Device) GetBrightness() uint8`
Returns current brightness level.

## Technical Notes

### Display Specifications
- Resolution: 96x16 pixels
- Color: Typically single-color (green/red/blue variants exist)
- Protocol: Custom BLE protocol over GATT

### GIF Requirements
- Format: GIF87a or GIF89a
- Recommended: 2-color palette for best results
- Size: Keep under 2KB for optimal performance

### Performance Considerations
- The device has a busy flag to prevent concurrent transmissions
- Larger GIFs (>500 bytes) require longer processing delays
- Chunk size is fixed at 80 bytes per BLE write
- Frame throttling recommended (~100ms minimum between updates)

### Tested Environments
- Linux with BlueZ
- macOS with Core Bluetooth (via TinyGo)
- Raspberry Pi (TinyGo native)

## Differences from Node.js Implementation

1. **Scanning**: Auto-discovers XYAOLED devices instead of requiring MAC address
2. **Type Safety**: Go's type system prevents many runtime errors
3. **Error Handling**: Explicit error returns instead of event emitters
4. **Synchronous**: Blocking calls instead of async/await (simpler for embedded)
5. **No Noble**: Uses TinyGo's native bluetooth package

## Credits

Protocol reverse-engineered from the excellent [leo](https://github.com/user/leo) Node.js library.
Ported professionally to Go with attention to protocol accuracy and Go idioms.

## License

Same as parent project.
