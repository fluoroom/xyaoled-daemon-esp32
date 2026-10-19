package main

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/gif"
	_ "image/jpeg"
	_ "image/png"
	"log"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
	"xyaoled-daemon/xyaoled"

	"golang.org/x/image/draw"
)

const brightnessFile = ".brightness"

// Display handles the LED panel display logic
type Display struct {
	device        *xyaoled.Device
	width         int
	height        int
	debugBLE      bool
	config        *Config
	reconnecting  bool
	sendMutex     sync.Mutex // Prevent concurrent sends to device
	currentBright uint8      // Track current brightness at Display level (more reliable than device level)
	isManualOff   bool       // Track if user explicitly turned screen off via /off endpoint
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

	display := &Display{
		device:        device,
		width:         cfg.DisplayWidth,
		height:        cfg.DisplayHeight,
		debugBLE:      cfg.DebugBLE,
		config:        cfg,
		currentBright: 125,
	}

	brightness := uint8(125)
	if loaded := display.loadBrightness(); loaded > 0 {
		brightness = loaded
		log.Printf("Restored brightness from file: %d", brightness)
	}
	if err := device.SetBrightness(brightness); err != nil {
		log.Printf("Warning: failed to set initial brightness: %v", err)
	} else {
		display.currentBright = brightness
	}

	go display.connectionMonitor()

	return display, nil
}

// ShowImage displays a static image (PNG, JPEG or GIF bytes) with a full reinit.
func (d *Display) ShowImage(data []byte) error {
	img, format, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return fmt.Errorf("failed to decode image: %w", err)
	}
	if d.debugBLE {
		log.Printf("[Display] Decoded %s image: %dx%d", format, img.Bounds().Dx(), img.Bounds().Dy())
	}

	gifData, err := d.imageToGif(d.fitToPanel(img))
	if err != nil {
		return fmt.Errorf("failed to process image: %w", err)
	}

	opts := xyaoled.SendOptions{LogHeader: true}
	if err := d.send(gifData, opts); err != nil {
		return fmt.Errorf("failed to send static image: %w", err)
	}
	log.Printf("✓ Sent static image to screen (bytes=%d)", len(gifData))
	return nil
}

// ShowAnimatedGif displays an animated GIF with a full reinit.
func (d *Display) ShowAnimatedGif(data []byte) error {
	src, err := gif.DecodeAll(bytes.NewReader(data))
	if err != nil {
		return fmt.Errorf("failed to decode GIF: %w", err)
	}
	if d.debugBLE {
		log.Printf("[Display] Decoded animated GIF: %d frames", len(src.Image))
	}

	gifData, err := d.processAnimatedGif(src)
	if err != nil {
		return fmt.Errorf("failed to process animated GIF: %w", err)
	}

	if err := d.send(gifData, xyaoled.SendOptions{}); err != nil {
		return fmt.Errorf("failed to send animated GIF: %w", err)
	}
	log.Printf("✓ Sent animated GIF to screen (frames=%d, bytes=%d)", len(src.Image), len(gifData))
	return nil
}

// encode converts image or GIF bytes to the GIF the panel plays, reporting
// whether it is a static image.
func (d *Display) encode(data []byte) (gifData []byte, static bool, err error) {
	if src, err := gif.DecodeAll(bytes.NewReader(data)); err == nil && len(src.Image) > 1 {
		gifData, err = d.processAnimatedGif(src)
		return gifData, false, err
	}
	img, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, false, fmt.Errorf("failed to decode image: %w", err)
	}
	gifData, err = d.imageToGif(d.fitToPanel(img))
	return gifData, true, err
}

// ShowPlaylist replaces the on-device playlist with items (images or GIFs),
// played in order.
func (d *Display) ShowPlaylist(items [][]byte) error {
	encoded := make([][]byte, len(items))
	static := make([]bool, len(items))
	for i, item := range items {
		var err error
		if encoded[i], static[i], err = d.encode(item); err != nil {
			return fmt.Errorf("playlist item %d: %w", i+1, err)
		}
	}
	for i := range encoded {
		opts := xyaoled.SendOptions{LogHeader: static[i], Append: i > 0}
		if err := d.send(encoded[i], opts); err != nil {
			return fmt.Errorf("failed to send playlist item %d/%d: %w", i+1, len(items), err)
		}
	}
	log.Printf("✓ Sent playlist of %d item(s)", len(items))
	return nil
}

