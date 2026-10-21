#include "xy_upload.h"

#include <stdlib.h>
#include <string.h>

#define NOTIFY_CAP 64
#define MAX_WRITE 512

#define LOG(s, ...)                                                    \
	do {                                                           \
		if ((s)->debug && (s)->link->log)                      \
			(s)->link->log((s)->link->ctx, __VA_ARGS__);   \
	} while (0)

const char *xy_err_text(int err)
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
	case XY_ERR_WRITE:
		return "write to the panel failed";
	case XY_ERR_READ:
		return "could not read the GIF";
	}
	return NULL;
}

void xy_session_init(xy_session_t *s, const xy_link_t *link)
{
	memset(s, 0, sizeof *s);
	s->link = link;
}

uint8_t xy_next_seq(xy_session_t *s)
{
	return s->seq++;
}

int xy_drain(xy_session_t *s)
{
	uint8_t buf[NOTIFY_CAP];
	int n;
	while ((n = s->link->recv(s->link->ctx, buf, sizeof buf, 0)) > 0) {
	}
	return n < 0 ? XY_ERR_NOT_CONNECTED : XY_OK;
}

static size_t payload(const xy_session_t *s)
{
	size_t n = s->link->att_payload(s->link->ctx);
	return n == 0 ? 20 : n > MAX_WRITE ? MAX_WRITE : n;
}

int xy_write_frame(xy_session_t *s, const uint8_t *frame, size_t len)
{
	size_t chunk = payload(s);
	for (size_t off = 0; off < len; off += chunk) {
		size_t n = len - off < chunk ? len - off : chunk;
		int err = s->link->write(s->link->ctx, frame + off, n);
		if (err != XY_OK)
			return err;
	}
	return XY_OK;
}

// wait_ack waits for the ack with index want (0 acks the program select).
// Duplicates and acks for other indices, such as late acks for the select,
// are skipped. While waiting for a packet ack, 88ff05 means the panel left
// receive mode.
static int wait_ack(xy_session_t *s, uint16_t want, uint32_t timeout_ms, xy_ack_t *out)
{
	const xy_link_t *l = s->link;
	uint32_t start = l->now_ms(l->ctx);
	for (;;) {
		uint32_t spent = l->now_ms(l->ctx) - start;
		if (spent >= timeout_ms)
			return XY_ERR_NO_ACK;
		uint8_t buf[NOTIFY_CAP];
		int n = l->recv(l->ctx, buf, sizeof buf, timeout_ms - spent);
		if (n < 0)
			return XY_ERR_NOT_CONNECTED;
		if (n == 0)
			return XY_ERR_NO_ACK;
		if (want > 0 && xy_is_not_ready(buf, (size_t)n))
			return XY_ERR_NOT_READY;
		xy_ack_t a;
		if (!xy_parse_ack(buf, (size_t)n, &a))
			continue;
		if (s->have_last_ack_ctr && a.ctr == s->last_ack_ctr)
			continue;
		s->last_ack_ctr = a.ctr;
		s->have_last_ack_ctr = true;
		if (a.index != want)
			continue;
		if (out)
			*out = a;
		return XY_OK;
	}
}

// wait_upload_applied waits for the 88ff05 the panel sends once it has
// stored an upload; commands sent before it can be rejected.
static void wait_upload_applied(xy_session_t *s)
{
	const xy_link_t *l = s->link;
	uint32_t start = l->now_ms(l->ctx);
	for (;;) {
		uint32_t spent = l->now_ms(l->ctx) - start;
		if (spent >= XY_UPLOAD_APPLIED_TIMEOUT_MS) {
			LOG(s, "no upload-applied notification after %u ms", (unsigned)spent);
			return;
		}
		uint8_t buf[NOTIFY_CAP];
		int n = l->recv(l->ctx, buf, sizeof buf, XY_UPLOAD_APPLIED_TIMEOUT_MS - spent);
		if (n < 0)
			return;
		if (n > 0 && xy_is_not_ready(buf, (size_t)n)) {
			LOG(s, "upload applied after %u ms", (unsigned)(l->now_ms(l->ctx) - start));
			return;
		}
	}
}

// select_program re-sends the select until the panel acks it; after a
// large upload the panel is busy for tens of seconds.
static int select_program(xy_session_t *s, uint8_t program, uint8_t seq)
{
	const xy_link_t *l = s->link;
	uint8_t cmd[XY_MAX_COMMAND];
	size_t n = xy_cmd_select_program(cmd, program, seq);
	uint32_t start = l->now_ms(l->ctx);
	for (;;) {
		int err = xy_drain(s);
		if (err == XY_OK)
			err = xy_write_frame(s, cmd, n);
		if (err != XY_OK)
			return err;
		err = wait_ack(s, 0, XY_SELECT_RETRY_MS, NULL);
		if (err == XY_OK || err == XY_ERR_NOT_CONNECTED)
			return err;
		uint32_t spent = l->now_ms(l->ctx) - start;
		if (spent > XY_PANEL_BUSY_TIMEOUT_MS)
			return XY_ERR_BUSY;
		LOG(s, "panel busy (%u s), re-sending program select", (unsigned)(spent / 1000));
	}
}

