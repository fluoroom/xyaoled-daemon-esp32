#include "xy_panel.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "xy_panel";

#define PREFERRED_MTU 517
#define MAX_WRITE 512 // the vendor app's chunk size
#define NOTIFY_MAX 64
#define NOTIFY_DEPTH 16
#define OP_TIMEOUT_MS 10000
#define WRITE_RETRY_MS 3000

#define SYNCED_BIT BIT0

typedef struct {
	uint8_t len; // 0: the link dropped
	uint8_t data[NOTIFY_MAX];
} notify_t;

static struct {
	SemaphoreHandle_t lock; // everything written to the panel
	SemaphoreHandle_t op_done;
	EventGroupHandle_t events;
	QueueHandle_t notify;
	int op_status;

	uint8_t own_addr_type;
	volatile uint16_t conn;
	bool connected;
	ble_addr_t peer;
	bool have_peer;
	uint16_t mtu;
	uint16_t svc_start, svc_end;
	uint16_t write_handle, notify_handle, notify_end, cccd_handle;

	const xy_panel_config_t *scan_cfg;

	uint8_t seq;
	uint8_t brightness;
	uint8_t last_ack_ctr;
	bool have_last_ack_ctr;
	xy_panel_info_t info;
	bool have_info;
	bool debug;
} p = {.conn = BLE_HS_CONN_HANDLE_NONE};

const char *xy_err_name(esp_err_t err)
{
	switch (err) {
	case XY_ERR_NOT_CONNECTED:
		return "device not connected";
	case XY_ERR_NOT_FOUND:
		return "scan timeout: no XYAOLED device found";
	case XY_ERR_HANDSHAKE:
		return "panel rejected the handshake";
	case XY_ERR_BUSY:
		return "panel busy: program select not acknowledged";
	case XY_ERR_NO_ACK:
		return "no ack from device";
	case XY_ERR_REJECTED:
		return "device rejected a packet";
	case XY_ERR_NOT_READY:
		return "panel is not accepting data";
	case XY_ERR_GATT:
		return "panel service or characteristics not found";
	}
	return esp_err_to_name(err);
}

static uint32_t now_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

static void dump(const char *dir, const uint8_t *b, size_t len)
{
	if (!p.debug)
		return;
	ESP_LOGI(TAG, "%s %u bytes", dir, (unsigned)len);
	ESP_LOG_BUFFER_HEX(TAG, b, len);
}

// ---- GAP and GATT callbacks (NimBLE host task) ----

static void op_finish(int status)
{
	p.op_status = status;
	xSemaphoreGive(p.op_done);
}

