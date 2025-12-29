package main

import (
	"bufio"
	"log"
	"os"
	"strconv"
	"strings"
)

// Config holds all daemon configuration
type Config struct {
	// LED Panel settings
	LEDPanelMAC   string
	DisplayWidth  int
	DisplayHeight int
	FPS           int

	// Server settings
	HTTPPort string
	HTTPHost string

	// Debug settings
	DebugBLE       bool
	VerboseLogging bool
}

// LoadConfig loads configuration from .env file
func LoadConfig() *Config {
	cfg := &Config{
		LEDPanelMAC:    "AA:BB:CC:DD:EE:FF",
		DisplayWidth:   96,
		DisplayHeight:  16,
		FPS:            1,
		HTTPPort:       "8080",
		HTTPHost:       "0.0.0.0",
		DebugBLE:       false,
		VerboseLogging: false,
	}

	// Try to load .env, fall back to .env.example
	envFile := ".env"
	if _, err := os.Stat(envFile); os.IsNotExist(err) || os.IsPermission(err) {
		envFile = ".env.example"
	}

	file, err := os.Open(envFile)
	if err != nil {
		log.Printf("Warning: could not open %s, using defaults: %v", envFile, err)
		return cfg
	}
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

		key := strings.TrimSpace(parts[0])
		value := strings.TrimSpace(parts[1])

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
		case "LED_FPS":
			if v, err := strconv.Atoi(value); err == nil {
				cfg.FPS = v
			}
		case "HTTP_PORT":
			cfg.HTTPPort = value
		case "HTTP_HOST":
			cfg.HTTPHost = value
		case "DEBUG_BLE":
			cfg.DebugBLE = value == "true"
		case "VERBOSE_LOGGING":
			cfg.VerboseLogging = value == "true"
		}
	}

	return cfg
}
