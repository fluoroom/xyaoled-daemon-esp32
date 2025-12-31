package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"errors"
	"image"
	"image/gif"
	"log"
	"math/rand"
	"strings"
	"sync"
	"time"

	"xyaoled-daemon/xyaoled"
)

var (
	experimentalMu     sync.Mutex
	experimentalCancel context.CancelFunc
)

// runExperimentalNoClearVariants uses the uploaded payload (static or animated) and
// sends it with multiple SendOptions combos, spacing attempts to observe on-screen effects.
// It should be called after a normal display/upload when TESTS is true. Runs async.
// When invoked again, it cancels the previous loop so only the latest upload is exercised.
func runExperimentalNoClearVariants(d *Display, base64Data string, isAnimated bool) {
	log.Println("[EXPERIMENTAL] TESTS=true: running no-clear send variants with uploaded payload")

	experimentalMu.Lock()
	if experimentalCancel != nil {
		experimentalCancel()
	}
	ctx, cancel := context.WithCancel(context.Background())
	experimentalCancel = cancel
	experimentalMu.Unlock()

	go func(ctx context.Context) {
		imgData, err := base64.StdEncoding.DecodeString(base64Data)
		if err != nil {
			log.Printf("[EXPERIMENTAL] base64 decode failed: %v", err)
			return
		}

		var payload []byte

		if isAnimated {
			gifImg, err := gif.DecodeAll(bytes.NewReader(imgData))
			if err != nil {
				log.Printf("[EXPERIMENTAL] gif decode failed: %v", err)
				return
			}
			payload, err = d.processAnimatedGif(gifImg)
			if err != nil {
				log.Printf("[EXPERIMENTAL] animated processing failed: %v", err)
				return
			}
		} else {
			img, _, err := image.Decode(bytes.NewReader(imgData))
			if err != nil {
				log.Printf("[EXPERIMENTAL] image decode failed: %v", err)
				return
			}
			payload, err = d.processImage(img)
			if err != nil {
				log.Printf("[EXPERIMENTAL] static processing failed: %v", err)
				return
			}
		}

		r := rand.New(rand.NewSource(time.Now().UnixNano()))
		variants := []struct {
			name string
			opts xyaoled.SendOptions
		}{
			{"baseline_force_reinit", xyaoled.SendOptions{ForceReinit: true}},
			{"skip_clear_only", xyaoled.SendOptions{SkipClear: true}},
			{"skip_clear_static_header", xyaoled.SendOptions{SkipClear: true, StaticHeader: !isAnimated}},
			{"skip_clear_force_reinit", xyaoled.SendOptions{SkipClear: true, ForceReinit: true}},
			{"skip_clear_best_effort", xyaoled.SendOptions{SkipClear: true, BestEffortClear: true}},
			{"skip_clear_static_header_best_effort", xyaoled.SendOptions{SkipClear: true, StaticHeader: !isAnimated, BestEffortClear: true}},
		}

		shouldReconnect := func(err error) bool {
			if err == nil {
				return false
			}
			msg := err.Error()
			return strings.Contains(msg, "Not connected") ||
				strings.Contains(msg, "device not connected") ||
				strings.Contains(msg, "WriteValue")
		}

		for {
			select {
			case <-ctx.Done():
				log.Println("[EXPERIMENTAL] canceled previous test loop")
				return
			default:
			}

			v := variants[r.Intn(len(variants))]
			d.sendMutex.Lock()
			err := d.device.SendGifWithOptions(payload, v.opts)
			d.sendMutex.Unlock()
			if err != nil {
				log.Printf("[EXPERIMENTAL] variant %s failed: %v", v.name, err)
				if shouldReconnect(err) && !d.reconnecting {
					log.Printf("[EXPERIMENTAL] triggering reconnect after failure: %v", err)
					go d.attemptReconnection()
				}
			} else {
				log.Printf("[EXPERIMENTAL] sent variant %s (opts=%+v)", v.name, v.opts)
			}

			select {
			case <-ctx.Done():
				log.Println("[EXPERIMENTAL] canceled previous test loop")
				return
			case <-time.After(8 * time.Second):
			}
		}
	}(ctx)
}

// experimentalConnect establishes a connection if currently disconnected.
func (d *Display) experimentalConnect() error {
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
	// Replace device and reset state similar to NewDisplay
	d.device = device
	d.firstSendDone = false
	d.lastGifData = nil
	d.lastGifIsAnim = false
	// Set a reasonable brightness baseline
	_ = d.device.SetBrightness(80)
	return nil
}

// experimentalDisconnect cleanly disconnects without tearing down the Display wrapper.
func (d *Display) experimentalDisconnect() error {
	if d.device == nil {
		return nil
	}
	return d.device.Disconnect()
}

