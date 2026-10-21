// Imaging tests against frames produced by the Go daemon's logic
// (gogen/main.go, which mirrors display.go). Run from firmware/test/host.
#include <stdint.h>
#include <stdlib.h>

#include "img.h"
#include "img_internal.h"
#include "test.h"

#define FIX "fixtures/"
#define OUT "build/"

static const img_target_t panel = {96, 16, false};

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

// Reader handing out at most chunk bytes per call, to exercise buffering.
typedef struct {
	const uint8_t *data;
	size_t len, pos, chunk;
} chunk_src_t;

static int chunk_read(void *ctx, uint8_t *buf, size_t len)
{
	chunk_src_t *s = ctx;
	size_t k = s->len - s->pos;
	if (k > len)
		k = len;
	if (k > s->chunk)
		k = s->chunk;
	memcpy(buf, s->data + s->pos, k);
	s->pos += k;
	return (int)k;
}

static img_err_t decode_chunked(const uint8_t *data, size_t len, size_t chunk, img_frames_t *fr, img_info_t *info)
{
	chunk_src_t s = {data, len, 0, chunk};
	img_reader_t r = {chunk_read, &s};
	return img_decode(&r, fr, info);
}

typedef struct {
	uint8_t *buf;
	size_t len, cap;
} mem_sink_t;

static int sink_write(void *ctx, const uint8_t *buf, size_t len)
{
	mem_sink_t *m = ctx;
	if (m->len + len > m->cap) {
		m->cap = (m->len + len) * 2;
		m->buf = realloc(m->buf, m->cap);
	}
	memcpy(m->buf + m->len, buf, len);
	m->len += len;
	return 0;
}

static int max_diff(const uint8_t *a, const uint8_t *b, size_t n, double *mean)
{
	int worst = 0;
	long sum = 0;
	for (size_t i = 0; i < n; i++) {
		int d = abs(a[i] - b[i]);
		sum += d;
		if (d > worst)
			worst = d;
	}
	if (mean)
		*mean = (double)sum / (double)n;
	return worst;
}

// check_fixture decodes FIX name.ext and compares every frame with
// name.rgb / name.delays within tolerance.
static void check_fixture(const char *src, const char *expect, const img_target_t *t, int tolerance,
			  double mean_tolerance)
{
	char path[256];
	size_t len, want_len;
	snprintf(path, sizeof path, FIX "%s", src);
	uint8_t *data = read_file(path, &len);
	snprintf(path, sizeof path, FIX "%s.rgb", expect);
	uint8_t *want = read_file(path, &want_len);
	CHECK(data && want, "missing fixture %s", src);
	if (!data || !want)
		goto out;
	size_t fb = (size_t)t->width * t->height * 3, nframes = want_len / fb;

	snprintf(path, sizeof path, FIX "%s.delays", expect);
	FILE *df = fopen(path, "r");
	for (size_t chunk = 1; chunk <= 4096; chunk *= 64) {
		img_frames_t *fr = img_frames_new_mem(t, 1000);
		img_info_t info;
		img_err_t err = decode_chunked(data, len, chunk, fr, &info);
		CHECK(err == IMG_OK, "%s (chunk %zu): %s", src, chunk, img_strerror(err));
		CHECK(img_frames_count(fr) == nframes, "%s: %zu frames, want %zu", src, img_frames_count(fr), nframes);
		uint8_t *rgb = malloc(fb);
		rewind(df);
		for (size_t i = 0; i < img_frames_count(fr) && i < nframes; i++) {
			uint16_t delay;
			unsigned want_delay = 0;
			img_frames_get(fr, i, rgb, &delay);
			double mean;
			int d = max_diff(rgb, want + i * fb, fb, &mean);
			CHECK(d <= tolerance && mean <= mean_tolerance, "%s frame %zu: max diff %d, mean %.2f", src, i, d,
			      mean);
			CHECK(fscanf(df, "%u", &want_delay) == 1, "delays file");
			if (nframes > 1)
				CHECK(delay == want_delay, "%s frame %zu delay %u, want %u", src, i, delay, want_delay);
		}
		free(rgb);
		img_frames_free(fr);
	}
	fclose(df);
out:
	free(data);
	free(want);
}