// ShowLive shows an image or GIF immediately without touching the playlist,
// as the vendor app's drawing screen does. The playlist comes back with
// ResumePlaylist.
func (d *Display) ShowLive(data []byte) error {
	gifData, static, err := d.encode(data)
	if err != nil {
		return err
	}
	program := uint8(xyaoled.ProgramLive)
	return d.send(gifData, xyaoled.SendOptions{LogHeader: static, Program: &program})
}

// send uploads gifData. Images are ignored while the screen is off
// (brightness <= 5).
func (d *Display) send(gifData []byte, opts xyaoled.SendOptions) error {
	if d.isScreenOff() {
		return fmt.Errorf("screen is off (brightness <=5); ignoring image send request")
	}

	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()

	err := d.device.SendGifWithOptions(gifData, opts)
	if err != nil && isConnectionError(err) && !d.reconnecting {
		log.Printf("Send failed with connection error, reconnecting: %v", err)
		go d.attemptReconnection()
	}
	return err
}

func isConnectionError(err error) bool {
	msg := err.Error()
	return strings.Contains(msg, "Not connected") || strings.Contains(msg, "device not connected") || strings.Contains(msg, "WriteValue")
}

func fillBlack(img *image.RGBA) {
	for i := 0; i < len(img.Pix); i += 4 {
		img.Pix[i], img.Pix[i+1], img.Pix[i+2], img.Pix[i+3] = 0, 0, 0, 255
	}
}

// fillBlackRect fills the region of img within r with opaque black.
func fillBlackRect(img *image.RGBA, r image.Rectangle) {
	r = r.Intersect(img.Bounds())
	for y := r.Min.Y; y < r.Max.Y; y++ {
		for x := r.Min.X; x < r.Max.X; x++ {
			img.SetRGBA(x, y, color.RGBA{0, 0, 0, 255})
		}
	}
}

// fitToPanel resizes img to the panel and rotates it when UPSIDE_DOWN is set.
func (d *Display) fitToPanel(img image.Image) image.Image {
	resized := d.resizeImage(img, d.width, d.height)
	if d.config.UpsideDown {
		return rotate180(resized)
	}
	return resized
}

