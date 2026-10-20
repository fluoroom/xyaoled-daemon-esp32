package main

import (
	"bufio"
	"log"
	"os"
	"strconv"
	"strings"
	"time"
)

// Config holds all daemon configuration
type Config struct {
	// LED Panel settings
	LEDPanelMAC   string
	DisplayWidth  int
	DisplayHeight int

	// Server settings
	HTTPPort string
	HTTPHost string

	// Debug settings
	DebugBLE bool

	// Display orientation
	UpsideDown bool

	// Highest rate at which /stream frames are sent over Bluetooth
	StreamFPS int
	// How long after the last /stream client disconnects the playlist resumes
	StreamResumeDelay time.Duration
}

// LoadConfig loads configuration from .env file and environment variables
// (environment wins, for Docker).
func LoadConfig() *Config {
	cfg := &Config{
		LEDPanelMAC:   "AA:BB:CC:DD:EE:FF",
		DisplayWidth:  96,
		DisplayHeight: 16,
		HTTPPort:      "11001",
		HTTPHost:      "0.0.0.0",
		StreamFPS:     4,

		StreamResumeDelay: 1500 * time.Millisecond,
	}

	// Try to load .env, fall back to .env.example
	envFile := ".env"
	if _, err := os.Stat(envFile); os.IsNotExist(err) || os.IsPermission(err) {
		envFile = ".env.example"
	}

	file, err := os.Open(envFile)
	if err != nil {
		log.Printf("Warning: could not open %s, using defaults: %v", envFile, err)
	} else {
		defer file.Close()
		scanner := bufio.NewScanner(file)
		for scanner.Scan() {
			line := strings.TrimSpace(scanner.Text())
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			parts := strings.SplitN(line, "=", 2)
			if len(parts) != 2 {
				continue
			}
			cfg.set(strings.TrimSpace(parts[0]), strings.TrimSpace(parts[1]))
		}
	}

	for _, key := range []string{"LED_PANEL_MAC", "LED_DISPLAY_WIDTH", "LED_DISPLAY_HEIGHT", "HTTP_PORT", "HTTP_HOST", "DEBUG_BLE", "UPSIDE_DOWN", "STREAM_FPS", "STREAM_RESUME_DELAY"} {
		if val := os.Getenv(key); val != "" {
			cfg.set(key, val)
		}
	}

	return cfg
}

// set applies one configuration key; unknown keys are ignored.
func (cfg *Config) set(key, value string) {
	switch key {
	case "LED_PANEL_MAC":
		cfg.LEDPanelMAC = value
	case "LED_DISPLAY_WIDTH":
		if v, err := strconv.Atoi(value); err == nil {
			cfg.DisplayWidth = v
		}
	case "LED_DISPLAY_HEIGHT":
		if v, err := strconv.Atoi(value); err == nil {
			cfg.DisplayHeight = v
		}
	case "HTTP_PORT":
		cfg.HTTPPort = value
	case "HTTP_HOST":
		cfg.HTTPHost = value
	case "DEBUG_BLE":
		cfg.DebugBLE = value == "true"
	case "UPSIDE_DOWN":
		cfg.UpsideDown = value == "true"
	case "STREAM_FPS":
		if v, err := strconv.Atoi(value); err == nil && v > 0 {
			cfg.StreamFPS = min(v, maxStreamFPS)
		}
	case "STREAM_RESUME_DELAY":
		if v, err := strconv.ParseFloat(value, 64); err == nil && v >= 0 {
			cfg.StreamResumeDelay = time.Duration(v * float64(time.Second))
		}
	}
}
