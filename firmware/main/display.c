#include "display.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"
#include "storage.h"
#include "xy_panel.h"

static const char *TAG = "display";

#define CONNECT_TIMEOUT_MS 30000
#define MONITOR_MS 5000
#define RETRY_MIN_MS 5000
#define RETRY_MAX_MS 60000
#define OFF_LEVEL 5
#define STREAM_MAX_FRAMES 4    // animated stream frames, held in RAM
#define MEM_GIF_CAP (48 * 1024) // largest GIF built in RAM
#define OUT_RESERVE (256 * 1024) // scratch kept free for the output GIF
#define PLAYLIST_MAX 32

#define IN_PATH SCRATCH "/in.gif"
#define FRAMES_PATH SCRATCH "/frames.bin"
#define OUT_PATH SCRATCH "/out.gif"

static struct {
	SemaphoreHandle_t work; // the pipeline and the scratch files
	atomic_bool streaming;
	atomic_bool reconnect;
	bool manual_off;
	uint8_t bright; // what the panel was last set to
	char error[112];
	display_live_stats_t live; // stream frames since the stream started
} d;

static portMUX_TYPE live_lock = portMUX_INITIALIZER_UNLOCKED; // d.live

// ---- Errors ----

const char *display_error_text(esp_err_t err)
{
	switch (err) {
	case DISPLAY_ERR_STREAMING:
		return "live stream or drawing in progress; playlist requests are ignored until it ends";
	case DISPLAY_ERR_SCREEN_OFF:
		return "screen is off (brightness <=5); ignoring image send request";
	case DISPLAY_ERR_MANUAL_OFF:
		return "screen is manually off; ignoring brightness change (use /on to enable)";
	case DISPLAY_ERR_IMAGE:
		return d.error;
	}
	return xy_err_name(err);
}

static esp_err_t image_error(const char *what, img_err_t err)
{
	snprintf(d.error, sizeof d.error, "%s: %s", what, img_strerror(err));
	return DISPLAY_ERR_IMAGE;
}

// ---- Panel configuration ----

static img_target_t target(void)
{
	const settings_t *s = settings_get();
	return (img_target_t){s->panel_width, s->panel_height, s->upside_down};
}

