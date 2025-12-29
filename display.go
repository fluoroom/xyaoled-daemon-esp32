package main

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"image"
	"image/color"
	"image/gif"
	"image/jpeg"
	"image/png"
	"log"
	"os"
	"time"
	"xyaoled-daemon/xyaoled"

	"golang.org/x/image/draw"
)

// Display handles the LED panel display logic
type Display struct {
	device        *xyaoled.Device
	width         int
	height        int
	fps           int
	frameDelay    time.Duration
	lastUpdate    time.Time
	debugBLE      bool
	animationSeq  int
	config        *Config
	reconnecting  bool
	lastGifData   []byte // Store last displayed content for redisplay after reconnect
	lastGifIsAnim bool   // Track if last content was animated
}

// NewDisplay creates a new display instance
func NewDisplay(cfg *Config) (*Display, error) {
	device, err := xyaoled.NewDevice(cfg.DebugBLE)
	if err != nil {
		return nil, fmt.Errorf("failed to create device: %w", err)
	}

	err = device.Connect(30 * time.Second)
	if err != nil {
		return nil, fmt.Errorf("failed to connect to LED panel: %w", err)
	}

	log.Println("✓ Successfully connected to XYAOLED LED panel!")

	// Set initial brightness to 80%
	if err := device.SetBrightness(80); err != nil {
		log.Printf("Warning: failed to set initial brightness: %v", err)
	}

	frameDelay := time.Second / time.Duration(cfg.FPS)

	display := &Display{
		device:       device,
		width:        cfg.DisplayWidth,
		height:       cfg.DisplayHeight,
		fps:          cfg.FPS,
		frameDelay:   frameDelay,
		lastUpdate:   time.Now(),
		debugBLE:     cfg.DebugBLE,
		config:       cfg,
		reconnecting: false,
	}

	// Start connection monitor
	go display.connectionMonitor()

	return display, nil
}

// loadAndDisplayDefaultGif loads and displays the default.gif file as a static startup image
func (d *Display) loadAndDisplayDefaultGif() error {
	// Read default.gif file
	data, err := os.ReadFile("default.gif")
	if err != nil {
		return fmt.Errorf("failed to read default.gif: %w", err)
	}

	// Decode GIF to get first frame
	gifImg, err := gif.DecodeAll(bytes.NewReader(data))
	if err != nil {
		return fmt.Errorf("failed to decode default.gif: %w", err)
	}

	if len(gifImg.Image) == 0 {
		return fmt.Errorf("default.gif has no frames")
	}

	// Get first frame
	frame := gifImg.Image[0]

	// Convert paletted image to RGBA
	bounds := frame.Bounds()
	rgba := image.NewRGBA(bounds)
	draw.Draw(rgba, bounds, frame, bounds.Min, draw.Src)

	// Process and send as static image
	gifData, err := d.processImage(rgba)
	if err != nil {
		return fmt.Errorf("failed to process default.gif: %w", err)
	}

	// Send to device once (no animation)
	if err := d.device.SendGif(gifData); err != nil {
		return fmt.Errorf("failed to send default.gif: %w", err)
	}

	log.Println("✓ Displayed default.gif on LED panel")
	return nil
}

// ShowImage displays a single image (base64 encoded)
func (d *Display) ShowImage(base64Data string) error {
	// Decode base64
	imgData, err := base64.StdEncoding.DecodeString(base64Data)
	if err != nil {
		return fmt.Errorf("failed to decode base64: %w", err)
	}

	// Detect image type and decode
	img, format, err := image.Decode(bytes.NewReader(imgData))
	if err != nil {
		return fmt.Errorf("failed to decode image: %w", err)
	}

	if d.debugBLE {
		log.Printf("[Display] Decoded %s image: %dx%d", format, img.Bounds().Dx(), img.Bounds().Dy())
	}

	// Resize and convert to GIF
	gifData, err := d.processImage(img)
	if err != nil {
		return fmt.Errorf("failed to process image: %w", err)
	}

	// Store last content for reconnection
	d.lastGifData = gifData
	d.lastGifIsAnim = false

	// Stop any running animation
	d.animationSeq++
	seq := d.animationSeq

	// Static images need to be re-sent periodically to stay on screen
	// Start a goroutine that keeps refreshing the static image
	go d.keepStaticImageAlive(gifData, seq)

	return nil
}