static int write_packet(xy_session_t *s, const xy_blob_t *gif, uint32_t i, uint8_t program, uint8_t seq,
			const uint8_t trailer[8], uint8_t *buf)
{
	uint8_t header[XY_FIRST_HEADER];
	size_t hl = xy_packet_header(header, gif->len, i, program, seq, trailer);
	uint32_t off, len;
	xy_packet_span(gif->len, i, &off, &len);
	size_t total = hl + len, chunk = payload(s);
	for (size_t pos = 0; pos < total; pos += chunk) {
		size_t n = total - pos < chunk ? total - pos : chunk, k = 0;
		if (pos < hl) {
			k = hl - pos < n ? hl - pos : n;
			memcpy(buf, header + pos, k);
		}
		if (k < n && gif->read_at(gif->ctx, off + (uint32_t)(pos + k - hl), buf + k, n - k) != 0)
			return XY_ERR_READ;
		int err = s->link->write(s->link->ctx, buf, n);
		if (err != XY_OK)
			return err;
	}
	return XY_OK;
}

static int upload_packets(xy_session_t *s, const xy_blob_t *gif, uint8_t program, uint8_t seq,
			  const uint8_t trailer[8])
{
	uint8_t *buf = malloc(MAX_WRITE);
	if (!buf)
		return XY_ERR_WRITE;
	uint32_t n = xy_packet_count(gif->len);
	int err = XY_OK;
	for (uint32_t i = 0; i < n && err == XY_OK; i++) {
		uint32_t off, len;
		xy_packet_span(gif->len, i, &off, &len);
		xy_ack_t a = {0};
		for (int attempt = 0; attempt <= XY_MAX_PACKET_RESENDS; attempt++) {
			LOG(s, "packet %u/%u (%u bytes), attempt %d", (unsigned)i + 1, (unsigned)n,
			    (unsigned)(xy_packet_header_len(i) + len), attempt + 1);
			if ((err = xy_drain(s)) != XY_OK)
				break;
			if ((err = write_packet(s, gif, i, program, seq, trailer, buf)) != XY_OK)
				break;
			err = wait_ack(s, (uint16_t)(i + 1), xy_ack_timeout_ms(xy_packet_header_len(i) + len), &a);
			if (err != XY_ERR_NO_ACK)
				break;
		}
		if (err == XY_OK && a.status < XY_ACK_STATUS_MIN_OK) {
			LOG(s, "packet %u/%u rejected (status 0x%02x)", (unsigned)i + 1, (unsigned)n, a.status);
			err = XY_ERR_REJECTED;
		}
	}
	free(buf);
	return err;
}

int xy_upload(xy_session_t *s, const xy_blob_t *gif, const xy_send_opts_t *o)
{
	const xy_link_t *l = s->link;
	bool live = o->program == XY_PROGRAM_LIVE;
	if (!live && !o->append) {
		// The clear is never acked; give the panel time to apply it or the
		// upload gets appended instead of replacing the playlist.
		uint8_t cmd[XY_MAX_COMMAND];
		int err = xy_drain(s);
		if (err == XY_OK)
			err = xy_write_frame(s, cmd, xy_cmd_clear_playlist(cmd, xy_next_seq(s)));
		if (err == XY_ERR_NOT_CONNECTED)
			return err;
		l->sleep_ms(l->ctx, XY_CLEAR_SETTLE_MS);
	}
	uint8_t seq = xy_next_seq(s), trailer[8];
	xy_trailer(trailer, o->static_image, o->frames, o->total_ms);
	int err;
	// A panel that answers data with 88ff05 has dropped out of receive mode:
	// select the program again and restart the upload.
	for (int attempt = 1;; attempt++) {
		LOG(s, "selecting program %u (seq %u)", o->program, seq);
		uint32_t t0 = l->now_ms(l->ctx);
		if ((err = select_program(s, o->program, seq)) != XY_OK)
			return err;
		uint32_t t1 = l->now_ms(l->ctx);
		LOG(s, "uploading %u bytes in %u packet(s)", (unsigned)gif->len, (unsigned)xy_packet_count(gif->len));
		err = upload_packets(s, gif, o->program, seq, trailer);
		s->select_ms = t1 - t0;
		s->data_ms = l->now_ms(l->ctx) - t1;
		if (err != XY_ERR_NOT_READY || attempt == XY_MAX_PACKET_RESENDS)
			break;
	}
	// Live uploads aren't stored, so the panel sends no 88ff05 for them.
	if (err == XY_OK && !live)
		wait_upload_applied(s);
	return err;
}
