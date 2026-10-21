// Port of reference/go-daemon/xyaoled/upload_test.go.
#include <stdint.h>
#include <stdlib.h>

#include "test.h"
#include "xy_proto.h"

static const uint8_t test_trailer[8] = {0x00, 0x01, 0x20, 0x03, 0x06, 0x00, 0x01, 0x00};

static void test_packets_single(void)
{
	uint32_t total = 1949, off, len;
	uint8_t h[XY_FIRST_HEADER];
	CHECK(xy_packet_count(total) == 1, "count %u", xy_packet_count(total));
	size_t n = xy_packet_header(h, total, 0, 0x02, 0x04, test_trailer);
	const uint8_t want[16] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0xb5, 0x07,
				  0x07, 0x02, 0x04, 0x9d, 0x07, 0x00, 0x02, 0x00};
	CHECK(n == 24, "header len %zu", n);
	CHECK(memcmp(h, want, 16) == 0, "header mismatch");
	CHECK(memcmp(h + 16, test_trailer, 8) == 0, "trailer mismatch");
	xy_packet_span(total, 0, &off, &len);
	CHECK(off == 0 && len == total, "span %u+%u", off, len);
}

static void test_packets_multi(void)
{
	uint32_t total = 94445, remaining = total, next = 0;
	uint32_t n = xy_packet_count(total);
	CHECK(n == 10, "count %u", n);
	for (uint32_t i = 0; i < n; i++) {
		uint8_t h[XY_FIRST_HEADER];
		uint32_t off, len;
		size_t hl = xy_packet_header(h, total, i, 0x03, 0x04, test_trailer);
		xy_packet_span(total, i, &off, &len);
		uint32_t size = hl + len;
		CHECK(size <= XY_MAX_PACKET, "packet %u is %u bytes", i, size);
		CHECK((uint32_t)(h[6] | h[7] << 8) == size, "packet %u length field", i);
		uint8_t want_type = i == 0 ? XY_PACKET_FIRST : i == n - 1 ? XY_PACKET_LAST : XY_PACKET_MIDDLE;
		CHECK(h[8] == want_type && h[9] == 0x03 && h[10] == 0x04, "packet %u type/program/seq", i);
		CHECK((uint32_t)(h[11] | h[12] << 8 | h[13] << 16) == remaining, "packet %u remaining", i);
		uint32_t field = h[14] | h[15] << 8;
		CHECK(field == (i == 0 ? n + 1 : i + 1), "packet %u count/index field %u", i, field);
		CHECK(hl == (i == 0 ? 24u : 16u), "packet %u header %zu", i, hl);
		CHECK(off == next, "packet %u offset %u, want %u", i, off, next);
		next = off + len;
		remaining -= len;
		if (i == n - 1)
			CHECK(size == 4613, "last packet %u bytes, want 4613", size);
	}
	CHECK(next == total && remaining == 0, "payload not covered: %u", next);
}

static void test_parse_ack(void)
{
	const uint8_t b[] = {0x88, 0xff, 0x03, 0x01, 0x00, 0xc2, 0x0b, 0x00, 0xff, 0x0b, 0x62};
	xy_ack_t a;
	CHECK(xy_parse_ack(b, sizeof b, &a), "not parsed");
	CHECK(a.index == 1 && a.ctr == 0xc2 && a.status == 0xff, "%u %x %x", a.index, a.ctr, a.status);
	const uint8_t other[] = {0x88, 0xff, 0x00, 0x02, 0x01};
	CHECK(!xy_parse_ack(other, sizeof other, &a), "non-03 parsed as ack");
}

static void test_ack_timeout(void)
{
	CHECK(xy_ack_timeout_ms(10000) == 8200, "%u", xy_ack_timeout_ms(10000));
	CHECK(xy_ack_timeout_ms(13) == 400, "%u", xy_ack_timeout_ms(13));
}