// ShowAnimatedGif displays an animated GIF (base64 encoded)
func (d *Display) ShowAnimatedGif(base64Data string) error {
	// Decode base64
	imgData, err := base64.StdEncoding.DecodeString(base64Data)
	if err != nil {
		return fmt.Errorf("failed to decode base64: %w", err)
	}

	// Decode GIF
	gifImg, err := gif.DecodeAll(bytes.NewReader(imgData))
	if err != nil {
		return fmt.Errorf("failed to decode GIF: %w", err)
	}

	if d.debugBLE {
		log.Printf("[Display] Decoded animated GIF: %d frames", len(gifImg.Image))
	}

	// Store last content for reconnection
	d.lastGifData = imgData
	d.lastGifIsAnim = true

	// Play animation at global FPS
	d.animationSeq++
	seq := d.animationSeq

	go d.playAnimation(gifImg, seq)

	return nil
}

// playAnimation plays an animated GIF at the global FPS rate
func (d *Display) playAnimation(gifImg *gif.GIF, seq int) {
	ticker := time.NewTicker(d.frameDelay)
	defer ticker.Stop()

	frameIdx := 0
	for range ticker.C {
		// Check if this animation is still current
		if seq != d.animationSeq {
			if d.debugBLE {
				log.Printf("[Display] Animation %d interrupted", seq)
			}
			return
		}

		// Get frame
		frame := gifImg.Image[frameIdx]

		// Convert paletted image to RGBA
		bounds := frame.Bounds()
		rgba := image.NewRGBA(bounds)
		draw.Draw(rgba, bounds, frame, bounds.Min, draw.Src)

		// Process and send
		gifData, err := d.processImage(rgba)
		if err != nil {
			log.Printf("Error processing frame %d: %v", frameIdx, err)
			return
		}

		err = d.device.SendGif(gifData)
		if err != nil {
			log.Printf("Error sending frame %d: %v (will attempt reconnection)", frameIdx, err)
			// Trigger reconnection
			go d.attemptReconnection()
			return
		}

		// Move to next frame
		frameIdx = (frameIdx + 1) % len(gifImg.Image)
	}
}

// keepStaticImageAlive periodically resends a static image to keep it displayed
func (d *Display) keepStaticImageAlive(gifData []byte, seq int) {
	// Send immediately
	if err := d.device.SendGif(gifData); err != nil {
		log.Printf("Error sending static image: %v", err)
		go d.attemptReconnection()
		return
	}

	// Then resend every 30 seconds to keep it alive
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()

	for range ticker.C {
		// Check if this image is still current
		if seq != d.animationSeq {
			if d.debugBLE {
				log.Printf("[Display] Static image %d interrupted", seq)
			}
			return
		}

		// Resend the image
		if err := d.device.SendGif(gifData); err != nil {
			log.Printf("Error refreshing static image: %v", err)
			go d.attemptReconnection()
			return
		}

		if d.debugBLE {
			log.Println("[Display] Static image refreshed")
		}
	}
}

// processImage resizes image to display dimensions and converts to optimized GIF
func (d *Display) processImage(img image.Image) ([]byte, error) {
	// Rotate 180 degrees if upside down
	if d.config.UpsideDown {
		img = d.rotate180(img)
	}

	// Resize to display dimensions
	resized := d.resizeImage(img, d.width, d.height)

	// Convert to GIF with custom palette
	return d.imageToGif(resized)
}

// rotate180 rotates an image 180 degrees
func (d *Display) rotate180(src image.Image) image.Image {
	bounds := src.Bounds()
	width := bounds.Dx()
	height := bounds.Dy()

	dst := image.NewRGBA(image.Rect(0, 0, width, height))

	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			// 180 degree rotation: (x, y) -> (width-1-x, height-1-y)
			dst.Set(width-1-x, height-1-y, src.At(bounds.Min.X+x, bounds.Min.Y+y))
		}
	}

	return dst
}

// resizeImage resizes an image to the specified dimensions
func (d *Display) resizeImage(src image.Image, width, height int) image.Image {
	dst := image.NewRGBA(image.Rect(0, 0, width, height))
	draw.NearestNeighbor.Scale(dst, dst.Bounds(), src, src.Bounds(), draw.Over, nil)
	return dst
}

