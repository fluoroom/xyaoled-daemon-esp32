// HTTP API: the endpoints of reference/go-daemon/handler.go, with the same
// paths, fields and status codes, plus /config and its /api/* endpoints.
// Request bodies are parsed as they arrive and never held whole in RAM.
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jsonb64.h"
#include "multipart.h"
#include "settings.h"
#include "storage.h"
#include "stream.h"
#include "wifi.h"
#include "xy_panel.h"
#if CONFIG_XY_QEMU
#include "xy_panel_sim.h"
#endif

static const char *TAG = "http";

#define API_BODY_MAX 4096
#define FIELD_MAX 256

static httpd_handle_t server, portal;
static uint8_t last_on_brightness = 125; // the level /on restores

extern const uint8_t testpage_start[] asm("_binary_test_html_start");
extern const uint8_t testpage_end[] asm("_binary_test_html_end");
extern const uint8_t configpage_start[] asm("_binary_config_html_start");
extern const uint8_t configpage_end[] asm("_binary_config_html_end");

// ---- Responses ----

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *o)
{
	char *text = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (status)
		httpd_resp_set_status(req, status);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, text ? text : "{}");
	free(text);
	return ESP_OK;
}

static cJSON *ok_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "status", "ok");
	return o;
}

static esp_err_t write_ok(httpd_req_t *req)
{
	return send_json(req, NULL, ok_json());
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *msg)
{
	httpd_resp_set_status(req, status);
	httpd_resp_set_type(req, "text/plain; charset=utf-8");
	httpd_resp_sendstr(req, msg);
	return ESP_OK;
}

// send_error mirrors the Go sendError: 409 while a stream holds the panel,
// 503 when the panel is not connected, otherwise 500 with msg (the error's
// own text when msg is NULL).
static esp_err_t send_error(httpd_req_t *req, esp_err_t err, const char *msg)
{
	if (err == DISPLAY_ERR_STREAMING)
		return send_text(req, "409 Conflict", display_error_text(err));
	if (err == XY_ERR_NOT_CONNECTED)
		return send_text(req, "503 Service Unavailable", "Device not ready");
	char text[192];
	if (msg)
		snprintf(text, sizeof text, "%s: %s", msg, display_error_text(err));
	else
		snprintf(text, sizeof text, "%s", display_error_text(err));
	ESP_LOGW(TAG, "%s %s: %s", req->method == HTTP_POST ? "POST" : "GET", req->uri, text);
	return send_text(req, "500 Internal Server Error", text);
}

static esp_err_t redirect(httpd_req_t *req, const char *to)
{
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", to);
	return httpd_resp_send(req, NULL, 0);
}

// ---- Request bodies ----

typedef struct {
	httpd_req_t *req;
	size_t left;
} body_t;

static int body_read(void *ctx, uint8_t *buf, size_t len)
{
	body_t *b = ctx;
	if (b->left == 0)
		return 0;
	if (len > b->left)
		len = b->left;
	for (int tries = 0; tries < 5; tries++) {
		int n = httpd_req_recv(b->req, (char *)buf, len);
		if (n == HTTPD_SOCK_ERR_TIMEOUT)
			continue;
		if (n <= 0)
			return -1;
		b->left -= (size_t)n;
		return n;
	}
	return -1;
}

static int body_src(void *ctx, uint8_t *buf, size_t len)
{
	return body_read(ctx, buf, len);
}

// read_small reads a whole small body (API JSON); NULL when too large.
static char *read_small(httpd_req_t *req)
{
	if (req->content_len > API_BODY_MAX)
		return NULL;
	char *buf = malloc(req->content_len + 1);
	body_t b = {req, req->content_len};
	size_t n = 0;
	int r;
	while (buf && (r = body_read(&b, (uint8_t *)buf + n, req->content_len - n)) > 0)
		n += (size_t)r;
	if (!buf || n != req->content_len) {
		free(buf);
		return NULL;
	}
	buf[n] = 0;
	return buf;
}

static int part_read(void *ctx, uint8_t *buf, size_t len)
{
	return mp_read(ctx, buf, len);
}

// Multipart request: the parser reads from the body.
typedef struct {
	body_t body;
	mp_t mp;
} form_t;

static form_t *form_open(httpd_req_t *req)
{
	char ct[160], boundary[MP_BOUNDARY_MAX + 1];
	if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct) != ESP_OK ||
	    !mp_boundary(ct, boundary, sizeof boundary))
		return NULL;
	form_t *f = malloc(sizeof *f);
	if (!f)
		return NULL;
	f->body = (body_t){req, req->content_len};
	mp_init(&f->mp, boundary, body_src, &f->body);
	return f;
}

static void read_field(mp_t *mp, char *out, size_t cap)
{
	size_t n = 0;
	int r;
	while ((r = mp_read(mp, (uint8_t *)out + n, cap - 1 - n)) > 0)
		n += (size_t)r;
	out[n] = 0; // the rest of an overlong value is skipped with the part
}

