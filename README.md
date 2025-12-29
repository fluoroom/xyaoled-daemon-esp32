# XYAOLED Daemon

HTTP daemon that receives images via REST API and displays them on an XYAOLED LED panel via Bluetooth.

## Features

- **Persistent BLE Connection**: Maintains constant Bluetooth connection to the LED panel
- **HTTP REST API**: Receives images via POST endpoint
- **Image Processing**: Accepts any image format (PNG, JPEG, GIF), resizes to 96x16
- **Animated GIF Support**: Plays animated GIFs at configurable FPS
- **Custom Color Palettes**: Optimizes GIF encoding with dynamic palette generation

## Installation

```bash
cd /home/nic0/projects/xyaoled-daemon
go mod download
go build -o xyaoled-daemon .
```

## Configuration

Copy `.env.example` to `.env` and configure:

```bash
cp .env.example .env
```

Edit `.env`:
```env
# LED Panel Settings
LED_PANEL_MAC=2A:4D:FD:AB:AE:16  # Your panel's MAC address
LED_DISPLAY_WIDTH=96
LED_DISPLAY_HEIGHT=16
LED_FPS=1                          # Frames per second

# Server Settings
HTTP_PORT=8080
HTTP_HOST=0.0.0.0

# Debug Settings
DEBUG_BLE=false
VERBOSE_LOGGING=false
```

## Usage

### Start the Daemon

```bash
./xyaoled-daemon
```

The daemon will:
1. Connect to the LED panel via Bluetooth
2. Start HTTP server on configured port
3. Wait for image display requests

### API Endpoints

#### POST /display

Display an image on the LED panel.

**Request:**
```json
{
  "image": "base64_encoded_image_data",
  "type": "static"  // or "animated" for GIFs
}
```

**Response:**
```json
{
  "status": "ok"
}
```

**Example (static image):**
```bash
# Encode image to base64
BASE64_IMG=$(base64 -w 0 myimage.png)

# Send to daemon
curl -X POST http://localhost:8080/display \
  -H "Content-Type: application/json" \
  -d "{\"image\":\"$BASE64_IMG\",\"type\":\"static\"}"
```

**Example (animated GIF):**
```bash
# Encode GIF to base64
BASE64_GIF=$(base64 -w 0 animation.gif)

# Send to daemon
curl -X POST http://localhost:8080/display \
  -H "Content-Type: application/json" \
  -d "{\"image\":\"$BASE64_GIF\",\"type\":\"animated\"}"
```

#### GET /health

Health check endpoint.

**Response:**
```json
{
  "status": "healthy",
  "config": {
    "width": 96,
    "height": 16,
    "fps": 1
  }
}
```

## Image Processing

The daemon automatically:
- Decodes PNG, JPEG, and GIF formats
- Resizes images to configured display dimensions (default 96x16)
- Generates optimized color palette for each frame
- Converts to GIF format for XYAOLED protocol
- Throttles updates to configured FPS

## Animated GIF Playback

When type is "animated":
- Daemon extracts all frames from the GIF
- Plays frames at the global FPS rate (from LED_FPS config)
- Original GIF timing is ignored
- New animation interrupts previous one

## Architecture

```
HTTP Request → Handler → Display → Image Processing → GIF Encoding → BLE → LED Panel
```

Components:
- `main.go`: Server setup and lifecycle
- `handler.go`: HTTP request handling
- `display.go`: Image processing and LED control
- `config.go`: Configuration management
- `xyaoled/`: Bluetooth LED panel protocol implementation

## Troubleshooting

### Connection Issues
- Ensure LED panel is powered on and discoverable
- Check MAC address in `.env` matches your panel
- Enable `DEBUG_BLE=true` for detailed logs

### Performance
- Reduce `LED_FPS` if frames are dropping
- Check network latency for large images
- Animated GIFs may need lower FPS (1-2)

### Image Quality
- LED panel only displays at 96x16 resolution
- Some panels are monochrome (single color only)
- Test with simple images first

## Development

Run with debug logging:
```bash
DEBUG_BLE=true ./xyaoled-daemon
```

## License

MIT
