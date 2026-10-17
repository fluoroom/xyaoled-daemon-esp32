# Build stage
FROM golang:1.21-alpine AS builder

# Install build dependencies
RUN apk add --no-cache git

# Set working directory
WORKDIR /app

# Copy go mod files
COPY go.mod go.sum ./

# Download dependencies
RUN go mod download

# Copy source code
COPY . .

# Build the application
RUN CGO_ENABLED=0 GOOS=linux go build -a -installsuffix cgo -o xyaoled-daemon .

# Final stage
FROM alpine:latest

# Install ca-certificates for HTTPS and bluetooth support
RUN apk --no-cache add ca-certificates bluez bluez-deprecated dbus

WORKDIR /root/

# Copy the binary from builder
COPY --from=builder /app/xyaoled-daemon .

# Copy default.gif if it exists
COPY --from=builder /app/default.gif .

# Copy .env if it exists (optional, can be overridden by environment variables)
COPY --from=builder /app/.env* ./

# Create necessary directories for D-Bus
RUN mkdir -p /var/run/dbus

# Expose the HTTP port
EXPOSE 11001

# Set environment variables with defaults
ENV HTTP_PORT=11001 \
    HTTP_HOST=0.0.0.0 \
    LED_PANEL_MAC=AA:BB:CC:DD:EE:FF \
    LED_DISPLAY_WIDTH=96 \
    LED_DISPLAY_HEIGHT=16 \
    DEBUG_BLE=false

# Run the daemon
CMD ["./xyaoled-daemon"]