// ---- Pages ----

static esp_err_t page(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	// Revalidate: a page from before a firmware update must not linger.
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t h_test(httpd_req_t *req)
{
	return page(req, testpage_start, testpage_end);
}

static esp_err_t h_config_page(httpd_req_t *req)
{
	return page(req, configpage_start, configpage_end);
}

static esp_err_t h_to_test(httpd_req_t *req)
{
	return redirect(req, "/test");
}

static esp_err_t h_root(httpd_req_t *req)
{
	return redirect(req, "/config");
}

// ---- Image endpoints ----

// /display, /display-r: {"image": "<base64>", "type": "static"|"animated"}.
typedef struct {
	body_t body;
	jb_t jb;
	uint8_t in[768], out[768 + JB_OUT_SLACK];
	size_t in_pos, in_len, out_pos, out_len;
	bool body_error;
} display_body_t;

static int display_read(void *ctx, uint8_t *buf, size_t len)
{
	display_body_t *d = ctx;
	while (d->out_pos == d->out_len) {
		if (d->jb.image_done || d->jb.err || d->jb.done)
			return d->jb.err ? -1 : 0;
		if (d->in_pos == d->in_len) {
			int n = body_read(&d->body, d->in, sizeof d->in);
			if (n <= 0) {
				d->body_error = n < 0;
				return n < 0 ? -1 : 0;
			}
			d->in_pos = 0;
			d->in_len = (size_t)n;
		}
		size_t used;
		int w = jb_feed(&d->jb, d->in + d->in_pos, d->in_len - d->in_pos, d->out, &used);
		if (w < 0)
			return -1;
		d->in_pos += used;
		d->out_pos = 0;
		d->out_len = (size_t)w;
	}
	size_t k = d->out_len - d->out_pos < len ? d->out_len - d->out_pos : len;
	memcpy(buf, d->out + d->out_pos, k);
	d->out_pos += k;
	return (int)k;
}

// display_finish parses the rest of the body (a "type" after the image).
static void display_finish(display_body_t *d)
{
	for (;;) {
		if (d->in_pos == d->in_len) {
			int n = body_read(&d->body, d->in, sizeof d->in);
			if (n <= 0)
				return;
			d->in_pos = 0;
			d->in_len = (size_t)n;
		}
		size_t used;
		if (jb_feed(&d->jb, d->in + d->in_pos, d->in_len - d->in_pos, d->out, &used) < 0 || d->jb.done)
			return;
		d->in_pos += used;
	}
}

static esp_err_t h_display(httpd_req_t *req)
{
	display_body_t *d = calloc(1, sizeof *d);
	if (!d)
		return send_text(req, "500 Internal Server Error", "out of memory");
	d->body = (body_t){req, req->content_len};
	jb_init(&d->jb);
	img_reader_t r = {display_read, d};

	// The type may come before the image; read up to the image first.
	uint8_t first;
	int n = display_read(d, &first, 1);
	esp_err_t res = ESP_OK;
	if (n < 0 || d->jb.err == JB_ERR_JSON) {
		res = send_text(req, "400 Bad Request", d->jb.err == JB_ERR_BASE64 ? "Invalid base64 image data"
										    : "Invalid JSON");
		goto out;
	}
	if (n == 0) {
		display_finish(d);
		res = send_text(req, "400 Bad Request", d->jb.err ? "Invalid JSON" : "Missing image data");
		goto out;
	}
	// Put the byte back in front of the stream.
	d->out_pos--;
	const char *type = jb_type(&d->jb);
	display_kind_t kind = strcmp(type, "animated") == 0 ? DISPLAY_ANIMATED
			      : strcmp(type, "static") == 0 ? DISPLAY_STATIC
							    : DISPLAY_AUTO;
	esp_err_t err = display_show(&r, kind);
	display_finish(d);
	if (d->jb.err == JB_ERR_BASE64)
		res = send_text(req, "400 Bad Request", "Invalid base64 image data");
	else if (err != ESP_OK)
		res = send_error(req, err, "Failed to display image");
	else
		res = write_ok(req);
out:
	free(d);
	return res;
}

// form_file streams the multipart part called field to fn.
static esp_err_t form_file(httpd_req_t *req, const char *field, esp_err_t (*fn)(const img_reader_t *r),
			   const char *fail_msg, const char *missing_msg)
{
	form_t *f = form_open(req);
	if (!f)
		return send_text(req, "400 Bad Request", "Invalid form");
	mp_part_t part;
	int pr;
	esp_err_t res = ESP_FAIL, err;
	while ((pr = mp_next_part(&f->mp, &part)) == 1) {
		if (strcmp(part.name, field) != 0)
			continue;
		img_reader_t r = {part_read, &f->mp};
		int64_t start = esp_timer_get_time();
		err = fn(&r);
		if (err != ESP_OK) {
			res = send_error(req, err, fail_msg);
		} else {
			cJSON *o = ok_json();
			if (fn == display_live)
				cJSON_AddNumberToObject(o, "ms", (double)((esp_timer_get_time() - start) / 1000));
			res = send_json(req, NULL, o);
		}
		free(f);
		return res;
	}
	free(f);
	return send_text(req, "400 Bad Request", pr < 0 ? "Invalid form" : missing_msg);
}

static esp_err_t show_auto(const img_reader_t *r)
{
	return display_show(r, DISPLAY_AUTO);
}

static esp_err_t show_animated(const img_reader_t *r)
{
	return display_show(r, DISPLAY_ANIMATED);
}

static esp_err_t h_upload(httpd_req_t *req)
{
	return form_file(req, "file", show_auto, "Failed to display image", "No file provided");
}

static esp_err_t h_upload_test_gif(httpd_req_t *req)
{
	return form_file(req, "gif", show_animated, NULL, "No file uploaded");
}

static esp_err_t h_live(httpd_req_t *req)
{
	char ct[64] = "";
	httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct);
	if (strncmp(ct, "multipart/", 10) == 0)
		return form_file(req, "file", display_live, NULL, "No image provided");
	if (req->content_len == 0)
		return send_text(req, "400 Bad Request", "No image provided");
	body_t b = {req, req->content_len};
	img_reader_t r = {body_read, &b};
	int64_t start = esp_timer_get_time();
	esp_err_t err = display_live(&r);
	if (err != ESP_OK)
		return send_text(req, "500 Internal Server Error", display_error_text(err));
	cJSON *o = ok_json();
	cJSON_AddNumberToObject(o, "ms", (double)((esp_timer_get_time() - start) / 1000));
	return send_json(req, NULL, o);
}

