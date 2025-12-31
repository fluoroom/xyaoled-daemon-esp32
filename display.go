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
	"sync"
	"time"
	"xyaoled-daemon/xyaoled"

	"golang.org/x/image/draw"
)

// Display handles the LED panel display logic
type Display struct {
	device               *xyaoled.Device
	width                int
	height               int
	frameIntervalSeconds int
	frameDelay           time.Duration
	lastUpdate           time.Time
	debugBLE             bool
	animationSeq         int
	config               *Config
	reconnecting         bool
	lastGifData          []byte     // Store last displayed content for redisplay after reconnect
	lastGifIsAnim        bool       // Track if last content was animated
	sendMutex            sync.Mutex // Prevent concurrent sends to device
	firstSendDone        bool       // Track first send after connection
	prevBrightness       uint8      // Remember last non-dim brightness for screen on/off experiments
}

func (d *Display) sendOpts(forceReinit bool, staticHeader bool, skipClear bool) xyaoled.SendOptions {
	return xyaoled.SendOptions{
		ForceReinit:     forceReinit,
		StaticHeader:    staticHeader,
		SkipClear:       skipClear,
		BestEffortClear: d.config.BestEffortClear,
	}
}

// sendBlackFrame clears the panel with a black full-screen frame
func (d *Display) sendBlackFrame() error {
	img := image.NewRGBA(image.Rect(0, 0, d.width, d.height))
	// fill opaque black
	for i := 0; i < len(img.Pix); i += 4 {
		img.Pix[i] = 0
		img.Pix[i+1] = 0
		img.Pix[i+2] = 0
		img.Pix[i+3] = 255
	}
	gifData, err := d.imageToGif(img)
	if err != nil {
		return err
	}
	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()
	return d.device.SendGif(gifData)
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

	frameDelay := time.Duration(cfg.FrameIntervalSeconds) * time.Second

	display := &Display{
		device:               device,
		width:                cfg.DisplayWidth,
		height:               cfg.DisplayHeight,
		frameIntervalSeconds: cfg.FrameIntervalSeconds,
		frameDelay:           frameDelay,
		lastUpdate:           time.Now(),
		debugBLE:             cfg.DebugBLE,
		config:               cfg,
		reconnecting:         false,
		firstSendDone:        false,
		prevBrightness:       100,
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

	// Send to device once (no animation) with static header; skip clear to match vendor behavior
	opts := d.sendOpts(false, true, d.config.SkipClear)
	// Default splash display previously used SkipClear; honor config flag now.
	log.Printf("[Display] Sending static image (forceReinit=%v, skipClear=%v, staticHeader=%v)", opts.ForceReinit, opts.SkipClear, opts.StaticHeader)
	d.sendMutex.Lock()
	sendErr := d.device.SendGifWithOptions(gifData, opts)
	d.sendMutex.Unlock()
	if sendErr != nil {
		return fmt.Errorf("failed to send default.gif: %w", sendErr)
	}
	// Treat default as first successful send after connection
	d.firstSendDone = true

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

	isFirstSend := !d.firstSendDone
	// Clear previous content unless globally skipping, but allow one-time clear on first send when skipping.
	if !d.config.SkipClear || isFirstSend {
		if err := d.sendBlackFrame(); err != nil {
			log.Printf("Warning: failed to clear panel before static image: %v", err)
		}
	}

	// Resize and convert to single-frame animated GIF to reuse animated send path
	gifData, err := d.processStaticAsAnimated(img)
	if err != nil {
		return fmt.Errorf("failed to process image: %w", err)
	}

	// Store last content for reconnection
	d.lastGifData = gifData
	d.lastGifIsAnim = false

	// Stop any running animation/refresh loops
	d.animationSeq++

	// Send via animated path (even single-frame). Keep force reinit to reset playlists/state.
	forceReinit := true
	skipClear := d.config.SkipClear && !isFirstSend
	opts := d.sendOpts(forceReinit, false, skipClear)
	// Match captured log header and slot (0x05) for static sends.
	logSlot := uint8(0x05)
	opts.LogHeader = true
	opts.SlotOverride = &logSlot
	d.sendMutex.Lock()
	sendErr := d.device.SendGifWithOptions(gifData, opts)
	d.sendMutex.Unlock()
	if sendErr != nil {
		return fmt.Errorf("failed to send static image: %w", sendErr)
	}

	// Mark first successful send after connection
	d.firstSendDone = true

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

	isFirstSend := !d.firstSendDone
	// Clear previous content unless globally skipping, but allow one-time clear on first send when skipping.
	if !d.config.SkipClear || isFirstSend {
		if err := d.sendBlackFrame(); err != nil {
			log.Printf("Warning: failed to clear panel before animation: %v", err)
		}
	}

	// Process and re-encode with sizing/rotation while preserving delays/looping
	processedGif, err := d.processAnimatedGifWithLoop(gifImg, true)
	if err != nil {
		return fmt.Errorf("failed to process animated GIF: %w", err)
	}

	// Stop any running animation/refresh loops
	d.animationSeq++

	// Store last content for reconnection (processed form)
	d.lastGifData = processedGif
	d.lastGifIsAnim = true

	// Send once with optional init/clear rules; LoopCount was forced to 0 in processing
	forceReinit := isFirstSend
	skipClear := d.config.SkipClear && !isFirstSend
	opts := d.sendOpts(forceReinit, false, skipClear)
	log.Printf("[Display] Sending animated GIF (forceReinit=%v, skipClear=%v, staticHeader=%v)", opts.ForceReinit, opts.SkipClear, opts.StaticHeader)
	d.sendMutex.Lock()
	sendErr := d.device.SendGifWithOptions(processedGif, opts)
	d.sendMutex.Unlock()
	if sendErr != nil {
		return fmt.Errorf("failed to send animated GIF: %w", sendErr)
	}

	// Mark first successful send after connection
	d.firstSendDone = true

	return nil
}

// processImage resizes image to display dimensions and converts to optimized GIF
func (d *Display) processImage(img image.Image) ([]byte, error) {
	// Resize to display dimensions first
	resized := d.resizeImage(img, d.width, d.height)

	// Rotate 180 degrees if upside down (after resizing so letterboxing is correct)
	if d.config.UpsideDown {
		if d.debugBLE {
			log.Println("[Display] Rotating image 180 degrees (UPSIDE_DOWN=true)")
		}
		resized = rotate180(resized)
	}

	// Convert to GIF with custom palette
	return d.imageToGif(resized)
}

// processStaticAsAnimated encodes a single-frame image as an animated-style GIF (looping) to reuse the animated send path.
func (d *Display) processStaticAsAnimated(img image.Image) ([]byte, error) {
	// Resize and rotate like animated path
	resized := d.resizeImage(img, d.width, d.height)
	if d.config.UpsideDown {
		resized = rotate180(resized)
	}

	// Ensure RGBA and flatten onto opaque black
	rgba := image.NewRGBA(resized.Bounds())
	for i := 0; i < len(rgba.Pix); i += 4 {
		rgba.Pix[i] = 0
		rgba.Pix[i+1] = 0
		rgba.Pix[i+2] = 0
		rgba.Pix[i+3] = 255
	}
	draw.Draw(rgba, resized.Bounds(), resized, resized.Bounds().Min, draw.Over)

	// Build palette
	colorMap := make(map[color.RGBA]bool)
	pix := rgba.Pix
	stride := rgba.Stride
	rect := rgba.Rect

	for y := rect.Min.Y; y < rect.Max.Y; y++ {
		for x := rect.Min.X; x < rect.Max.X; x++ {
			i := (y-rect.Min.Y)*stride + (x-rect.Min.X)*4
			col := color.RGBA{R: pix[i], G: pix[i+1], B: pix[i+2], A: pix[i+3]}
			colorMap[col] = true
			if len(colorMap) >= 256 {
				break
			}
		}
		if len(colorMap) >= 256 {
			break
		}
	}

	palette := make(color.Palette, 0, len(colorMap))
	for col := range colorMap {
		palette = append(palette, col)
		if len(palette) >= 256 {
			break
		}
	}
	if len(palette) == 0 {
		palette = append(palette, color.RGBA{0, 0, 0, 255})
	}

	paletted := image.NewPaletted(rgba.Bounds(), palette)
	draw.FloydSteinberg.Draw(paletted, rgba.Bounds(), rgba, image.Point{})

	out := &gif.GIF{
		Image:     []*image.Paletted{paletted},
		Delay:     []int{60000}, // long hold to avoid splash
		LoopCount: 0,            // infinite
	}

	var buf bytes.Buffer
	if err := gif.EncodeAll(&buf, out); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// processAnimatedGif resizes all frames, applies optional rotation, and re-encodes preserving delays/looping (infinite by default)
func (d *Display) processAnimatedGif(src *gif.GIF) ([]byte, error) {
	return d.processAnimatedGifWithLoop(src, true)
}

// processAnimatedGifWithLoop allows choosing infinite (loop count 0) or single-loop (1) behavior.
func (d *Display) processAnimatedGifWithLoop(src *gif.GIF, loopInfinite bool) ([]byte, error) {
	var out gif.GIF
	if loopInfinite {
		out.LoopCount = 0 // infinite
	} else {
		out.LoopCount = 1
	}

	// Fallback delay derived from configured frameDelay (centiseconds)
	fallbackDelay := 5 // Fallback delay (centiseconds) for frames with zero delay; use a small sane default (50ms)

	for i, frame := range src.Image {
		// Convert paletted frame to RGBA and flatten over opaque black to avoid transparency bleed
		bounds := frame.Bounds()
		rgba := image.NewRGBA(bounds)
		for i := 0; i < len(rgba.Pix); i += 4 {
			rgba.Pix[i] = 0
			rgba.Pix[i+1] = 0
			rgba.Pix[i+2] = 0
			rgba.Pix[i+3] = 255
		}
		draw.Draw(rgba, bounds, frame, bounds.Min, draw.Over)

		processed := d.resizeImage(rgba, d.width, d.height)
		if d.config.UpsideDown {
			processed = rotate180(processed)
		}

		// Build palette up to 256 colors
		colorMap := make(map[color.RGBA]bool)
		prgba, ok := processed.(*image.RGBA)
		if !ok {
			// ensure RGBA
			tmp := image.NewRGBA(processed.Bounds())
			draw.Draw(tmp, processed.Bounds(), processed, processed.Bounds().Min, draw.Src)
			prgba = tmp
		}

		pix := prgba.Pix
		stride := prgba.Stride
		rect := prgba.Rect
		for y := rect.Min.Y; y < rect.Max.Y && len(colorMap) < 256; y++ {
			for x := rect.Min.X; x < rect.Max.X && len(colorMap) < 256; x++ {
				i := (y-rect.Min.Y)*stride + (x-rect.Min.X)*4
				col := color.RGBA{R: pix[i], G: pix[i+1], B: pix[i+2], A: pix[i+3]}
				colorMap[col] = true
			}
		}

		palette := make(color.Palette, 0, len(colorMap))
		for col := range colorMap {
			palette = append(palette, col)
			if len(palette) >= 256 {
				break
			}
		}
		if len(palette) == 0 {
			palette = append(palette, color.RGBA{0, 0, 0, 255})
		}

		paletted := image.NewPaletted(processed.Bounds(), palette)
		draw.FloydSteinberg.Draw(paletted, processed.Bounds(), processed, image.Point{})

		out.Image = append(out.Image, paletted)
		out.Disposal = append(out.Disposal, gif.DisposalBackground)
		delay := src.Delay[i]
		if delay == 0 {
			delay = fallbackDelay
		}
		out.Delay = append(out.Delay, delay)
	}

	var buf bytes.Buffer
	if err := gif.EncodeAll(&buf, &out); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// rotate180 rotates an image 180 degrees
func rotate180(src image.Image) image.Image {
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

// resizeImage resizes an image to fit within the specified dimensions (contain mode)
// Maintains aspect ratio and centers the image on a black background
func (d *Display) resizeImage(src image.Image, width, height int) image.Image {
	srcBounds := src.Bounds()
	srcWidth := srcBounds.Dx()
	srcHeight := srcBounds.Dy()

	// Calculate scaling factor to fit within bounds (contain)
	scaleX := float64(width) / float64(srcWidth)
	scaleY := float64(height) / float64(srcHeight)
	scale := scaleX
	if scaleY < scaleX {
		scale = scaleY
	}

	// Calculate new dimensions maintaining aspect ratio
	newWidth := int(float64(srcWidth) * scale)
	newHeight := int(float64(srcHeight) * scale)

	// Create black background at target size (opaque)
	dst := image.NewRGBA(image.Rect(0, 0, width, height))
	for i := 0; i < len(dst.Pix); i += 4 {
		dst.Pix[i] = 0
		dst.Pix[i+1] = 0
		dst.Pix[i+2] = 0
		dst.Pix[i+3] = 255
	}

	// Calculate offset to center the scaled image
	offsetX := (width - newWidth) / 2
	offsetY := (height - newHeight) / 2

	// Scale and draw the image centered
	scaledRect := image.Rect(offsetX, offsetY, offsetX+newWidth, offsetY+newHeight)
	draw.NearestNeighbor.Scale(dst, scaledRect, src, srcBounds, draw.Src, nil)

	return dst
}

// imageToGif converts an image to GIF format with optimized palette
func (d *Display) imageToGif(img image.Image) ([]byte, error) {
	var buf bytes.Buffer
	bounds := img.Bounds()

	// For RGBA images, directly access pixels; otherwise convert.
	rgbaImg, ok := img.(*image.RGBA)
	if !ok {
		rgbaImg = image.NewRGBA(bounds)
		draw.Draw(rgbaImg, bounds, img, bounds.Min, draw.Src)
	}

	// Flatten onto opaque black to avoid transparency leaks on device background.
	opaque := image.NewRGBA(bounds)
	for i := 0; i < len(opaque.Pix); i += 4 {
		opaque.Pix[i] = 0
		opaque.Pix[i+1] = 0
		opaque.Pix[i+2] = 0
		opaque.Pix[i+3] = 255
	}
	draw.Draw(opaque, bounds, rgbaImg, bounds.Min, draw.Over)
	rgbaImg = opaque

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
			// Stop if we hit the GIF limit
			if len(colorMap) >= 256 {
				break
			}
		}
		if len(colorMap) >= 256 {
			break
		}
	}

	// Convert to palette (limit to 256 colors max for GIF)
	palette := make(color.Palette, 0, len(colorMap))
	for col := range colorMap {
		palette = append(palette, col)
		if len(palette) >= 256 {
			break
		}
	}

	if len(palette) == 0 {
		palette = append(palette, color.RGBA{0, 0, 0, 255})
	}

	if d.debugBLE {
		log.Printf("[Display] GIF palette: %d unique colors", len(palette))
	}

	// Convert to paletted image (will quantize to nearest color if needed)
	paletted := image.NewPaletted(bounds, palette)
	draw.FloydSteinberg.Draw(paletted, bounds, rgbaImg, image.Point{})

	// Encode as GIF
	// Use a short delay similar to vendor static send (e.g., ~90ms) and rely on keepalive for persistence
	gifImage := &gif.GIF{
		Image:     []*image.Paletted{paletted},
		Delay:     []int{60000}, // centiseconds (10 minutes) to avoid visible blinking
		LoopCount: 0,            // loop forever (but effectively static due to long delay)
	}

	err := gif.EncodeAll(&buf, gifImage)
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
		d.firstSendDone = false
		log.Println("✓ Successfully reconnected to XYAOLED LED panel!")

		// Set brightness
		if err := device.SetBrightness(80); err != nil {
			log.Printf("Warning: failed to set brightness: %v", err)
		}

		// Redisplay last content if available
		if d.lastGifData != nil {
			log.Println("Restoring last displayed content...")
			// Replay once (animated or static) using same send options as primary paths
			d.animationSeq++
			forceReinit := !d.firstSendDone
			skipClear := d.config.SkipClear && d.firstSendDone
			opts := d.sendOpts(forceReinit, false, skipClear)
			go func(data []byte, isAnim bool, opts xyaoled.SendOptions) {
				d.sendMutex.Lock()
				defer d.sendMutex.Unlock()
				log.Printf("[Display] Restoring last content (anim=%v, forceReinit=%v, skipClear=%v, staticHeader=%v)", isAnim, opts.ForceReinit, opts.SkipClear, opts.StaticHeader)
				if err := d.device.SendGifWithOptions(data, opts); err != nil {
					if isAnim {
						log.Printf("Error restoring animated content: %v", err)
					} else {
						log.Printf("Error restoring static content: %v", err)
					}
				} else {
					d.firstSendDone = true
				}
			}(d.lastGifData, d.lastGifIsAnim, opts)
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
