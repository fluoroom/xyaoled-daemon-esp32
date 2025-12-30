package main

import (
	"encoding/base64"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"strings"
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

// displayImage handles the actual display logic (shared by multiple endpoints)
func (h *Handler) displayImage(base64Data string, isAnimated bool) error {
	if isAnimated {
		return h.display.ShowAnimatedGif(base64Data)
	}
	return h.display.ShowImage(base64Data)
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