static esp_err_t h_playlist(httpd_req_t *req)
{
	form_t *f = form_open(req);
	if (!f)
		return send_text(req, "400 Bad Request", "Invalid form");
	display_playlist_t *pl;
	esp_err_t err = display_playlist_begin(&pl);
	if (err != ESP_OK) {
		free(f);
		return send_error(req, err, NULL);
	}
	mp_part_t part;
	int pr;
	while (err == ESP_OK && (pr = mp_next_part(&f->mp, &part)) == 1) {
		if (strcmp(part.name, "file") != 0)
			continue;
		img_reader_t r = {part_read, &f->mp};
		err = display_playlist_add(pl, &r);
	}
	free(f);
	int items = 0;
	if (err != ESP_OK) {
		display_playlist_finish(pl, false, NULL);
		return send_error(req, err, NULL);
	}
	if (pr < 0) {
		display_playlist_finish(pl, false, NULL);
		return send_text(req, "400 Bad Request", "Unreadable file");
	}
	err = display_playlist_finish(pl, true, &items);
	if (items == 0)
		return send_text(req, "400 Bad Request", "No files provided");
	if (err != ESP_OK)
		return send_error(req, err, NULL);
	cJSON *o = ok_json();
	cJSON_AddNumberToObject(o, "items", items);
	return send_json(req, NULL, o);
}

// ---- /uploadTest ----

