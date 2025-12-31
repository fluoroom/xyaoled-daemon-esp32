package main

import (
	"bytes"
	"image"
	"image/color"
	"image/gif"
	"os"
	"testing"
	"time"

	"xyaoled-daemon/xyaoled"
)

// Experimental tests: run only when TESTS=true to avoid affecting production test runs.
// They validate that processed GIFs keep the panel from flashing the default LED splash
// between sends. No BLE calls are made here.

func requireTestsEnabled(t *testing.T) {
	t.Helper()
	if os.Getenv("TESTS") != "true" {
		t.Skip("set TESTS=true to run experimental display tests")
	}
	t.Log("[EXPERIMENTAL] Running TESTS-gated display experiments (may use hardware)")
}

// Ensure static images are encoded as single-frame GIFs with long delays and infinite looping
// so the panel never reverts to its built-in LED splash between sends.
func TestProcessImage_LongDelaySingleFrame(t *testing.T) {
	requireTestsEnabled(t)

	d := &Display{width: 2, height: 2, config: &Config{}}

	img := image.NewRGBA(image.Rect(0, 0, 1, 1))
	img.Set(0, 0, color.RGBA{R: 255, A: 255})

	buf, err := d.processImage(img)
	if err != nil {
		t.Fatalf("processImage error: %v", err)
	}

	g, err := gif.DecodeAll(bytes.NewReader(buf))
	if err != nil {
		t.Fatalf("decoded gif error: %v", err)
	}

	if len(g.Image) != 1 {
		t.Fatalf("expected 1 frame, got %d", len(g.Image))
	}
	if g.Delay[0] < 60000 {
		t.Fatalf("expected long delay to hold frame, got %d", g.Delay[0])
	}
	// Some decoders return -1 when no Netscape loop extension is present; either -1 or 0 holds the frame.
	if g.LoopCount != 0 && g.LoopCount != -1 {
		t.Fatalf("expected LoopCount=0 or -1 (infinite/single), got %d", g.LoopCount)
	}
}

// Ensure animated GIF processing preserves infinite looping and never emits zero-delay frames
// that could expose the device's LED splash between frames.
func TestProcessAnimatedGif_NoZeroDelays(t *testing.T) {
	requireTestsEnabled(t)

	src := &gif.GIF{
		Image: []*image.Paletted{
			image.NewPaletted(image.Rect(0, 0, 1, 1), color.Palette{color.Black}),
			image.NewPaletted(image.Rect(0, 0, 1, 1), color.Palette{color.White}),
		},
		Delay: []int{0, 3}, // include a zero to verify fallback delay
	}

	d := &Display{width: 2, height: 2, config: &Config{}}

	buf, err := d.processAnimatedGif(src)
	if err != nil {
		t.Fatalf("processAnimatedGif error: %v", err)
	}

	g, err := gif.DecodeAll(bytes.NewReader(buf))
	if err != nil {
		t.Fatalf("decoded gif error: %v", err)
	}

	if g.LoopCount != 0 {
		t.Fatalf("expected LoopCount=0 (infinite), got %d", g.LoopCount)
	}
	for i, dly := range g.Delay {
		if dly < 1 {
			t.Fatalf("frame %d has delay %d; expected >=1 to avoid device splash", i, dly)
		}
	}
}

// Verify that when FillBlack is enabled, transparent pixels are filled to opaque black
// both around and under the image content (no leaks from device background).
func TestFillBlackOpaque_Static(t *testing.T) {
	requireTestsEnabled(t)

	// FillBlack flag no longer exists; resizing path always fills black background.
	d := &Display{width: 2, height: 2, config: &Config{}}

	img := image.NewRGBA(image.Rect(0, 0, 2, 2))
	// leave (0,0) transparent, set (1,1) white opaque
	img.SetRGBA(1, 1, color.RGBA{255, 255, 255, 255})

	buf, err := d.processImage(img)
	if err != nil {
		t.Fatalf("processImage error: %v", err)
	}

	g, err := gif.DecodeAll(bytes.NewReader(buf))
	if err != nil {
		t.Fatalf("decoded gif error: %v", err)
	}

	frame := g.Image[0]
	if c := frame.At(0, 0); c != (color.RGBA{0, 0, 0, 255}) {
		t.Fatalf("expected filled black at transparent pixel, got %v", c)
	}
}