// processAnimatedGif resizes all frames, applies optional rotation, and
// re-encodes with infinite looping, preserving per-frame delays.
//
// Source GIFs may be "keyframed": each decoded frame only covers the
// sub-rectangle that changed since the previous frame (per GIF disposal
// rules), rather than the full picture. We composite frames onto a
// full-canvas buffer, honoring each frame's disposal method, so every
// frame we send to the device is a complete, flattened image.
//
// All frames are then quantized against one shared palette (encoded as a
// single Global Color Table) rather than a palette per frame. Go's encoder
// only reuses one color table across frames when every frame's Palette is
// literally the same slice as GIF.Config.ColorModel; otherwise it emits a
// fresh Local Color Table for every single frame. The panel's GIF decoder
// doesn't handle a per-frame Local Color Table on every frame — it loses
// sync on the data partway through, which is why only the first few frames
// of a many-frame GIF would show and the rest played back garbled.
func (d *Display) processAnimatedGif(src *gif.GIF) ([]byte, error) {
	// Frames with zero delay get 50ms so the device never flashes its splash
	const fallbackDelay = 5

	canvasW, canvasH := src.Config.Width, src.Config.Height
	if canvasW == 0 || canvasH == 0 {
		b := src.Image[0].Bounds()
		canvasW, canvasH = b.Dx(), b.Dy()
	}
	canvas := image.NewRGBA(image.Rect(0, 0, canvasW, canvasH))
	fillBlack(canvas)

	frames := make([]*image.RGBA, 0, len(src.Image))
	delays := make([]int, 0, len(src.Image))
	var restoreTo *image.RGBA
	for i, frame := range src.Image {
		disposal := byte(gif.DisposalNone)
		if i < len(src.Disposal) {
			disposal = src.Disposal[i]
		}

		if disposal == gif.DisposalPrevious {
			restoreTo = image.NewRGBA(canvas.Bounds())
			copy(restoreTo.Pix, canvas.Pix)
		}

		draw.Draw(canvas, frame.Bounds(), frame, frame.Bounds().Min, draw.Over)

		snapshot := image.NewRGBA(canvas.Bounds())
		copy(snapshot.Pix, canvas.Pix)
		frames = append(frames, toRGBA(d.fitToPanel(snapshot)))

		delay := src.Delay[i]
		if delay == 0 {
			delay = fallbackDelay
		}
		delays = append(delays, delay)

		switch disposal {
		case gif.DisposalBackground:
			fillBlackRect(canvas, frame.Bounds())
		case gif.DisposalPrevious:
			if restoreTo != nil {
				canvas = restoreTo
			}
		}
	}

	palette := buildSharedPalette(frames)
	bounds := frames[0].Bounds()
	out := gif.GIF{
		LoopCount: 0,
		Config:    image.Config{ColorModel: palette, Width: bounds.Dx(), Height: bounds.Dy()},
	}
	for i, f := range frames {
		out.Image = append(out.Image, paletteFrame(f, palette))
		out.Disposal = append(out.Disposal, gif.DisposalBackground)
		out.Delay = append(out.Delay, delays[i])
	}

	var buf bytes.Buffer
	if err := gif.EncodeAll(&buf, &out); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// imageToGif encodes img as a single-frame GIF held for 10 minutes and
// looping forever, so the panel never falls back to its splash.
func (d *Display) imageToGif(img image.Image) ([]byte, error) {
	bounds := img.Bounds()
	opaque := image.NewRGBA(bounds)
	fillBlack(opaque)
	draw.Draw(opaque, bounds, img, bounds.Min, draw.Over)

	paletted := toPaletted(opaque)
	if d.debugBLE {
		log.Printf("[Display] GIF palette: %d unique colors", len(paletted.Palette))
	}

	var buf bytes.Buffer
	err := gif.EncodeAll(&buf, &gif.GIF{
		Image:     []*image.Paletted{paletted},
		Delay:     []int{60000},
		LoopCount: 0,
	})
	if err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// toRGBA converts img to *image.RGBA, copying only if necessary.
func toRGBA(img image.Image) *image.RGBA {
	if rgba, ok := img.(*image.RGBA); ok {
		return rgba
	}
	bounds := img.Bounds()
	rgba := image.NewRGBA(bounds)
	draw.Draw(rgba, bounds, img, bounds.Min, draw.Src)
	return rgba
}

// buildSharedPalette scans every frame and returns a palette of up to 256
// distinct colors shared across all of them, so an animation can be encoded
// with one Global Color Table instead of a Local Color Table per frame.
func buildSharedPalette(frames []*image.RGBA) color.Palette {
	seen := make(map[color.RGBA]bool)
	palette := make(color.Palette, 0, 256)
	for _, f := range frames {
		b := f.Bounds()
		for y := b.Min.Y; y < b.Max.Y && len(palette) < 256; y++ {
			for x := b.Min.X; x < b.Max.X && len(palette) < 256; x++ {
				c := f.RGBAAt(x, y)
				if !seen[c] {
					seen[c] = true
					palette = append(palette, c)
				}
			}
		}
		if len(palette) >= 256 {
			break
		}
	}
	if len(palette) == 0 {
		palette = append(palette, color.RGBA{0, 0, 0, 255})
	}
	return palette
}

// paletteFrame quantizes rgba against the given palette. The caller must
// pass the same palette slice for every frame of an animation so Go's GIF
// encoder recognizes it as one shared Global Color Table (it checks slice
// identity, not just equal values).
func paletteFrame(rgba *image.RGBA, palette color.Palette) *image.Paletted {
	bounds := rgba.Bounds()
	paletted := image.NewPaletted(bounds, palette)
	draw.FloydSteinberg.Draw(paletted, bounds, rgba, bounds.Min)
	return paletted
}

// toPaletted converts img using a palette of its first 256 distinct colors.
func toPaletted(img image.Image) *image.Paletted {
	rgba := toRGBA(img)
	return paletteFrame(rgba, buildSharedPalette([]*image.RGBA{rgba}))
}

// rotate180 rotates an image 180 degrees
func rotate180(src image.Image) image.Image {
	bounds := src.Bounds()
	width := bounds.Dx()
	height := bounds.Dy()

	dst := image.NewRGBA(image.Rect(0, 0, width, height))
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			dst.Set(width-1-x, height-1-y, src.At(bounds.Min.X+x, bounds.Min.Y+y))
		}
	}
	return dst
}

