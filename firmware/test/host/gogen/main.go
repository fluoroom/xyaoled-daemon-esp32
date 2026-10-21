// Fixture generator and checker for the imaging host tests.
//
//	go run . gen <dir>              write source images and the frames the Go daemon makes of them
//	go run . verify <gif> <frames>  decode a GIF written by the C encoder with Go's image/gif and
//	                                compare it with raw RGB frames
//	go run . anim <w> <h> <n> <out> write an n-frame w×h animation to out.gif and the frames the Go
//	                                daemon makes of it to out.rgb (for the QEMU volume test)
//
// The expected frames follow reference/go-daemon/display.go: animations
// are composited at full size (processAnimatedGif) and every frame or still
// is fitted with resizeImage (NearestNeighbor.Scale, draw.Src) on black.
package main

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/gif"
	"image/jpeg"
	"image/png"
	"os"
	"path/filepath"

	"golang.org/x/image/draw"
)

const panelW, panelH = 96, 16

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: gogen gen <dir> | verify <gif> <frames>")
		os.Exit(2)
	}
	var err error
	switch os.Args[1] {
	case "gen":
		err = gen(os.Args[2])
	case "verify":
		err = verify(os.Args[2], os.Args[3])
	case "anim":
		err = longAnim(os.Args[2:])
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func fillBlack(img *image.RGBA) {
	for i := 0; i < len(img.Pix); i += 4 {
		img.Pix[i], img.Pix[i+1], img.Pix[i+2], img.Pix[i+3] = 0, 0, 0, 255
	}
}

// resize is display.go resizeImage.
func resize(src image.Image) *image.RGBA {
	b := src.Bounds()
	scale := min(float64(panelW)/float64(b.Dx()), float64(panelH)/float64(b.Dy()))
	nw, nh := int(float64(b.Dx())*scale), int(float64(b.Dy())*scale)
	dst := image.NewRGBA(image.Rect(0, 0, panelW, panelH))
	fillBlack(dst)
	ox, oy := (panelW-nw)/2, (panelH-nh)/2
	draw.NearestNeighbor.Scale(dst, image.Rect(ox, oy, ox+nw, oy+nh), src, b, draw.Src, nil)
	return dst
}

func rotate180(src *image.RGBA) *image.RGBA {
	dst := image.NewRGBA(src.Bounds())
	w, h := src.Bounds().Dx(), src.Bounds().Dy()
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			dst.Set(w-1-x, h-1-y, src.At(x, y))
		}
	}
	return dst
}

// still is ShowImage: fit, then flatten onto opaque black (imageToGif).
func still(src image.Image) *image.RGBA {
	r := resize(src)
	out := image.NewRGBA(r.Bounds())
	fillBlack(out)
	draw.Draw(out, out.Bounds(), r, image.Point{}, draw.Over)
	return out
}

// animated is processAnimatedGif up to the palette step.
func animated(src *gif.GIF) ([]*image.RGBA, []int) {
	w, h := src.Config.Width, src.Config.Height
	canvas := image.NewRGBA(image.Rect(0, 0, w, h))
	fillBlack(canvas)
	var frames []*image.RGBA
	var delays []int
	var restoreTo *image.RGBA
	for i, frame := range src.Image {
		disposal := src.Disposal[i]
		if disposal == gif.DisposalPrevious {
			restoreTo = image.NewRGBA(canvas.Bounds())
			copy(restoreTo.Pix, canvas.Pix)
		}
		draw.Draw(canvas, frame.Bounds(), frame, frame.Bounds().Min, draw.Over)
		snap := image.NewRGBA(canvas.Bounds())
		copy(snap.Pix, canvas.Pix)
		frames = append(frames, resize(snap))
		d := src.Delay[i]
		if d == 0 {
			d = 5
		}
		delays = append(delays, d)
		switch disposal {
		case gif.DisposalBackground:
			r := frame.Bounds().Intersect(canvas.Bounds())
			for y := r.Min.Y; y < r.Max.Y; y++ {
				for x := r.Min.X; x < r.Max.X; x++ {
					canvas.SetRGBA(x, y, color.RGBA{0, 0, 0, 255})
				}
			}
		case gif.DisposalPrevious:
			if restoreTo != nil {
				canvas = restoreTo
			}
		}
	}
	return frames, delays
}

