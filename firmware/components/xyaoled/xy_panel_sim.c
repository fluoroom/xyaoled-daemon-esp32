// Simulated panel for the QEMU build (CONFIG_XY_QEMU): the xy_panel API
// over xy_upload, with an in-process panel that reassembles frames from
// the writes, checks packet headers and answers like the real one (acks
// with a small delay, 88ff05 after playlist uploads). The last uploaded
// GIF is kept so tests can fetch what the panel would play.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "xy_panel.h"
#include "xy_panel_sim.h"

static const char *TAG = "xy_panel_sim";

#define NOTIFY_MAX 64
#define ACK_DELAY_MS 15

typedef struct {
	uint8_t len;
	uint8_t data[NOTIFY_MAX];
} notify_t;

static struct {
	SemaphoreHandle_t lock;
	QueueHandle_t notify;
	xy_session_t session;
	bool connected, debug;
	uint8_t brightness;
	bool power;

	// Panel side.
	uint8_t frame[XY_MAX_PACKET + 32];
	size_t have;
	uint8_t ctr, program, seq;
	uint16_t next_index;
	uint32_t total, got;
	uint8_t *upload, trailer[8];
	int stored_items;
	xy_panel_sim_stats_t stats;
	uint8_t *last;
	uint32_t last_len;
	uint8_t last_trailer[8], last_program;
} s;

const char *xy_err_name(esp_err_t err)
{
	const char *t = xy_err_text(err);
	return t ? t : esp_err_to_name(err);
}

static void notify(const uint8_t *b, size_t n)
{
	notify_t item = {.len = (uint8_t)n};
	memcpy(item.data, b, n);
	xQueueSend(s.notify, &item, 0);
}

static void ack(uint16_t idx)
{
	vTaskDelay(pdMS_TO_TICKS(ACK_DELAY_MS));
	s.ctr++;
	uint8_t b[11] = {0x88, 0xff, 0x03, (uint8_t)idx, (uint8_t)(idx >> 8), s.ctr, 0x0b, 0x00, 0xff, 0x0b, 0};
	for (int i = 0; i < 10; i++)
		b[10] += b[i];
	notify(b, sizeof b);
}

static void on_frame(const uint8_t *f, size_t n)
{
	uint8_t type = f[8];
	switch (type) {
	case 0x00: {
		static const uint8_t hs[] = {0x88, 0xff, 0x00, 0x02, 0x01, 0x04, 0x60, 0x00, 0x10,
					     0x00, 0x08, 0x01, 0x7d, 0x64, 0x06, 0x01, 0x00};
		notify(hs, sizeof hs);
		return;
	}
	case 0x04:
		s.stats.selects++;
		s.program = f[9];
		s.seq = f[10];
		s.next_index = 1;
		ack(0);
		return;
	case 0x05:
		s.stats.clears++;
		s.stored_items = 0;
		return;
	case 0x0a:
		s.stats.resumes++;
		return;
	case 0x11:
		s.power = f[11] == 0x01;
		return;
	case 0x12:
		s.brightness = f[11];
		return;
	}
	if (type < 0x07 || type > 0x09)
		return;
	uint32_t remaining = f[11] | f[12] << 8 | (uint32_t)f[13] << 16;
	size_t header = type == 0x07 ? XY_FIRST_HEADER : XY_NEXT_HEADER;
	uint16_t index = type == 0x07 ? 1 : (uint16_t)(f[14] | f[15] << 8);
	if (f[9] != s.program || f[10] != s.seq || index != s.next_index ||
	    (type != 0x07 && remaining != s.total - s.got)) {
		s.stats.header_errors++;
		ESP_LOGE(TAG, "bad packet header (type %02x index %u)", type, index);
	}
	if (type == 0x07) {
		free(s.upload);
		s.upload = malloc(remaining ? remaining : 1);
		s.total = remaining;
		s.got = 0;
		memcpy(s.trailer, f + 16, 8);
	}
	if (s.upload && s.got + (n - header) <= s.total) {
		memcpy(s.upload + s.got, f + header, n - header);
		s.got += (uint32_t)(n - header);
	}
	s.next_index = index + 1;
	s.stats.packets++;
	ack(index);
	if (s.got == s.total && s.upload) {
		free(s.last);
		s.last = s.upload;
		s.last_len = s.total;
		memcpy(s.last_trailer, s.trailer, 8);
		s.last_program = s.program;
		s.upload = NULL;
		s.stats.uploads++;
		ESP_LOGI(TAG, "panel got a %u-byte GIF for program %u", (unsigned)s.last_len, s.last_program);
		if (s.program == XY_PROGRAM_PLAYLIST) {
			const uint8_t stored[] = {0x88, 0xff, 0x05, 0x01, (uint8_t)s.stored_items++, 0, 0, 0, 0, 0x0b, 0};
			notify(stored, sizeof stored);
		}
	}
}

static int link_write(void *ctx, const uint8_t *data, size_t len)
{
	if (!s.connected)
		return XY_ERR_NOT_CONNECTED;
	if (s.have + len > sizeof s.frame)
		return XY_ERR_WRITE;
	memcpy(s.frame + s.have, data, len);
	s.have += len;
	while (s.have >= 8) {
		size_t flen = s.frame[6] | s.frame[7] << 8;
		if (flen < 8 || flen > sizeof s.frame) {
			s.have = 0;
			return XY_ERR_WRITE;
		}
		if (s.have < flen)
			break;
		on_frame(s.frame, flen);
		memmove(s.frame, s.frame + flen, s.have - flen);
		s.have -= flen;
	}
	return XY_OK;
}