static bool op_wait(uint32_t timeout_ms)
{
	return xSemaphoreTake(p.op_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static bool name_matches(const uint8_t *name, size_t len, const char *want)
{
	size_t wl = strlen(want);
	if (wl == 0 || len < wl)
		return false;
	for (size_t i = 0; i + wl <= len; i++) {
		size_t k = 0;
		while (k < wl && toupper(name[i + k]) == toupper((unsigned char)want[k]))
			k++;
		if (k == wl)
			return true;
	}
	return false;
}

static bool scan_matches(const struct ble_gap_disc_desc *d)
{
	const xy_panel_config_t *cfg = p.scan_cfg;
	if (cfg->by_mac) {
		for (int i = 0; i < 6; i++)
			if (d->addr.val[i] != cfg->mac[5 - i])
				return false;
		return true;
	}
	struct ble_hs_adv_fields f;
	if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0 || !f.name)
		return false;
	if (p.debug)
		ESP_LOGI(TAG, "found '%.*s'", f.name_len, f.name);
	return name_matches(f.name, f.name_len, cfg->name_match);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
	switch (ev->type) {
	case BLE_GAP_EVENT_DISC:
		if (p.scan_cfg && scan_matches(&ev->disc)) {
			p.peer = ev->disc.addr;
			p.have_peer = true;
			p.scan_cfg = NULL;
			ble_gap_disc_cancel();
			op_finish(0);
		}
		return 0;
	case BLE_GAP_EVENT_DISC_COMPLETE:
		if (p.scan_cfg) {
			p.scan_cfg = NULL;
			op_finish(XY_ERR_NOT_FOUND);
		}
		return 0;
	case BLE_GAP_EVENT_CONNECT:
		if (ev->connect.status == 0)
			p.conn = ev->connect.conn_handle;
		op_finish(ev->connect.status == 0 ? 0 : ESP_FAIL);
		return 0;
	case BLE_GAP_EVENT_DISCONNECT: {
		ESP_LOGW(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
		p.conn = BLE_HS_CONN_HANDLE_NONE;
		p.connected = false;
		notify_t n = {.len = 0};
		xQueueSend(p.notify, &n, 0); // wakes anyone waiting for an ack
		return 0;
	}
	case BLE_GAP_EVENT_NOTIFY_RX: {
		if (ev->notify_rx.attr_handle != p.notify_handle)
			return 0;
		notify_t n;
		uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
		if (len > NOTIFY_MAX)
			len = NOTIFY_MAX;
		if (len == 0 || ble_hs_mbuf_to_flat(ev->notify_rx.om, n.data, len, NULL) != 0)
			return 0;
		n.len = (uint8_t)len;
		dump("<--", n.data, len);
		// Only recent notifications matter: drop when full, as the Go
		// driver does.
		xQueueSend(p.notify, &n, 0);
		return 0;
	}
	case BLE_GAP_EVENT_MTU:
		p.mtu = ev->mtu.value;
		return 0;
	}
	return 0;
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
	if (err->status == 0)
		p.mtu = mtu;
	op_finish(err->status);
	return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
	if (err->status == 0) {
		p.svc_start = svc->start_handle;
		p.svc_end = svc->end_handle;
		return 0;
	}
	op_finish(err->status == BLE_HS_EDONE ? 0 : err->status);
	return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
	if (err->status == 0) {
		if (ble_uuid_cmp(&chr->uuid.u, BLE_UUID16_DECLARE(0xae01)) == 0)
			p.write_handle = chr->val_handle;
		else if (ble_uuid_cmp(&chr->uuid.u, BLE_UUID16_DECLARE(0xae02)) == 0)
			p.notify_handle = chr->val_handle;
		else if (p.notify_handle && !p.notify_end)
			p.notify_end = chr->def_handle - 1; // the next characteristic bounds ae02's descriptors
		return 0;
	}
	op_finish(err->status == BLE_HS_EDONE ? 0 : err->status);
	return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
		  void *arg)
{
	if (err->status == 0) {
		if (!p.cccd_handle && ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0)
			p.cccd_handle = dsc->handle;
		return 0;
	}
	op_finish(err->status == BLE_HS_EDONE ? 0 : err->status);
	return 0;
}

static int write_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
	op_finish(err->status);
	return 0;
}

static void on_sync(void)
{
	ble_hs_util_ensure_addr(0);
	ble_hs_id_infer_auto(0, &p.own_addr_type);
	xEventGroupSetBits(p.events, SYNCED_BIT);
	ESP_LOGI(TAG, "BLE host ready");
}

static void on_reset(int reason)
{
	xEventGroupClearBits(p.events, SYNCED_BIT);
	ESP_LOGW(TAG, "BLE host reset (reason %d)", reason);
}

static void host_task(void *arg)
{
	nimble_port_run();
	nimble_port_freertos_deinit();
}

esp_err_t xy_panel_init(void)
{
	p.lock = xSemaphoreCreateMutex();
	p.op_done = xSemaphoreCreateBinary();
	p.events = xEventGroupCreate();
	p.notify = xQueueCreate(NOTIFY_DEPTH, sizeof(notify_t));
	if (!p.lock || !p.op_done || !p.events || !p.notify)
		return ESP_ERR_NO_MEM;
	esp_err_t err = nimble_port_init();
	if (err != ESP_OK)
		return err;
	ble_hs_cfg.sync_cb = on_sync;
	ble_hs_cfg.reset_cb = on_reset;
	ble_att_set_preferred_mtu(PREFERRED_MTU);
	nimble_port_freertos_init(host_task);
	return ESP_OK;
}

void xy_panel_set_debug(bool debug)
{
	p.debug = debug;
}

// ---- Writing ----

static size_t att_payload(void)
{
	uint16_t mtu = p.conn != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(p.conn) : 0;
	if (mtu <= 3)
		return 20;
	size_t n = mtu - 3u;
	return n > MAX_WRITE ? MAX_WRITE : n;
}

// write_raw sends one ATT write without response, waiting for buffers when
// the stack is out of them.
static esp_err_t write_raw(const uint8_t *data, size_t len)
{
	if (!p.connected && p.conn == BLE_HS_CONN_HANDLE_NONE)
		return XY_ERR_NOT_CONNECTED;
	uint32_t start = now_ms();
	for (;;) {
		uint16_t conn = p.conn;
		if (conn == BLE_HS_CONN_HANDLE_NONE)
			return XY_ERR_NOT_CONNECTED;
		int rc = ble_gattc_write_no_rsp_flat(conn, p.write_handle, data, (uint16_t)len);
		if (rc == 0)
			return ESP_OK;
		if ((rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) || now_ms() - start > WRITE_RETRY_MS) {
			ESP_LOGW(TAG, "write failed: %d", rc);
			return rc == BLE_HS_ENOTCONN ? XY_ERR_NOT_CONNECTED : ESP_FAIL;
		}
		vTaskDelay(pdMS_TO_TICKS(2));
	}
}

// write_frame writes a protocol frame split into ATT-sized writes.
static esp_err_t write_frame(const uint8_t *frame, size_t len)
{
	dump("-->", frame, len);
	size_t chunk = att_payload();
	for (size_t off = 0; off < len; off += chunk) {
		size_t n = len - off < chunk ? len - off : chunk;
		esp_err_t err = write_raw(frame + off, n);
		if (err != ESP_OK)
			return err;
	}
	return ESP_OK;
}

static void drain(void)
{
	notify_t n;
	while (xQueueReceive(p.notify, &n, 0) == pdTRUE) {
	}
}

static uint8_t next_seq(void)
{
	return p.seq++;
}

// ---- Connecting ----

static void op_reset(void)
{
	xSemaphoreTake(p.op_done, 0);
}

// gatt_op waits for the procedure rc started. Use through GATT_OP, which
// clears a result a timed-out procedure may have left behind before
// starting the next one.
static esp_err_t gatt_op(int rc)
{
	if (rc != 0)
		return ESP_FAIL;
	if (!op_wait(OP_TIMEOUT_MS))
		return ESP_ERR_TIMEOUT;
	return p.op_status == 0 ? ESP_OK : ESP_FAIL;
}

#define GATT_OP(call) (op_reset(), gatt_op(call))

// initialize runs the connect handshake and reads the panel's answer.
static esp_err_t initialize(void)
{
	drain();
	for (int i = 0; i < XY_INIT_FRAMES; i++) {
		esp_err_t err = write_frame(xy_init_frames[i], xy_init_frame_lens[i]);
		if (err != ESP_OK)
			return err;
		vTaskDelay(pdMS_TO_TICKS(i == 0 ? XY_INIT_DELAY_MS : XY_COMMAND_DELAY_MS));
		if (i == 0) {
			notify_t n;
			while (xQueueReceive(p.notify, &n, 0) == pdTRUE) {
				if (n.len == 0 || xy_is_handshake_rejected(n.data, n.len))
					return n.len == 0 ? XY_ERR_NOT_CONNECTED : XY_ERR_HANDSHAKE;
				if (xy_parse_handshake(n.data, n.len, &p.info)) {
					p.have_info = true;
					p.brightness = p.info.brightness;
					ESP_LOGI(TAG, "panel %ux%u, brightness %u", p.info.width, p.info.height,
						 p.info.brightness);
				}
			}
		}
	}
	return ESP_OK;
}

static esp_err_t connect_locked(const xy_panel_config_t *cfg, uint32_t timeout_ms)
{
	if (p.connected)
		return ESP_OK;
	if (!(xEventGroupWaitBits(p.events, SYNCED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000)) & SYNCED_BIT))
		return ESP_ERR_INVALID_STATE;
	p.debug = cfg->debug;
	op_reset();

	struct ble_gap_disc_params disc = {.passive = 0, .filter_duplicates = 0};
	p.scan_cfg = cfg;
	ESP_LOGI(TAG, "scanning for %s%s (%u ms)", cfg->by_mac ? "MAC " : "a name containing ",
		 cfg->by_mac ? "" : cfg->name_match, (unsigned)timeout_ms);
	if (ble_gap_disc(p.own_addr_type, (int32_t)timeout_ms, &disc, gap_event, NULL) != 0) {
		p.scan_cfg = NULL;
		return ESP_FAIL;
	}
	if (!op_wait(timeout_ms + 2000)) {
		p.scan_cfg = NULL;
		ble_gap_disc_cancel();
		return XY_ERR_NOT_FOUND;
	}
	if (p.op_status != 0)
		return p.op_status;

	char addr[18];
	xy_panel_address(addr);
	ESP_LOGI(TAG, "connecting to %s", addr);
	struct ble_gap_conn_params cp = {
		.scan_itvl = 0x10, .scan_window = 0x10, .itvl_min = 12, .itvl_max = 24,
		.latency = 0, .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0,
	};
	esp_err_t err = GATT_OP(ble_gap_connect(p.own_addr_type, &p.peer, OP_TIMEOUT_MS, &cp, gap_event, NULL));
	if (err != ESP_OK)
		return err;

	p.write_handle = p.notify_handle = p.notify_end = p.cccd_handle = 0;
	p.svc_start = p.svc_end = 0;
	if ((err = GATT_OP(ble_gattc_exchange_mtu(p.conn, mtu_cb, NULL))) != ESP_OK)
		ESP_LOGW(TAG, "MTU exchange failed, using %u", ble_att_mtu(p.conn));
	ble_gap_set_data_len(p.conn, 251, 2120);

	if ((err = GATT_OP(ble_gattc_disc_svc_by_uuid(p.conn, BLE_UUID16_DECLARE(0xae00), svc_cb, NULL))) != ESP_OK ||
	    !p.svc_start)
		goto gatt_fail;
	if ((err = GATT_OP(ble_gattc_disc_all_chrs(p.conn, p.svc_start, p.svc_end, chr_cb, NULL))) != ESP_OK ||
	    !p.write_handle || !p.notify_handle)
		goto gatt_fail;
	uint16_t end = p.notify_end ? p.notify_end : p.svc_end;
	if (end > p.notify_handle &&
	    GATT_OP(ble_gattc_disc_all_dscs(p.conn, p.notify_handle, end, dsc_cb, NULL)) != ESP_OK)
		ESP_LOGW(TAG, "descriptor discovery failed");
	if (!p.cccd_handle)
		p.cccd_handle = p.notify_handle + 1;
	static const uint8_t enable[2] = {0x01, 0x00};
	if ((err = GATT_OP(ble_gattc_write_flat(p.conn, p.cccd_handle, enable, sizeof enable, write_cb, NULL))) !=
	    ESP_OK)
		goto gatt_fail;

	p.connected = true;
	ESP_LOGI(TAG, "connected, MTU %u, writes of %u bytes", ble_att_mtu(p.conn), (unsigned)att_payload());
	if ((err = initialize()) != ESP_OK) {
		ESP_LOGE(TAG, "handshake failed: %s", xy_err_name(err));
		ble_gap_terminate(p.conn, BLE_ERR_REM_USER_CONN_TERM);
		p.connected = false;
		return err;
	}
	return ESP_OK;

gatt_fail:
	ESP_LOGE(TAG, "GATT setup failed");
	ble_gap_terminate(p.conn, BLE_ERR_REM_USER_CONN_TERM);
	return XY_ERR_GATT;
}

