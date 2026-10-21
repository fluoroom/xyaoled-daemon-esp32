// xy_upload against a simulated panel: it reassembles frames from the
// writes, checks every packet header, and answers with acks that can be
// lost, duplicated, replaced by 88ff05 or rejected. Time is virtual.
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "test.h"
#include "xy_upload.h"

typedef struct {
	// Link
	uint32_t now;
	size_t payload;
	bool disconnected;
	int disconnect_after_writes; // 0: never
	int writes;

	// Notifications to the host
	uint8_t q[64][64];
	size_t qlen[64];
	int qhead, qtail;
	uint8_t ctr;

	// Frame reassembly
	uint8_t frame[XY_MAX_PACKET + 64];
	size_t have;

	// Behaviour
	int busy_selects;     // selects ignored before one is acked
	int drop_ack_packet;  // packet whose first ack is lost (0: none)
	int not_ready_packet; // packet answered once with 88ff05 instead of an ack
	int reject_packet;    // packet acked with a failing status
	bool duplicate_acks;  // every ack is sent twice with the same counter
	bool drop_all_acks;
	int stored_items;

	// What arrived
	int clears, selects, packets_seen;
	uint8_t last_program, last_seq;
	uint8_t trailer[8];
	uint8_t *payload_buf;
	uint32_t payload_len, expected_total;
	int resends; // packets received again with an index already seen
	uint16_t next_index;
	bool header_error;
} sim_t;

static void notify(sim_t *s, const uint8_t *b, size_t n)
{
	memcpy(s->q[s->qtail], b, n);
	s->qlen[s->qtail] = n;
	s->qtail = (s->qtail + 1) % 64;
}

static void send_ack(sim_t *s, uint16_t idx, uint8_t status, bool new_ctr)
{
	if (new_ctr)
		s->ctr++;
	uint8_t b[11] = {0x88, 0xff, 0x03, (uint8_t)idx, (uint8_t)(idx >> 8), s->ctr, 0x0b, 0x00, status, 0x0b, 0};
	for (int i = 0; i < 10; i++)
		b[10] += b[i];
	notify(s, b, sizeof b);
}

static void on_frame(sim_t *s, const uint8_t *f, size_t n)
{
	uint8_t type = f[8];
	if (type == 0x05) {
		s->clears++;
		return;
	}
	if (type == 0x04) {
		s->selects++;
		s->last_program = f[9];
		s->last_seq = f[10];
		if (s->busy_selects > 0) {
			s->busy_selects--;
			return;
		}
		send_ack(s, 0, 0xff, true);
		s->next_index = 1;
		return;
	}
	if (type < 0x07 || type > 0x09)
		return;
	uint32_t remaining = f[11] | f[12] << 8 | (uint32_t)f[13] << 16;
	uint16_t field = (uint16_t)(f[14] | f[15] << 8);
	size_t header = type == 0x07 ? 24 : 16;
	uint16_t index = type == 0x07 ? 1 : field;
	if (f[9] != s->last_program || f[10] != s->last_seq)
		s->header_error = true;
	if (type == 0x07) {
		memcpy(s->trailer, f + 16, 8);
		s->expected_total = remaining;
		s->payload_len = 0;
		free(s->payload_buf);
		s->payload_buf = malloc(remaining + 1);
	}
	if (index < s->next_index) {
		s->resends++;
	} else {
		if (s->expected_total - s->payload_len != remaining || index != s->next_index)
			s->header_error = true;
		memcpy(s->payload_buf + s->payload_len, f + header, n - header);
		s->payload_len += (uint32_t)(n - header);
		s->next_index = index + 1;
	}
	s->packets_seen++;
	if (s->not_ready_packet == index) {
		s->not_ready_packet = 0;
		const uint8_t nr[] = {0x88, 0xff, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00};
		notify(s, nr, sizeof nr);
		s->next_index = 1; // the panel dropped the upload
		return;
	}
	if (s->drop_all_acks)
		return;
	if (s->drop_ack_packet == index) {
		s->drop_ack_packet = 0;
		return;
	}
	send_ack(s, index, s->reject_packet == index ? 0x10 : 0xff, true);
	if (s->duplicate_acks)
		send_ack(s, index, 0xff, false);
	bool last = s->payload_len == s->expected_total;
	if (last && s->last_program == XY_PROGRAM_PLAYLIST) {
		const uint8_t stored[] = {0x88, 0xff, 0x05, 0x01, (uint8_t)s->stored_items++, 0, 0, 0, 0, 0x0b, 0};
		notify(s, stored, sizeof stored);
	}
}