static void parse_brightness_steps(const char *raw, uint8_t *out, int *n)
{
	*n = 0;
	const char *p = raw;
	while (*p && *n < 32) {
		char *end;
		long v = strtol(p, &end, 10);
		if (end != p)
			out[(*n)++] = (uint8_t)(v < 0 ? 0 : v > 125 ? 125 : v);
		p = strchr(end, ',');
		if (!p)
			break;
		p++;
	}
	if (*n == 0) {
		static const uint8_t def[] = {0, 32, 64, 96, 125};
		memcpy(out, def, sizeof def);
		*n = 5;
	}
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c |= 0x20;
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// A file part saved to scratch because it came before the command.
static int saved_read(void *ctx, uint8_t *buf, size_t len)
{
	size_t n = fread(buf, 1, len, ctx);
	return n == 0 && ferror((FILE *)ctx) ? -1 : (int)n;
}

static esp_err_t show_part(mp_t *mp, const char *saved_path, display_kind_t kind)
{
	if (saved_path) {
		FILE *f = fopen(saved_path, "rb");
		if (!f)
			return ESP_FAIL;
		img_reader_t r = {saved_read, f};
		esp_err_t err = display_show(&r, kind);
		fclose(f);
		return err;
	}
	img_reader_t r = {part_read, mp};
	return display_show(&r, kind);
}

static esp_err_t h_upload_test(httpd_req_t *req)
{
	form_t *f = form_open(req);
	if (!f)
		return send_text(req, "400 Bad Request", "Invalid form");
	char command[48] = "", hex[FIELD_MAX] = "", steps[FIELD_MAX] = "";
	// Which file field the command needs and its kind.
	const char *want = NULL;
	display_kind_t kind = DISPLAY_STATIC;
	bool known = true, done = false, sent_static = false;
	char saved[3][40] = {"", "", ""}; // animated, static, other, when they came first
	static const char *file_fields[3] = {"animated", "static", "other"};
	esp_err_t err = ESP_OK;
	mp_part_t part;
	int pr = 0;

	while (err == ESP_OK && (pr = mp_next_part(&f->mp, &part)) == 1) {
		if (!part.is_file) {
			char *dst = strcmp(part.name, "command") == 0      ? command
				    : strcmp(part.name, "hex") == 0        ? hex
				    : strcmp(part.name, "brightness") == 0 ? steps
									   : NULL;
			char tmp[8];
			if (dst)
				read_field(&f->mp, dst, dst == command ? sizeof command : FIELD_MAX);
			else
				read_field(&f->mp, tmp, sizeof tmp);
			continue;
		}
		int slot = -1;
		for (int i = 0; i < 3; i++)
			if (strcmp(part.name, file_fields[i]) == 0)
				slot = i;
		if (slot < 0)
			continue;
		if (command[0]) {
			// Command known: stream the part straight to the panel.
			bool animated_cmd = strncmp(command, "send_animated", 13) == 0 ||
					    strcmp(command, "send_playlist") == 0;
			if ((animated_cmd && slot == 0) || (!animated_cmd && strncmp(command, "send_static", 11) == 0 &&
								 slot == 1) ||
			    (strcmp(command, "send_other_static_no_reinit") == 0 && slot == 2)) {
				err = show_part(&f->mp, NULL, slot == 0 ? DISPLAY_ANIMATED : DISPLAY_STATIC);
				done = true;
				continue;
			}
			// send_playlist sends the animation, then the still, as Go does.
			if (strcmp(command, "send_playlist") == 0 && slot == 1 && done) {
				err = show_part(&f->mp, NULL, DISPLAY_STATIC);
				sent_static = true;
				continue;
			}
		}
		// Command not seen yet: keep the file for later.
		snprintf(saved[slot], sizeof saved[slot], SCRATCH "/form_%s", file_fields[slot]);
		FILE *out = fopen(saved[slot], "wb");
		uint8_t buf[512];
		int n;
		while (out && (n = mp_read(&f->mp, buf, sizeof buf)) > 0)
			if (fwrite(buf, 1, (size_t)n, out) != (size_t)n)
				break;
		if (out)
			fclose(out);
	}
	free(f);
	if (pr < 0 && err == ESP_OK) {
		err = ESP_FAIL;
		goto finish;
	}
	if (err != ESP_OK)
		goto finish;

	if (strcmp(command, "connect") == 0) {
		err = display_connect();
	} else if (strcmp(command, "disconnect") == 0) {
		display_disconnect();
	} else if (strncmp(command, "send_animated", 13) == 0 || strcmp(command, "send_playlist") == 0) {
		want = "animated";
		kind = DISPLAY_ANIMATED;
		if (!done && !saved[0][0])
			err = ESP_ERR_NOT_FOUND;
		else if (!done)
			err = show_part(NULL, saved[0], kind);
		if (err == ESP_OK && strcmp(command, "send_playlist") == 0 && !sent_static && saved[1][0])
			err = show_part(NULL, saved[1], DISPLAY_STATIC);
	} else if (strncmp(command, "send_static", 11) == 0 || strcmp(command, "send_other_static_no_reinit") == 0) {
		int slot = command[5] == 'o' ? 2 : 1;
		want = file_fields[slot];
		if (!done && !saved[slot][0])
			err = ESP_ERR_NOT_FOUND;
		else if (!done)
			err = show_part(NULL, saved[slot], DISPLAY_STATIC);
	} else if (strcmp(command, "raw") == 0) {
		// Hex body after the length field, spaces allowed.
		uint8_t body[120];
		size_t n = 0;
		int hi = -1;
		for (const char *p = hex; *p && err == ESP_OK; p++) {
			if (*p == ' ')
				continue;
			int v = hexval(*p);
			if (v < 0 || n == sizeof body)
				err = ESP_ERR_INVALID_ARG;
			else if (hi < 0)
				hi = v;
			else
				body[n++] = (uint8_t)(hi << 4 | v), hi = -1;
		}
		if (err == ESP_OK && (hi >= 0 || n == 0))
			err = ESP_ERR_INVALID_ARG;
		if (err == ESP_OK)
			err = display_raw(body, n);
	} else if (strcmp(command, "brightness_cycle") == 0) {
		uint8_t levels[32];
		int n;
		parse_brightness_steps(steps, levels, &n);
		for (int i = 0; i < n && err == ESP_OK; i++) {
			err = display_set_brightness_unless_off(levels[i]);
			vTaskDelay(pdMS_TO_TICKS(200));
		}
	} else if (strcmp(command, "screen_off") == 0) {
		err = display_turn_off();
	} else if (strcmp(command, "screen_on") == 0) {
		err = display_turn_on(80);
	} else {
		known = false;
	}
finish:
	for (int i = 0; i < 3; i++)
		if (saved[i][0])
			unlink(saved[i]);
	if (!known)
		return send_text(req, "400 Bad Request", "Unknown command");
	if (err == ESP_ERR_NOT_FOUND) {
		char msg[48];
		snprintf(msg, sizeof msg, "missing %s upload", want);
		return send_text(req, "500 Internal Server Error", msg);
	}
	if (err == ESP_ERR_INVALID_ARG)
		return send_text(req, "500 Internal Server Error", "invalid hex");
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "/uploadTest %s failed: %s", command, display_error_text(err));
		return send_error(req, err, NULL);
	}
	httpd_resp_set_type(req, "text/plain");
	return httpd_resp_sendstr(req, "ok");
}