static void test_trailers(void)
{
	uint8_t t[8];
	// Vendor capture: 172 frames × 30 ms.
	const uint8_t vendor[8] = {0x00, 0x01, 0xa0, 0x00, 0xac, 0x00, 0x05, 0x00};
	xy_trailer(t, false, 172, 172 * 30);
	CHECK(memcmp(t, vendor, 8) == 0, "vendor capture");
	// Older capture: 164 frames × 40 ms.
	const uint8_t older[8] = {0x00, 0x01, 0x30, 0x02, 0xa4, 0x00, 0x06, 0x00};
	xy_trailer(t, false, 164, 164 * 40);
	CHECK(memcmp(t, older, 8) == 0, "older capture");
	const uint8_t stat[8] = {0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00};
	xy_trailer(t, true, 1, 600000);
	CHECK(memcmp(t, stat, 8) == 0, "static");
}

static void test_commands(void)
{
	uint8_t b[XY_MAX_COMMAND];
	const uint8_t bright[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x10, 0x00,
				  0x12, 0x00, 0x07, 0x7d, 0x64, 0x00, 0x00, 0x00};
	CHECK(xy_cmd_brightness(b, 7, 125) == sizeof bright && memcmp(b, bright, sizeof bright) == 0, "brightness");
	xy_cmd_brightness(b, 0, 200);
	CHECK(b[11] == 125 && b[12] == 100, "brightness clamp %u %u", b[11], b[12]);
	xy_cmd_brightness(b, 0, 40);
	CHECK(b[12] == 32, "40 × 0.8 = %u", b[12]);
	const uint8_t power[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x0e, 0x00, 0x11, 0x00, 0x09, 0x02, 0x01, 0x01};
	CHECK(xy_cmd_power(b, 9, false) == sizeof power && memcmp(b, power, sizeof power) == 0, "power off");
	const uint8_t clear[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x11, 0x00, 0x05,
				 0x00, 0x06, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
	CHECK(xy_cmd_clear_playlist(b, 6) == sizeof clear && memcmp(b, clear, sizeof clear) == 0, "clear");
	CHECK(memcmp(xy_init_frames[4], clear, sizeof clear) == 0, "init clear matches builder");
	const uint8_t resume[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x0f, 0x00, 0x0a, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00};
	CHECK(xy_cmd_resume(b, 5) == sizeof resume && memcmp(b, resume, sizeof resume) == 0, "resume");
	const uint8_t sel[] = {0x99, 0xaa, 0x00, 0x2e, 0xff, 0x88, 0x0d, 0x00, 0x04, 0x03, 0x02, 0xff, 0x00};
	CHECK(xy_cmd_select_program(b, 3, 2) == sizeof sel && memcmp(b, sel, sizeof sel) == 0, "select");
	const uint8_t body[] = {0x0a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
	CHECK(xy_cmd_raw(b, body, sizeof body) == 15 && b[6] == 15 && memcmp(b + 8, body, sizeof body) == 0, "raw");
	CHECK(xy_init_frame_lens[0] == 31 && xy_init_frames[0][6] == 0x1f, "handshake length");
}

static void test_notifications(void)
{
	const uint8_t hs[] = {0x88, 0xff, 0x00, 0x02, 0x01, 0x04, 0x60, 0x00, 0x10, 0x00, 0x08, 0x01, 0x28, 0x20, 0x06, 0x01, 0x00};
	xy_panel_info_t info;
	CHECK(xy_parse_handshake(hs, sizeof hs, &info), "handshake not parsed");
	CHECK(info.width == 96 && info.height == 16 && info.brightness == 40, "%ux%u %u", info.width, info.height,
	      info.brightness);
	const uint8_t stored[] = {0x88, 0xff, 0x05, 0x01, 0x02};
	CHECK(xy_is_not_ready(stored, sizeof stored), "88ff05");
	const uint8_t rej[] = {0x88, 0xff, 0x0f, 0x60};
	CHECK(xy_is_handshake_rejected(rej, sizeof rej), "88ff0f");
	const uint8_t ack[] = {0x88, 0xff, 0x03, 0x01, 0x00, 0xc2, 0x0b, 0x00, 0xff, 0x0b, 0x62};
	CHECK(xy_checksum_ok(ack, sizeof ack), "checksum of captured ack");
}

int main(void)
{
	static const struct test_case cases[] = {
		TEST(test_packets_single), TEST(test_packets_multi), TEST(test_parse_ack), TEST(test_ack_timeout),
		TEST(test_trailers),       TEST(test_commands),      TEST(test_notifications),
	};
	return run_tests("proto", cases, sizeof cases / sizeof cases[0]) != 0;
}