esp_err_t xy_panel_connect(const xy_panel_config_t *cfg, uint32_t timeout_ms)
{
	xSemaphoreTake(p.lock, portMAX_DELAY);
	esp_err_t err = connect_locked(cfg, timeout_ms);
	xSemaphoreGive(p.lock);
	return err;
}

void xy_panel_disconnect(void)
{
	xSemaphoreTake(p.lock, portMAX_DELAY);
	uint16_t conn = p.conn;
	p.connected = false;
	if (conn != BLE_HS_CONN_HANDLE_NONE) {
		ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
		for (int i = 0; i < 50 && p.conn != BLE_HS_CONN_HANDLE_NONE; i++)
			vTaskDelay(pdMS_TO_TICKS(20));
	}
	xSemaphoreGive(p.lock);
}

bool xy_panel_connected(void)
{
	if (!p.connected)
		return false;
	struct ble_gap_conn_desc d;
	return p.conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(p.conn, &d) == 0;
}

bool xy_panel_info(xy_panel_info_t *info)
{
	if (p.have_info)
		*info = p.info;
	return p.have_info;
}

void xy_panel_address(char out[18])
{
	const uint8_t *v = p.peer.val;
	if (!p.have_peer) {
		strcpy(out, "");
		return;
	}
	snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", v[5], v[4], v[3], v[2], v[1], v[0]);
}

