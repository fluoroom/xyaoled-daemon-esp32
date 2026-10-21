// Wi-Fi: joins the saved network; without one, or when it stays
// unreachable, opens the setup portal (an access point with a DNS server
// that answers every name with the board, so phones open /config by
// themselves) while it keeps retrying the saved network.
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

#define WIFI_AP_IP "192.168.4.1"

typedef enum { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED } wifi_state_t;

typedef struct {
	wifi_state_t state;
	char ssid[33];
	char ip[16];
	int rssi;
	bool ap_active;
	bool trying; // a /config connection attempt is running
	char reason[48];
} wifi_status_t;

esp_err_t wifi_start(void);
void wifi_status(wifi_status_t *s);
const char *wifi_state_name(wifi_state_t s);

// Called with true when the setup portal opens and false when it closes.
void wifi_set_ap_hook(void (*hook)(bool active));

// Networks in range, strongest first: [{"ssid", "rssi", "secure"}].
cJSON *wifi_scan(void);

// wifi_try connects to ssid with the access point kept up; on success
// the credentials are saved and the portal closes a few seconds later.
// The outcome shows in wifi_status.
esp_err_t wifi_try(const char *ssid, const char *password);