func writeFrames(path string, frames []*image.RGBA, delays []int) error {
	var buf bytes.Buffer
	for _, f := range frames {
		for i := 0; i < len(f.Pix); i += 4 {
			buf.Write(f.Pix[i : i+3])
		}
	}
	if err := os.WriteFile(path+".rgb", buf.Bytes(), 0o644); err != nil {
		return err
	}
	var d bytes.Buffer
	for _, v := range delays {
		fmt.Fprintf(&d, "%d\n", v)
	}
	return os.WriteFile(path+".delays", d.Bytes(), 0o644)
}

var testPalette = color.Palette{
	color.RGBA{0, 0, 0, 255}, color.RGBA{255, 0, 0, 255}, color.RGBA{0, 255, 0, 255},
	color.RGBA{0, 0, 255, 255}, color.RGBA{255, 255, 0, 255}, color.RGBA{255, 255, 255, 255},
	color.RGBA{128, 64, 32, 255}, color.RGBA{0, 0, 0, 0}, // index 7 transparent
}

func pattern(r image.Rectangle, seed int) *image.Paletted {
	p := image.NewPaletted(r, testPalette)
	for y := r.Min.Y; y < r.Max.Y; y++ {
		for x := r.Min.X; x < r.Max.X; x++ {
			p.SetColorIndex(x, y, uint8((x/3+y/2+seed)%7))
		}
	}
	return p
}

// keyframed: full first frame, then partial frames with transparency and
// every disposal method.
func keyframed(w, h int) *gif.GIF {
	g := &gif.GIF{Config: image.Config{ColorModel: testPalette, Width: w, Height: h}}
	add := func(img *image.Paletted, delay int, disposal byte) {
		g.Image = append(g.Image, img)
		g.Delay = append(g.Delay, delay)
		g.Disposal = append(g.Disposal, disposal)
	}
	add(pattern(image.Rect(0, 0, w, h), 0), 10, gif.DisposalNone)
	f1 := pattern(image.Rect(w/4, h/4, w*3/4, h*3/4), 3)
	for y := f1.Rect.Min.Y; y < f1.Rect.Max.Y; y += 2 {
		for x := f1.Rect.Min.X; x < f1.Rect.Max.X; x++ {
			f1.SetColorIndex(x, y, 7)
		}
	}
	add(f1, 0, gif.DisposalBackground)
	add(pattern(image.Rect(w/2, 0, w, h/2), 5), 20, gif.DisposalPrevious)
	add(pattern(image.Rect(0, h/2, w/2, h), 1), 7, gif.DisposalNone)
	return g
}

// dither is display.go toPaletted: the first 256 distinct colours, then
// draw.FloydSteinberg.
func dither(src *image.RGBA) *image.RGBA {
	seen := map[color.RGBA]bool{}
	var pal color.Palette
	b := src.Bounds()
	for y := b.Min.Y; y < b.Max.Y && len(pal) < 256; y++ {
		for x := b.Min.X; x < b.Max.X && len(pal) < 256; x++ {
			if c := src.RGBAAt(x, y); !seen[c] {
				seen[c] = true
				pal = append(pal, c)
			}
		}
	}
	p := image.NewPaletted(b, pal)
	draw.FloydSteinberg.Draw(p, b, src, b.Min)
	out := image.NewRGBA(b)
	draw.Draw(out, b, p, b.Min, draw.Src)
	return out
}

func encodeGif(g *gif.GIF) ([]byte, error) {
	var buf bytes.Buffer
	err := gif.EncodeAll(&buf, g)
	return buf.Bytes(), err
}