// ---- Commands ----

static esp_err_t command(size_t (*build)(uint8_t *, uint8_t), const char *what)
{
	uint8_t b[XY_MAX_COMMAND];
	xSemaphoreTake(p.lock, portMAX_DELAY);
	esp_err_t err = p.connected ? write_frame(b, build(b, next_seq())) : XY_ERR_NOT_CONNECTED;
	xSemaphoreGive(p.lock);
	if (err != ESP_OK)
		ESP_LOGW(TAG, "%s failed: %s", what, xy_err_name(err));
	return err;
}

esp_err_t xy_panel_set_brightness(uint8_t level)
{
	if (level > XY_MAX_BRIGHTNESS)
		level = XY_MAX_BRIGHTNESS;
	uint8_t b[XY_MAX_COMMAND];
	xSemaphoreTake(p.lock, portMAX_DELAY);
	esp_err_t err = p.connected ? write_frame(b, xy_cmd_brightness(b, next_seq(), level)) : XY_ERR_NOT_CONNECTED;
	if (err == ESP_OK)
		p.brightness = level;
	xSemaphoreGive(p.lock);
	return err;
}

uint8_t xy_panel_brightness(void)
{
	return p.brightness;
}

static size_t build_power_on(uint8_t *b, uint8_t seq)
{
	return xy_cmd_power(b, seq, true);
}

