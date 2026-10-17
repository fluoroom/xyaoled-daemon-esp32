# Docker Setup for XYAOLED Daemon

This document provides instructions for running the XYAOLED Daemon using Docker.

## Quick Start

### 1. Build the Docker Image

```bash
docker build -t xyaoled-daemon .
```

### 2. Configure Environment Variables

Create a `.env` file from the example:

```bash
cp .env.example .env
```

Edit `.env` and set your LED panel MAC address:

```env
LED_PANEL_MAC=2A:4D:FD:AB:AE:16  # Replace with your panel's MAC
HTTP_PORT=11001
```

### 3. Run with Docker Compose (Recommended)

```bash
docker-compose up -d
```

### 4. Run with Docker CLI

```bash
docker run -d \
  --name xyaoled-daemon \
  --network host \
  --privileged \
  -v /var/run/dbus:/var/run/dbus \
  -v /sys/class/bluetooth:/sys/class/bluetooth \
  --device /dev/bus/usb:/dev/bus/usb \
  -e LED_PANEL_MAC=2A:4D:FD:AB:AE:16 \
  -e HTTP_PORT=11001 \
  -p 11001:11001 \
  xyaoled-daemon
```

## Docker Configuration

### Environment Variables

All configuration is done via environment variables:

- `LED_PANEL_MAC` - Bluetooth MAC address of your LED panel (required)
- `LED_DISPLAY_WIDTH` - Display width in pixels (default: 96)
- `LED_DISPLAY_HEIGHT` - Display height in pixels (default: 16)
- `HTTP_PORT` - HTTP server port (default: 11001)
- `HTTP_HOST` - HTTP server host (default: 0.0.0.0)
- `DEBUG_BLE` - Enable Bluetooth debugging (default: false)

### Bluetooth Access

The container requires special privileges for Bluetooth access:

- `--network host` - Uses host networking for Bluetooth
- `--privileged` - Required for Bluetooth device access
- `-v /var/run/dbus:/var/run/dbus` - D-Bus socket for Bluetooth communication
- `-v /sys/class/bluetooth:/sys/class/bluetooth` - Bluetooth system interface
- `--device /dev/bus/usb:/dev/bus/usb` - USB device access

## Docker Compose

The included `docker-compose.yml` handles all the necessary configuration:

```yaml
services:
  xyaoled-daemon:
    build: .
    ports:
      - "11001:11001"
    environment:
      - LED_PANEL_MAC=${LED_PANEL_MAC}
    network_mode: host
    privileged: true
```

### Commands

Start the service:
```bash
docker-compose up -d
```

View logs:
```bash
docker-compose logs -f
```

Stop the service:
```bash
docker-compose down
```

Rebuild after changes:
```bash
docker-compose up -d --build
```

## Usage

Once running, the API is available at `http://localhost:11001`:

### Display an Image

```bash
curl -X POST http://localhost:11001/display \
  -H "Content-Type: image/png" \
  --data-binary @image.png
```

### Health Check

```bash
curl http://localhost:11001/health
```

## Troubleshooting

### Bluetooth Connection Issues

1. Check if Bluetooth is available on the host:
```bash
docker exec xyaoled-daemon hciconfig
```

2. Verify D-Bus socket is accessible:
```bash
docker exec xyaoled-daemon ls -la /var/run/dbus
```

3. Check container logs:
```bash
docker-compose logs -f
```

### Permission Issues

Ensure your user has Bluetooth permissions on the host system:

```bash
sudo usermod -a -G bluetooth $USER
```

Restart the Docker daemon if needed:

```bash
sudo systemctl restart docker
```

## Building for Production

### Multi-stage Build

The Dockerfile uses a multi-stage build to create a minimal image:

1. **Builder stage** - Compiles the Go application
2. **Runtime stage** - Minimal Alpine Linux with only the binary

Final image size: ~15-20 MB

### Security Considerations

- The container runs as root (required for Bluetooth)
- Uses `--privileged` mode (required for hardware access)
- Consider network isolation if not using Bluetooth

## Development

### Live Development with Docker

Mount your local code for development:

```bash
docker run -it --rm \
  --network host \
  --privileged \
  -v $(pwd):/app \
  -v /var/run/dbus:/var/run/dbus \
  -w /app \
  golang:1.21-alpine \
  go run .
```

### Testing without Bluetooth Hardware

For development without hardware, you can test the HTTP API:

```bash
docker run -d \
  -p 11001:11001 \
  -e LED_PANEL_MAC=00:00:00:00:00:00 \
  xyaoled-daemon
```

Note: Bluetooth operations will fail, but the HTTP server will start.