// resizeImage resizes an image to fit within the specified dimensions (contain mode)
// Maintains aspect ratio and centers the image on a black background
func (d *Display) resizeImage(src image.Image, width, height int) image.Image {
	srcBounds := src.Bounds()
	scale := min(float64(width)/float64(srcBounds.Dx()), float64(height)/float64(srcBounds.Dy()))
	newWidth := int(float64(srcBounds.Dx()) * scale)
	newHeight := int(float64(srcBounds.Dy()) * scale)

	dst := image.NewRGBA(image.Rect(0, 0, width, height))
	fillBlack(dst)

	offsetX := (width - newWidth) / 2
	offsetY := (height - newHeight) / 2
	scaledRect := image.Rect(offsetX, offsetY, offsetX+newWidth, offsetY+newHeight)
	draw.NearestNeighbor.Scale(dst, scaledRect, src, srcBounds, draw.Src, nil)
	return dst
}

// connectionMonitor monitors the connection and attempts reconnection if needed
func (d *Display) connectionMonitor() {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()

	for range ticker.C {
		if d.reconnecting || d.device == nil {
			continue
		}
		if !d.device.IsConnected() {
			log.Println("⚠ Connection lost! Attempting to reconnect...")
			go d.attemptReconnection()
			continue
		}
		if err := d.device.Ping(); err != nil {
			log.Printf("⚠ Connection ping failed: %v. Attempting to reconnect...", err)
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
	// Start with a modest retry delay and use exponential backoff (capped)
	retryDelay := 5 * time.Second
	maxDelay := 60 * time.Second

	for attempt := 1; ; attempt++ {
		log.Printf("Reconnection attempt %d...", attempt)

		// Disconnect first and give BlueZ a moment to drop stale object paths
		// after a device power-cycle.
		if d.device != nil {
			_ = d.device.Disconnect()
			time.Sleep(1 * time.Second)
		}

		device, err := xyaoled.NewDevice(d.debugBLE)
		if err != nil {
			log.Printf("Failed to create device: %v. Retrying in %v...", err, retryDelay)
			time.Sleep(retryDelay)
			retryDelay = min(maxDelay, retryDelay*2)
			continue
		}

		if err := device.Connect(30 * time.Second); err != nil {
			// DBus/WriteValue errors are often transient right after a power-cycle
			if strings.Contains(err.Error(), "WriteValue") {
				log.Printf("Connect/initialize failed with WriteValue error: %v. Waiting longer before retrying...", err)
				time.Sleep(5 * time.Second)
			} else {
				log.Printf("Failed to connect: %v. Retrying in %v...", err, retryDelay)
				time.Sleep(retryDelay)
			}
			retryDelay = min(maxDelay, retryDelay*2)
			continue
		}

		d.device = device
		log.Println("✓ Successfully reconnected to XYAOLED LED panel!")

		// The reconnect handshake wakes the screen, so restore the saved
		// brightness (including the <=5 "off" state).
		saved := d.loadBrightness()
		if saved == 0 {
			saved = 80
		}
		if err := device.SetBrightness(saved); err != nil {
			log.Printf("Warning: failed to restore brightness: %v", err)
		} else {
			d.currentBright = saved
			log.Printf("Restored brightness to %d after reconnect", saved)
		}
		return
	}
}

// isScreenOff reports whether brightness is at the "off" level (<= 5).
func (d *Display) isScreenOff() bool {
	return d.currentBright <= 5
}

// saveBrightness persists the current brightness to a file in the project folder
func (d *Display) saveBrightness(level uint8) error {
	if err := os.WriteFile(brightnessFile, []byte(strconv.Itoa(int(level))), 0644); err != nil {
		return fmt.Errorf("failed to save brightness: %w", err)
	}
	return nil
}

// loadBrightness reads the saved brightness from file, returns 0 if not found/error
func (d *Display) loadBrightness() uint8 {
	data, err := os.ReadFile(brightnessFile)
	if err != nil {
		return 0
	}
	val, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil || val < 0 || val > 255 {
		return 0
	}
	return uint8(val)
}

// SetBrightness sets brightness on the device and persists it to disk
func (d *Display) SetBrightness(level uint8) error {
	if d.device == nil {
		return fmt.Errorf("device not ready")
	}
	if err := d.device.SetBrightness(level); err != nil {
		return err
	}
	d.currentBright = level
	if err := d.saveBrightness(level); err != nil {
		log.Printf("Warning: failed to persist brightness: %v", err)
	}
	return nil
}

// SetBrightnessIfNotManualOff sets brightness only if screen is not manually off.
func (d *Display) SetBrightnessIfNotManualOff(level uint8) error {
	if d.isManualOff {
		return fmt.Errorf("screen is manually off; ignoring brightness change (use /on to enable)")
	}
	return d.SetBrightness(level)
}

// TurnOff sets brightness to 5 and marks screen as manually off
func (d *Display) TurnOff() error {
	if err := d.SetBrightness(5); err != nil {
		return err
	}
	d.isManualOff = true
	return nil
}

// TurnOn restores brightness and clears manual off flag
func (d *Display) TurnOn(level uint8) error {
	d.isManualOff = false
	return d.SetBrightness(level)
}

// PowerOn sends the panel's hardware power-on command (distinct from /on,
// which only restores brightness).
func (d *Display) PowerOn() error {
	if d.device == nil {
		return fmt.Errorf("device not ready")
	}
	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()
	return d.device.Power(true)
}

// PowerOff sends the panel's hardware power-off command (distinct from /off,
// which only dims brightness to 5).
func (d *Display) PowerOff() error {
	if d.device == nil {
		return fmt.Errorf("device not ready")
	}
	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()
	return d.device.Power(false)
}

// ClearPlaylist clears the screen and wipes the on-device playlist.
func (d *Display) ClearPlaylist() error {
	if d.device == nil {
		return fmt.Errorf("device not ready")
	}
	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()
	return d.device.ClearPlaylist()
}

// ClearScreen blanks the screen with a black live frame; the playlist is
// kept and comes back with ResumePlaylist.
func (d *Display) ClearScreen() error {
	img := image.NewRGBA(image.Rect(0, 0, d.width, d.height))
	fillBlack(img)
	gifData, err := d.imageToGif(img)
	if err != nil {
		return err
	}
	program := uint8(xyaoled.ProgramLive)
	return d.send(gifData, xyaoled.SendOptions{LogHeader: true, Program: &program})
}

// ResumePlaylist leaves the live view (ShowLive, ClearScreen) and plays the
// playlist again.
func (d *Display) ResumePlaylist() error {
	if d.device == nil {
		return fmt.Errorf("device not ready")
	}
	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()
	return d.device.ResumePlaylist()
}

// SendCommand writes a raw command body (for protocol experiments).
func (d *Display) SendCommand(body []byte) error {
	if d.device == nil {
		return fmt.Errorf("device not ready")
	}
	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()
	return d.device.SendCommand(body)
}

// Connect establishes a connection if currently disconnected.
func (d *Display) Connect() error {
	if d.device != nil && d.device.IsConnected() {
		return nil
	}

	device, err := xyaoled.NewDevice(d.debugBLE)
	if err != nil {
		return err
	}
	if err := device.Connect(30 * time.Second); err != nil {
		return err
	}
	d.device = device
	_ = d.device.SetBrightness(80)
	d.currentBright = 80
	return nil
}

// Close disconnects from the LED panel
func (d *Display) Close() error {
	if d.device != nil {
		return d.device.Disconnect()
	}
	return nil
}