static int link_recv(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
	notify_t n;
	if (xQueueReceive(s.notify, &n, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
		return 0;
	size_t k = n.len < cap ? n.len : cap;
	memcpy(buf, n.data, k);
	return (int)k;
}

static void link_sleep(void *ctx, uint32_t ms)
{
	vTaskDelay(pdMS_TO_TICKS(ms));
}

static uint32_t link_now(void *ctx)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

static size_t link_payload(void *ctx)
{
	return 512;
}

static void link_log(void *ctx, const char *fmt, ...)
{
	char line[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	ESP_LOGI(TAG, "%s", line);
}

static const xy_link_t link = {link_write, link_recv, link_sleep, link_now, link_payload, link_log, NULL};

esp_err_t xy_panel_init(void)
{
	s.lock = xSemaphoreCreateMutex();
	s.notify = xQueueCreate(16, sizeof(notify_t));
	s.brightness = 125;
	return s.lock && s.notify ? ESP_OK : ESP_ERR_NO_MEM;
}

void xy_panel_set_debug(bool debug)
{
	s.debug = debug;
}

esp_err_t xy_panel_connect(const xy_panel_config_t *cfg, uint32_t timeout_ms)
{
	xSemaphoreTake(s.lock, portMAX_DELAY);
	if (!s.connected) {
		xy_session_init(&s.session, &link);
		s.connected = true;
		s.have = 0;
		for (int i = 0; i < XY_INIT_FRAMES; i++) {
			xy_write_frame(&s.session, xy_init_frames[i], xy_init_frame_lens[i]);
			vTaskDelay(pdMS_TO_TICKS(i == 0 ? XY_INIT_DELAY_MS : XY_COMMAND_DELAY_MS));
		}
		xy_drain(&s.session);
		ESP_LOGI(TAG, "simulated panel connected (96x16)");
	}
	xSemaphoreGive(s.lock);
	return ESP_OK;
}

void xy_panel_disconnect(void)
{
	s.connected = false;
}

bool xy_panel_connected(void)
{
	return s.connected;
}

bool xy_panel_info(xy_panel_info_t *info)
{
	*info = (xy_panel_info_t){96, 16, s.brightness};
	return true;
}

void xy_panel_address(char out[18])
{
	strcpy(out, "SI:MU:LA:TE:D0:00");
}

static esp_err_t command(size_t len, const uint8_t *frame)
{
	xSemaphoreTake(s.lock, portMAX_DELAY);
	esp_err_t err = s.connected ? xy_write_frame(&s.session, frame, len) : XY_ERR_NOT_CONNECTED;
	xSemaphoreGive(s.lock);
	return err;
}

esp_err_t xy_panel_set_brightness(uint8_t level)
{
	uint8_t b[XY_MAX_COMMAND];
	return command(xy_cmd_brightness(b, xy_next_seq(&s.session), level), b);
}

uint8_t xy_panel_brightness(void)
{
	return s.brightness;
}

esp_err_t xy_panel_power(bool on)
{
	uint8_t b[XY_MAX_COMMAND];
	return command(xy_cmd_power(b, xy_next_seq(&s.session), on), b);
}

esp_err_t xy_panel_clear_playlist(void)
{
	uint8_t b[XY_MAX_COMMAND];
	return command(xy_cmd_clear_playlist(b, xy_next_seq(&s.session)), b);
}

esp_err_t xy_panel_resume(void)
{
	uint8_t b[XY_MAX_COMMAND];
	return command(xy_cmd_resume(b, xy_next_seq(&s.session)), b);
}

esp_err_t xy_panel_reinit(void)
{
	return ESP_OK;
}

esp_err_t xy_panel_raw(const uint8_t *body, size_t len)
{
	uint8_t frame[256];
	if (len + 8 > sizeof frame)
		return ESP_ERR_INVALID_SIZE;
	return command(xy_cmd_raw(frame, body, len), frame);
}

static int mem_read_at(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
	memcpy(buf, (const uint8_t *)ctx + off, len);
	return 0;
}

void xy_blob_mem(xy_blob_t *b, const uint8_t *data, uint32_t len)
{
	*b = (xy_blob_t){len, mem_read_at, (void *)data};
}

static int file_read_at(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
	return fseek(ctx, (long)off, SEEK_SET) == 0 && fread(buf, 1, len, ctx) == len ? 0 : -1;
}

void xy_blob_file(xy_blob_t *b, void *file, uint32_t len)
{
	*b = (xy_blob_t){len, file_read_at, file};
}

esp_err_t xy_panel_send_gif(const xy_blob_t *gif, const xy_send_opts_t *opts)
{
	xSemaphoreTake(s.lock, portMAX_DELAY);
	s.session.debug = s.debug;
	esp_err_t err = s.connected ? xy_upload(&s.session, gif, opts) : XY_ERR_NOT_CONNECTED;
	xSemaphoreGive(s.lock);
	return err;
}

uint16_t xy_panel_conn_interval(void)
{
	return 0;
}

void xy_panel_last_timing(uint32_t *select_ms, uint32_t *data_ms)
{
	xSemaphoreTake(s.lock, portMAX_DELAY);
	*select_ms = s.session.select_ms;
	*data_ms = s.session.data_ms;
	xSemaphoreGive(s.lock);
}

const uint8_t *xy_panel_sim_last(uint32_t *len, uint8_t trailer[8], uint8_t *program)
{
	*len = s.last_len;
	memcpy(trailer, s.last_trailer, 8);
	*program = s.last_program;
	return s.last;
}

void xy_panel_sim_stats(xy_panel_sim_stats_t *out)
{
	*out = s.stats;
	out->brightness = s.brightness;
	out->power = s.power;
}
