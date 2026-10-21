#include "wifi.h"

#include <string.h>

#include "captive_dns.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "wifi";

#define FALLBACK_MS (120 * 1000) // saved network unreachable this long: open the portal
#define TRY_TIMEOUT_MS (20 * 1000)
#define AP_CLOSE_DELAY_MS (10 * 1000) // after success, so the page can show the address
#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000

static struct {
	esp_netif_t *sta, *ap;
	wifi_status_t st;
	void (*ap_hook)(bool);
	esp_timer_handle_t fallback, retry, ap_close, try_timeout;
	uint32_t retry_ms;
	bool scanning;
	bool mdns_up;
	char try_ssid[33], try_pass[65];
	int dns_sock;
	TaskHandle_t dns_task;
} w = {.dns_sock = -1};

const char *wifi_state_name(wifi_state_t s)
{
	static const char *names[] = {"idle", "connecting", "connected", "failed"};
	return names[s];
}

void wifi_set_ap_hook(void (*hook)(bool))
{
	w.ap_hook = hook;
}

void wifi_status(wifi_status_t *s)
{
	*s = w.st;
	wifi_ap_record_t ap;
	if (w.st.state == WIFI_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
		s->rssi = ap.rssi;
}

// ---- Captive DNS: every A query is answered with the AP address ----

static void dns_task(void *arg)
{
	uint8_t buf[512];
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		int n = recvfrom(w.dns_sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 0)
			break; // socket closed: the portal is gone
		static const uint8_t ip[4] = {192, 168, 4, 1};
		size_t len = captive_dns_answer(buf, (size_t)n, sizeof buf, ip);
		if (len == 0)
			continue;
		sendto(w.dns_sock, buf, len, 0, (struct sockaddr *)&from, fl);
	}
	w.dns_task = NULL;
	vTaskDelete(NULL);
}

static void dns_start(void)
{
	if (w.dns_sock >= 0)
		return;
	w.dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
	if (w.dns_sock < 0 || bind(w.dns_sock, (struct sockaddr *)&a, sizeof a) < 0) {
		ESP_LOGE(TAG, "captive DNS: cannot bind port 53");
		if (w.dns_sock >= 0)
			close(w.dns_sock);
		w.dns_sock = -1;
		return;
	}
	xTaskCreate(dns_task, "dns", 3072, NULL, 4, &w.dns_task);
}

static void dns_stop(void)
{
	if (w.dns_sock < 0)
		return;
	int s = w.dns_sock;
	w.dns_sock = -1;
	shutdown(s, SHUT_RDWR);
	close(s);
}

// ---- Access point ----

static void ap_open(void)
{
	if (w.st.ap_active)
		return;
	wifi_config_t ap = {
		.ap = {.ssid_len = 0, .channel = 1, .max_connection = 3, .authmode = WIFI_AUTH_WPA2_PSK,
		       .pmf_cfg = {.required = false}},
	};
	strlcpy((char *)ap.ap.ssid, CONFIG_XY_AP_SSID, sizeof ap.ap.ssid);
	strlcpy((char *)ap.ap.password, CONFIG_XY_AP_PASSWORD, sizeof ap.ap.password);
	if (strlen(CONFIG_XY_AP_PASSWORD) < 8)
		ap.ap.authmode = WIFI_AUTH_OPEN;
	esp_wifi_set_mode(WIFI_MODE_APSTA);
	esp_wifi_set_config(WIFI_IF_AP, &ap);
	w.st.ap_active = true;
	dns_start();
	ESP_LOGW(TAG, "setup portal open: join '%s' (password '%s'), then http://" WIFI_AP_IP "/", CONFIG_XY_AP_SSID,
		 CONFIG_XY_AP_PASSWORD);
	if (w.ap_hook)
		w.ap_hook(true);
}

static void ap_close(void)
{
	if (!w.st.ap_active)
		return;
	if (w.ap_hook)
		w.ap_hook(false);
	dns_stop();
	esp_wifi_set_mode(WIFI_MODE_STA);
	w.st.ap_active = false;
	ESP_LOGI(TAG, "setup portal closed");
}

static void ap_close_cb(void *arg)
{
	if (w.st.state == WIFI_CONNECTED)
		ap_close();
}

static void fallback_cb(void *arg)
{
	if (w.st.state != WIFI_CONNECTED) {
		ESP_LOGW(TAG, "'%s' unreachable for %d s", w.st.ssid, FALLBACK_MS / 1000);
		ap_open();
	}
}

