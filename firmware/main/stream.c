// /stream: every binary WebSocket message is an image shown in the live
// program, at most STREAM_FPS per second. Only the newest frame is kept,
// so a fast sender never makes the panel lag. A port of stream.go: the
// playlist is blocked from the first connect until it resumes, which
// happens stream_resume_delay after the last client leaves unless one
// comes back; one task sends both frames and the resume, so a late frame
// cannot land after it.
//
// Unlike Go, frames are encoded by a second task while the previous one
// is being sent. The panel acks the next program select at once if it
// comes right after the last data ack, but ~100 ms later if it comes once
// the panel has started drawing (measured on the panel), so encoding
// between the two (80 ms here, ~1 ms on a PC) cost almost two encodes per
// frame. The encoder starts late enough that the frame it takes is about
// as fresh as when encoding and sending took turns.
#include "stream.h"

#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"

static const char *TAG = "stream";

#define MAX_FRAME (64 * 1024)
#define MAX_CLIENTS 4

// Margin for the encoder to finish before the send in progress ends.
#define ENCODE_MARGIN_US 30000

static struct {
	SemaphoreHandle_t mu;
	TaskHandle_t task, encoder;
	esp_timer_handle_t pending; // resume countdown after the last disconnect
	uint8_t *latest;            // newest frame received, not encoded yet
	size_t latest_len;
	display_live_frame_t *ready; // encoded, waiting for the sender
	bool want;                   // the sender is ready for another frame
	uint32_t gen;                // bumped at each resume: drops frames encoded for an ended stream
	int64_t send_start_us;       // of the send in progress, 0 when idle
	int64_t send_us, encode_us;  // the last ones, for the encoder's start

	int clients;
	int fds[MAX_CLIENTS];
	bool resume;
	uint32_t interval_ms;
} s;

static uint32_t fps_interval(int fps)
{
	if (fps < 1)
		fps = 1;
	if (fps > STREAM_MAX_FPS)
		fps = STREAM_MAX_FPS;
	return 1000 / (uint32_t)fps;
}

static void set_fps(int fps)
{
	xSemaphoreTake(s.mu, portMAX_DELAY);
	s.interval_ms = fps_interval(fps);
	xSemaphoreGive(s.mu);
	ESP_LOGI(TAG, "stream rate set to %u fps", (unsigned)(1000 / fps_interval(fps)));
}

static void push(uint8_t *frame, size_t len)
{
	xSemaphoreTake(s.mu, portMAX_DELAY);
	free(s.latest); // dropped: a newer frame arrived first
	s.latest = frame;
	s.latest_len = len;
	xSemaphoreGive(s.mu);
	xTaskNotifyGive(s.encoder);
}

// encoder turns the newest frame into the panel's GIF whenever the sender
// wants one. While a send is in progress it waits until just before it
// should end, so the frame it takes is as new as possible.
static void encoder(void *arg)
{
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		xSemaphoreTake(s.mu, portMAX_DELAY);
		bool want = s.want && s.latest;
		int64_t start_at = s.send_start_us ? s.send_start_us + s.send_us - s.encode_us - ENCODE_MARGIN_US : 0;
		xSemaphoreGive(s.mu);
		if (!want)
			continue;
		int64_t wait_us = start_at - esp_timer_get_time();
		if (wait_us > 0)
			vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));

		xSemaphoreTake(s.mu, portMAX_DELAY);
		uint8_t *frame = s.latest;
		size_t len = s.latest_len;
		uint32_t gen = s.gen;
		s.latest = NULL;
		if (frame)
			s.want = false;
		xSemaphoreGive(s.mu);
		if (!frame)
			continue; // dropped by a disconnect meanwhile; wait for the next push

		display_live_frame_t *f = NULL;
		esp_err_t err = display_live_encode(frame, len, &f);
		free(frame);
		if (err != ESP_OK)
			ESP_LOGW(TAG, "frame failed: %s", display_error_text(err));

		xSemaphoreTake(s.mu, portMAX_DELAY);
		if (f) {
			s.encode_us = display_live_encode_us(f);
			if (gen == s.gen) {
				display_live_free(s.ready);
				s.ready = f;
				f = NULL;
			}
		} else {
			s.want = true; // try the next frame
		}
		xSemaphoreGive(s.mu);
		display_live_free(f);
		xTaskNotifyGive(s.task);
	}
}

static void expire(void *arg)
{
	xSemaphoreTake(s.mu, portMAX_DELAY);
	if (s.clients == 0) { // a client that reconnected just as the timer fired wins
		s.resume = true;
		s.interval_ms = fps_interval(settings_get()->stream_fps);
	}
	xSemaphoreGive(s.mu);
	xTaskNotifyGive(s.task);
}

