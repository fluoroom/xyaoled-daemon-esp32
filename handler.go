package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"image/gif"
	"io"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"
)

// ImageRequest represents the request body for image display
type ImageRequest struct {
	ImageData string `json:"image"` // Base64 encoded image
	Type      string `json:"type"`  // "static" or "animated"; detected when empty
}

// Handler manages HTTP requests
type Handler struct {
	display          *Display
	lastOnBrightness uint8
}

// NewHandler creates a new HTTP handler
func NewHandler(display *Display) *Handler {
	return &Handler{display: display, lastOnBrightness: 125}
}

// isAnimatedGif reports whether data is a GIF with more than one frame.
func isAnimatedGif(data []byte) bool {
	g, err := gif.DecodeAll(bytes.NewReader(data))
	return err == nil && len(g.Image) > 1
}

// show displays data as animated or static. An empty kind is detected from
// the content. Every send does a full reinit.
func (h *Handler) show(data []byte, kind string) error {
	animated := kind == "animated" || (kind == "" && isAnimatedGif(data))
	if animated {
		return h.display.ShowAnimatedGif(data)
	}
	return h.display.ShowImage(data)
}

func writeOK(w http.ResponseWriter, extra map[string]interface{}) {
	resp := map[string]interface{}{"status": "ok"}
	for k, v := range extra {
		resp[k] = v
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func readFormFile(r *http.Request, field string) []byte {
	file, _, err := r.FormFile(field)
	if err != nil {
		return nil
	}
	defer file.Close()
	data, err := io.ReadAll(file)
	if err != nil {
		return nil
	}
	return data
}

// HandleDisplay handles POST /display and /display-r (identical: both reinit).
func (h *Handler) HandleDisplay(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req ImageRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}
	if req.ImageData == "" {
		http.Error(w, "Missing image data", http.StatusBadRequest)
		return
	}
	data, err := base64.StdEncoding.DecodeString(req.ImageData)
	if err != nil {
		http.Error(w, "Invalid base64 image data", http.StatusBadRequest)
		return
	}

	if err := h.show(data, req.Type); err != nil {
		log.Printf("Error displaying image: %v", err)
		http.Error(w, "Failed to display image", http.StatusInternalServerError)
		return
	}
	writeOK(w, nil)
}

// HandleHealth handles GET /health endpoint
func (h *Handler) HandleHealth(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status": "healthy",
		"config": map[string]interface{}{
			"width":  h.display.width,
			"height": h.display.height,
		},
	})
}

// HandleUpload handles POST /upload (multipart "file"); GET redirects to /test.
func (h *Handler) HandleUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		http.Redirect(w, r, "/test", http.StatusFound)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := r.ParseMultipartForm(10 << 20); err != nil {
		http.Error(w, "File too large", http.StatusBadRequest)
		return
	}
	data := readFormFile(r, "file")
	if len(data) == 0 {
		http.Error(w, "No file provided", http.StatusBadRequest)
		return
	}
	if err := h.show(data, ""); err != nil {
		log.Printf("Error displaying uploaded image: %v", err)
		http.Error(w, "Failed to display image", http.StatusInternalServerError)
		return
	}
	writeOK(w, nil)
}