// ---- Station ----

static void sta_config(const char *ssid, const char *pass)
{
	wifi_config_t c = {0};
	strlcpy((char *)c.sta.ssid, ssid, sizeof c.sta.ssid);
	strlcpy((char *)c.sta.password, pass, sizeof c.sta.password);
	c.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
	c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
	esp_wifi_set_config(WIFI_IF_STA, &c);
	strlcpy(w.st.ssid, ssid, sizeof w.st.ssid);
}

static void connect_now(void)
{
	if (!w.st.ssid[0] || w.scanning)
		return;
	w.st.state = WIFI_CONNECTING;
	esp_wifi_connect();
}

static void retry_cb(void *arg)
{
	if (w.st.state != WIFI_CONNECTED)
		connect_now();
}

static const char *reason_text(uint8_t r)
{
	switch (r) {
	case WIFI_REASON_AUTH_FAIL:
	case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
	case WIFI_REASON_HANDSHAKE_TIMEOUT:
	case WIFI_REASON_AUTH_EXPIRE:
		return "wrong password";
	case WIFI_REASON_NO_AP_FOUND:
		return "network not found";
	default:
		return "connection failed";
	}
}

// try_end finishes a /config attempt: back to the saved network on failure.
static void try_end(bool ok, const char *reason)
{
	if (!w.st.trying)
		return;
	w.st.trying = false;
	esp_timer_stop(w.try_timeout);
	if (ok) {
		cJSON *j = cJSON_CreateObject();
		cJSON_AddStringToObject(j, "wifi_ssid", w.try_ssid);
		cJSON_AddStringToObject(j, "wifi_password", w.try_pass);
		char err[64];
		if (settings_apply(j, false, err, sizeof err, NULL, NULL) != ESP_OK)
			ESP_LOGE(TAG, "could not save the network: %s", err);
		cJSON_Delete(j);
		esp_timer_stop(w.ap_close);
		esp_timer_start_once(w.ap_close, AP_CLOSE_DELAY_MS * 1000ULL);
		return;
	}
	w.st.state = WIFI_FAILED;
	strlcpy(w.st.reason, reason, sizeof w.st.reason);
	ESP_LOGW(TAG, "'%s': %s", w.try_ssid, reason);
	const settings_t *s = settings_get();
	sta_config(s->wifi_ssid, s->wifi_password);
	esp_wifi_disconnect();
	if (s->wifi_ssid[0])
		esp_timer_start_once(w.retry, RETRY_MIN_MS * 1000ULL);
}

static void try_timeout_cb(void *arg)
{
	try_end(false, "timed out");
}

static void mdns_up(void)
{
	const settings_t *s = settings_get();
	if (!w.mdns_up && mdns_init() == ESP_OK) {
		mdns_hostname_set(s->hostname);
		mdns_instance_name_set("XYAOLED");
		mdns_service_add(NULL, "_http", "_tcp", s->http_port, NULL, 0);
		w.mdns_up = true;
		ESP_LOGI(TAG, "mDNS: http://%s.local:%u", s->hostname, s->http_port);
	}
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		connect_now();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t *d = data;
		bool was_connected = w.st.state == WIFI_CONNECTED;
		w.st.ip[0] = 0;
		w.st.state = WIFI_FAILED;
		strlcpy(w.st.reason, reason_text(d->reason), sizeof w.st.reason);
		if (w.st.trying &&
		    (d->reason == WIFI_REASON_AUTH_FAIL || d->reason == WIFI_REASON_NO_AP_FOUND ||
		     d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT)) {
			try_end(false, reason_text(d->reason));
			return;
		}
		if (was_connected) {
			ESP_LOGW(TAG, "lost '%s' (reason %d)", w.st.ssid, d->reason);
			w.retry_ms = RETRY_MIN_MS;
			esp_timer_stop(w.fallback);
			esp_timer_start_once(w.fallback, FALLBACK_MS * 1000ULL);
		}
		if (w.st.trying) {
			esp_timer_stop(w.retry);
			esp_timer_start_once(w.retry, 500 * 1000ULL); // keep trying until the attempt times out
			return;
		}
		esp_timer_stop(w.retry);
		esp_timer_start_once(w.retry, (uint64_t)w.retry_ms * 1000);
		w.retry_ms = w.retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : w.retry_ms * 2;
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *e = data;
		snprintf(w.st.ip, sizeof w.st.ip, IPSTR, IP2STR(&e->ip_info.ip));
		w.st.state = WIFI_CONNECTED;
		w.st.reason[0] = 0;
		w.retry_ms = RETRY_MIN_MS;
		esp_timer_stop(w.fallback);
		ESP_LOGI(TAG, "connected to '%s', IP %s", w.st.ssid, w.st.ip);
		mdns_up();
		if (w.st.trying) {
			try_end(true, NULL);
		} else if (w.st.ap_active) {
			esp_timer_stop(w.ap_close);
			esp_timer_start_once(w.ap_close, AP_CLOSE_DELAY_MS * 1000ULL);
		}
	}
}

