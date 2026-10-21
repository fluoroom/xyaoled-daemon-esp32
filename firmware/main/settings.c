#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";

#define NS "xyaoled"
#define KEY_CONFIG "config"
#define KEY_BRIGHTNESS "brightness"

static settings_t current;
static uint8_t brightness;
static SemaphoreHandle_t lock;

typedef enum { T_STR, T_U16, T_U8, T_BOOL, T_MATCH, T_SECONDS } field_type_t;

typedef struct {
	const char *key;
	field_type_t type;
	size_t offset, size; // size: string buffer size
	long min, max;       // numbers; strings: length
	unsigned changes;    // SETTINGS_* flags when it changes
	bool secret;         // only in exports
	bool (*valid)(const char *s);
} field_t;

static bool valid_hostname(const char *s)
{
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '-')
			return false;
	return true;
}

static bool valid_mac(const char *s)
{
	unsigned b[6];
	char end;
	return s[0] == 0 || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &end) == 6;
}

static bool valid_password(const char *s)
{
	size_t n = strlen(s);
	return n == 0 || (n >= 8 && n <= 63);
}

#define F(k, t, field, mn, mx, ch) \
	{k, t, offsetof(settings_t, field), sizeof(((settings_t *)0)->field), mn, mx, ch, false, NULL}

static const field_t fields[] = {
	F("wifi_ssid", T_STR, wifi_ssid, 0, 32, SETTINGS_RESTART),
	{"wifi_password", T_STR, offsetof(settings_t, wifi_password), sizeof current.wifi_password, 0, 63,
	 SETTINGS_RESTART, true, valid_password},
	{"hostname", T_STR, offsetof(settings_t, hostname), sizeof current.hostname, 1, 31, SETTINGS_RESTART, false,
	 valid_hostname},
	F("http_port", T_U16, http_port, 1, 65535, SETTINGS_RESTART),
	F("panel_width", T_U16, panel_width, 1, 256, 0),
	F("panel_height", T_U16, panel_height, 1, 64, 0),
	F("panel_match", T_MATCH, panel_by_mac, 0, 0, SETTINGS_RECONNECT),
	F("panel_name", T_STR, panel_name, 1, 23, SETTINGS_RECONNECT),
	{"panel_mac", T_STR, offsetof(settings_t, panel_mac), sizeof current.panel_mac, 0, 17, SETTINGS_RECONNECT,
	 false, valid_mac},
	F("upside_down", T_BOOL, upside_down, 0, 1, 0),
	F("passthrough", T_BOOL, passthrough, 0, 1, 0),
	F("stream_fps", T_U8, stream_fps, 1, 5, 0),
	F("stream_resume_delay", T_SECONDS, stream_resume_ms, 0, 60000, 0),
	F("debug_ble", T_BOOL, debug_ble, 0, 1, 0),
};

void settings_defaults(settings_t *s)
{
	memset(s, 0, sizeof *s);
	strlcpy(s->wifi_ssid, CONFIG_XY_WIFI_SSID, sizeof s->wifi_ssid);
	strlcpy(s->wifi_password, CONFIG_XY_WIFI_PASSWORD, sizeof s->wifi_password);
	strlcpy(s->hostname, CONFIG_XY_HOSTNAME, sizeof s->hostname);
	s->http_port = CONFIG_XY_HTTP_PORT;
	s->panel_width = CONFIG_XY_PANEL_WIDTH;
	s->panel_height = CONFIG_XY_PANEL_HEIGHT;
	strlcpy(s->panel_name, CONFIG_XY_PANEL_NAME, sizeof s->panel_name);
	s->passthrough = true;
	s->stream_fps = CONFIG_XY_STREAM_FPS;
	s->stream_resume_ms = CONFIG_XY_STREAM_RESUME_MS;
}

