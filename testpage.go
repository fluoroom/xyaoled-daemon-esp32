package main

import (
	_ "embed"
	"net/http"
)

//go:embed testpage.html
var testPageHTML []byte

// HandleTestPage serves the manual test page (connect, send, brightness).
func (h *Handler) HandleTestPage(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(testPageHTML)
}