static void test_gif_keyframed(void)
{
	check_fixture("anim_small.gif", "anim_small", &panel, 0, 0);
	check_fixture("anim_wide.gif", "anim_wide", &panel, 0, 0);
	check_fixture("anim_large.gif", "anim_large", &panel, 0, 0);
}

static void test_upside_down(void)
{
	img_target_t flipped = {96, 16, true};
	check_fixture("anim_small.gif", "anim_small_flipped", &flipped, 0, 0);
}

static void test_png(void)
{
	check_fixture("png_small.png", "png_small", &panel, 0, 0);
	// Alpha over black: Go rounds through 16-bit premultiplication.
	check_fixture("png_rgba.png", "png_rgba", &panel, 1, 0.5);
}

static void test_jpeg(void)
{
	// A different decoder, and TJpgDec pre-scaling averages pixels where
	// Go samples one: only close, not equal.
	check_fixture("photo.jpg", "photo", &panel, 255, 12.0);
}

// Encoding then decoding gives the same frames back while there are 256
// colours or fewer.
static void roundtrip(const char *src, const char *out_name)
{
	char path[256];
	size_t len;
	snprintf(path, sizeof path, FIX "%s", src);
	uint8_t *data = read_file(path, &len);
	img_frames_t *fr = img_frames_new_mem(&panel, 1000);
	img_info_t info;
	CHECK(img_decode_mem(data, len, fr, &info) == IMG_OK, "%s decode", src);

	mem_sink_t sink = {0};
	img_writer_t w = {sink_write, &sink};
	img_gif_info_t gi;
	CHECK(img_encode_gif(fr, &w, &gi) == IMG_OK, "%s encode", src);
	CHECK(gi.bytes == sink.len, "bytes %u vs %zu", gi.bytes, sink.len);
	CHECK(gi.frames == img_frames_count(fr), "frames");
	CHECK(gi.static_image == (img_frames_count(fr) == 1), "static flag");

	img_frames_t *back = img_frames_new_mem(&panel, 1000);
	img_info_t binfo;
	img_err_t err = img_decode_mem(sink.buf, sink.len, back, &binfo);
	CHECK(err == IMG_OK, "%s re-decode: %s", src, img_strerror(err));
	CHECK(img_frames_count(back) == img_frames_count(fr), "%s frame count", src);
	size_t fb = 96 * 16 * 3;
	uint8_t *a = malloc(fb), *b = malloc(fb);
	uint32_t total = 0;
	for (size_t i = 0; i < img_frames_count(fr) && i < img_frames_count(back); i++) {
		uint16_t da, db;
		img_frames_get(fr, i, a, &da);
		img_frames_get(back, i, b, &db);
		CHECK(memcmp(a, b, fb) == 0, "%s frame %zu differs after roundtrip", src, i);
		total += da;
	}
	if (!gi.static_image)
		CHECK(gi.total_ms == total * 10, "total ms %u, want %u", gi.total_ms, total * 10);
	else
		CHECK(gi.total_ms == 600000, "still held %u ms", gi.total_ms);

	// For the image/gif check (make verify-go).
	snprintf(path, sizeof path, OUT "%s.gif", out_name);
	FILE *f = fopen(path, "wb");
	fwrite(sink.buf, 1, sink.len, f);
	fclose(f);
	snprintf(path, sizeof path, OUT "%s.rgb", out_name);
	f = fopen(path, "wb");
	for (size_t i = 0; i < img_frames_count(fr); i++) {
		img_frames_get(fr, i, a, NULL);
		fwrite(a, 1, fb, f);
	}
	fclose(f);

	free(a);
	free(b);
	free(sink.buf);
	free(data);
	img_frames_free(fr);
	img_frames_free(back);
}