static size_t build_power_off(uint8_t *b, uint8_t seq)
{
	return xy_cmd_power(b, seq, false);
}

esp_err_t xy_panel_power(bool on)
{
	return command(on ? build_power_on : build_power_off, "power");
}

esp_err_t xy_panel_clear_playlist(void)
{
	return command(xy_cmd_clear_playlist, "clear playlist");
}

esp_err_t xy_panel_resume(void)
{
	return command(xy_cmd_resume, "resume");
}

esp_err_t xy_panel_reinit(void)
{
	xSemaphoreTake(p.lock, portMAX_DELAY);
	esp_err_t err = p.connected ? initialize() : XY_ERR_NOT_CONNECTED;
	xSemaphoreGive(p.lock);
	return err;
}

esp_err_t xy_panel_raw(const uint8_t *body, size_t len)
{
	uint8_t frame[256];
	if (len + 8 > sizeof frame)
		return ESP_ERR_INVALID_SIZE;
	size_t n = xy_cmd_raw(frame, body, len);
	xSemaphoreTake(p.lock, portMAX_DELAY);
	esp_err_t err = XY_ERR_NOT_CONNECTED;
	if (p.connected) {
		drain();
		err = write_frame(frame, n);
	}
	xSemaphoreGive(p.lock);
	return err;
}

// ---- Uploads ----

static int mem_read_at(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
	memcpy(buf, (const uint8_t *)ctx + off, len);
	return 0;
}