func gen(dir string) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	// Animated GIFs: small (upscaled), wide and large (downscaled).
	for _, c := range []struct {
		name string
		w, h int
	}{{"anim_small", 40, 20}, {"anim_wide", 300, 30}, {"anim_large", 1000, 300}} {
		data, err := encodeGif(keyframed(c.w, c.h))
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(dir, c.name+".gif"), data, 0o644); err != nil {
			return err
		}
		src, err := gif.DecodeAll(bytes.NewReader(data))
		if err != nil {
			return err
		}
		frames, delays := animated(src)
		if err := writeFrames(filepath.Join(dir, c.name), frames, delays); err != nil {
			return err
		}
		if c.name == "anim_small" {
			for i := range frames {
				frames[i] = rotate180(frames[i])
			}
			if err := writeFrames(filepath.Join(dir, c.name+"_flipped"), frames, delays); err != nil {
				return err
			}
		}
	}

	// A panel-sized animation for passthrough, and one that fails it
	// (local colour tables, as Go writes when frame palettes differ).
	// Without the transparent entry: Go flags any alpha-0 palette colour as
	// the transparent index.
	opaque := testPalette[:7]
	pass := &gif.GIF{Config: image.Config{ColorModel: opaque, Width: panelW, Height: panelH}}
	lct := &gif.GIF{}
	for i := 0; i < 3; i++ {
		p := pattern(image.Rect(0, 0, panelW, panelH), i)
		p.Palette = opaque
		pass.Image = append(pass.Image, p)
		pass.Delay = append(pass.Delay, 4)
		p = pattern(image.Rect(0, 0, panelW, panelH), i)
		p.Palette = append(color.Palette{}, opaque...)
		lct.Image = append(lct.Image, p)
		lct.Delay = append(lct.Delay, 4)
	}
	for name, g := range map[string]*gif.GIF{"pass_ok": pass, "pass_lct": lct} {
		data, err := encodeGif(g)
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(dir, name+".gif"), data, 0o644); err != nil {
			return err
		}
	}

	// For trying passthrough_partial on the panel (test/panel): animations
	// the conservative check refuses, and a solid still to send before
	// them, so whatever shows through transparent pixels is visible.
	if err := panelGifs(dir); err != nil {
		return err
	}

	// Stills: an RGBA PNG with alpha and more than 256 colours, a tiny
	// paletted PNG (upscaled), and a baseline JPEG.
	rgba := image.NewNRGBA(image.Rect(0, 0, 200, 50))
	for y := 0; y < 50; y++ {
		for x := 0; x < 200; x++ {
			a := uint8(255)
			if x < 40 {
				a = uint8(x * 6)
			}
			rgba.SetNRGBA(x, y, color.NRGBA{uint8(x), uint8(y * 5), uint8(255 - x), a})
		}
	}
	small := pattern(image.Rect(0, 0, 8, 4), 2)
	small.Palette = testPalette[:7]
	for name, img := range map[string]image.Image{"png_rgba": rgba, "png_small": small} {
		var buf bytes.Buffer
		if err := png.Encode(&buf, img); err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(dir, name+".png"), buf.Bytes(), 0o644); err != nil {
			return err
		}
		dec, _, err := image.Decode(bytes.NewReader(buf.Bytes()))
		if err != nil {
			return err
		}
		st := still(dec)
		if err := writeFrames(filepath.Join(dir, name), []*image.RGBA{st}, []int{0}); err != nil {
			return err
		}
		// What the panel gets: imageToGif's palette (first 256 colours)
		// with Floyd–Steinberg.
		if err := writeFrames(filepath.Join(dir, name+"_dithered"), []*image.RGBA{dither(st)}, []int{0}); err != nil {
			return err
		}
	}

	photo := image.NewRGBA(image.Rect(0, 0, 320, 80))
	for y := 0; y < 80; y++ {
		for x := 0; x < 320; x++ {
			c := color.RGBA{uint8(x * 255 / 320), uint8(y * 3), 128, 255}
			if (x/40+y/40)%2 == 0 {
				c = color.RGBA{240, 240, 240, 255}
			}
			photo.SetRGBA(x, y, c)
		}
	}
	var jbuf bytes.Buffer
	if err := jpeg.Encode(&jbuf, photo, &jpeg.Options{Quality: 95}); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(dir, "photo.jpg"), jbuf.Bytes(), 0o644); err != nil {
		return err
	}
	dec, err := jpeg.Decode(bytes.NewReader(jbuf.Bytes()))
	if err != nil {
		return err
	}
	return writeFrames(filepath.Join(dir, "photo"), []*image.RGBA{still(dec)}, []int{0})
}