// set_field validates one JSON value and stores it into s.
static bool set_field(settings_t *s, const field_t *f, const cJSON *v, char *err, size_t err_len)
{
	uint8_t *p = (uint8_t *)s + f->offset;
	switch (f->type) {
	case T_STR: {
		if (!cJSON_IsString(v)) {
			snprintf(err, err_len, "%s must be a string", f->key);
			return false;
		}
		size_t n = strlen(v->valuestring);
		if ((long)n < f->min || (long)n > f->max || (f->valid && !f->valid(v->valuestring))) {
			snprintf(err, err_len, "%s: invalid value", f->key);
			return false;
		}
		memcpy(p, v->valuestring, n + 1);
		return true;
	}
	case T_BOOL:
		if (!cJSON_IsBool(v)) {
			snprintf(err, err_len, "%s must be true or false", f->key);
			return false;
		}
		*(bool *)p = cJSON_IsTrue(v);
		return true;
	case T_MATCH:
		if (!cJSON_IsString(v) || (strcmp(v->valuestring, "name") && strcmp(v->valuestring, "mac"))) {
			snprintf(err, err_len, "%s must be \"name\" or \"mac\"", f->key);
			return false;
		}
		*(bool *)p = strcmp(v->valuestring, "mac") == 0;
		return true;
	default: {
		if (!cJSON_IsNumber(v)) {
			snprintf(err, err_len, "%s must be a number", f->key);
			return false;
		}
		double d = f->type == T_SECONDS ? v->valuedouble * 1000.0 : v->valuedouble;
		if (d < f->min || d > f->max || (f->type != T_SECONDS && d != (long)d)) {
			if (f->type == T_SECONDS)
				snprintf(err, err_len, "%s must be %g to %g", f->key, f->min / 1000.0, f->max / 1000.0);
			else
				snprintf(err, err_len, "%s must be %ld to %ld", f->key, f->min, f->max);
			return false;
		}
		if (f->type == T_U8)
			*p = (uint8_t)d;
		else
			*(uint16_t *)p = (uint16_t)(d + 0.5);
		return true;
	}
	}
}

static cJSON *field_json(const settings_t *s, const field_t *f)
{
	const uint8_t *p = (const uint8_t *)s + f->offset;
	switch (f->type) {
	case T_STR:
		return cJSON_CreateString((const char *)p);
	case T_BOOL:
		return cJSON_CreateBool(*(const bool *)p);
	case T_MATCH:
		return cJSON_CreateString(*(const bool *)p ? "mac" : "name");
	case T_U8:
		return cJSON_CreateNumber(*p);
	case T_U16:
		return cJSON_CreateNumber(*(const uint16_t *)p);
	case T_SECONDS:
		return cJSON_CreateNumber(*(const uint16_t *)p / 1000.0);
	}
	return cJSON_CreateNull();
}

static cJSON *to_json(const settings_t *s, bool secrets)
{
	cJSON *o = cJSON_CreateObject();
	for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
		if (secrets || !fields[i].secret)
			cJSON_AddItemToObject(o, fields[i].key, field_json(s, &fields[i]));
	return o;
}

static esp_err_t save(const settings_t *s)
{
	cJSON *o = to_json(s, true);
	char *text = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (!text)
		return ESP_ERR_NO_MEM;
	nvs_handle_t h;
	esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
	if (err == ESP_OK) {
		// One key holds every setting, so a save is all or nothing.
		err = nvs_set_str(h, KEY_CONFIG, text);
		if (err == ESP_OK)
			err = nvs_commit(h);
		nvs_close(h);
	}
	free(text);
	return err;
}

// merge applies json onto s; false (and err) on the first invalid value.
static bool merge(settings_t *s, const cJSON *json, bool import, char *err, size_t err_len, cJSON *unknown,
		  int *new_brightness)
{
	const cJSON *item;
	cJSON_ArrayForEach(item, json)
	{
		const field_t *f = NULL;
		for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
			if (strcmp(item->string, fields[i].key) == 0)
				f = &fields[i];
		if (f) {
			if (!set_field(s, f, item, err, err_len))
				return false;
		} else if (import && strcmp(item->string, "version") == 0) {
			if (!cJSON_IsNumber(item) || item->valueint < 1 || item->valueint > SETTINGS_VERSION) {
				snprintf(err, err_len, "unsupported config version");
				return false;
			}
		} else if (import && strcmp(item->string, "brightness") == 0) {
			if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 125) {
				snprintf(err, err_len, "brightness must be 0 to 125");
				return false;
			}
			*new_brightness = item->valueint;
		} else if (unknown) {
			cJSON_AddItemToArray(unknown, cJSON_CreateString(item->string));
		}
	}
	return true;
}