// ---- Brightness, power and playlist commands ----

static esp_err_t h_off(httpd_req_t *req)
{
	if (!display_connected())
		return send_text(req, "503 Service Unavailable", "Device not ready");
	uint8_t cur = display_brightness();
	if (cur >= 5)
		last_on_brightness = cur;
	esp_err_t err = display_turn_off();
	if (err != ESP_OK)
		return send_text(req, "500 Internal Server Error", display_error_text(err));
	cJSON *o = ok_json();
	cJSON_AddStringToObject(o, "brightness", "5");
	return send_json(req, NULL, o);
}

static esp_err_t h_on(httpd_req_t *req)
{
	if (!display_connected())
		return send_text(req, "503 Service Unavailable", "Device not ready");
	uint8_t target = last_on_brightness < 5 ? 5 : last_on_brightness > 125 ? 125 : last_on_brightness;
	esp_err_t err = display_turn_on(target);
	if (err != ESP_OK)
		return send_text(req, "500 Internal Server Error", display_error_text(err));
	cJSON *o = ok_json();
	cJSON_AddNumberToObject(o, "brightness", target);
	return send_json(req, NULL, o);
}

static esp_err_t h_brightness(httpd_req_t *req)
{
	if (!display_connected())
		return send_text(req, "503 Service Unavailable", "Device not ready");
	const char *v = req->uri + strlen("/brightness/");
	char *end;
	long val = strtol(v, &end, 10);
	if (end == v || (*end && *end != '?'))
		return send_text(req, "400 Bad Request", "Invalid brightness");
	uint8_t level = (uint8_t)(val < 5 ? 5 : val > 125 ? 125 : val);
	last_on_brightness = level;
	esp_err_t err = display_set_brightness_unless_off(level);
	if (err != ESP_OK)
		return send_text(req, "409 Conflict", display_error_text(err));
	cJSON *o = ok_json();
	cJSON_AddNumberToObject(o, "brightness", level);
	return send_json(req, NULL, o);
}

static esp_err_t simple(httpd_req_t *req, esp_err_t err, bool map_stream)
{
	if (err == ESP_OK)
		return write_ok(req);
	if (map_stream)
		return send_error(req, err, NULL);
	return send_text(req, "500 Internal Server Error", display_error_text(err));
}

static esp_err_t h_power_on(httpd_req_t *req)
{
	return simple(req, display_power(true), false);
}

static esp_err_t h_power_off(httpd_req_t *req)
{
	return simple(req, display_power(false), false);
}

static esp_err_t h_clear(httpd_req_t *req)
{
	return simple(req, display_clear_playlist(), true);
}

static esp_err_t h_clear_screen(httpd_req_t *req)
{
	return simple(req, display_clear_screen(), false);
}

static esp_err_t h_resume(httpd_req_t *req)
{
	return simple(req, display_resume(), true);
}

static esp_err_t h_reconnect(httpd_req_t *req)
{
	display_reconnect();
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "status", "reconnecting");
	return send_json(req, "202 Accepted", o);
}

static esp_err_t h_health(httpd_req_t *req)
{
	const settings_t *s = settings_get();
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "status", "healthy");
	cJSON *c = cJSON_AddObjectToObject(o, "config");
	cJSON_AddNumberToObject(c, "width", s->panel_width);
	cJSON_AddNumberToObject(c, "height", s->panel_height);
	cJSON_AddNumberToObject(c, "stream_fps", s->stream_fps);
	cJSON_AddNumberToObject(c, "stream_resume_delay_ms", s->stream_resume_ms);
	cJSON_AddNumberToObject(c, "stream_colors", s->stream_colors);
	cJSON_AddBoolToObject(o, "panel_connected", display_connected());
	cJSON_AddNumberToObject(o, "free_heap", heap_caps_get_free_size(MALLOC_CAP_8BIT));
	return send_json(req, NULL, o);
}

// ---- /api: settings, status, Wi-Fi ----