// longAnim writes an n-frame animation of full frames and its expected
// panel frames.
// panelGifs writes pt_solid.png (solid blue) and three panel-sized
// animations, each looping every 2 s:
//
//	pt_transparent  two full frames; the left half is transparent, the right
//	                half alternates green and red
//	pt_partial      a full yellow frame, then a 16×16 red square stepping
//	                right in partial frames that are not disposed (a trail)
//	pt_dispose      the same with background disposal (no trail)
func panelGifs(dir string) error {
	full := image.Rect(0, 0, panelW, panelH)
	clear := color.RGBA{}
	green, red := color.RGBA{0, 255, 0, 255}, color.RGBA{255, 0, 0, 255}
	yellow := color.RGBA{255, 255, 0, 255}

	solid := image.NewRGBA(full)
	for i := 0; i < len(solid.Pix); i += 4 {
		solid.Pix[i+2], solid.Pix[i+3] = 255, 255
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, solid); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(dir, "pt_solid.png"), buf.Bytes(), 0o644); err != nil {
		return err
	}

	pal := color.Palette{clear, green, red}
	tr := &gif.GIF{Config: image.Config{ColorModel: pal, Width: panelW, Height: panelH}}
	for i := uint8(1); i <= 2; i++ {
		f := image.NewPaletted(full, pal)
		for y := 0; y < panelH; y++ {
			for x := panelW / 2; x < panelW; x++ {
				f.SetColorIndex(x, y, i)
			}
		}
		tr.Image = append(tr.Image, f)
		tr.Delay = append(tr.Delay, 100)
	}

	pal = color.Palette{yellow, red}
	moving := func(disposal byte) *gif.GIF {
		g := &gif.GIF{Config: image.Config{ColorModel: pal, Width: panelW, Height: panelH}}
		g.Image = append(g.Image, image.NewPaletted(full, pal)) // all yellow
		g.Delay = append(g.Delay, 50)
		g.Disposal = append(g.Disposal, gif.DisposalNone)
		for x := 0; x < panelW; x += 16 {
			sq := image.NewPaletted(image.Rect(x, 0, x+16, panelH), pal)
			for i := range sq.Pix {
				sq.Pix[i] = 1
			}
			g.Image = append(g.Image, sq)
			g.Delay = append(g.Delay, 25)
			g.Disposal = append(g.Disposal, disposal)
		}
		return g
	}

	for name, g := range map[string]*gif.GIF{
		"pt_transparent": tr, "pt_partial": moving(gif.DisposalNone), "pt_dispose": moving(gif.DisposalBackground),
	} {
		data, err := encodeGif(g)
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(dir, name+".gif"), data, 0o644); err != nil {
			return err
		}
	}
	return nil
}

func longAnim(args []string) error {
	if len(args) != 4 {
		return fmt.Errorf("anim <w> <h> <n> <out>")
	}
	var w, h, n int
	fmt.Sscan(args[0], &w)
	fmt.Sscan(args[1], &h)
	fmt.Sscan(args[2], &n)
	opaque := testPalette[:7]
	g := &gif.GIF{Config: image.Config{ColorModel: opaque, Width: w, Height: h}}
	for i := 0; i < n; i++ {
		p := pattern(image.Rect(0, 0, w, h), i)
		p.Palette = opaque
		g.Image = append(g.Image, p)
		g.Delay = append(g.Delay, 3)
		g.Disposal = append(g.Disposal, gif.DisposalNone)
	}
	data, err := encodeGif(g)
	if err != nil {
		return err
	}
	if err := os.WriteFile(args[3]+".gif", data, 0o644); err != nil {
		return err
	}
	src, err := gif.DecodeAll(bytes.NewReader(data))
	if err != nil {
		return err
	}
	frames, delays := animated(src)
	return writeFrames(args[3], frames, delays)
}

// verify decodes a GIF with image/gif and compares its (full, opaque)
// frames with raw RGB frames.
func verify(gifPath, framesPath string) error {
	data, err := os.ReadFile(gifPath)
	if err != nil {
		return err
	}
	g, err := gif.DecodeAll(bytes.NewReader(data))
	if err != nil {
		return fmt.Errorf("%s: image/gif rejects it: %w", gifPath, err)
	}
	want, err := os.ReadFile(framesPath)
	if err != nil {
		return err
	}
	frameBytes := g.Config.Width * g.Config.Height * 3
	if len(g.Image)*frameBytes != len(want) {
		return fmt.Errorf("%s: %d frames of %dx%d, want %d bytes", gifPath, len(g.Image), g.Config.Width, g.Config.Height, len(want))
	}
	for i, img := range g.Image {
		for y := 0; y < g.Config.Height; y++ {
			for x := 0; x < g.Config.Width; x++ {
				r, gg, b, _ := img.At(x, y).RGBA()
				o := i*frameBytes + (y*g.Config.Width+x)*3
				if byte(r>>8) != want[o] || byte(gg>>8) != want[o+1] || byte(b>>8) != want[o+2] {
					return fmt.Errorf("%s: frame %d pixel %d,%d differs", gifPath, i, x, y)
				}
			}
		}
	}
	fmt.Printf("%s: ok, %d frame(s), loop %d\n", gifPath, len(g.Image), g.LoopCount)
	return nil
}