static void test_encode_roundtrip(void)
{
	roundtrip("anim_small.gif", "enc_anim_small");
	roundtrip("anim_large.gif", "enc_anim_large");
	roundtrip("png_small.png", "enc_png_small");
}

static void test_encode_dithered(void)
{
	// More than 256 colours: dithered, still a valid GIF of the right size.
	size_t len;
	uint8_t *data = read_file(FIX "png_rgba.png", &len);
	img_frames_t *fr = img_frames_new_mem(&panel, 4);
	img_info_t info;
	CHECK(img_decode_mem(data, len, fr, &info) == IMG_OK, "decode");
	const uint8_t *pal;
	CHECK(img_frames_palette(fr, &pal) == 256, "palette capped at 256");
	mem_sink_t sink = {0};
	img_writer_t w = {sink_write, &sink};
	img_gif_info_t gi;
	CHECK(img_encode_gif(fr, &w, &gi) == IMG_OK, "encode");
	FILE *f = fopen(OUT "enc_png_rgba.gif", "wb");
	fwrite(sink.buf, 1, sink.len, f);
	fclose(f);
	img_frames_t *back = img_frames_new_mem(&panel, 4);
	CHECK(img_decode_mem(sink.buf, sink.len, back, &info) == IMG_OK, "re-decode");
	size_t fb = 96 * 16 * 3;
	uint8_t *a = malloc(fb), *b = malloc(fb);
	img_frames_get(fr, 0, a, NULL);
	img_frames_get(back, 0, b, NULL);
	// The palette is the first 256 colours met, as in Go, so the error is
	// large either way; it must be no worse than Go's own dithering.
	size_t go_len;
	uint8_t *go = read_file(FIX "png_rgba_dithered.rgb", &go_len);
	double ours, theirs;
	max_diff(a, b, fb, &ours);
	max_diff(a, go, fb, &theirs);
	CHECK(ours <= theirs * 1.1 + 0.5, "dithered mean error %.2f, Go %.2f", ours, theirs);
	printf("  dithering mean error: ours %.2f, Go %.2f\n", ours, theirs);
	f = fopen(OUT "enc_png_rgba.rgb", "wb");
	fwrite(b, 1, fb, f);
	fclose(f);
	free(go);
	free(a);
	free(b);
	free(sink.buf);
	free(data);
	img_frames_free(fr);
	img_frames_free(back);
}

static void test_encode_large_panel(void)
{
	// 256×64 noise: the LZW table fills and must be cleared mid-frame.
	img_target_t big = {256, 64, false};
	img_frames_t *fr = img_frames_new_mem(&big, 2);
	size_t fb = 256 * 64 * 3;
	uint8_t *rgb = malloc(fb), *back_rgb = malloc(fb);
	uint32_t seed = 1;
	for (size_t i = 0; i < fb; i += 3) {
		seed = seed * 1103515245 + 12345;
		uint8_t v = (uint8_t)((seed >> 16) % 200);
		rgb[i] = v, rgb[i + 1] = (uint8_t)(v / 2), rgb[i + 2] = 7;
	}
	img_frames_add(fr, rgb, 3);
	img_frames_add(fr, rgb, 3);
	mem_sink_t sink = {0};
	img_writer_t w = {sink_write, &sink};
	CHECK(img_encode_gif(fr, &w, NULL) == IMG_OK, "encode");
	FILE *f = fopen(OUT "enc_noise.gif", "wb");
	fwrite(sink.buf, 1, sink.len, f);
	fclose(f);
	f = fopen(OUT "enc_noise.rgb", "wb");
	fwrite(rgb, 1, fb, f);
	fwrite(rgb, 1, fb, f);
	fclose(f);
	img_frames_t *back = img_frames_new_mem(&big, 2);
	img_info_t info;
	CHECK(img_decode_mem(sink.buf, sink.len, back, &info) == IMG_OK, "re-decode");
	img_frames_get(back, 1, back_rgb, NULL);
	CHECK(memcmp(rgb, back_rgb, fb) == 0, "noise frame differs");
	free(rgb);
	free(back_rgb);
	free(sink.buf);
	img_frames_free(fr);
	img_frames_free(back);
}

