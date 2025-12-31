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
	LEDPanelMAC     string
	DisplayWidth    int
	DisplayHeight   int
	SkipClear       bool
	BestEffortClear bool
	Tests           bool

	// Server settings
	HTTPPort string
	HTTPHost string

	// Debug settings
	DebugBLE       bool
	VerboseLogging bool

	// Display orientation
	UpsideDown bool

	// Reset settings
	ResetInterval int // Send reset every N sends (0 = disabled)
	// Whether to use ATT-sized chunking, pacing and pauses when sending GIFs
	ChunkLimits bool
}

// LoadConfig loads configuration from .env file and environment variables
func LoadConfig() *Config {
	cfg := &Config{
		LEDPanelMAC:     "AA:BB:CC:DD:EE:FF",
		DisplayWidth:    96,
		DisplayHeight:   16,
		SkipClear:       false,
		BestEffortClear: false,
		Tests:           false,
		HTTPPort:        "11001",
		HTTPHost:        "0.0.0.0",
		DebugBLE:        false,
		VerboseLogging:  false,
		UpsideDown:      false,
		ResetInterval:   0,
		ChunkLimits:     true,
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
			case "SKIP_CLEAR":
				cfg.SkipClear = strings.EqualFold(value, "true")
			case "BEST_EFFORT_CLEAR":
				cfg.BestEffortClear = strings.EqualFold(value, "true")
			case "TESTS":
				cfg.Tests = strings.EqualFold(value, "true")
			case "HTTP_PORT":
				cfg.HTTPPort = value
			case "HTTP_HOST":
				cfg.HTTPHost = value
			case "DEBUG_BLE":
				cfg.DebugBLE = value == "true"
			case "VERBOSE_LOGGING":
				cfg.VerboseLogging = value == "true"
			case "UPSIDE_DOWN":
				cfg.UpsideDown = value == "true"
			case "RESET_INTERVAL":
				if v, err := strconv.Atoi(value); err == nil {
					cfg.ResetInterval = v
				}
			case "CHUNK_LIMITS":
				cfg.ChunkLimits = strings.EqualFold(value, "true")
			}
		}
	}

	// Override with environment variables (for Docker)
	if val := os.Getenv("LED_PANEL_MAC"); val != "" {
		cfg.LEDPanelMAC = val
	}
	if val := os.Getenv("LED_DISPLAY_WIDTH"); val != "" {
		if v, err := strconv.Atoi(val); err == nil {
			cfg.DisplayWidth = v
		}
	}
	if val := os.Getenv("LED_DISPLAY_HEIGHT"); val != "" {
		if v, err := strconv.Atoi(val); err == nil {
			cfg.DisplayHeight = v
		}
	}
	if val := os.Getenv("SKIP_CLEAR"); val != "" {
		cfg.SkipClear = strings.EqualFold(val, "true")
	}
	if val := os.Getenv("BEST_EFFORT_CLEAR"); val != "" {
		cfg.BestEffortClear = strings.EqualFold(val, "true")
	}
	if val := os.Getenv("TESTS"); val != "" {
		cfg.Tests = strings.EqualFold(val, "true")
	}
	if val := os.Getenv("HTTP_PORT"); val != "" {
		cfg.HTTPPort = val
	}
	if val := os.Getenv("HTTP_HOST"); val != "" {
		cfg.HTTPHost = val
	}
	if val := os.Getenv("DEBUG_BLE"); val != "" {
		cfg.DebugBLE = val == "true"
	}
	if val := os.Getenv("VERBOSE_LOGGING"); val != "" {
		cfg.VerboseLogging = val == "true"
	}
	if val := os.Getenv("UPSIDE_DOWN"); val != "" {
		cfg.UpsideDown = val == "true"
	}
	if val := os.Getenv("RESET_INTERVAL"); val != "" {
		if v, err := strconv.Atoi(val); err == nil {
			cfg.ResetInterval = v
		}
	}
	if val := os.Getenv("CHUNK_LIMITS"); val != "" {
		cfg.ChunkLimits = strings.EqualFold(val, "true")
	}

	return cfg
}