// experimentalSendStatic sends a static image buffer with explicit options.
func (d *Display) experimentalSendStatic(buf []byte, opts xyaoled.SendOptions) error {
	if err := d.experimentalConnect(); err != nil {
		return err
	}
	img, _, err := image.Decode(bytes.NewReader(buf))
	if err != nil {
		return err
	}
	payload, err := d.processStaticAsAnimated(img)
	if err != nil {
		return err
	}
	if !opts.BestEffortClear {
		opts.BestEffortClear = true
	}
	// Force animated header path for static content
	opts.StaticHeader = false
	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()
	return d.device.SendGifWithOptions(payload, opts)
}

// experimentalSendAnimated sends an animated GIF buffer with explicit options.
func (d *Display) experimentalSendAnimated(buf []byte, opts xyaoled.SendOptions) error {
	if err := d.experimentalConnect(); err != nil {
		return err
	}
	gifImg, err := gif.DecodeAll(bytes.NewReader(buf))
	if err != nil {
		return err
	}
	payload, err := d.processAnimatedGifWithLoop(gifImg, true)
	if err != nil {
		return err
	}
	if !opts.BestEffortClear {
		opts.BestEffortClear = true
	}
	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()
	return d.device.SendGifWithOptions(payload, opts)
}

// experimentalSendPlaylist sends an animated GIF then a static image, both with ForceReinit,
// to mimic the app's two-item playlist behavior.
func (d *Display) experimentalSendPlaylist(animBuf, staticBuf []byte) error {
	if len(animBuf) == 0 || len(staticBuf) == 0 {
		return errors.New("playlist requires both animated (a1) and static (s1) payloads")
	}
	if err := d.experimentalSendAnimated(animBuf, xyaoled.SendOptions{ForceReinit: true}); err != nil {
		return err
	}
	time.Sleep(200 * time.Millisecond)
	return d.experimentalSendStatic(staticBuf, xyaoled.SendOptions{ForceReinit: true, StaticHeader: true})
}

// experimentalSendStaticNoReinit mirrors the "no reinit" path from the app.
func (d *Display) experimentalSendStaticNoReinit(buf []byte, staticHeader bool) error {
	return d.experimentalSendStatic(buf, xyaoled.SendOptions{SkipClear: true, StaticHeader: staticHeader})
}

// experimentalSendAnimatedNoReinit mirrors the app's animated refresh without reinit/clear.
// loopInfinite controls LoopCount: true => LoopCount=0, false => LoopCount=1.
func (d *Display) experimentalSendAnimatedNoReinit(buf []byte, loopInfinite bool) error {
	if err := d.experimentalConnect(); err != nil {
		return err
	}
	gifImg, err := gif.DecodeAll(bytes.NewReader(buf))
	if err != nil {
		return err
	}
	payload, err := d.processAnimatedGifWithLoop(gifImg, loopInfinite)
	if err != nil {
		return err
	}
	d.sendMutex.Lock()
	defer d.sendMutex.Unlock()
	return d.device.SendGifWithOptions(payload, xyaoled.SendOptions{SkipClear: true, BestEffortClear: true})
}

// experimentalBrightnessCycle steps through brightness levels with a small pause.
func (d *Display) experimentalBrightnessCycle(levels []uint8, pause time.Duration) error {
	if err := d.experimentalConnect(); err != nil {
		return err
	}
	if pause == 0 {
		pause = 200 * time.Millisecond
	}
	for _, lvl := range levels {
		if err := d.device.SetBrightness(lvl); err != nil {
			return err
		}
		time.Sleep(pause)
	}
	return nil
}

// experimentalScreenOff dims to minimum and pushes a black frame.
func (d *Display) experimentalScreenOff() error {
	if err := d.experimentalConnect(); err != nil {
		return err
	}
	// remember prior brightness (avoid zero; 0 treated as unknown)
	if cur := d.device.GetBrightness(); cur > 1 {
		d.prevBrightness = cur
	}
	// Best-effort black frame, then drop brightness
	_ = d.sendBlackFrame()
	return d.device.SetBrightness(1)
}

// experimentalScreenOn restores a comfortable brightness.
func (d *Display) experimentalScreenOn() error {
	if err := d.experimentalConnect(); err != nil {
		return err
	}
	target := d.prevBrightness
	if target == 0 {
		target = 100
	}
	if target > 125 {
		target = 125
	}

	// Restore brightness first
	if err := d.device.SetBrightness(target); err != nil {
		return err
	}

	// If we have last content, resend it to wake the panel (skip clear to avoid splash)
	if d.lastGifData != nil {
		opts := d.sendOpts(false, !d.lastGifIsAnim, true)
		d.sendMutex.Lock()
		err := d.device.SendGifWithOptions(d.lastGifData, opts)
		d.sendMutex.Unlock()
		if err != nil {
			return err
		}
		d.firstSendDone = true
	}

	return nil
}