esp_err_t settings_init(void)
{
	lock = xSemaphoreCreateMutex();
	settings_defaults(&current);
	nvs_handle_t h;
	if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
		return ESP_OK; // nothing saved yet
	size_t len = 0;
	if (nvs_get_str(h, KEY_CONFIG, NULL, &len) == ESP_OK && len > 0) {
		char *text = malloc(len);
		if (text && nvs_get_str(h, KEY_CONFIG, text, &len) == ESP_OK) {
			cJSON *json = cJSON_Parse(text);
			settings_t s = current;
			char err[64];
			int b = -1;
			if (json && merge(&s, json, false, err, sizeof err, NULL, &b))
				current = s;
			else
				ESP_LOGE(TAG, "saved settings invalid (%s), using defaults", json ? err : "not JSON");
			cJSON_Delete(json);
		}
		free(text);
	}
	nvs_get_u8(h, KEY_BRIGHTNESS, &brightness);
	nvs_close(h);
	return ESP_OK;
}

const settings_t *settings_get(void)
{
	return &current;
}

esp_err_t settings_apply(const cJSON *json, bool import, char *err, size_t err_len, cJSON **unknown,
			 unsigned *changes)
{
	if (!cJSON_IsObject(json)) {
		snprintf(err, err_len, "expected a JSON object");
		return ESP_ERR_INVALID_ARG;
	}
	if (unknown)
		*unknown = cJSON_CreateArray();
	xSemaphoreTake(lock, portMAX_DELAY);
	settings_t s = current;
	int b = -1;
	esp_err_t res = ESP_ERR_INVALID_ARG;
	if (merge(&s, json, import, err, err_len, unknown ? *unknown : NULL, &b)) {
		unsigned ch = 0;
		for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
			const field_t *f = &fields[i];
			if (memcmp((uint8_t *)&s + f->offset, (uint8_t *)&current + f->offset, f->size) != 0)
				ch |= f->changes;
		}
		if ((res = save(&s)) == ESP_OK) {
			current = s;
			if (changes)
				*changes = ch;
			if (b >= 0)
				settings_save_brightness((uint8_t)b);
		} else {
			snprintf(err, err_len, "could not save: %s", esp_err_to_name(res));
		}
	}
	xSemaphoreGive(lock);
	return res;
}

cJSON *settings_json(bool export)
{
	xSemaphoreTake(lock, portMAX_DELAY);
	cJSON *o = to_json(&current, export);
	xSemaphoreGive(lock);
	if (export) {
		cJSON_AddNumberToObject(o, "version", SETTINGS_VERSION);
		cJSON_AddNumberToObject(o, "brightness", brightness);
	}
	return o;
}

cJSON *settings_restart_keys(void)
{
	cJSON *a = cJSON_CreateArray();
	for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
		if (fields[i].changes & SETTINGS_RESTART)
			cJSON_AddItemToArray(a, cJSON_CreateString(fields[i].key));
	return a;
}

esp_err_t settings_reset(void)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
	if (err != ESP_OK)
		return err;
	err = nvs_erase_all(h);
	if (err == ESP_OK)
		err = nvs_commit(h);
	nvs_close(h);
	xSemaphoreTake(lock, portMAX_DELAY);
	settings_defaults(&current);
	brightness = 0;
	xSemaphoreGive(lock);
	return err;
}

uint8_t settings_brightness(void)
{
	return brightness;
}

esp_err_t settings_save_brightness(uint8_t level)
{
	if (level == brightness)
		return ESP_OK;
	nvs_handle_t h;
	esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
	if (err != ESP_OK)
		return err;
	err = nvs_set_u8(h, KEY_BRIGHTNESS, level);
	if (err == ESP_OK)
		err = nvs_commit(h);
	nvs_close(h);
	if (err == ESP_OK)
		brightness = level;
	return err;
}