// imageToGif converts an image to GIF format with optimized palette
func (d *Display) imageToGif(img image.Image) ([]byte, error) {
	var buf bytes.Buffer
	bounds := img.Bounds()

	// For RGBA images, directly access pixels
	rgbaImg, ok := img.(*image.RGBA)
	if !ok {
		// Convert to RGBA first
		rgbaImg = image.NewRGBA(bounds)
		for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
			for x := bounds.Min.X; x < bounds.Max.X; x++ {
				rgbaImg.Set(x, y, img.At(x, y))
			}
		}
	}

	// Build custom palette from unique colors (single pass)
	colorMap := make(map[color.RGBA]bool)
	pix := rgbaImg.Pix
	stride := rgbaImg.Stride
	rect := rgbaImg.Rect

	for y := rect.Min.Y; y < rect.Max.Y; y++ {
		for x := rect.Min.X; x < rect.Max.X; x++ {
			i := (y-rect.Min.Y)*stride + (x-rect.Min.X)*4
			col := color.RGBA{
				R: pix[i],
				G: pix[i+1],
				B: pix[i+2],
				A: pix[i+3],
			}
			colorMap[col] = true
		}
	}

	// Convert to palette
	palette := make(color.Palette, 0, len(colorMap))
	for col := range colorMap {
		palette = append(palette, col)
	}

	if len(palette) == 0 {
		palette = append(palette, color.RGBA{0, 0, 0, 255})
	}

	if d.debugBLE {
		log.Printf("[Display] GIF palette: %d unique colors", len(palette))
	}

	// Convert to paletted image
	paletted := image.NewPaletted(bounds, palette)
	for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
		for x := bounds.Min.X; x < bounds.Max.X; x++ {
			paletted.Set(x, y, rgbaImg.At(x, y))
		}
	}

	// Encode as simple GIF
	err := gif.Encode(&buf, paletted, nil)
	if err != nil {
		return nil, err
	}

	return buf.Bytes(), nil
}

// connectionMonitor monitors the connection and attempts reconnection if needed
func (d *Display) connectionMonitor() {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()

	for range ticker.C {
		// Skip if already reconnecting
		if d.reconnecting {
			continue
		}

		// Check if device is still connected
		if d.device != nil && !d.device.IsConnected() {
			log.Println("⚠ Connection lost! Attempting to reconnect...")
			go d.attemptReconnection()
		}
	}
}

// attemptReconnection tries to reconnect to the device
func (d *Display) attemptReconnection() {
	if d.reconnecting {
		return
	}
	d.reconnecting = true
	defer func() { d.reconnecting = false }()

	retryDelay := 5 * time.Second
	attempt := 0

	for {
		attempt++
		log.Printf("Reconnection attempt %d...", attempt)

		// Try to disconnect first (cleanup)
		if d.device != nil {
			d.device.Disconnect()
		}

		// Create new device
		device, err := xyaoled.NewDevice(d.debugBLE)
		if err != nil {
			log.Printf("Failed to create device: %v. Retrying in %v...", err, retryDelay)
			time.Sleep(retryDelay)
			continue
		}

		// Try to connect
		err = device.Connect(30 * time.Second)
		if err != nil {
			log.Printf("Failed to connect: %v. Retrying in %v...", err, retryDelay)
			time.Sleep(retryDelay)
			continue
		}

		// Success!
		d.device = device
		log.Println("✓ Successfully reconnected to XYAOLED LED panel!")

		// Set brightness
		if err := device.SetBrightness(80); err != nil {
			log.Printf("Warning: failed to set brightness: %v", err)
		}

		// Redisplay last content if available
		if d.lastGifData != nil {
			log.Println("Restoring last displayed content...")
			if d.lastGifIsAnim {
				// Replay the animation
				d.animationSeq++
				seq := d.animationSeq
				// Decode the GIF to get frames
				gifImg, err := gif.DecodeAll(bytes.NewReader(d.lastGifData))
				if err == nil {
					go d.playAnimation(gifImg, seq)
				}
			} else {
				// Redisplay static image with keep-alive
				d.animationSeq++
				seq := d.animationSeq
				go d.keepStaticImageAlive(d.lastGifData, seq)
			}
		} else {
			// Load default.gif if no last content
			if err := d.loadAndDisplayDefaultGif(); err != nil {
				log.Printf("Warning: failed to load default.gif: %v", err)
			}
		}

		return // Successfully reconnected
	}
}

// Close disconnects from the LED panel
func (d *Display) Close() error {
	if d.device != nil {
		return d.device.Disconnect()
	}
	return nil
}

func init() {
	// Register image formats
	image.RegisterFormat("png", "png", png.Decode, png.DecodeConfig)
	image.RegisterFormat("jpeg", "\xff\xd8", jpeg.Decode, jpeg.DecodeConfig)
	image.RegisterFormat("gif", "GIF8", gif.Decode, gif.DecodeConfig)
}
