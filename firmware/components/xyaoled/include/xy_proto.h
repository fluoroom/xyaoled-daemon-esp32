// XYAOLED panel protocol: frame builders and parsers, with no I/O.
// See reference/go-daemon/xyaoled/README.md for the protocol itself; this
// is a port of the Go driver (xyaoled/device.go, xyaoled/upload.go).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Programs an upload can go to.
#define XY_PROGRAM_PLAYLIST 0x02 // appended to the on-device playlist
#define XY_PROGRAM_LIVE 0x03     // shown at once, not stored

#define XY_MAX_BRIGHTNESS 125

#define XY_MAX_PACKET 10000
#define XY_FIRST_HEADER 24
#define XY_NEXT_HEADER 16
#define XY_PACKET_FIRST 0x07
#define XY_PACKET_MIDDLE 0x08
#define XY_PACKET_LAST 0x09
#define XY_ACK_STATUS_MIN_OK 0x14
#define XY_MAX_PACKET_RESENDS 3

// Timing, in milliseconds.
#define XY_INIT_DELAY_MS 300
#define XY_COMMAND_DELAY_MS 30
#define XY_SELECT_RETRY_MS 1000
#define XY_PANEL_BUSY_TIMEOUT_MS 60000
#define XY_UPLOAD_APPLIED_TIMEOUT_MS 5000
#define XY_CLEAR_SETTLE_MS 500

// Longest command frame built here (the handshake).
#define XY_MAX_COMMAND 32

// The connect handshake: five fixed frames sent in order, with
// XY_INIT_DELAY_MS after the first and XY_COMMAND_DELAY_MS after the rest.
#define XY_INIT_FRAMES 5
extern const uint8_t *const xy_init_frames[XY_INIT_FRAMES];
extern const size_t xy_init_frame_lens[XY_INIT_FRAMES];

// Command builders. Each writes a complete frame into out (at least
// XY_MAX_COMMAND bytes) and returns its length.
size_t xy_cmd_brightness(uint8_t *out, uint8_t seq, uint8_t level);
size_t xy_cmd_power(uint8_t *out, uint8_t seq, bool on);
size_t xy_cmd_clear_playlist(uint8_t *out, uint8_t seq);
size_t xy_cmd_resume(uint8_t *out, uint8_t seq);
size_t xy_cmd_select_program(uint8_t *out, uint8_t program, uint8_t seq);

// xy_cmd_raw wraps body (the command from its type byte on) in magic and
// length. out must hold 8 + body_len bytes. Returns 0 if it would not fit
// the 16-bit length field.
size_t xy_cmd_raw(uint8_t *out, const uint8_t *body, size_t body_len);

// Animation trailer: the 8 bytes after the packet count in the first upload
// packet. static_image selects the vendor's fixed static trailer; otherwise
// frames and total_ms (sum of frame delays) describe the GIF.
void xy_trailer(uint8_t out[8], bool static_image, uint32_t frames, uint32_t total_ms);

// Upload packet layout. An upload of total payload bytes is split into
// xy_packet_count(total) packets; packet i carries payload bytes
// [*offset, *offset + *len) after a header of xy_packet_header_len(i) bytes.
uint32_t xy_packet_count(uint32_t total);
size_t xy_packet_header_len(uint32_t i);
void xy_packet_span(uint32_t total, uint32_t i, uint32_t *offset, uint32_t *len);

// xy_packet_header writes packet i's header into out (XY_FIRST_HEADER
// bytes at most) and returns its length.
size_t xy_packet_header(uint8_t *out, uint32_t total, uint32_t i, uint8_t program, uint8_t seq,
			const uint8_t trailer[8]);

// Ack timeout for a packet of packet_len bytes: 200 ms per 250 bytes + 200 ms.
uint32_t xy_ack_timeout_ms(size_t packet_len);

// Notifications (ae02).
typedef struct {
	uint16_t index; // 0 acks a program select, n acks packet n
	uint8_t ctr;    // notification counter; a repeat is a duplicate
	uint8_t status; // >= XY_ACK_STATUS_MIN_OK is OK
} xy_ack_t;

bool xy_parse_ack(const uint8_t *b, size_t len, xy_ack_t *ack);
// 88ff05: an upload was stored, or the panel is not in receive mode.
bool xy_is_not_ready(const uint8_t *b, size_t len);
// The last byte is the low byte of the sum of the others.
bool xy_checksum_ok(const uint8_t *b, size_t len);

typedef struct {
	uint16_t width, height;
	uint8_t brightness;
} xy_panel_info_t;

// 88ff00: handshake accepted, with the panel's size and brightness.
bool xy_parse_handshake(const uint8_t *b, size_t len, xy_panel_info_t *info);
// 88ff0f: handshake rejected; the panel disconnects.
bool xy_is_handshake_rejected(const uint8_t *b, size_t len);
