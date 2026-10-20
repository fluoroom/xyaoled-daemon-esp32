package main

import (
	"bytes"
	"errors"
	"image"
	"image/color"
	"image/gif"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func testDisplay() *Display {
	return &Display{width: 96, height: 16, config: &Config{}}
}

func TestImageToGifHoldsSingleFrame(t *testing.T) {
	d := testDisplay()
	src := image.NewRGBA(image.Rect(0, 0, 10, 10))
	src.Set(0, 0, color.RGBA{R: 255, A: 255})

	data, err := d.imageToGif(d.fitToPanel(src))
	if err != nil {
		t.Fatal(err)
	}
	g, err := gif.DecodeAll(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	if len(g.Image) != 1 || g.Delay[0] != 60000 {
		t.Fatalf("frames=%d delay=%v, want 1 frame held 60000cs", len(g.Image), g.Delay)
	}
	if b := g.Image[0].Bounds(); b.Dx() != 96 || b.Dy() != 16 {
		t.Fatalf("size %v, want 96x16", b)
	}
}

func TestProcessAnimatedGif(t *testing.T) {
	d := testDisplay()
	pal := color.Palette{color.Black, color.White}
	src := &gif.GIF{
		Image: []*image.Paletted{image.NewPaletted(image.Rect(0, 0, 32, 8), pal), image.NewPaletted(image.Rect(0, 0, 32, 8), pal)},
		Delay: []int{0, 20},
	}

	data, err := d.processAnimatedGif(src)
	if err != nil {
		t.Fatal(err)
	}
	g, err := gif.DecodeAll(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	if len(g.Image) != 2 || g.Delay[0] != 5 || g.Delay[1] != 20 {
		t.Fatalf("frames=%d delays=%v, want 2 frames with delays [5 20]", len(g.Image), g.Delay)
	}
	if g.LoopCount != 0 {
		t.Fatalf("LoopCount=%d, want 0 (infinite)", g.LoopCount)
	}
}

func TestParseBrightnessSteps(t *testing.T) {
	got := parseBrightnessSteps("10, 200,x,-3")
	if len(got) != 3 || got[0] != 10 || got[1] != 125 || got[2] != 0 {
		t.Fatalf("got %v", got)
	}
	if len(parseBrightnessSteps("")) != 5 {
		t.Fatal("empty input should give default steps")
	}
}

func TestTestPageAndRedirects(t *testing.T) {
	h := NewHandler(testDisplay())

	rec := httptest.NewRecorder()
	h.HandleTestPage(rec, httptest.NewRequest("GET", "/test", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "/uploadTest") {
		t.Fatalf("test page: %d", rec.Code)
	}

	for path, fn := range map[string]http.HandlerFunc{"/upload": h.HandleUpload, "/uploadTest": h.HandleUploadTest, "/uploadTestGif": h.HandleUploadTestGif} {
		rec := httptest.NewRecorder()
		fn(rec, httptest.NewRequest("GET", path, nil))
		if rec.Code != http.StatusFound || rec.Header().Get("Location") != "/test" {
			t.Errorf("%s: %d %q", path, rec.Code, rec.Header().Get("Location"))
		}
	}
}

func TestUploadTestRejectsBadRequests(t *testing.T) {
	h := NewHandler(testDisplay())
	post := func(cmd string) int {
		var body bytes.Buffer
		body.WriteString("--x\r\nContent-Disposition: form-data; name=\"command\"\r\n\r\n" + cmd + "\r\n--x--\r\n")
		req := httptest.NewRequest("POST", "/uploadTest", &body)
		req.Header.Set("Content-Type", "multipart/form-data; boundary=x")
		rec := httptest.NewRecorder()
		h.HandleUploadTest(rec, req)
		return rec.Code
	}
	if code := post("nope"); code != http.StatusBadRequest {
		t.Errorf("unknown command: %d", code)
	}
	if code := post("send_animated"); code != http.StatusInternalServerError {
		t.Errorf("send without file: %d", code)
	}
}

func TestStreamingBlocksPlaylistRequests(t *testing.T) {
	d := testDisplay()
	d.currentBright = 80
	d.streaming.Store(true)

	frame, err := d.imageToGif(image.NewRGBA(image.Rect(0, 0, 96, 16)))
	if err != nil {
		t.Fatal(err)
	}
	if err := d.ShowImage(frame); !errors.Is(err, errStreaming) {
		t.Errorf("ShowImage while streaming: got %v, want errStreaming", err)
	}
	if err := d.ShowPlaylist([][]byte{frame}); !errors.Is(err, errStreaming) {
		t.Errorf("ShowPlaylist while streaming: got %v", err)
	}
	if err := d.ClearPlaylist(); err != errStreaming {
		t.Errorf("ClearPlaylist while streaming: got %v", err)
	}

	rec := httptest.NewRecorder()
	NewHandler(d).HandleClearPlaylist(rec, httptest.NewRequest(http.MethodGet, "/clear", nil))
	if rec.Code != http.StatusConflict {
		t.Errorf("/clear while streaming: status %d, want 409", rec.Code)
	}
}

func TestStreamResumesAfterDelayUnlessReconnected(t *testing.T) {
	d := testDisplay()
	s := NewStreamer(d, 7, 50*time.Millisecond)

	s.connect()
	if err := d.ResumePlaylist(); !errors.Is(err, errStreaming) {
		t.Fatalf("/resume while streaming: got %v, want errStreaming", err)
	}

	// A drop followed by a reconnect inside the delay keeps the live view.
	s.disconnect()
	time.Sleep(20 * time.Millisecond)
	s.connect()
	time.Sleep(100 * time.Millisecond)
	if !d.streaming.Load() {
		t.Fatal("playlist resumed although the client reconnected within the delay")
	}

	// A real disconnect holds until the delay has passed, then resumes.
	s.disconnect()
	time.Sleep(20 * time.Millisecond)
	if !d.streaming.Load() {
		t.Fatal("playlist resumed before the delay")
	}
	deadline := time.Now().Add(time.Second)
	for d.streaming.Load() && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	if d.streaming.Load() {
		t.Fatal("playlist never resumed after the delay")
	}
}