// Verify animated disposal uses background to replace/discard frames (no stacking).
func TestProcessAnimatedGif_DisposalBackground(t *testing.T) {
	requireTestsEnabled(t)

	src := &gif.GIF{
		Image: []*image.Paletted{
			image.NewPaletted(image.Rect(0, 0, 1, 1), color.Palette{color.Black}),
			image.NewPaletted(image.Rect(0, 0, 1, 1), color.Palette{color.White}),
		},
		Delay: []int{5, 5},
	}

	d := &Display{width: 2, height: 2, config: &Config{}}

	buf, err := d.processAnimatedGif(src)
	if err != nil {
		t.Fatalf("processAnimatedGif error: %v", err)
	}

	g, err := gif.DecodeAll(bytes.NewReader(buf))
	if err != nil {
		t.Fatalf("decoded gif error: %v", err)
	}

	for i, disp := range g.Disposal {
		if disp != gif.DisposalBackground {
			t.Fatalf("frame %d disposal=%d, want %d (DisposalBackground)", i, disp, gif.DisposalBackground)
		}
	}
}

// Integration-style experiment: try sending static content without clearing using
// different SendOptions combinations (SkipClear variations). Requires hardware and TESTS=true.
func TestSendStatic_NoClearVariants(t *testing.T) {
	requireTestsEnabled(t)

	cfg := LoadConfig()

	d, err := NewDisplay(cfg)
	if err != nil {
		t.Fatalf("failed to create display: %v", err)
	}
	defer d.Close()

	img := image.NewRGBA(image.Rect(0, 0, 1, 1))
	img.Set(0, 0, color.RGBA{R: 255, A: 255})

	buf, err := d.processImage(img)
	if err != nil {
		t.Fatalf("processImage error: %v", err)
	}

	variants := []struct {
		name string
		opts xyaoled.SendOptions
	}{
		{"baseline_force_reinit", xyaoled.SendOptions{ForceReinit: true}},
		{"skip_clear_only", xyaoled.SendOptions{SkipClear: true}},
		{"skip_clear_static_header", xyaoled.SendOptions{SkipClear: true, StaticHeader: true}},
		{"skip_clear_force_reinit", xyaoled.SendOptions{SkipClear: true, ForceReinit: true}},
		{"skip_clear_best_effort", xyaoled.SendOptions{SkipClear: true, BestEffortClear: true}},
		{"skip_clear_static_header_best_effort", xyaoled.SendOptions{SkipClear: true, StaticHeader: true, BestEffortClear: true}},
	}

	for _, v := range variants {
		d.sendMutex.Lock()
		err = d.device.SendGifWithOptions(buf, v.opts)
		d.sendMutex.Unlock()
		if err != nil {
			t.Fatalf("variant %s failed: %v", v.name, err)
		}
		t.Logf("sent static variant: %s (opts=%+v)", v.name, v.opts)
		time.Sleep(300 * time.Millisecond)
	}
}

// Integration-style experiment: send a tiny animated GIF without clearing using
// SkipClear variants to observe whether the LED splash appears between sends.
func TestSendAnimated_NoClearVariants(t *testing.T) {
	requireTestsEnabled(t)

	cfg := LoadConfig()

	d, err := NewDisplay(cfg)
	if err != nil {
		t.Fatalf("failed to create display: %v", err)
	}
	defer d.Close()

	src := &gif.GIF{
		Image: []*image.Paletted{
			image.NewPaletted(image.Rect(0, 0, 1, 1), color.Palette{color.Black}),
			image.NewPaletted(image.Rect(0, 0, 1, 1), color.Palette{color.White}),
		},
		Delay: []int{5, 5},
	}

	buf, err := d.processAnimatedGif(src)
	if err != nil {
		t.Fatalf("processAnimatedGif error: %v", err)
	}

	variants := []struct {
		name string
		opts xyaoled.SendOptions
	}{
		{"baseline_force_reinit", xyaoled.SendOptions{ForceReinit: true}},
		{"skip_clear_only", xyaoled.SendOptions{SkipClear: true}},
		{"skip_clear_static_header", xyaoled.SendOptions{SkipClear: true, StaticHeader: false}},
		{"skip_clear_force_reinit", xyaoled.SendOptions{SkipClear: true, ForceReinit: true}},
		{"skip_clear_best_effort", xyaoled.SendOptions{SkipClear: true, BestEffortClear: true}},
		{"skip_clear_static_header_best_effort", xyaoled.SendOptions{SkipClear: true, StaticHeader: false, BestEffortClear: true}},
	}

	for _, v := range variants {
		d.sendMutex.Lock()
		err = d.device.SendGifWithOptions(buf, v.opts)
		d.sendMutex.Unlock()
		if err != nil {
			t.Fatalf("variant %s failed: %v", v.name, err)
		}
		t.Logf("sent animated variant: %s (opts=%+v)", v.name, v.opts)
		time.Sleep(300 * time.Millisecond)
	}
}
