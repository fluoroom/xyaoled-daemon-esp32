package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"

	"xyaoled-daemon/xyaoled"
)

// ImageRequest represents the request body for image display
type ImageRequest struct {
	ImageData string `json:"image"` // Base64 encoded image
	Type      string `json:"type"`  // "static" or "animated"
}

// Handler manages HTTP requests
type Handler struct {
	display *Display
	config  *Config
}

// NewHandler creates a new HTTP handler
func NewHandler(display *Display, cfg *Config) *Handler {
	return &Handler{
		display: display,
		config:  cfg,
	}
}

// parseBrightnessSteps converts a comma-separated list into brightness levels (0-125 observed range).
func parseBrightnessSteps(raw string) []uint8 {
	defaults := []uint8{0, 32, 64, 96, 125}
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return defaults
	}

	parts := strings.Split(raw, ",")
	out := make([]uint8, 0, len(parts))
	for _, p := range parts {
		v, err := strconv.Atoi(strings.TrimSpace(p))
		if err != nil {
			continue
		}
		if v < 0 {
			v = 0
		}
		if v > 125 {
			v = 125
		}
		out = append(out, uint8(v))
	}
	if len(out) == 0 {
		return defaults
	}
	return out
}

func readOptionalFile(r *http.Request, field string) ([]byte, string, error) {
	file, header, err := r.FormFile(field)
	if err != nil {
		return nil, "", err
	}
	defer file.Close()
	data, err := io.ReadAll(file)
	if err != nil {
		return nil, "", err
	}
	return data, header.Filename, nil
}

// displayImage handles the actual display logic (shared by multiple endpoints)
func (h *Handler) displayImage(base64Data string, isAnimated bool) error {
	if isAnimated {
		return h.display.ShowAnimatedGif(base64Data)
	}
	if err := h.display.ShowImage(base64Data); err != nil {
		return err
	}
	if h.config.Tests {
		runExperimentalNoClearVariants(h.display, base64Data, isAnimated)
	}
	return nil
}