static int sim_write(void *ctx, const uint8_t *data, size_t len)
{
	sim_t *s = ctx;
	if (s->disconnected)
		return XY_ERR_NOT_CONNECTED;
	if (len > s->payload)
		return XY_ERR_WRITE;
	if (s->disconnect_after_writes && ++s->writes >= s->disconnect_after_writes) {
		s->disconnected = true;
		return XY_ERR_NOT_CONNECTED;
	}
	memcpy(s->frame + s->have, data, len);
	s->have += len;
	while (s->have >= 8) {
		size_t flen = s->frame[6] | s->frame[7] << 8;
		if (s->have < flen)
			break;
		on_frame(s, s->frame, flen);
		memmove(s->frame, s->frame + flen, s->have - flen);
		s->have -= flen;
	}
	return XY_OK;
}

static int sim_recv(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
	sim_t *s = ctx;
	if (s->disconnected)
		return -1;
	if (s->qhead == s->qtail) {
		s->now += timeout_ms ? timeout_ms : 0;
		return 0;
	}
	size_t n = s->qlen[s->qhead] < cap ? s->qlen[s->qhead] : cap;
	memcpy(buf, s->q[s->qhead], n);
	s->qhead = (s->qhead + 1) % 64;
	s->now += 20; // ack latency
	return (int)n;
}

static void sim_sleep(void *ctx, uint32_t ms)
{
	((sim_t *)ctx)->now += ms;
}

static uint32_t sim_now(void *ctx)
{
	return ((sim_t *)ctx)->now;
}

static size_t sim_payload(void *ctx)
{
	return ((sim_t *)ctx)->payload;
}

static int mem_read_at(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
	memcpy(buf, (const uint8_t *)ctx + off, len);
	return 0;
}

static uint8_t *gif_data(uint32_t len)
{
	uint8_t *g = malloc(len);
	for (uint32_t i = 0; i < len; i++)
		g[i] = (uint8_t)(i * 13 + i / 251);
	return g;
}

static int run(sim_t *s, uint32_t len, xy_send_opts_t *o, uint8_t **gif_out)
{
	xy_link_t link = {sim_write, sim_recv, sim_sleep, sim_now, sim_payload, NULL, s};
	xy_session_t sess;
	xy_session_init(&sess, &link);
	uint8_t *gif = gif_data(len);
	xy_blob_t blob = {len, mem_read_at, gif};
	int err = xy_upload(&sess, &blob, o);
	if (gif_out)
		*gif_out = gif;
	else
		free(gif);
	return err;
}

static void check_payload(sim_t *s, const uint8_t *gif, uint32_t len)
{
	CHECK(!s->header_error, "packet header mismatch");
	CHECK(s->payload_len == len && s->payload_buf && memcmp(s->payload_buf, gif, len) == 0,
	      "payload %u bytes, want %u", s->payload_len, len);
}

static sim_t *sim_new(size_t payload)
{
	sim_t *s = calloc(1, sizeof *s);
	s->payload = payload;
	return s;
}

static void sim_free(sim_t *s)
{
	free(s->payload_buf);
	free(s);
}

static void test_live_upload(void)
{
	sim_t *s = sim_new(512);
	uint8_t *gif;
	xy_send_opts_t o = {.program = XY_PROGRAM_LIVE, .frames = 3, .total_ms = 1200};
	CHECK(run(s, 25000, &o, &gif) == XY_OK, "upload failed");
	check_payload(s, gif, 25000);
	CHECK(s->clears == 0 && s->selects == 1 && s->packets_seen == 3, "clears %d selects %d packets %d", s->clears,
	      s->selects, s->packets_seen);
	const uint8_t want[8] = {0x00, 0x01, 0xc8, 0x00, 0x03, 0x00, 0x01, 0x00};
	CHECK(memcmp(s->trailer, want, 8) == 0, "trailer");
	free(gif);
	sim_free(s);
}