void xy_blob_mem(xy_blob_t *b, const uint8_t *data, uint32_t len)
{
	b->len = len;
	b->read_at = mem_read_at;
	b->ctx = (void *)data;
}

static int file_read_at(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
	FILE *f = ctx;
	if (fseek(f, (long)off, SEEK_SET) != 0 || fread(buf, 1, len, f) != len)
		return -1;
	return 0;
}

void xy_blob_file(xy_blob_t *b, void *file, uint32_t len)
{
	b->len = len;
	b->read_at = file_read_at;
	b->ctx = file;
}

// wait_ack waits for the ack with index want (0 acks the program select).
// Duplicates and acks for other indices are skipped; while waiting for a
// packet ack, 88ff05 means the panel left receive mode.
static esp_err_t wait_ack(uint16_t want, uint32_t timeout_ms, xy_ack_t *out)
{
	uint32_t start = now_ms();
	for (;;) {
		uint32_t spent = now_ms() - start;
		if (spent >= timeout_ms)
			return XY_ERR_NO_ACK;
		notify_t n;
		if (xQueueReceive(p.notify, &n, pdMS_TO_TICKS(timeout_ms - spent)) != pdTRUE)
			return XY_ERR_NO_ACK;
		if (n.len == 0)
			return XY_ERR_NOT_CONNECTED;
		if (want > 0 && xy_is_not_ready(n.data, n.len))
			return XY_ERR_NOT_READY;
		xy_ack_t a;
		if (!xy_parse_ack(n.data, n.len, &a))
			continue;
		if (p.have_last_ack_ctr && a.ctr == p.last_ack_ctr)
			continue;
		p.last_ack_ctr = a.ctr;
		p.have_last_ack_ctr = true;
		if (a.index != want)
			continue;
		if (out)
			*out = a;
		return ESP_OK;
	}
}

// wait_upload_applied waits for the 88ff05 the panel sends once it has
// stored an upload; commands sent before it can be rejected.
static void wait_upload_applied(void)
{
	uint32_t start = now_ms();
	while (now_ms() - start < XY_UPLOAD_APPLIED_TIMEOUT_MS) {
		notify_t n;
		if (xQueueReceive(p.notify, &n, pdMS_TO_TICKS(100)) != pdTRUE)
			continue;
		if (n.len == 0 || xy_is_not_ready(n.data, n.len)) {
			if (p.debug)
				ESP_LOGI(TAG, "upload applied after %u ms", (unsigned)(now_ms() - start));
			return;
		}
	}
	if (p.debug)
		ESP_LOGI(TAG, "no upload-applied notification");
}

static esp_err_t select_program(uint8_t program, uint8_t seq)
{
	uint8_t cmd[XY_MAX_COMMAND];
	size_t n = xy_cmd_select_program(cmd, program, seq);
	uint32_t start = now_ms();
	for (;;) {
		drain();
		esp_err_t err = write_frame(cmd, n);
		if (err != ESP_OK)
			return err;
		err = wait_ack(0, XY_SELECT_RETRY_MS, NULL);
		if (err == ESP_OK || err == XY_ERR_NOT_CONNECTED)
			return err;
		if (now_ms() - start > XY_PANEL_BUSY_TIMEOUT_MS)
			return XY_ERR_BUSY;
		if (p.debug)
			ESP_LOGI(TAG, "panel busy (%u s), re-sending program select",
				 (unsigned)((now_ms() - start) / 1000));
	}
}

static esp_err_t write_packet(const xy_blob_t *gif, uint32_t i, uint8_t program, uint8_t seq,
			      const uint8_t trailer[8], uint8_t *buf)
{
	uint8_t header[XY_FIRST_HEADER];
	size_t hl = xy_packet_header(header, gif->len, i, program, seq, trailer);
	uint32_t off, len;
	xy_packet_span(gif->len, i, &off, &len);
	size_t total = hl + len, chunk = att_payload();
	if (p.debug)
		ESP_LOGI(TAG, "packet %u (%u bytes)", (unsigned)i + 1, (unsigned)total);
	for (size_t pos = 0; pos < total; pos += chunk) {
		size_t n = total - pos < chunk ? total - pos : chunk, k = 0;
		if (pos < hl) {
			k = hl - pos < n ? hl - pos : n;
			memcpy(buf, header + pos, k);
		}
		if (k < n && gif->read_at(gif->ctx, off + (uint32_t)(pos + k - hl), buf + k, n - k) != 0)
			return ESP_FAIL;
		esp_err_t err = write_raw(buf, n);
		if (err != ESP_OK)
			return err;
	}
	return ESP_OK;
}

