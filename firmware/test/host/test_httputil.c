#include <stdint.h>
#include <stdlib.h>

#include "jsonb64.h"
#include "multipart.h"
#include "test.h"

typedef struct {
	const uint8_t *data;
	size_t len, pos, chunk;
} src_t;

static int src_read(void *ctx, uint8_t *buf, size_t len)
{
	src_t *s = ctx;
	size_t k = s->len - s->pos;
	if (k > len)
		k = len;
	if (k > s->chunk)
		k = s->chunk;
	memcpy(buf, s->data + s->pos, k);
	s->pos += k;
	return (int)k;
}

static size_t build_body(uint8_t *out, const char *boundary, const uint8_t *file, size_t file_len)
{
	size_t n = 0;
	n += (size_t)sprintf((char *)out + n, "preamble\r\n--%s\r\n", boundary);
	n += (size_t)sprintf((char *)out + n, "Content-Disposition: form-data; name=\"command\"\r\n\r\nsend_static\r\n");
	n += (size_t)sprintf((char *)out + n, "--%s\r\nContent-Disposition: form-data; name=\"static\"; filename=\"a.png\"\r\n"
					      "Content-Type: image/png\r\n\r\n", boundary);
	memcpy(out + n, file, file_len);
	n += file_len;
	n += (size_t)sprintf((char *)out + n, "\r\n--%s\r\nContent-Disposition: form-data; name=\"empty\"\r\n\r\n", boundary);
	n += (size_t)sprintf((char *)out + n, "\r\n--%s--\r\nepilogue", boundary);
	return n;
}

static void test_multipart(void)
{
	const char *boundary = "----WebKitFormBoundaryAbC123";
	// File data with near-miss delimiters in it.
	uint8_t file[3000];
	for (size_t i = 0; i < sizeof file; i++)
		file[i] = (uint8_t)(i * 7);
	memcpy(file + 100, "\r\n------WebKitFormBoundaryAbC12", 31);
	memcpy(file + 2990, "\r\n--", 4);
	uint8_t *body = malloc(8192);
	size_t len = build_body(body, boundary, file, sizeof file);

	for (size_t chunk = 1; chunk <= 4096; chunk = chunk * 3 + 1) {
		src_t s = {body, len, 0, chunk};
		mp_t *mp = malloc(sizeof *mp);
		mp_init(mp, boundary, src_read, &s);
		mp_part_t part;
		CHECK(mp_next_part(mp, &part) == 1, "first part (chunk %zu)", chunk);
		CHECK(strcmp(part.name, "command") == 0 && !part.is_file, "name '%s'", part.name);
		char text[32];
		size_t tl = 0;
		int n;
		while ((n = mp_read(mp, (uint8_t *)text + tl, sizeof text - 1 - tl)) > 0)
			tl += (size_t)n;
		text[tl] = 0;
		CHECK(strcmp(text, "send_static") == 0, "value '%s' (chunk %zu)", text, chunk);

		CHECK(mp_next_part(mp, &part) == 1, "second part");
		CHECK(strcmp(part.name, "static") == 0 && part.is_file && strcmp(part.filename, "a.png") == 0 &&
			      strcmp(part.content_type, "image/png") == 0,
		      "file part '%s' '%s' '%s'", part.name, part.filename, part.content_type);
		uint8_t got[4000];
		size_t total = 0;
		while ((n = mp_read(mp, got + total, chunk)) > 0)
			total += (size_t)n;
		CHECK(n == 0, "read error");
		CHECK(total == sizeof file && memcmp(got, file, sizeof file) == 0, "file body %zu bytes (chunk %zu)",
		      total, chunk);

		CHECK(mp_next_part(mp, &part) == 1 && strcmp(part.name, "empty") == 0, "third part");
		CHECK(mp_read(mp, got, sizeof got) == 0, "empty body");
		CHECK(mp_next_part(mp, &part) == 0, "end");
		free(mp);
	}

	// Skipping a part without reading it.
	src_t s = {body, len, 0, 100};
	mp_t mp;
	mp_part_t part;
	mp_init(&mp, boundary, src_read, &s);
	CHECK(mp_next_part(&mp, &part) == 1 && mp_next_part(&mp, &part) == 1 && mp_next_part(&mp, &part) == 1 &&
		      strcmp(part.name, "empty") == 0,
	      "skip parts");

	// Truncated in the file part.
	s = (src_t){body, 400, 0, 64};
	mp_init(&mp, boundary, src_read, &s);
	mp_next_part(&mp, &part);
	mp_next_part(&mp, &part);
	uint8_t got[4000];
	int n;
	while ((n = mp_read(&mp, got, sizeof got)) > 0) {
	}
	CHECK(n < 0, "truncated body not reported");
	free(body);
}

