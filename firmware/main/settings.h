// Settings: Kconfig defaults, overridden by what /config saved in NVS.
// Replaces the Go daemon's .env file and its .brightness file.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#define SETTINGS_VERSION 1

typedef struct {
	char wifi_ssid[33];
	char wifi_password[65];
	char hostname[32];
	uint16_t http_port;
	uint16_t panel_width, panel_height;
	bool panel_by_mac;
	char panel_name[24];
	char panel_mac[18]; // "AA:BB:CC:DD:EE:FF"
	bool upside_down;
	bool passthrough;
	bool passthrough_partial; // experimental: pass partial/transparent frames
	uint8_t stream_fps;
	uint16_t stream_colors; // at most this many per stream frame (256: as Go)
	uint16_t stream_resume_ms;
	bool debug_ble;
} settings_t;

// What changed between two settings, so the caller knows what to apply.
enum {
	SETTINGS_RESTART = 1 << 0,   // Wi-Fi, host name or port: needs a restart
	SETTINGS_RECONNECT = 1 << 1, // panel matching: reconnect the panel
};

esp_err_t settings_init(void);
const settings_t *settings_get(void);
void settings_defaults(settings_t *s);

// settings_apply validates json onto a copy of the current settings and,
// if every value is valid, saves and adopts it. Keys missing from json
// keep their value; unknown keys are listed in *unknown (a cJSON array
// the caller frees, may be NULL). On failure nothing changes and err
// describes the first bad value. *changes gets the SETTINGS_* flags.
// With import set, "version" and "brightness" are accepted too.
esp_err_t settings_apply(const cJSON *json, bool import, char *err, size_t err_len, cJSON **unknown,
			 unsigned *changes);

// settings_json returns the settings as JSON (caller deletes). With
// export set, the Wi-Fi password, the saved brightness and the format
// version are included.
cJSON *settings_json(bool export);

// Names of the settings that need a restart, for the /config page.
cJSON *settings_restart_keys(void);

esp_err_t settings_reset(void);

// Saved brightness (the Go .brightness file); 0 when none was saved.
uint8_t settings_brightness(void);
esp_err_t settings_save_brightness(uint8_t level);