// HandleUploadTest serves a form and executes scripted command sequences for manual hardware trials.
func (h *Handler) HandleUploadTest(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		html := `<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>XYAOLED Upload Test</title>
<style>
body{font-family:sans-serif;max-width:640px;margin:40px auto;padding:16px;line-height:1.4}
label{display:block;margin-top:12px;font-weight:600}
input,select,button{width:100%;padding:10px;margin-top:6px;font-size:15px}
button{background:#111;color:#fff;border:none;border-radius:4px;cursor:pointer}
button:hover{background:#333}
.hint{color:#555;font-size:13px;margin-top:4px}
.msg{margin-top:14px;padding:10px;border-radius:4px}
.ok{background:#d4edda;color:#155724}
.err{background:#f8d7da;color:#721c24}
</style>
</head>
<body>
<h1>Upload Test Commands</h1>
<p>Use this to replay the observed app command patterns against the panel.</p>
<form id="testForm" method="POST" enctype="multipart/form-data">
  <label for="command">Command</label>
  <select name="command" id="command" required>
	<option value="connect">hello: connect</option>
	<option value="send_animated_reinit">with reinit: send animated (a1)</option>
	<option value="send_animated_no_reinit">no reinit: send animated (a1)</option>
	<option value="send_static_reinit">with reinit: send static (s1)</option>
	<option value="send_playlist">with reinit: send a1 + s1 playlist</option>
	<option value="send_static_no_reinit">no reinit: send static (s1)</option>
	<option value="send_other_static_no_reinit">no reinit: send static (other)</option>
	<option value="brightness_cycle">command: brightness x3</option>
	<option value="screen_off">command: screen off</option>
	<option value="screen_on">command: screen on</option>
	<option value="disconnect">bye: disconnect</option>
  </select>

  <label for="animated">Animated GIF (a1)</label>
  <input type="file" name="animated" id="animated" accept="image/gif">
  <div class="hint">Required for animated commands and playlist.</div>

	<label><input type="checkbox" name="loop_infinite" id="loop_infinite" checked> Force infinite loop (animated)</label>

  <label for="static">Static image (s1)</label>
  <input type="file" name="static" id="static" accept="image/*,.gif">
  <div class="hint">Used by static and playlist commands.</div>

  <label for="other">Other static image ("other")</label>
  <input type="file" name="other" id="other" accept="image/*,.gif">
  <div class="hint">Used only by the second no-reinit static send.</div>

	<label for="brightness">Brightness steps (comma separated, 0-125)</label>
	<input type="text" name="brightness" id="brightness" placeholder="0,32,64,96,125" inputmode="numeric" pattern="[0-9, ]*">

  <button type="submit">Run</button>
</form>
<div id="message" class="msg" style="display:none"></div>
<script>
document.getElementById('testForm').onsubmit=async(e)=>{
  e.preventDefault();
  const form=e.target;
  const msg=document.getElementById('message');
  msg.style.display='block';
  msg.textContent='Running...';
  msg.className='msg';
  try{
	const res=await fetch('/uploadTest',{method:'POST',body:new FormData(form)});
	const text=await res.text();
	if(res.ok){
	  msg.textContent=text||'✓ Command sent';
	  msg.className='msg ok';
	}else{
	  msg.textContent=text||'✗ Failed';
	  msg.className='msg err';
	}
  }catch(err){
	msg.textContent='✗ Error: '+err.message;
	msg.className='msg err';
  }
};
</script>
</body>
</html>`
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write([]byte(html))
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

	cmd := r.FormValue("command")
	brightnessRaw := r.FormValue("brightness")
	loopInfinite := r.FormValue("loop_infinite") != ""

	var (
		animated, static, other []byte
	)

	if file, _, ferr := readOptionalFile(r, "animated"); ferr == nil {
		animated = file
	}
	if file, _, ferr := readOptionalFile(r, "static"); ferr == nil {
		static = file
	}
	if file, _, ferr := readOptionalFile(r, "other"); ferr == nil {
		other = file
	}

	requirePayload := func(data []byte, name string) error {
		if len(data) == 0 {
			return fmt.Errorf("missing %s upload", name)
		}
		return nil
	}

	var runErr error
	switch cmd {
	case "connect":
		runErr = h.display.experimentalConnect()
	case "send_animated_reinit":
		if runErr = requirePayload(animated, "animated"); runErr == nil {
			runErr = h.display.experimentalSendAnimated(animated, xyaoled.SendOptions{ForceReinit: true})
		}
	case "send_animated_no_reinit":
		if runErr = requirePayload(animated, "animated"); runErr == nil {
			runErr = h.display.experimentalSendAnimatedNoReinit(animated, loopInfinite)
		}
	case "send_static_reinit":
		if runErr = requirePayload(static, "static"); runErr == nil {
			runErr = h.display.experimentalSendStatic(static, xyaoled.SendOptions{ForceReinit: true, StaticHeader: true})
		}
	case "send_playlist":
		runErr = h.display.experimentalSendPlaylist(animated, static)
	case "send_static_no_reinit":
		if runErr = requirePayload(static, "static"); runErr == nil {
			runErr = h.display.experimentalSendStaticNoReinit(static, true)
		}
	case "send_other_static_no_reinit":
		if runErr = requirePayload(other, "other"); runErr == nil {
			runErr = h.display.experimentalSendStaticNoReinit(other, true)
		}
	case "brightness_cycle":
		steps := parseBrightnessSteps(brightnessRaw)
		runErr = h.display.experimentalBrightnessCycle(steps, 200*time.Millisecond)
	case "screen_off":
		runErr = h.display.experimentalScreenOff()
	case "screen_on":
		runErr = h.display.experimentalScreenOn()
	case "disconnect":
		runErr = h.display.experimentalDisconnect()
	default:
		http.Error(w, "Unknown command", http.StatusBadRequest)
		return
	}

	if runErr != nil {
		log.Printf("/uploadTest %s failed: %v", cmd, runErr)
		http.Error(w, runErr.Error(), http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusOK)
	w.Write([]byte("ok"))
}

// HandleDisplay handles POST /display endpoint
func (h *Handler) HandleDisplay(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req ImageRequest
	err := json.NewDecoder(r.Body).Decode(&req)
	if err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	if req.ImageData == "" {
		http.Error(w, "Missing image data", http.StatusBadRequest)
		return
	}

	// Default to static if type not specified
	if req.Type == "" {
		req.Type = "static"
	}

	// Display image
	if err := h.displayImage(req.ImageData, req.Type == "animated"); err != nil {
		log.Printf("Error displaying image: %v", err)
		http.Error(w, "Failed to display image", http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

// HandleHealth handles GET /health endpoint
func (h *Handler) HandleHealth(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status": "healthy",
		"config": map[string]interface{}{
			"width":                  h.config.DisplayWidth,
			"height":                 h.config.DisplayHeight,
			"frame_interval_seconds": h.config.FrameIntervalSeconds,
		},
	})
}

// HandleUpload handles GET and POST /upload endpoint
func (h *Handler) HandleUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		// Serve the upload form
		html := `<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Upload Image</title>
<style>
body{font-family:sans-serif;max-width:500px;margin:50px auto;padding:20px}
input,button{display:block;width:100%;margin:10px 0;padding:10px;font-size:16px}
button{background:#007bff;color:#fff;border:none;cursor:pointer}
button:hover{background:#0056b3}
.msg{padding:10px;margin:10px 0;border-radius:4px}
.success{background:#d4edda;color:#155724}
.error{background:#f8d7da;color:#721c24}
</style>
</head>
<body>
<h1>Upload Image to LED Display</h1>
<form id="uploadForm" enctype="multipart/form-data">
<input type="file" id="file" accept="image/*,.gif" required>
<button type="submit">Upload & Display</button>
</form>
<div id="message"></div>
<script>
document.getElementById('uploadForm').onsubmit=async(e)=>{
e.preventDefault();
const file=document.getElementById('file').files[0];
if(!file)return;
const msg=document.getElementById('message');
msg.textContent='Uploading...';
msg.className='msg';
try{
const reader=new FileReader();
reader.onload=async()=>{
const base64=reader.result.split(',')[1];
const isAnimated=file.name.toLowerCase().endsWith('.gif');
const res=await fetch('/display',{
method:'POST',
headers:{'Content-Type':'application/json'},
body:JSON.stringify({image:base64,type:isAnimated?'animated':'static'})
});
if(res.ok){
msg.textContent='✓ Image displayed successfully!';
msg.className='msg success';
}else{
msg.textContent='✗ Failed to display image';
msg.className='msg error';
}
};
reader.readAsDataURL(file);
}catch(err){
msg.textContent='✗ Error: '+err.message;
msg.className='msg error';
}
};
</script>
</body>
</html>`
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write([]byte(html))
		return
	}

	if r.Method == http.MethodPost {
		// Handle file upload
		err := r.ParseMultipartForm(10 << 20) // 10 MB max
		if err != nil {
			http.Error(w, "File too large", http.StatusBadRequest)
			return
		}

		file, header, err := r.FormFile("file")
		if err != nil {
			http.Error(w, "No file provided", http.StatusBadRequest)
			return
		}
		defer file.Close()

		// Read file data
		data, err := io.ReadAll(file)
		if err != nil {
			http.Error(w, "Failed to read file", http.StatusInternalServerError)
			return
		}

		// Encode to base64
		base64Data := base64.StdEncoding.EncodeToString(data)

		// Determine if animated GIF and display
		isAnimated := strings.HasSuffix(strings.ToLower(header.Filename), ".gif")
		if err := h.displayImage(base64Data, isAnimated); err != nil {
			log.Printf("Error displaying uploaded image: %v", err)
			http.Error(w, "Failed to display image", http.StatusInternalServerError)
			return
		}

		w.WriteHeader(http.StatusOK)
		json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
		return
	}

	http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
}