esp_err_t wifi_start(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	w.sta = esp_netif_create_default_wifi_sta();
	w.ap = esp_netif_create_default_wifi_ap();
	const settings_t *s = settings_get();
	esp_netif_set_hostname(w.sta, s->hostname);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	esp_wifi_set_storage(WIFI_STORAGE_RAM); // credentials live in our settings
	esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

	esp_timer_create_args_t t = {.callback = fallback_cb, .name = "wifi_fallback"};
	esp_timer_create(&t, &w.fallback);
	t.callback = retry_cb, t.name = "wifi_retry";
	esp_timer_create(&t, &w.retry);
	t.callback = ap_close_cb, t.name = "wifi_ap_close";
	esp_timer_create(&t, &w.ap_close);
	t.callback = try_timeout_cb, t.name = "wifi_try";
	esp_timer_create(&t, &w.try_timeout);
	w.retry_ms = RETRY_MIN_MS;

	esp_wifi_set_mode(WIFI_MODE_STA);
	if (s->wifi_ssid[0])
		sta_config(s->wifi_ssid, s->wifi_password);
	ESP_ERROR_CHECK(esp_wifi_start());
	if (s->wifi_ssid[0]) {
		ESP_LOGI(TAG, "joining '%s'", s->wifi_ssid);
		esp_timer_start_once(w.fallback, FALLBACK_MS * 1000ULL);
	} else {
		ESP_LOGW(TAG, "no network configured");
		ap_open();
	}
	return ESP_OK;
}

static int by_rssi(const void *a, const void *b)
{
	return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

cJSON *wifi_scan(void)
{
	cJSON *list = cJSON_CreateArray();
	// A scan cannot run while the station is busy connecting.
	w.scanning = true;
	esp_timer_stop(w.retry);
	if (w.st.state != WIFI_CONNECTED && !w.st.trying)
		esp_wifi_disconnect();
	wifi_scan_config_t sc = {.show_hidden = false};
	if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
		uint16_t n = 20;
		wifi_ap_record_t *recs = calloc(n, sizeof *recs);
		if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
			qsort(recs, n, sizeof *recs, by_rssi);
			for (uint16_t i = 0; i < n; i++) {
				if (!recs[i].ssid[0])
					continue;
				bool dup = false;
				for (uint16_t k = 0; k < i; k++)
					dup |= strcmp((char *)recs[k].ssid, (char *)recs[i].ssid) == 0;
				if (dup)
					continue;
				cJSON *o = cJSON_CreateObject();
				cJSON_AddStringToObject(o, "ssid", (char *)recs[i].ssid);
				cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
				cJSON_AddBoolToObject(o, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
				cJSON_AddItemToArray(list, o);
			}
		}
		free(recs);
	} else {
		esp_wifi_clear_ap_list();
	}
	w.scanning = false;
	if (w.st.state != WIFI_CONNECTED)
		esp_timer_start_once(w.retry, RETRY_MIN_MS * 1000ULL);
	return list;
}

esp_err_t wifi_try(const char *ssid, const char *password)
{
	if (!ssid[0] || strlen(ssid) > 32 || strlen(password) > 63 || (password[0] && strlen(password) < 8))
		return ESP_ERR_INVALID_ARG;
	strlcpy(w.try_ssid, ssid, sizeof w.try_ssid);
	strlcpy(w.try_pass, password, sizeof w.try_pass);
	w.st.trying = true;
	w.st.reason[0] = 0;
	esp_timer_stop(w.retry);
	esp_timer_stop(w.ap_close);
	esp_wifi_disconnect();
	sta_config(ssid, password);
	w.st.state = WIFI_CONNECTING;
	esp_wifi_connect();
	esp_timer_stop(w.try_timeout);
	esp_timer_start_once(w.try_timeout, TRY_TIMEOUT_MS * 1000ULL);
	ESP_LOGI(TAG, "trying '%s'", ssid);
	return ESP_OK;
}