static esp_err_t upload_packets(const xy_blob_t *gif, uint8_t program, uint8_t seq, const uint8_t trailer[8])
{
	uint8_t *buf = malloc(MAX_WRITE);
	if (!buf)
		return ESP_ERR_NO_MEM;
	uint32_t n = xy_packet_count(gif->len);
	esp_err_t err = ESP_OK;
	for (uint32_t i = 0; i < n && err == ESP_OK; i++) {
		uint32_t off, len;
		xy_packet_span(gif->len, i, &off, &len);
		xy_ack_t a;
		for (int attempt = 0; attempt <= XY_MAX_PACKET_RESENDS; attempt++) {
			drain();
			if ((err = write_packet(gif, i, program, seq, trailer, buf)) != ESP_OK)
				break;
			err = wait_ack((uint16_t)(i + 1), xy_ack_timeout_ms(xy_packet_header_len(i) + len), &a);
			if (err != XY_ERR_NO_ACK)
				break;
		}
		if (err == ESP_OK && a.status < XY_ACK_STATUS_MIN_OK) {
			ESP_LOGE(TAG, "packet %u/%u rejected (status 0x%02x)", (unsigned)i + 1, (unsigned)n, a.status);
			err = XY_ERR_REJECTED;
		}
	}
	free(buf);
	return err;
}

static esp_err_t send_attempt(const xy_blob_t *gif, const xy_send_opts_t *o)
{
	if (!p.connected)
		return XY_ERR_NOT_CONNECTED;
	bool live = o->program == XY_PROGRAM_LIVE;
	if (!live && !o->append) {
		// The clear is never acked; give the panel time to apply it or the
		// upload gets appended instead of replacing the playlist.
		uint8_t cmd[XY_MAX_COMMAND];
		drain();
		write_frame(cmd, xy_cmd_clear_playlist(cmd, next_seq()));
		vTaskDelay(pdMS_TO_TICKS(XY_CLEAR_SETTLE_MS));
	}
	uint8_t seq = next_seq(), trailer[8];
	xy_trailer(trailer, o->static_image, o->frames, o->total_ms);
	esp_err_t err;
	for (int attempt = 1;; attempt++) {
		if ((err = select_program(o->program, seq)) != ESP_OK)
			return err;
		if (p.debug)
			ESP_LOGI(TAG, "uploading %u bytes in %u packet(s)", (unsigned)gif->len,
				 (unsigned)xy_packet_count(gif->len));
		err = upload_packets(gif, o->program, seq, trailer);
		if (err != XY_ERR_NOT_READY || attempt == XY_MAX_PACKET_RESENDS)
			break;
	}
	if (err == ESP_OK && !live)
		wait_upload_applied();
	return err;
}

esp_err_t xy_panel_send_gif(const xy_blob_t *gif, const xy_send_opts_t *opts)
{
	esp_err_t err = ESP_OK;
	for (int attempt = 1; attempt <= 3; attempt++) {
		xSemaphoreTake(p.lock, portMAX_DELAY);
		err = send_attempt(gif, opts);
		xSemaphoreGive(p.lock);
		if (err == ESP_OK || err == XY_ERR_NOT_CONNECTED || err == XY_ERR_BUSY)
			return err;
		ESP_LOGW(TAG, "send attempt %d/3 failed: %s", attempt, xy_err_name(err));
		if (attempt < 3)
			vTaskDelay(pdMS_TO_TICKS(50 * attempt));
	}
	return err;
}
