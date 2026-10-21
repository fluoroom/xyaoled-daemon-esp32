// Acknowledged uploads and command writes over an abstract link, so the
// logic of reference/go-daemon/xyaoled/upload.go runs (and is tested) the
// same on the host and over NimBLE.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xy_proto.h"

// Error codes; they double as esp_err_t values on the device.
#define XY_OK 0
#define XY_ERR_BASE 0x7100
#define XY_ERR_NOT_CONNECTED (XY_ERR_BASE + 1)
#define XY_ERR_NOT_FOUND (XY_ERR_BASE + 2) // scan found no panel
#define XY_ERR_HANDSHAKE (XY_ERR_BASE + 3) // panel rejected the handshake (88ff0f)
#define XY_ERR_BUSY (XY_ERR_BASE + 4)      // program select never acknowledged
#define XY_ERR_NO_ACK (XY_ERR_BASE + 5)    // upload packet never acknowledged
#define XY_ERR_REJECTED (XY_ERR_BASE + 6)  // ack status below XY_ACK_STATUS_MIN_OK
#define XY_ERR_NOT_READY (XY_ERR_BASE + 7) // 88ff05 instead of an ack, resends exhausted
#define XY_ERR_GATT (XY_ERR_BASE + 8)      // service or characteristics missing
#define XY_ERR_WRITE (XY_ERR_BASE + 9)     // the link refused a write
#define XY_ERR_READ (XY_ERR_BASE + 10)     // the GIF could not be read

// Text for XY_ERR_* codes; NULL for others.
const char *xy_err_text(int err);

typedef struct {
	// One write without response of len bytes (at most att_payload()).
	int (*write)(void *ctx, const uint8_t *data, size_t len);
	// Next notification within timeout_ms: its length (> 0), 0 on timeout,
	// < 0 when the link dropped.
	int (*recv)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
	void (*sleep_ms)(void *ctx, uint32_t ms);
	uint32_t (*now_ms)(void *ctx);
	size_t (*att_payload)(void *ctx);
	void (*log)(void *ctx, const char *fmt, ...); // debug log, may be NULL
	void *ctx;
} xy_link_t;

// Per-connection protocol state.
typedef struct {
	const xy_link_t *link;
	uint8_t seq;
	uint8_t last_ack_ctr;
	bool have_last_ack_ctr;
	bool debug;
	// Timing of the last xy_upload: program select (until acked) and data.
	uint32_t select_ms, data_ms;
} xy_session_t;

void xy_session_init(xy_session_t *s, const xy_link_t *link);
uint8_t xy_next_seq(xy_session_t *s);
// Drops pending notifications so an old ack is never taken for a new one.
// Returns < 0 if the link dropped meanwhile.
int xy_drain(xy_session_t *s);
// Writes a protocol frame split into ATT-sized writes.
int xy_write_frame(xy_session_t *s, const uint8_t *frame, size_t len);

// The GIF to upload, read in pieces: from RAM or from a file.
typedef struct {
	uint32_t len;
	// Reads len bytes at off into buf; returns 0 or -1.
	int (*read_at)(void *ctx, uint32_t off, uint8_t *buf, size_t len);
	void *ctx;
} xy_blob_t;

typedef struct {
	uint8_t program;   // XY_PROGRAM_PLAYLIST or XY_PROGRAM_LIVE
	bool append;       // playlist: add to it instead of replacing it
	bool static_image; // static trailer
	uint32_t frames;   // animation trailer: frame count and total delay
	uint32_t total_ms;
} xy_send_opts_t;

// One upload attempt (Go sendGifAttempt): clear the playlist unless live
// or appending, select the program, send the packets with their acks and
// resends, and for playlist uploads wait for the panel to store it.
int xy_upload(xy_session_t *s, const xy_blob_t *gif, const xy_send_opts_t *opts);