static void panel_config(xy_panel_config_t *c)
{
	const settings_t *s = settings_get();
	memset(c, 0, sizeof *c);
	strlcpy(c->name_match, s->panel_name, sizeof c->name_match);
	c->debug = s->debug_ble;
	if (s->panel_by_mac) {
		unsigned b[6];
		if (sscanf(s->panel_mac, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
			c->by_mac = true;
			for (int i = 0; i < 6; i++)
				c->mac[i] = (uint8_t)b[i];
		}
	}
}

// ---- Pipeline ----

// A reader that first returns bytes already taken from the source (the
// header, read to choose the path), then the rest of the source.
typedef struct {
	const img_reader_t *src;
	uint8_t head[13];
	size_t head_len, head_pos;
	FILE *tee; // copy of everything read, for passthrough
	bool tee_error;
} prefix_t;

static int prefix_read(void *ctx, uint8_t *buf, size_t len)
{
	prefix_t *p = ctx;
	int n;
	if (p->head_pos < p->head_len) {
		size_t k = p->head_len - p->head_pos;
		if (k > len)
			k = len;
		memcpy(buf, p->head + p->head_pos, k);
		p->head_pos += k;
		n = (int)k;
	} else {
		n = p->src->read(p->src->ctx, buf, len);
	}
	if (n > 0 && p->tee && fwrite(buf, 1, (size_t)n, p->tee) != (size_t)n)
		p->tee_error = true;
	return n;
}

static int file_read(void *ctx, uint8_t *buf, size_t len)
{
	size_t n = fread(buf, 1, len, (FILE *)ctx);
	return n == 0 && ferror((FILE *)ctx) ? -1 : (int)n;
}

static int file_write(void *ctx, const uint8_t *buf, size_t len)
{
	return fwrite(buf, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

typedef struct {
	uint8_t *buf;
	size_t len, cap;
} mem_out_t;

static int mem_write(void *ctx, const uint8_t *buf, size_t len)
{
	mem_out_t *m = ctx;
	if (m->len + len > m->cap) {
		size_t cap = m->cap ? m->cap * 2 : 4096;
		while (cap < m->len + len)
			cap *= 2;
		if (cap > MEM_GIF_CAP)
			cap = MEM_GIF_CAP;
		if (m->len + len > cap)
			return -1;
		uint8_t *b = realloc(m->buf, cap);
		if (!b)
			return -1;
		m->buf = b;
		m->cap = cap;
	}
	memcpy(m->buf + m->len, buf, len);
	m->len += len;
	return 0;
}

// The panel's GIF for one upload: in a file, or in RAM for stream frames.
typedef struct {
	char path[40];
	mem_out_t mem;
	bool static_image;
	uint32_t frames, total_ms, bytes;
	uint32_t decode_us, encode_us; // time decode_encode took
} encoded_t;

static void encoded_free(encoded_t *e)
{
	free(e->mem.buf);
	e->mem.buf = NULL;
	if (e->path[0])
		unlink(e->path);
	e->path[0] = 0;
}

static size_t frame_limit(const img_target_t *t)
{
	size_t fb = (size_t)t->width * t->height * 3, free_bytes = storage_free();
	return free_bytes > OUT_RESERVE + fb ? (free_bytes - OUT_RESERVE) / fb : 1;
}

// decode_encode decodes r into frames and encodes the panel's GIF.
static esp_err_t decode_encode(const img_reader_t *r, display_kind_t kind, bool in_ram, encoded_t *out)
{
	img_target_t t = target();
	size_t limit = kind == DISPLAY_STATIC ? 1 : in_ram ? STREAM_MAX_FRAMES : frame_limit(&t);
	img_frames_t *frames = in_ram ? img_frames_new_mem(&t, limit) : img_frames_new_file(&t, FRAMES_PATH, limit);
	if (!frames)
		return image_error("frame store", IMG_ERR_NOMEM);
	img_info_t info;
	int64_t t0 = esp_timer_get_time();
	img_err_t ierr = img_decode(r, frames, &info);
	out->decode_us = (uint32_t)(esp_timer_get_time() - t0);
	// "static" keeps the first frame of an animation.
	if (ierr == IMG_ERR_FULL && kind == DISPLAY_STATIC)
		ierr = IMG_OK;
	esp_err_t err = ESP_OK;
	if (ierr != IMG_OK) {
		err = image_error("failed to decode image", ierr);
	} else if (kind == DISPLAY_ANIMATED && info.format != IMG_FMT_GIF) {
		err = image_error("failed to decode GIF", IMG_ERR_FORMAT);
	} else {
		img_gif_info_t gi;
		FILE *f = NULL;
		img_writer_t w = {mem_write, &out->mem};
		if (!in_ram) {
			if (!(f = fopen(out->path, "wb"))) {
				err = image_error("scratch", IMG_ERR_IO);
				goto done;
			}
			w = (img_writer_t){file_write, f};
		}
		t0 = esp_timer_get_time();
		ierr = img_encode_gif(frames, &w, &gi);
		out->encode_us = (uint32_t)(esp_timer_get_time() - t0);
		if (f && fclose(f) != 0 && ierr == IMG_OK)
			ierr = IMG_ERR_IO;
		if (ierr != IMG_OK) {
			err = image_error("failed to process image", ierr);
		} else {
			out->static_image = gi.static_image;
			out->frames = gi.frames;
			out->total_ms = gi.total_ms;
			out->bytes = gi.bytes;
			ESP_LOGI(TAG, "%ux%u %s → %u frame(s), %u ms, %u bytes", (unsigned)info.src_width,
				 (unsigned)info.src_height,
				 info.format == IMG_FMT_GIF ? "GIF" : info.format == IMG_FMT_PNG ? "PNG" : "JPEG",
				 (unsigned)gi.frames, (unsigned)gi.total_ms, (unsigned)gi.bytes);
		}
	}
done:
	img_frames_free(frames);
	if (!in_ram)
		unlink(FRAMES_PATH);
	return err;
}

// encode turns an upload into the panel's GIF in out_path (in RAM when
// out_path is NULL). A GIF already panel-sized is stored as received and,
// if it passes the checks, sent as it is.
static esp_err_t encode(const img_reader_t *r, display_kind_t kind, const char *out_path, encoded_t *out)
{
	memset(out, 0, sizeof *out);
	if (out_path)
		strlcpy(out->path, out_path, sizeof out->path);
	prefix_t p = {.src = r};
	while (p.head_len < sizeof p.head) {
		int n = r->read(r->ctx, p.head + p.head_len, sizeof p.head - p.head_len);
		if (n < 0)
			return image_error("failed to read upload", IMG_ERR_IO);
		if (n == 0)
			break;
		p.head_len += (size_t)n;
	}
	img_reader_t pr = {prefix_read, &p};
	img_target_t t = target();
	const settings_t *s = settings_get();

	if (out_path && s->passthrough && kind != DISPLAY_STATIC && img_detect(p.head, p.head_len) == IMG_FMT_GIF &&
	    p.head_len == sizeof p.head && (p.head[6] | p.head[7] << 8) == t.width &&
	    (p.head[8] | p.head[9] << 8) == t.height && !t.upside_down) {
		// Keep a copy while scanning; it is either sent or decoded.
		if (!(p.tee = fopen(IN_PATH, "wb")))
			return image_error("scratch", IMG_ERR_IO);
		img_gif_scan_t scan;
		img_err_t serr = img_gif_scan(&pr, &scan);
		// Read whatever follows the trailer so the copy is complete.
		uint8_t sink[64];
		while (serr == IMG_OK && pr.read(pr.ctx, sink, sizeof sink) > 0) {
		}
		bool ok = fclose(p.tee) == 0 && !p.tee_error;
		p.tee = NULL;
		const char *why = "not a valid GIF";
		if (ok && serr == IMG_OK && img_gif_passthrough(&scan, &t, s->passthrough_partial, &why)) {
			ESP_LOGI(TAG, "passthrough: %u frames, %u ms%s%s", (unsigned)scan.frames, (unsigned)scan.total_ms,
				 scan.partial_frame ? ", partial frames" : "", scan.transparency ? ", transparency" : "");
			rename(IN_PATH, out->path);
			FILE *f = fopen(out->path, "rb");
			long size = -1;
			if (f && fseek(f, 0, SEEK_END) == 0)
				size = ftell(f);
			if (f)
				fclose(f);
			if (size <= 0)
				return image_error("scratch", IMG_ERR_IO);
			out->frames = scan.frames;
			out->total_ms = scan.total_ms;
			out->bytes = (uint32_t)size;
			return ESP_OK;
		}
		ESP_LOGI(TAG, "no passthrough: %s", ok ? why : "scratch write failed");
		FILE *in = fopen(IN_PATH, "rb");
		if (!in)
			return image_error("scratch", IMG_ERR_IO);
		img_reader_t fr = {file_read, in};
		esp_err_t err = decode_encode(&fr, kind, false, out);
		fclose(in);
		unlink(IN_PATH);
		return err;
	}
	return decode_encode(&pr, kind, out_path == NULL, out);
}

// send_checks mirrors the Go send(): playlist uploads are refused while
// streaming, and every image while the screen is off.
static esp_err_t send_checks(bool live)
{
	if (!live && atomic_load(&d.streaming)) {
		ESP_LOGW(TAG, "ignoring playlist upload: %s", display_error_text(DISPLAY_ERR_STREAMING));
		return DISPLAY_ERR_STREAMING;
	}
	if (d.bright <= OFF_LEVEL)
		return DISPLAY_ERR_SCREEN_OFF;
	if (!xy_panel_connected())
		return XY_ERR_NOT_CONNECTED;
	return ESP_OK;
}

static esp_err_t send_encoded(const encoded_t *e, uint8_t program, bool append)
{
	xy_send_opts_t o = {.program = program, .append = append, .static_image = e->static_image,
			    .frames = e->frames, .total_ms = e->total_ms};
	xy_blob_t blob;
	FILE *f = NULL;
	if (e->mem.buf) {
		xy_blob_mem(&blob, e->mem.buf, (uint32_t)e->mem.len);
	} else {
		if (!(f = fopen(e->path, "rb")))
			return image_error("scratch", IMG_ERR_IO);
		xy_blob_file(&blob, f, e->bytes);
	}
	esp_err_t err = xy_panel_send_gif(&blob, &o);
	if (f)
		fclose(f);
	if (err == XY_ERR_NOT_CONNECTED)
		display_reconnect();
	return err;
}

static esp_err_t show(const img_reader_t *r, display_kind_t kind, uint8_t program)
{
	esp_err_t err = send_checks(program == XY_PROGRAM_LIVE);
	if (err != ESP_OK)
		return err;
	xSemaphoreTake(d.work, portMAX_DELAY);
	encoded_t e;
	err = encode(r, kind, OUT_PATH, &e);
	if (err == ESP_OK)
		err = send_encoded(&e, program, false);
	encoded_free(&e);
	xSemaphoreGive(d.work);
	if (err == ESP_OK)
		ESP_LOGI(TAG, "✓ sent to the %s", program == XY_PROGRAM_LIVE ? "live view" : "playlist");
	return err;
}

esp_err_t display_show(const img_reader_t *r, display_kind_t kind)
{
	return show(r, kind, XY_PROGRAM_PLAYLIST);
}

esp_err_t display_live(const img_reader_t *r)
{
	return show(r, DISPLAY_AUTO, XY_PROGRAM_LIVE);
}

typedef struct {
	const uint8_t *data;
	size_t len, pos;
} mem_in_t;

static int mem_read(void *ctx, uint8_t *buf, size_t len)
{
	mem_in_t *m = ctx;
	size_t k = m->len - m->pos < len ? m->len - m->pos : len;
	memcpy(buf, m->data + m->pos, k);
	m->pos += k;
	return (int)k;
}

struct display_live_frame {
	encoded_t e;
};

esp_err_t display_live_encode(const uint8_t *data, size_t len, display_live_frame_t **out)
{
	display_live_frame_t *f = calloc(1, sizeof *f);
	if (!f)
		return image_error("frame", IMG_ERR_NOMEM);
	mem_in_t m = {data, len, 0};
	img_reader_t r = {mem_read, &m};
	esp_err_t err = encode(&r, DISPLAY_AUTO, NULL, &f->e);
	if (err != ESP_OK) {
		display_live_free(f);
		return err;
	}
	*out = f;
	return ESP_OK;
}

uint32_t display_live_encode_us(const display_live_frame_t *f)
{
	return f->e.decode_us + f->e.encode_us;
}

void display_live_free(display_live_frame_t *f)
{
	if (f) {
		encoded_free(&f->e);
		free(f);
	}
}

esp_err_t display_live_send(display_live_frame_t *f)
{
	esp_err_t err = send_checks(true);
	if (err == ESP_OK) {
		xSemaphoreTake(d.work, portMAX_DELAY);
		int64_t t0 = esp_timer_get_time();
		err = send_encoded(&f->e, XY_PROGRAM_LIVE, false);
		uint32_t sent_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
		xSemaphoreGive(d.work);
		if (err == ESP_OK) {
			uint32_t sel, data;
			xy_panel_last_timing(&sel, &data);
			taskENTER_CRITICAL(&live_lock);
			display_live_stats_t *st = &d.live;
			st->frames++;
			st->decode_us += f->e.decode_us;
			st->encode_us += f->e.encode_us;
			st->select_ms += sel;
			st->data_ms += data;
			st->bytes += f->e.bytes;
			if (sent_ms > st->max_send_ms)
				st->max_send_ms = sent_ms;
			taskEXIT_CRITICAL(&live_lock);
		}
	}
	display_live_free(f);
	return err;
}

// ---- Playlist ----

struct display_playlist {
	int count;
	encoded_t items[PLAYLIST_MAX];
};

esp_err_t display_playlist_begin(display_playlist_t **out)
{
	esp_err_t err = send_checks(false);
	if (err != ESP_OK)
		return err;
	display_playlist_t *pl = calloc(1, sizeof *pl);
	if (!pl)
		return ESP_ERR_NO_MEM;
	xSemaphoreTake(d.work, portMAX_DELAY);
	*out = pl;
	return ESP_OK;
}

esp_err_t display_playlist_add(display_playlist_t *pl, const img_reader_t *r)
{
	if (pl->count == PLAYLIST_MAX) {
		snprintf(d.error, sizeof d.error, "playlist: at most %d items", PLAYLIST_MAX);
		return DISPLAY_ERR_IMAGE;
	}
	char path[40];
	snprintf(path, sizeof path, SCRATCH "/item%02d.gif", pl->count);
	esp_err_t err = encode(r, DISPLAY_AUTO, path, &pl->items[pl->count]);
	if (err != ESP_OK) {
		char msg[sizeof d.error];
		strlcpy(msg, d.error, sizeof msg);
		int k = snprintf(d.error, sizeof d.error, "playlist item %d: ", pl->count + 1);
		strlcpy(d.error + k, msg, sizeof d.error - (size_t)k);
		return err;
	}
	pl->count++;
	return ESP_OK;
}

esp_err_t display_playlist_finish(display_playlist_t *pl, bool send, int *items)
{
	esp_err_t err = ESP_OK;
	if (items)
		*items = pl->count;
	if (send && (err = send_checks(false)) == ESP_OK) {
		for (int i = 0; i < pl->count && err == ESP_OK; i++) {
			err = send_encoded(&pl->items[i], XY_PROGRAM_PLAYLIST, i > 0);
			if (err != ESP_OK)
				ESP_LOGE(TAG, "playlist item %d/%d: %s", i + 1, pl->count, xy_err_name(err));
		}
		if (err == ESP_OK)
			ESP_LOGI(TAG, "✓ sent playlist of %d item(s)", pl->count);
	}
	for (int i = 0; i < pl->count; i++)
		encoded_free(&pl->items[i]);
	free(pl);
	xSemaphoreGive(d.work);
	return err;
}

// ---- Screen and playlist commands ----

esp_err_t display_clear_screen(void)
{
	esp_err_t err = send_checks(true);
	if (err != ESP_OK)
		return err;
	xSemaphoreTake(d.work, portMAX_DELAY);
	img_target_t t = target();
	img_frames_t *frames = img_frames_new_mem(&t, 1);
	encoded_t e = {0};
	img_writer_t w = {mem_write, &e.mem};
	img_gif_info_t gi;
	if (!frames || img_black_frame(frames) != IMG_OK || img_encode_gif(frames, &w, &gi) != IMG_OK) {
		err = image_error("black frame", IMG_ERR_NOMEM);
	} else {
		e.static_image = true;
		e.frames = 1;
		err = send_encoded(&e, XY_PROGRAM_LIVE, false);
	}
	img_frames_free(frames);
	encoded_free(&e);
	xSemaphoreGive(d.work);
	return err;
}

esp_err_t display_stream_resume(void)
{
	return xy_panel_resume();
}

esp_err_t display_resume(void)
{
	if (atomic_load(&d.streaming))
		return DISPLAY_ERR_STREAMING;
	return xy_panel_resume();
}

esp_err_t display_clear_playlist(void)
{
	if (atomic_load(&d.streaming))
		return DISPLAY_ERR_STREAMING;
	return xy_panel_clear_playlist();
}

// ---- Brightness ----

esp_err_t display_set_brightness(uint8_t level)
{
	esp_err_t err = xy_panel_set_brightness(level);
	if (err != ESP_OK)
		return err;
	d.bright = level;
	if (settings_save_brightness(level) != ESP_OK)
		ESP_LOGW(TAG, "could not save brightness");
	return ESP_OK;
}

esp_err_t display_set_brightness_unless_off(uint8_t level)
{
	if (d.manual_off)
		return DISPLAY_ERR_MANUAL_OFF;
	return display_set_brightness(level);
}

esp_err_t display_turn_off(void)
{
	esp_err_t err = display_set_brightness(OFF_LEVEL);
	if (err == ESP_OK)
		d.manual_off = true;
	return err;
}

esp_err_t display_turn_on(uint8_t level)
{
	d.manual_off = false;
	return display_set_brightness(level);
}

uint8_t display_brightness(void)
{
	return d.bright;
}

esp_err_t display_power(bool on)
{
	return xy_panel_power(on);
}

esp_err_t display_raw(const uint8_t *body, size_t len)
{
	return xy_panel_raw(body, len);
}

// ---- Connection ----

bool display_connected(void)
{
	return xy_panel_connected();
}

void display_set_streaming(bool on)
{
	if (on && !atomic_load(&d.streaming)) {
		taskENTER_CRITICAL(&live_lock);
		d.live = (display_live_stats_t){0};
		taskEXIT_CRITICAL(&live_lock);
	}
	atomic_store(&d.streaming, on);
}

void display_live_stats(display_live_stats_t *out)
{
	taskENTER_CRITICAL(&live_lock);
	*out = d.live;
	taskEXIT_CRITICAL(&live_lock);
}

bool display_streaming(void)
{
	return atomic_load(&d.streaming);
}

// after_connect restores the brightness: the handshake wakes the screen.
static void after_connect(uint8_t fallback)
{
	uint8_t level = settings_brightness();
	if (level == 0)
		level = fallback;
	if (xy_panel_set_brightness(level) == ESP_OK) {
		d.bright = level;
		ESP_LOGI(TAG, "brightness %u", level);
	}
}

esp_err_t display_connect(void)
{
	if (xy_panel_connected())
		return ESP_OK;
	xy_panel_config_t c;
	panel_config(&c);
	esp_err_t err = xy_panel_connect(&c, CONNECT_TIMEOUT_MS);
	if (err == ESP_OK && xy_panel_set_brightness(80) == ESP_OK)
		d.bright = 80;
	return err;
}

void display_disconnect(void)
{
	xy_panel_disconnect();
}

void display_reconnect(void)
{
	atomic_store(&d.reconnect, true);
}

// monitor connects at boot and whenever the link is lost, with
// exponential backoff, like the Go connectionMonitor.
static void monitor(void *arg)
{
	uint32_t delay = RETRY_MIN_MS;
	bool first = true;
	for (;;) {
		if (atomic_exchange(&d.reconnect, false) && xy_panel_connected()) {
			ESP_LOGI(TAG, "reconnecting");
			xy_panel_disconnect();
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
		if (!xy_panel_connected()) {
			if (!first)
				ESP_LOGW(TAG, "⚠ panel not connected, connecting...");
			xy_panel_config_t c;
			panel_config(&c);
			xy_panel_set_debug(c.debug);
			esp_err_t err = xy_panel_connect(&c, CONNECT_TIMEOUT_MS);
			if (err == ESP_OK) {
				ESP_LOGI(TAG, "✓ connected to the panel");
				// Go: 125 on the first connect, 80 after a reconnect,
				// unless a level was saved.
				after_connect(first ? 125 : 80);
				first = false;
				delay = RETRY_MIN_MS;
			} else {
				ESP_LOGW(TAG, "connect failed: %s; retrying in %u s", xy_err_name(err),
					 (unsigned)(delay / 1000));
				vTaskDelay(pdMS_TO_TICKS(delay));
				delay = delay * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : delay * 2;
				continue;
			}
		}
		xy_panel_set_debug(settings_get()->debug_ble);
		for (int i = 0; i < MONITOR_MS / 250 && !atomic_load(&d.reconnect); i++)
			vTaskDelay(pdMS_TO_TICKS(250));
	}
}

esp_err_t display_init(void)
{
	d.work = xSemaphoreCreateMutex();
	d.bright = 125;
	if (!d.work)
		return ESP_ERR_NO_MEM;
	esp_err_t err = xy_panel_init();
	if (err != ESP_OK)
		return err;
	return xTaskCreate(monitor, "panel_monitor", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
