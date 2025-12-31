package main

import (
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	log.Println("XYAOLED Daemon - LED Display Server")
	log.Println("====================================")

	// Load configuration
	cfg := LoadConfig()
	log.Printf("Configuration loaded:")
	log.Printf("  LED Panel MAC: %s", cfg.LEDPanelMAC)
	log.Printf("  Display Size: %dx%d", cfg.DisplayWidth, cfg.DisplayHeight)
	log.Printf("  Frame Interval: %d seconds", cfg.FrameIntervalSeconds)
	log.Printf("  Upside Down: %v", cfg.UpsideDown)
	log.Printf("  HTTP Server: %s:%s", cfg.HTTPHost, cfg.HTTPPort)
	log.Printf("  Debug BLE: %v", cfg.DebugBLE)

	// Create display
	log.Println("Connecting to LED panel...")
	display, err := NewDisplay(cfg)
	if err != nil {
		log.Fatalf("Failed to create display: %v", err)
	}
	defer display.Close()

	// Create HTTP handler
	handler := NewHandler(display, cfg)

	// Setup routes
	http.HandleFunc("/display", handler.HandleDisplay)
	http.HandleFunc("/display-r", handler.HandleDisplayReinit)
	http.HandleFunc("/health", handler.HandleHealth)
	http.HandleFunc("/upload", handler.HandleUpload)
	http.HandleFunc("/uploadTest", handler.HandleUploadTest)
	http.HandleFunc("/off", handler.HandleOff)
	http.HandleFunc("/on", handler.HandleOn)
	http.HandleFunc("/brightness/", handler.HandleBrightness)
	http.HandleFunc("/reconnect", handler.HandleReconnect)

	// Start server
	addr := cfg.HTTPHost + ":" + cfg.HTTPPort
	log.Printf("Starting HTTP server on %s", addr)
	log.Println("Endpoints:")
	log.Println("  POST /display - Display an image")
	log.Println("  POST /display-r - Display an image with reinit")
	log.Println("  GET  /health  - Health check")
	log.Println("  GET  /upload  - Upload form")
	log.Println("  POST /upload  - Upload file")
	log.Println("  GET  /uploadTest - Manual test harness")
	log.Println("  POST /uploadTest - Run scripted command")
	log.Println("  GET  /off - Dim to 5")
	log.Println("  GET  /on - Restore brightness before off")
	log.Println("  GET /brightness/{n} - Set brightness 5-125")
	log.Println("  GET /reconnect - Trigger reconnect")

	// Handle graceful shutdown
	go func() {
		if err := http.ListenAndServe(addr, nil); err != nil {
			log.Fatalf("HTTP server error: %v", err)
		}
	}()

	// Wait for interrupt signal
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)
	<-sigChan

	log.Println("\nShutting down gracefully...")
}
