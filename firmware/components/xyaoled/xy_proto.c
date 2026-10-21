#include "xy_proto.h"

#include <string.h>

static const uint8_t magic[6] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88};

// Handshake / clock sync (type 0000). The 6 bytes after "01 00" are
// checked by the panel and their derivation is unknown, so this replays a
// frame it accepts (2025-12-27 00:22:41, a Saturday).
static const uint8_t init_handshake[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x1f, 0x00, 0x00, 0x00, 0x02,
					 0x00, 0x19, 0x0c, 0x1b, 0x00, 0x16, 0x29, 0x06, 0x01, 0x00, 0xc5,
					 0x7c, 0x82, 0x2e, 0x07, 0x11, 0x00, 0x00, 0x00, 0x00};
// Power off, then on (type 0011, long form).
static const uint8_t init_power_off[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x12, 0x00, 0x11,
					 0x00, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
static const uint8_t init_power_on[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x12, 0x00, 0x11,
					0x00, 0x04, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
// Resume (type 000a) and clear playlist (type 0005).
static const uint8_t init_resume[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x0f, 0x00,
				      0x0a, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00};
static const uint8_t init_clear[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x11, 0x00, 0x05,
				     0x00, 0x06, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};

const uint8_t *const xy_init_frames[XY_INIT_FRAMES] = {init_handshake, init_power_off, init_power_on,
						       init_resume, init_clear};
const size_t xy_init_frame_lens[XY_INIT_FRAMES] = {sizeof init_handshake, sizeof init_power_off,
						   sizeof init_power_on, sizeof init_resume,
						   sizeof init_clear};

// frame writes magic and length, then body, and returns the frame length.
static size_t frame(uint8_t *out, const uint8_t *body, size_t body_len)
{
	size_t n = 8 + body_len;
	memcpy(out, magic, sizeof magic);
	out[6] = (uint8_t)n;
	out[7] = (uint8_t)(n >> 8);
	memcpy(out + 8, body, body_len);
	return n;
}

size_t xy_cmd_brightness(uint8_t *out, uint8_t seq, uint8_t level)
{
	if (level > XY_MAX_BRIGHTNESS)
		level = XY_MAX_BRIGHTNESS;
	// The second level byte is level × 0.8, truncated as the Go float32 was.
	uint8_t body[] = {0x12, 0x00, seq, level, (uint8_t)(level * 4 / 5), 0x00, 0x00, 0x00};
	return frame(out, body, sizeof body);
}

size_t xy_cmd_power(uint8_t *out, uint8_t seq, bool on)
{
	uint8_t body[] = {0x11, 0x00, seq, on ? 0x01 : 0x02, 0x01, 0x01};
	return frame(out, body, sizeof body);
}

size_t xy_cmd_clear_playlist(uint8_t *out, uint8_t seq)
{
	uint8_t body[] = {0x05, 0x00, seq, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
	return frame(out, body, sizeof body);
}

size_t xy_cmd_resume(uint8_t *out, uint8_t seq)
{
	uint8_t body[] = {0x0a, 0x00, seq, 0x00, 0x00, 0x00, 0x00};
	return frame(out, body, sizeof body);
}

size_t xy_cmd_select_program(uint8_t *out, uint8_t program, uint8_t seq)
{
	uint8_t body[] = {0x04, program, seq, 0xff, 0x00};
	return frame(out, body, sizeof body);
}

size_t xy_cmd_raw(uint8_t *out, const uint8_t *body, size_t body_len)
{
	if (8 + body_len > 0xffff)
		return 0;
	return frame(out, body, body_len);
}

void xy_trailer(uint8_t out[8], bool static_image, uint32_t frames, uint32_t total_ms)
{
	static const uint8_t static_trailer[8] = {0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00};
	if (static_image) {
		memcpy(out, static_trailer, 8);
		return;
	}
	if (frames > 0xffff)
		frames = 0xffff;
	uint32_t ms = total_ms % 1000, secs = total_ms / 1000;
	if (secs > 0xffff)
		secs = 0xffff;
	uint8_t t[8] = {0x00, 0x01, (uint8_t)ms, (uint8_t)(ms >> 8), (uint8_t)frames, (uint8_t)(frames >> 8),
			(uint8_t)secs, (uint8_t)(secs >> 8)};
	memcpy(out, t, 8);
}

#define FIRST_CAP (XY_MAX_PACKET - XY_FIRST_HEADER)
#define NEXT_CAP (XY_MAX_PACKET - XY_NEXT_HEADER)

uint32_t xy_packet_count(uint32_t total)
{
	if (total <= FIRST_CAP)
		return 1;
	return 1 + (total - FIRST_CAP + NEXT_CAP - 1) / NEXT_CAP;
}

size_t xy_packet_header_len(uint32_t i)
{
	return i == 0 ? XY_FIRST_HEADER : XY_NEXT_HEADER;
}

void xy_packet_span(uint32_t total, uint32_t i, uint32_t *offset, uint32_t *len)
{
	uint32_t off = i == 0 ? 0 : FIRST_CAP + (i - 1) * NEXT_CAP;
	uint32_t cap = i == 0 ? FIRST_CAP : NEXT_CAP;
	if (off > total)
		off = total;
	*offset = off;
	*len = total - off < cap ? total - off : cap;
}

size_t xy_packet_header(uint8_t *out, uint32_t total, uint32_t i, uint8_t program, uint8_t seq,
			const uint8_t trailer[8])
{
	uint32_t n = xy_packet_count(total), off, len;
	xy_packet_span(total, i, &off, &len);
	size_t header = xy_packet_header_len(i);
	uint32_t size = header + len, remaining = total - off;

	uint8_t type = XY_PACKET_MIDDLE;
	if (i == 0)
		type = XY_PACKET_FIRST;
	else if (i == n - 1)
		type = XY_PACKET_LAST;

	memcpy(out, magic, sizeof magic);
	out[6] = (uint8_t)size;
	out[7] = (uint8_t)(size >> 8);
	out[8] = type;
	out[9] = program;
	out[10] = seq;
	out[11] = (uint8_t)remaining;
	out[12] = (uint8_t)(remaining >> 8);
	out[13] = (uint8_t)(remaining >> 16);
	// The first packet carries the packet count + 1, the others their
	// 1-based index.
	uint32_t field = i == 0 ? n + 1 : i + 1;
	out[14] = (uint8_t)field;
	out[15] = (uint8_t)(field >> 8);
	if (i == 0)
		memcpy(out + 16, trailer, 8);
	return header;
}

uint32_t xy_ack_timeout_ms(size_t packet_len)
{
	return (uint32_t)((packet_len + 249) / 250 * 200 + 200);
}

static bool has_prefix(const uint8_t *b, size_t len, uint8_t type)
{
	return len >= 3 && b[0] == 0x88 && b[1] == 0xff && b[2] == type;
}

bool xy_parse_ack(const uint8_t *b, size_t len, xy_ack_t *ack)
{
	if (len < 9 || !has_prefix(b, len, 0x03))
		return false;
	ack->index = (uint16_t)(b[3] | b[4] << 8);
	ack->ctr = b[5];
	ack->status = b[8];
	return true;
}

bool xy_is_not_ready(const uint8_t *b, size_t len)
{
	return has_prefix(b, len, 0x05);
}

bool xy_checksum_ok(const uint8_t *b, size_t len)
{
	if (len < 2)
		return false;
	uint8_t sum = 0;
	for (size_t i = 0; i + 1 < len; i++)
		sum += b[i];
	return sum == b[len - 1];
}

bool xy_parse_handshake(const uint8_t *b, size_t len, xy_panel_info_t *info)
{
	// 88 ff 00 02 01 04 <width u16> <height u16> 08 01 <level> ...
	if (len < 13 || !has_prefix(b, len, 0x00))
		return false;
	info->width = (uint16_t)(b[6] | b[7] << 8);
	info->height = (uint16_t)(b[8] | b[9] << 8);
	info->brightness = b[12];
	return true;
}

bool xy_is_handshake_rejected(const uint8_t *b, size_t len)
{
	return has_prefix(b, len, 0x0f);
}
