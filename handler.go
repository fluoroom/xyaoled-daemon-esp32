package main

import (
	"encoding/json"
	"log"
	"net/http"
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
	var displayErr error
	if req.Type == "animated" {
		displayErr = h.display.ShowAnimatedGif(req.ImageData)
	} else {
		displayErr = h.display.ShowImage(req.ImageData)
	}

	if displayErr != nil {
		log.Printf("Error displaying image: %v", displayErr)
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
