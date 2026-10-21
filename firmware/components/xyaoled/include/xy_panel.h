// BLE connection to an XYAOLED panel over NimBLE: the ESP-IDF counterpart
// of reference/go-daemon/xyaoled/device.go and upload.go. Every call
// blocks until the panel answers or times out; one lock serialises
// everything written, since a command in the middle of an upload corrupts
// the frame being reassembled.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "xy_proto.h"

#define XY_ERR_BASE 0x7100
#define XY_ERR_NOT_CONNECTED (XY_ERR_BASE + 1)
#define XY_ERR_NOT_FOUND (XY_ERR_BASE + 2)     // scan found no panel
#define XY_ERR_HANDSHAKE (XY_ERR_BASE + 3)     // panel rejected the handshake (88ff0f)
#define XY_ERR_BUSY (XY_ERR_BASE + 4)          // program select never acknowledged
#define XY_ERR_NO_ACK (XY_ERR_BASE + 5)        // upload packet never acknowledged
#define XY_ERR_REJECTED (XY_ERR_BASE + 6)      // ack status below XY_ACK_STATUS_MIN_OK
#define XY_ERR_NOT_READY (XY_ERR_BASE + 7)     // 88ff05 instead of an ack, resends exhausted
#define XY_ERR_GATT (XY_ERR_BASE + 8)          // service or characteristics missing

const char *xy_err_name(esp_err_t err);

typedef struct {
	// Connect to the first device whose name contains name_match
	// (case-insensitive), or, when by_mac is set, to mac (as printed,
	// most significant byte first).
	char name_match[24];
	bool by_mac;
	uint8_t mac[6];
	bool debug; // log every write and notification
} xy_panel_config_t;

// xy_panel_init starts the NimBLE host. Call once, after nvs_flash_init.
esp_err_t xy_panel_init(void);

void xy_panel_set_debug(bool debug);

// xy_panel_connect scans for up to timeout_ms, connects, discovers ae00,
// subscribes to ae02 and runs the handshake.
esp_err_t xy_panel_connect(const xy_panel_config_t *cfg, uint32_t timeout_ms);
void xy_panel_disconnect(void);
bool xy_panel_connected(void);
// The panel's answer to the last handshake (size and brightness).
bool xy_panel_info(xy_panel_info_t *info);
// Peer address of the current or last connection, as printed.
void xy_panel_address(char out[18]);

esp_err_t xy_panel_set_brightness(uint8_t level);
uint8_t xy_panel_brightness(void);
esp_err_t xy_panel_power(bool on);
esp_err_t xy_panel_clear_playlist(void);
esp_err_t xy_panel_resume(void);
// Rerun the connect handshake on the open connection.
esp_err_t xy_panel_reinit(void);
// Raw command from its type byte on; magic and length are added.
esp_err_t xy_panel_raw(const uint8_t *body, size_t len);

// The GIF to upload, read in pieces: from RAM or from a file.
typedef struct {
	uint32_t len;
	// Reads len bytes at off into buf; returns 0 or -1.
	int (*read_at)(void *ctx, uint32_t off, uint8_t *buf, size_t len);
	void *ctx;
} xy_blob_t;

void xy_blob_mem(xy_blob_t *b, const uint8_t *data, uint32_t len);
// The blob reads f with fseek/fread; f stays open for the upload.
void xy_blob_file(xy_blob_t *b, void *file, uint32_t len);

typedef struct {
	uint8_t program;   // XY_PROGRAM_PLAYLIST or XY_PROGRAM_LIVE
	bool append;       // playlist: add to it instead of replacing it
	bool static_image; // static trailer
	uint32_t frames;   // animation trailer: frame count and total delay
	uint32_t total_ms;
} xy_send_opts_t;

// xy_panel_send_gif uploads a GIF: clears the playlist first unless live or
// appending, selects the program, sends the acknowledged packets and, for
// playlist uploads, waits for the panel to store it. Retried up to three
// times, except on connection errors.
esp_err_t xy_panel_send_gif(const xy_blob_t *gif, const xy_send_opts_t *opts);