static void test_file_store_and_limit(void)
{
	size_t len;
	uint8_t *data = read_file(FIX "anim_small.gif", &len);
	img_frames_t *fr = img_frames_new_file(&panel, OUT "frames.bin", 1000);
	CHECK(fr != NULL, "file store");
	img_info_t info;
	CHECK(img_decode_mem(data, len, fr, &info) == IMG_OK, "decode to file store");
	CHECK(img_frames_count(fr) == 4, "%zu frames", img_frames_count(fr));
	size_t want_len;
	uint8_t *want = read_file(FIX "anim_small.rgb", &want_len), *rgb = malloc(4608);
	img_frames_get(fr, 2, rgb, NULL);
	CHECK(memcmp(rgb, want + 2 * 4608, 4608) == 0, "frame 2 from file");
	img_frames_free(fr);

	fr = img_frames_new_mem(&panel, 2);
	CHECK(img_decode_mem(data, len, fr, &info) == IMG_ERR_FULL, "frame limit");
	img_frames_free(fr);
	free(rgb);
	free(want);
	free(data);
}

static void test_fit(void)
{
	// The inverse mapping agrees with sampling for many sizes.
	uint32_t sizes[] = {1, 2, 7, 16, 40, 95, 96, 97, 300, 1000, 4096};
	for (size_t a = 0; a < sizeof sizes / sizeof *sizes; a++)
		for (size_t b = 0; b < sizeof sizes / sizeof *sizes; b++) {
			img_fit_t fit;
			img_fit_init(&fit, &panel, sizes[a], sizes[b], sizes[a], sizes[b]);
			for (uint32_t s = 0; s < sizes[a]; s++) {
				int t0, t1;
				img_fit_cols(&fit, s, s + 1, &t0, &t1);
				for (int tx = fit.off_x; tx < fit.off_x + fit.new_w; tx++) {
					bool in = img_fit_sx(&fit, tx) == s;
					CHECK(in == (tx >= t0 && tx < t1), "%ux%u col %u tx %d", sizes[a], sizes[b], s, tx);
				}
			}
		}
	// Extreme aspect: nothing visible, nothing drawn.
	img_fit_t fit;
	img_fit_init(&fit, &panel, 5000, 1, 5000, 1);
	CHECK(fit.new_h == 0, "new_h %d", fit.new_h);
}

static void test_detect(void)
{
	CHECK(img_detect((const uint8_t *)"GIF89a", 6) == IMG_FMT_GIF, "gif");
	CHECK(img_detect((const uint8_t *)"\x89PNG\r\n\x1a\n", 8) == IMG_FMT_PNG, "png");
	CHECK(img_detect((const uint8_t *)"\xff\xd8\xff\xe0", 4) == IMG_FMT_JPEG, "jpeg");
	CHECK(img_detect((const uint8_t *)"BM", 2) == IMG_FMT_UNKNOWN, "bmp");
	img_frames_t *fr = img_frames_new_mem(&panel, 4);
	img_info_t info;
	CHECK(img_decode_mem((const uint8_t *)"not an image", 12, fr, &info) == IMG_ERR_FORMAT, "rejects text");
	CHECK(img_decode_mem((const uint8_t *)"", 0, fr, &info) == IMG_ERR_FORMAT, "rejects empty");
	img_frames_free(fr);
}

static img_err_t scan_file(const char *path, img_gif_scan_t *s)
{
	size_t len;
	uint8_t *data = read_file(path, &len);
	chunk_src_t src = {data, len, 0, 100};
	img_reader_t r = {chunk_read, &src};
	img_err_t err = img_gif_scan(&r, s);
	free(data);
	return err;
}