// HandleUploadTest handles POST /uploadTest scripted commands; GET redirects to /test.
// Older command names (no-reinit and playlist variants) map onto the reinit sends.
func (h *Handler) HandleUploadTest(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		http.Redirect(w, r, "/test", http.StatusFound)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := r.ParseMultipartForm(20 << 20); err != nil {
		http.Error(w, "Invalid form", http.StatusBadRequest)
		return
	}

	file := func(field string) ([]byte, error) {
		if data := readFormFile(r, field); len(data) > 0 {
			return data, nil
		}
		return nil, fmt.Errorf("missing %s upload", field)
	}

	var err error
	switch cmd := r.FormValue("command"); cmd {
	case "connect":
		err = h.display.Connect()
	case "disconnect":
		err = h.display.Close()
	case "send_animated", "send_animated_reinit", "send_animated_no_reinit":
		var data []byte
		if data, err = file("animated"); err == nil {
			err = h.display.ShowAnimatedGif(data)
		}
	case "send_static", "send_static_reinit", "send_static_no_reinit":
		var data []byte
		if data, err = file("static"); err == nil {
			err = h.display.ShowImage(data)
		}
	case "send_other_static_no_reinit":
		var data []byte
		if data, err = file("other"); err == nil {
			err = h.display.ShowImage(data)
		}
	case "send_playlist":
		var data []byte
		if data, err = file("animated"); err == nil {
			err = h.display.ShowAnimatedGif(data)
		}
		if err == nil {
			if data := readFormFile(r, "static"); len(data) > 0 {
				err = h.display.ShowImage(data)
			}
		}
	case "brightness_cycle":
		for _, step := range parseBrightnessSteps(r.FormValue("brightness")) {
			if err = h.display.SetBrightnessIfNotManualOff(step); err != nil {
				break
			}
			time.Sleep(200 * time.Millisecond)
		}
	case "screen_off":
		err = h.display.TurnOff()
	case "screen_on":
		err = h.display.TurnOn(80)
	default:
		http.Error(w, "Unknown command", http.StatusBadRequest)
		return
	}

	if err != nil {
		log.Printf("/uploadTest %s failed: %v", r.FormValue("command"), err)
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Write([]byte("ok"))
}

// HandleUploadTestGif handles POST /uploadTestGif (multipart "gif"), sending it
// as an animated GIF; GET redirects to /test.
func (h *Handler) HandleUploadTestGif(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		http.Redirect(w, r, "/test", http.StatusFound)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := r.ParseMultipartForm(10 << 20); err != nil {
		http.Error(w, "Failed to parse form", http.StatusBadRequest)
		return
	}
	data := readFormFile(r, "gif")
	if len(data) == 0 {
		http.Error(w, "No file uploaded", http.StatusBadRequest)
		return
	}
	if err := h.display.ShowAnimatedGif(data); err != nil {
		log.Printf("/uploadTestGif failed: %v", err)
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeOK(w, nil)
}

// parseBrightnessSteps converts a comma-separated list into brightness levels (0-125).
func parseBrightnessSteps(raw string) []uint8 {
	var out []uint8
	for _, p := range strings.Split(raw, ",") {
		v, err := strconv.Atoi(strings.TrimSpace(p))
		if err != nil {
			continue
		}
		out = append(out, uint8(max(0, min(125, v))))
	}
	if len(out) == 0 {
		return []uint8{0, 32, 64, 96, 125}
	}
	return out
}

// HandleOff sets brightness to 5 and remembers previous level.
func (h *Handler) HandleOff(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if h.display.device == nil {
		http.Error(w, "Device not ready", http.StatusServiceUnavailable)
		return
	}
	if cur := h.display.device.GetBrightness(); cur >= 5 {
		h.lastOnBrightness = cur
	}
	if err := h.display.TurnOff(); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeOK(w, map[string]interface{}{"brightness": "5"})
}

// HandleOn restores brightness to the level saved before /off.
func (h *Handler) HandleOn(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if h.display.device == nil {
		http.Error(w, "Device not ready", http.StatusServiceUnavailable)
		return
	}
	target := max(5, min(125, h.lastOnBrightness))
	if err := h.display.TurnOn(target); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeOK(w, map[string]interface{}{"brightness": target})
}

// HandleBrightness sets brightness to the provided value (clamped 5-125).
func (h *Handler) HandleBrightness(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if h.display.device == nil {
		http.Error(w, "Device not ready", http.StatusServiceUnavailable)
		return
	}
	val, err := strconv.Atoi(strings.TrimPrefix(r.URL.Path, "/brightness/"))
	if err != nil {
		http.Error(w, "Invalid brightness", http.StatusBadRequest)
		return
	}
	level := uint8(max(5, min(125, val)))
	h.lastOnBrightness = level
	if err := h.display.SetBrightnessIfNotManualOff(level); err != nil {
		http.Error(w, err.Error(), http.StatusConflict)
		return
	}
	writeOK(w, map[string]interface{}{"brightness": level})
}

// HandlePowerOn sends the panel's hardware power-on command.
func (h *Handler) HandlePowerOn(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := h.display.PowerOn(); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeOK(w, nil)
}

// HandlePowerOff sends the panel's hardware power-off command.
func (h *Handler) HandlePowerOff(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := h.display.PowerOff(); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeOK(w, nil)
}

// HandleClearPlaylist clears the screen and wipes the on-device playlist.
func (h *Handler) HandleClearPlaylist(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := h.display.ClearPlaylist(); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeOK(w, nil)
}

// HandleReconnect triggers a reconnect attempt.
func (h *Handler) HandleReconnect(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	go h.display.attemptReconnection()
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusAccepted)
	json.NewEncoder(w).Encode(map[string]string{"status": "reconnecting"})
}