static esp_err_t h_api_config_get(httpd_req_t *req)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddItemToObject(o, "settings", settings_json(false));
	cJSON_AddBoolToObject(o, "wifi_password_set", settings_get()->wifi_password[0] != 0);
	cJSON_AddItemToObject(o, "restart_keys", settings_restart_keys());
	settings_t def;
	settings_defaults(&def);
	cJSON *d = cJSON_AddObjectToObject(o, "defaults");
	cJSON_AddNumberToObject(d, "http_port", def.http_port);
	cJSON_AddNumberToObject(d, "panel_width", def.panel_width);
	cJSON_AddNumberToObject(d, "panel_height", def.panel_height);
	cJSON_AddStringToObject(d, "panel_name", def.panel_name);
	cJSON_AddStringToObject(d, "hostname", def.hostname);
	cJSON_AddNumberToObject(d, "stream_fps", def.stream_fps);
	cJSON_AddNumberToObject(d, "stream_resume_delay", def.stream_resume_ms / 1000.0);
	return send_json(req, NULL, o);
}

static esp_err_t apply_settings(httpd_req_t *req, bool import)
{
	char *body = read_small(req);
	cJSON *json = body ? cJSON_Parse(body) : NULL;
	free(body);
	cJSON *o = cJSON_CreateObject();
	if (!json) {
		cJSON_AddStringToObject(o, "error", body ? "invalid JSON" : "body missing or too large");
		return send_json(req, "400 Bad Request", o);
	}
	char err[96];
	cJSON *unknown = NULL;
	unsigned changes = 0;
	esp_err_t res = settings_apply(json, import, err, sizeof err, &unknown, &changes);
	cJSON_Delete(json);
	if (res != ESP_OK) {
		cJSON_Delete(unknown);
		cJSON_AddStringToObject(o, "error", err);
		return send_json(req, res == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", o);
	}
	if (changes & SETTINGS_RECONNECT)
		display_reconnect();
	cJSON_AddStringToObject(o, "status", "ok");
	cJSON_AddBoolToObject(o, "restart_required", changes & SETTINGS_RESTART);
	cJSON_AddItemToObject(o, "unknown_keys", unknown);
	return send_json(req, NULL, o);
}

static esp_err_t h_api_config_post(httpd_req_t *req)
{
	return apply_settings(req, false);
}

static esp_err_t h_api_import(httpd_req_t *req)
{
	return apply_settings(req, true);
}

static esp_err_t h_api_export(httpd_req_t *req)
{
	httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"xyaoled-config.json\"");
	cJSON *o = settings_json(true);
	char *text = cJSON_Print(o);
	cJSON_Delete(o);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, text ? text : "{}");
	free(text);
	return ESP_OK;
}

static void restart_cb(void *arg)
{
	esp_restart();
}

static void restart_soon(void)
{
	esp_timer_create_args_t t = {.callback = restart_cb, .name = "restart"};
	esp_timer_handle_t h;
	if (esp_timer_create(&t, &h) == ESP_OK)
		esp_timer_start_once(h, 700 * 1000);
}

static esp_err_t h_api_restart(httpd_req_t *req)
{
	restart_soon();
	return write_ok(req);
}

static esp_err_t h_api_reset(httpd_req_t *req)
{
	esp_err_t err = settings_reset();
	if (err != ESP_OK)
		return send_text(req, "500 Internal Server Error", esp_err_to_name(err));
	restart_soon();
	return write_ok(req);
}