static void test_boundary(void)
{
	char b[80];
	CHECK(mp_boundary("multipart/form-data; boundary=abc", b, sizeof b) && strcmp(b, "abc") == 0, "plain");
	CHECK(mp_boundary("multipart/form-data; boundary=\"a b;c\"", b, sizeof b) && strcmp(b, "a b;c") == 0,
	      "quoted");
	CHECK(mp_boundary("Multipart/Form-Data; charset=x; boundary=zz; x=y", b, sizeof b) && strcmp(b, "zz") == 0,
	      "params");
	CHECK(!mp_boundary("application/json", b, sizeof b), "json");
	CHECK(!mp_boundary("multipart/form-data", b, sizeof b), "no boundary");
}

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64enc(const uint8_t *in, size_t n, char *out, bool escape_slash, bool wrap)
{
	size_t o = 0;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
		char q[4] = {b64chars[v >> 18 & 63], b64chars[v >> 12 & 63], i + 1 < n ? b64chars[v >> 6 & 63] : '=',
			     i + 2 < n ? b64chars[v & 63] : '='};
		for (int k = 0; k < 4; k++) {
			if (escape_slash && q[k] == '/')
				out[o++] = '\\';
			out[o++] = q[k];
		}
		if (wrap && i % 57 == 54) {
			out[o++] = '\\';
			out[o++] = 'n';
		}
	}
	out[o] = 0;
	return o;
}

static int parse(const char *json, size_t chunk, uint8_t *out, size_t *out_len, jb_t *jb)
{
	jb_init(jb);
	size_t len = strlen(json), pos = 0, w = 0;
	uint8_t *buf = malloc(chunk + JB_OUT_SLACK);
	while (pos < len) {
		size_t k = len - pos < chunk ? len - pos : chunk, used;
		int n = jb_feed(jb, (const uint8_t *)json + pos, k, buf, &used);
		if (n < 0) {
			free(buf);
			return n;
		}
		memcpy(out + w, buf, (size_t)n);
		w += (size_t)n;
		pos += used;
		if (jb->state == 8 && pos < len) // S_DONE: the rest is ignored
			break;
	}
	free(buf);
	*out_len = w;
	return 0;
}

static void test_jsonb64(void)
{
	uint8_t data[1000];
	for (size_t i = 0; i < sizeof data; i++)
		data[i] = (uint8_t)(i * 31 + 7);
	char *b64 = malloc(4000), *json = malloc(5000);
	uint8_t *out = malloc(1200);
	jb_t jb;
	size_t n;

	for (size_t len = 0; len < 5; len++) {
		b64enc(data, 998 + len % 3, b64, false, false);
		sprintf(json, "{\"image\":\"%s\",\"type\":\"animated\"}", b64);
		for (size_t chunk = 1; chunk < 600; chunk = chunk * 2 + 3) {
			CHECK(parse(json, chunk, out, &n, &jb) == 0, "parse (chunk %zu)", chunk);
			CHECK(n == 998 + len % 3 && memcmp(out, data, n) == 0, "decoded %zu bytes", n);
			CHECK(jb_image_complete(&jb) && strcmp(jb_type(&jb), "animated") == 0, "type '%s'",
			      jb_type(&jb));
		}
	}

	// Type first, other keys with nested values, escaped slashes, wrapping.
	b64enc(data, 600, b64, true, true);
	sprintf(json, " { \"type\" : \"static\", \"meta\": {\"a\": [1, \"}\", {\"b\": null}]}, \"n\": -1.5e3, "
		      "\"flag\": true, \"image\" : \"%s\" , \"x\": \"\\\"image\\\"\" } ",
		b64);
	CHECK(parse(json, 7, out, &n, &jb) == 0, "complex parse");
	CHECK(n == 600 && memcmp(out, data, 600) == 0, "complex decoded %zu", n);
	CHECK(jb_image_complete(&jb) && strcmp(jb_type(&jb), "static") == 0, "complex type '%s'", jb_type(&jb));

	CHECK(parse("{\"type\":\"static\"}", 4, out, &n, &jb) == 0 && !jb_image_complete(&jb), "missing image");
	CHECK(parse("[1,2]", 4, out, &n, &jb) == JB_ERR_JSON, "array");
	CHECK(parse("not json", 4, out, &n, &jb) == JB_ERR_JSON, "text");
	CHECK(parse("{\"image\":\"ab$d\"}", 4, out, &n, &jb) == JB_ERR_BASE64, "bad base64");
	CHECK(parse("{\"image\":\"abcde\"}", 4, out, &n, &jb) == JB_ERR_BASE64, "dangling base64 char");
	CHECK(parse("{\"image\":\"QUJD\"", 4, out, &n, &jb) == 0 && !jb_image_complete(&jb), "unterminated object");
	CHECK(parse("{\"averyveryverylongkey\":\"QUJD\",\"image\":\"QUI=\"}", 3, out, &n, &jb) == 0 && n == 2 &&
		      memcmp(out, "AB", 2) == 0,
	      "long key skipped");
	free(b64);
	free(json);
	free(out);
}

int main(void)
{
	static const struct test_case cases[] = {TEST(test_boundary), TEST(test_multipart), TEST(test_jsonb64)};
	return run_tests("httputil", cases, sizeof cases / sizeof cases[0]) != 0;
}