static void test_passthrough(void)
{
	img_gif_scan_t s;
	const char *why;
	CHECK(scan_file(FIX "pass_ok.gif", &s) == IMG_OK, "scan pass_ok");
	CHECK(s.frames == 3 && s.total_ms == 120 && s.width == 96 && s.height == 16, "%u frames %u ms", s.frames,
	      s.total_ms);
	CHECK(img_gif_passthrough(&s, &panel, false, &why), "pass_ok refused: %s", why);
	img_target_t flipped = {96, 16, true};
	CHECK(!img_gif_passthrough(&s, &flipped, false, &why), "upside down passes");

	CHECK(scan_file(FIX "pass_lct.gif", &s) == IMG_OK, "scan pass_lct");
	CHECK(s.local_color_table && !img_gif_passthrough(&s, &panel, false, &why), "local colour tables pass");

	CHECK(scan_file(FIX "anim_small.gif", &s) == IMG_OK, "scan anim_small");
	CHECK(s.frames == 4 && s.zero_delay && s.partial_frame && s.transparency, "anim_small flags");
	CHECK(!img_gif_passthrough(&s, &panel, false, &why) && strstr(why, "size"), "anim_small: %s", why);

	// The panel experiments: refused by default, passed with allow_partial.
	CHECK(scan_file(FIX "pt_transparent.gif", &s) == IMG_OK, "scan pt_transparent");
	CHECK(s.frames == 2 && s.total_ms == 2000 && s.transparency && !s.partial_frame, "pt_transparent flags");
	CHECK(!img_gif_passthrough(&s, &panel, false, &why) && strstr(why, "transparency"), "pt_transparent: %s", why);
	CHECK(img_gif_passthrough(&s, &panel, true, &why), "pt_transparent refused: %s", why);
	CHECK(scan_file(FIX "pt_dispose.gif", &s) == IMG_OK, "scan pt_dispose");
	CHECK(s.frames == 7 && s.total_ms == 2000 && s.partial_frame, "pt_dispose flags");
	CHECK(!img_gif_passthrough(&s, &panel, false, &why) && strstr(why, "cover"), "pt_dispose: %s", why);
	CHECK(img_gif_passthrough(&s, &panel, true, &why), "pt_dispose refused: %s", why);

	CHECK(scan_file(FIX "png_small.png", &s) == IMG_ERR_FORMAT, "png scanned as gif");
}

static void test_corrupt_inputs(void)
{
	// Truncations and random byte flips must fail cleanly (ASan watches).
	const char *files[] = {FIX "anim_small.gif", FIX "anim_wide.gif", FIX "png_rgba.png", FIX "photo.jpg"};
	uint32_t seed = 42;
	for (size_t f = 0; f < sizeof files / sizeof *files; f++) {
		size_t len;
		uint8_t *data = read_file(files[f], &len), *mut = malloc(len);
		for (int round = 0; round < 300; round++) {
			memcpy(mut, data, len);
			size_t n = len;
			if (round % 3 == 0) {
				seed = seed * 1103515245 + 12345;
				n = (seed >> 8) % len;
			} else {
				for (int k = 0; k < 1 + round % 8; k++) {
					seed = seed * 1103515245 + 12345;
					mut[(seed >> 8) % len] ^= (uint8_t)(seed >> 3);
				}
			}
			img_frames_t *fr = img_frames_new_mem(&panel, 64);
			img_info_t info;
			img_decode_mem(mut, n, fr, &info);
			img_frames_free(fr);
			chunk_src_t src = {mut, n, 0, 512};
			img_reader_t r = {chunk_read, &src};
			img_gif_scan_t s;
			img_gif_scan(&r, &s);
		}
		free(mut);
		free(data);
	}
}

int main(void)
{
	static const struct test_case cases[] = {
		TEST(test_fit),
		TEST(test_detect),
		TEST(test_gif_keyframed),
		TEST(test_upside_down),
		TEST(test_png),
		TEST(test_jpeg),
		TEST(test_encode_roundtrip),
		TEST(test_encode_dithered),
		TEST(test_encode_large_panel),
		TEST(test_file_store_and_limit),
		TEST(test_passthrough),
		TEST(test_corrupt_inputs),
	};
	return run_tests("imaging", cases, sizeof cases / sizeof cases[0]) != 0;
}