static void test_playlist_replace_and_append(void)
{
	sim_t *s = sim_new(512);
	uint8_t *gif;
	xy_send_opts_t o = {.program = XY_PROGRAM_PLAYLIST, .static_image = true, .frames = 1};
	CHECK(run(s, 900, &o, &gif) == XY_OK, "replace failed");
	check_payload(s, gif, 900);
	CHECK(s->clears == 1 && s->stored_items == 1, "clears %d stored %d", s->clears, s->stored_items);
	const uint8_t stat[8] = {0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00};
	CHECK(memcmp(s->trailer, stat, 8) == 0, "static trailer");
	CHECK(s->now >= XY_CLEAR_SETTLE_MS && s->now < XY_UPLOAD_APPLIED_TIMEOUT_MS,
	      "stored notification not used (%u ms)", s->now);
	free(gif);
	o.append = true;
	CHECK(run(s, 700, &o, NULL) == XY_OK, "append failed");
	CHECK(s->clears == 1 && s->stored_items == 2, "append cleared");
	sim_free(s);
}

static void test_lost_and_duplicate_acks(void)
{
	sim_t *s = sim_new(512);
	s->drop_ack_packet = 2;
	s->duplicate_acks = true;
	uint8_t *gif;
	xy_send_opts_t o = {.program = XY_PROGRAM_LIVE, .frames = 2, .total_ms = 80};
	CHECK(run(s, 35000, &o, &gif) == XY_OK, "upload failed");
	check_payload(s, gif, 35000);
	CHECK(s->resends == 1, "resends %d", s->resends);
	free(gif);
	sim_free(s);
}

static void test_not_ready_restarts(void)
{
	sim_t *s = sim_new(512);
	s->not_ready_packet = 2;
	uint8_t *gif;
	xy_send_opts_t o = {.program = XY_PROGRAM_LIVE, .frames = 2, .total_ms = 80};
	CHECK(run(s, 25000, &o, &gif) == XY_OK, "upload failed");
	CHECK(s->selects == 2, "selects %d, want a re-select", s->selects);
	CHECK(s->payload_len == 25000 && memcmp(s->payload_buf, gif, 25000) == 0, "payload after restart");
	free(gif);
	sim_free(s);
}

static void test_rejected(void)
{
	sim_t *s = sim_new(512);
	s->reject_packet = 1;
	xy_send_opts_t o = {.program = XY_PROGRAM_LIVE, .frames = 1};
	CHECK(run(s, 500, &o, NULL) == XY_ERR_REJECTED, "rejection not reported");
	sim_free(s);
}

static void test_busy(void)
{
	sim_t *s = sim_new(512);
	s->busy_selects = 5;
	xy_send_opts_t o = {.program = XY_PROGRAM_LIVE, .frames = 1};
	CHECK(run(s, 500, &o, NULL) == XY_OK, "busy panel not waited for");
	CHECK(s->selects == 6, "selects %d", s->selects);
	sim_free(s);

	s = sim_new(512);
	s->busy_selects = 1000;
	CHECK(run(s, 500, &o, NULL) == XY_ERR_BUSY, "never-acked select");
	CHECK(s->now >= XY_PANEL_BUSY_TIMEOUT_MS && s->now < XY_PANEL_BUSY_TIMEOUT_MS + 5000, "gave up after %u ms",
	      s->now);
	sim_free(s);
}

static void test_no_ack_and_disconnect(void)
{
	sim_t *s = sim_new(512);
	s->drop_all_acks = true;
	xy_send_opts_t o = {.program = XY_PROGRAM_LIVE, .frames = 1};
	CHECK(run(s, 500, &o, NULL) == XY_ERR_NO_ACK, "no ack");
	CHECK(s->packets_seen == 1 + XY_MAX_PACKET_RESENDS, "packet sent %d times", s->packets_seen);
	sim_free(s);

	s = sim_new(512);
	s->disconnect_after_writes = 10;
	CHECK(run(s, 25000, &o, NULL) == XY_ERR_NOT_CONNECTED, "disconnect");
	sim_free(s);
}

static void test_small_mtu(void)
{
	sim_t *s = sim_new(20);
	uint8_t *gif;
	xy_send_opts_t o = {.program = XY_PROGRAM_LIVE, .frames = 2, .total_ms = 100};
	CHECK(run(s, 12345, &o, &gif) == XY_OK, "20-byte writes");
	check_payload(s, gif, 12345);
	free(gif);
	sim_free(s);
}

int main(void)
{
	static const struct test_case cases[] = {
		TEST(test_live_upload), TEST(test_playlist_replace_and_append), TEST(test_lost_and_duplicate_acks),
		TEST(test_not_ready_restarts), TEST(test_rejected), TEST(test_busy), TEST(test_no_ack_and_disconnect),
		TEST(test_small_mtu),
	};
	return run_tests("upload", cases, sizeof cases / sizeof cases[0]) != 0;
}