// run sends encoded frames, at most one per interval, and the resume.
static void run(void *arg)
{
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		xSemaphoreTake(s.mu, portMAX_DELAY);
		display_live_frame_t *f = s.ready;
		bool resume = s.resume;
		uint32_t interval = s.interval_ms;
		s.ready = NULL;
		s.resume = false;
		if (resume) {
			s.gen++;
			s.want = false;
		} else {
			// Encode the next frame during this send (or now, when idle).
			s.want = true;
			s.send_start_us = f ? esp_timer_get_time() : 0;
		}
		xSemaphoreGive(s.mu);

		if (resume) {
			display_live_free(f);
			display_set_streaming(false);
			esp_err_t err = display_stream_resume();
			if (err == ESP_OK)
				ESP_LOGI(TAG, "stream ended, playlist resumed");
			else
				ESP_LOGW(TAG, "stream ended, resume failed: %s", display_error_text(err));
			continue;
		}
		xTaskNotifyGive(s.encoder);
		if (!f)
			continue;
		int64_t start = esp_timer_get_time();
		esp_err_t err = display_live_send(f);
		if (err != ESP_OK)
			ESP_LOGW(TAG, "frame failed: %s", display_error_text(err));
		int64_t spent_us = esp_timer_get_time() - start;
		xSemaphoreTake(s.mu, portMAX_DELAY);
		s.send_us = spent_us;
		s.send_start_us = 0;
		xSemaphoreGive(s.mu);
		if (spent_us / 1000 < interval)
			vTaskDelay(pdMS_TO_TICKS(interval - spent_us / 1000));
		// A frame encoded while sending is waiting; pick it up.
		xTaskNotifyGive(s.task);
	}
}

static void client_connect(int fd)
{
	xSemaphoreTake(s.mu, portMAX_DELAY);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (s.fds[i] < 0) {
			s.fds[i] = fd;
			s.clients++;
			break;
		}
	s.resume = false;
	s.want = true;
	esp_timer_stop(s.pending);
	xSemaphoreGive(s.mu);
	display_set_streaming(true);
}

void stream_on_close(int fd)
{
	xSemaphoreTake(s.mu, portMAX_DELAY);
	bool ours = false;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (s.fds[i] == fd) {
			s.fds[i] = -1;
			s.clients--;
			ours = true;
		}
	if (ours && s.clients == 0) {
		// The panel keeps showing the last frame meanwhile.
		free(s.latest);
		s.latest = NULL;
		esp_timer_stop(s.pending);
		esp_timer_start_once(s.pending, (uint64_t)settings_get()->stream_resume_ms * 1000);
	}
	xSemaphoreGive(s.mu);
	if (ours)
		ESP_LOGI(TAG, "stream client disconnected (fd %d)", fd);
}

// on_handshake runs once the WebSocket handshake is answered (ESP-IDF v6
// no longer calls the URI handler for it).
static esp_err_t on_handshake(httpd_req_t *req)
{
	int fd = httpd_req_to_sockfd(req);
	xSemaphoreTake(s.mu, portMAX_DELAY);
	bool full = s.clients >= MAX_CLIENTS;
	xSemaphoreGive(s.mu);
	if (full)
		return ESP_FAIL;
	client_connect(fd);
	char q[32], v[8];
	if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
	    httpd_query_key_value(q, "fps", v, sizeof v) == ESP_OK)
		set_fps(atoi(v));
	ESP_LOGI(TAG, "stream client connected (fd %d)", fd);
	return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
	httpd_ws_frame_t f = {0};
	esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
	if (err != ESP_OK)
		return err;
	if (f.len > MAX_FRAME) {
		ESP_LOGW(TAG, "frame of %u bytes over the %u-byte limit; closing", (unsigned)f.len, MAX_FRAME);
		return ESP_FAIL;
	}
	if (f.len == 0)
		return ESP_OK;
	uint8_t *buf = malloc(f.len + 1);
	if (!buf)
		return ESP_ERR_NO_MEM;
	f.payload = buf;
	if ((err = httpd_ws_recv_frame(req, &f, f.len)) != ESP_OK) {
		free(buf);
		return err;
	}
	if (f.type == HTTPD_WS_TYPE_BINARY) {
		push(buf, f.len);
		return ESP_OK;
	}
	if (f.type == HTTPD_WS_TYPE_TEXT) {
		buf[f.len] = 0;
		if (strncmp((char *)buf, "fps=", 4) == 0)
			set_fps(atoi((char *)buf + 4));
	}
	free(buf);
	return ESP_OK;
}

esp_err_t stream_register(httpd_handle_t hd)
{
	s.mu = xSemaphoreCreateMutex();
	for (int i = 0; i < MAX_CLIENTS; i++)
		s.fds[i] = -1;
	s.interval_ms = fps_interval(settings_get()->stream_fps);
	esp_timer_create_args_t t = {.callback = expire, .name = "stream_resume"};
	if (!s.mu || esp_timer_create(&t, &s.pending) != ESP_OK ||
	    xTaskCreate(run, "stream", 4096, NULL, 5, &s.task) != pdPASS ||
	    xTaskCreate(encoder, "stream_enc", 6144, NULL, 5, &s.encoder) != pdPASS)
		return ESP_ERR_NO_MEM;
	httpd_uri_t u = {.uri = "/stream",
			 .method = HTTP_GET,
			 .handler = ws_handler,
			 .is_websocket = true,
			 .ws_post_handshake_cb = on_handshake};
	return httpd_register_uri_handler(hd, &u);
}