static esp_err_t h_api_status(httpd_req_t *req)
{
	cJSON *o = cJSON_CreateObject();
	const esp_app_desc_t *app = esp_app_get_description();
	cJSON_AddStringToObject(o, "version", app->version);
	cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
	cJSON_AddNumberToObject(o, "free_heap", heap_caps_get_free_size(MALLOC_CAP_8BIT));
	cJSON_AddNumberToObject(o, "min_free_heap", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
	cJSON_AddNumberToObject(o, "largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

	wifi_status_t ws;
	wifi_status(&ws);
	cJSON *w = cJSON_AddObjectToObject(o, "wifi");
	cJSON_AddStringToObject(w, "state", wifi_state_name(ws.state));
	cJSON_AddStringToObject(w, "ssid", ws.ssid);
	cJSON_AddStringToObject(w, "ip", ws.ip);
	cJSON_AddNumberToObject(w, "rssi", ws.rssi);
	cJSON_AddBoolToObject(w, "ap_active", ws.ap_active);
	cJSON_AddBoolToObject(w, "trying", ws.trying);
	cJSON_AddStringToObject(w, "reason", ws.reason);
	cJSON_AddStringToObject(w, "hostname", settings_get()->hostname);

	cJSON *p = cJSON_AddObjectToObject(o, "panel");
	cJSON_AddBoolToObject(p, "connected", display_connected());
	char addr[18];
	xy_panel_address(addr);
	cJSON_AddStringToObject(p, "address", addr);
	xy_panel_info_t info;
	if (xy_panel_info(&info)) {
		cJSON_AddNumberToObject(p, "width", info.width);
		cJSON_AddNumberToObject(p, "height", info.height);
	}
	cJSON_AddNumberToObject(p, "brightness", display_brightness());
	cJSON_AddBoolToObject(p, "streaming", display_streaming());
	cJSON_AddNumberToObject(p, "conn_interval_ms", xy_panel_conn_interval() * 1.25);

	// Average time per stream frame since the stream started (decode and
	// encode overlap the previous frame's send).
	display_live_stats_t ls;
	display_live_stats(&ls);
	cJSON *st = cJSON_AddObjectToObject(o, "stream");
	cJSON_AddNumberToObject(st, "frames", ls.frames);
	if (ls.frames) {
		double n = ls.frames;
		cJSON_AddNumberToObject(st, "decode_ms", ls.decode_us / 1000.0 / n);
		cJSON_AddNumberToObject(st, "encode_ms", ls.encode_us / 1000.0 / n);
		cJSON_AddNumberToObject(st, "select_ms", ls.select_ms / n);
		cJSON_AddNumberToObject(st, "data_ms", ls.data_ms / n);
		cJSON_AddNumberToObject(st, "max_send_ms", ls.max_send_ms);
		cJSON_AddNumberToObject(st, "gif_bytes", ls.bytes / n);
		cJSON_AddNumberToObject(st, "last_colors", ls.last_colors);
	}

	size_t total, used;
	storage_usage(&total, &used);
	cJSON *sc = cJSON_AddObjectToObject(o, "scratch");
	cJSON_AddNumberToObject(sc, "total", total);
	cJSON_AddNumberToObject(sc, "used", used);
	return send_json(req, NULL, o);
}

static esp_err_t h_api_scan(httpd_req_t *req)
{
	return send_json(req, NULL, wifi_scan());
}

static esp_err_t h_api_wifi_connect(httpd_req_t *req)
{
	char *body = read_small(req);
	cJSON *json = body ? cJSON_Parse(body) : NULL;
	free(body);
	const cJSON *ssid = cJSON_GetObjectItem(json, "ssid"), *pass = cJSON_GetObjectItem(json, "password");
	cJSON *o = cJSON_CreateObject();
	if (!cJSON_IsString(ssid) || (pass && !cJSON_IsString(pass)) ||
	    wifi_try(ssid->valuestring, pass ? pass->valuestring : "") != ESP_OK) {
		cJSON_Delete(json);
		cJSON_AddStringToObject(o, "error", "need an SSID, and a password of 8 to 63 characters or none");
		return send_json(req, "400 Bad Request", o);
	}
	cJSON_Delete(json);
	cJSON_AddStringToObject(o, "status", "connecting");
	return send_json(req, "202 Accepted", o);
}

#if CONFIG_XY_QEMU
// ---- QEMU build: what the simulated panel received ----

static esp_err_t h_sim_last(httpd_req_t *req)
{
	uint32_t len;
	uint8_t trailer[8], program;
	const uint8_t *gif = xy_panel_sim_last(&len, trailer, &program);
	if (!gif)
		return send_text(req, "404 Not Found", "nothing uploaded yet");
	char hex[17], prog[4];
	for (int i = 0; i < 8; i++)
		snprintf(hex + i * 2, 3, "%02x", trailer[i]);
	snprintf(prog, sizeof prog, "%u", program);
	httpd_resp_set_hdr(req, "X-Trailer", hex);
	httpd_resp_set_hdr(req, "X-Program", prog);
	httpd_resp_set_type(req, "image/gif");
	return httpd_resp_send(req, (const char *)gif, len);
}

static esp_err_t h_sim_stats(httpd_req_t *req)
{
	xy_panel_sim_stats_t st;
	xy_panel_sim_stats(&st);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "uploads", st.uploads);
	cJSON_AddNumberToObject(o, "packets", st.packets);
	cJSON_AddNumberToObject(o, "selects", st.selects);
	cJSON_AddNumberToObject(o, "clears", st.clears);
	cJSON_AddNumberToObject(o, "resumes", st.resumes);
	cJSON_AddNumberToObject(o, "header_errors", st.header_errors);
	cJSON_AddNumberToObject(o, "brightness", st.brightness);
	cJSON_AddBoolToObject(o, "power", st.power);
	return send_json(req, NULL, o);
}
#endif

// ---- Setup portal on port 80 ----

static esp_err_t h_portal(httpd_req_t *req)
{
	char to[64];
	snprintf(to, sizeof to, "http://" WIFI_AP_IP ":%u/config", settings_get()->http_port);
	return redirect(req, to);
}

static void portal_hook(bool active)
{
	if (active && !portal && settings_get()->http_port != 80) {
		httpd_config_t c = HTTPD_DEFAULT_CONFIG();
		c.server_port = 80;
		c.ctrl_port = 32769;
		c.stack_size = 3072;
		c.max_uri_handlers = 2;
		c.max_open_sockets = 3;
		c.uri_match_fn = httpd_uri_match_wildcard;
		if (httpd_start(&portal, &c) == ESP_OK) {
			httpd_uri_t u = {.uri = "/*", .method = HTTP_GET, .handler = h_portal};
			httpd_register_uri_handler(portal, &u);
		}
	} else if (!active && portal) {
		httpd_stop(portal);
		portal = NULL;
	}
}

// ---- Server ----

static void on_close(httpd_handle_t hd, int fd)
{
	stream_on_close(fd);
	close(fd);
}

typedef struct {
	const char *uri;
	httpd_method_t method;
	esp_err_t (*fn)(httpd_req_t *);
} route_t;

static const route_t routes[] = {
	{"/", HTTP_GET, h_root},
	{"/health", HTTP_GET, h_health},
	{"/test", HTTP_GET, h_test},
	{"/config", HTTP_GET, h_config_page},
	{"/display", HTTP_POST, h_display},
	{"/display-r", HTTP_POST, h_display},
	{"/upload", HTTP_POST, h_upload},
	{"/upload", HTTP_GET, h_to_test},
	{"/uploadTest", HTTP_POST, h_upload_test},
	{"/uploadTest", HTTP_GET, h_to_test},
	{"/uploadTestGif", HTTP_POST, h_upload_test_gif},
	{"/uploadTestGif", HTTP_GET, h_to_test},
	{"/playlist", HTTP_POST, h_playlist},
	{"/live", HTTP_POST, h_live},
	{"/off", HTTP_GET, h_off},
	{"/on", HTTP_GET, h_on},
	{"/brightness/*", HTTP_GET, h_brightness},
	{"/power/on", HTTP_GET, h_power_on},
	{"/power/off", HTTP_GET, h_power_off},
	{"/clear", HTTP_GET, h_clear},
	{"/clear-screen", HTTP_GET, h_clear_screen},
	{"/resume", HTTP_GET, h_resume},
	{"/reconnect", HTTP_GET, h_reconnect},
	{"/api/config", HTTP_GET, h_api_config_get},
	{"/api/config", HTTP_POST, h_api_config_post},
	{"/api/config/export", HTTP_GET, h_api_export},
	{"/api/config/import", HTTP_POST, h_api_import},
	{"/api/config/reset", HTTP_POST, h_api_reset},
	{"/api/restart", HTTP_POST, h_api_restart},
	{"/api/status", HTTP_GET, h_api_status},
	{"/api/wifi/scan", HTTP_GET, h_api_scan},
	{"/api/wifi/connect", HTTP_POST, h_api_wifi_connect},
#if CONFIG_XY_QEMU
	{"/api/sim/last", HTTP_GET, h_sim_last},
	{"/api/sim/stats", HTTP_GET, h_sim_stats},
#endif
};

esp_err_t http_start(void)
{
	httpd_config_t c = HTTPD_DEFAULT_CONFIG();
	c.server_port = settings_get()->http_port;
	c.stack_size = 10240; // images are decoded in the handlers
	c.max_uri_handlers = sizeof routes / sizeof *routes + 3; // + /stream, /api/stream/frame, one spare
	c.uri_match_fn = httpd_uri_match_wildcard;
	c.lru_purge_enable = true;
	c.recv_wait_timeout = 20;
	c.send_wait_timeout = 20;
	c.close_fn = on_close;
	esp_err_t err = httpd_start(&server, &c);
	if (err != ESP_OK)
		return err;
	for (size_t i = 0; i < sizeof routes / sizeof *routes; i++) {
		httpd_uri_t u = {.uri = routes[i].uri, .method = routes[i].method, .handler = routes[i].fn};
		httpd_register_uri_handler(server, &u);
	}
	if ((err = stream_register(server)) != ESP_OK)
		return err;
	wifi_set_ap_hook(portal_hook);
	// The portal may have opened before the server existed (first boot).
	wifi_status_t ws;
	wifi_status(&ws);
	if (ws.ap_active)
		portal_hook(true);
	ESP_LOGI(TAG, "HTTP server on port %u", c.server_port);
	ESP_LOGI(TAG, "Endpoints:");
	ESP_LOGI(TAG, "  POST /display, /display-r - Display an image (JSON, base64)");
	ESP_LOGI(TAG, "  GET  /health, /test, /config");
	ESP_LOGI(TAG, "  POST /upload, /uploadTest, /uploadTestGif, /playlist, /live");
	ESP_LOGI(TAG, "  GET  /off, /on, /brightness/{n}, /power/on, /power/off");
	ESP_LOGI(TAG, "  GET  /clear, /clear-screen, /resume, /reconnect");
	ESP_LOGI(TAG, "  WS   /stream");
	ESP_LOGI(TAG, "  /api/config, /api/config/export, /api/config/import, /api/status, /api/wifi/*");
	return ESP_OK;
}
