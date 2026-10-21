#include <stdlib.h>
#include <string.h>

#include "img_internal.h"

const char *img_strerror(img_err_t err)
{
	switch (err) {
	case IMG_OK:
		return "ok";
	case IMG_ERR_FORMAT:
		return "not a PNG, JPEG or GIF image";
	case IMG_ERR_CORRUPT:
		return "invalid or truncated image data";
	case IMG_ERR_TOO_BIG:
		return "image dimensions too large";
	case IMG_ERR_NOMEM:
		return "out of memory";
	case IMG_ERR_IO:
		return "read or write failed";
	case IMG_ERR_FULL:
		return "too many frames for the available space";
	case IMG_ERR_UNSUPPORTED:
		return "unsupported image variant";
	}
	return "unknown error";
}

void br_init(img_br_t *br, const img_reader_t *src)
{
	memset(br, 0, sizeof *br);
	br->src = src;
}

bool br_fill(img_br_t *br)
{
	if (br->pos < br->len)
		return true;
	if (br->eof || br->error)
		return false;
	int n = br->src->read(br->src->ctx, br->buf, sizeof br->buf);
	if (n < 0)
		br->error = true;
	if (n <= 0) {
		br->eof = true;
		return false;
	}
	br->pos = 0;
	br->len = (size_t)n;
	return true;
}

bool br_read(img_br_t *br, uint8_t *out, size_t n)
{
	while (n > 0) {
		if (!br_fill(br))
			return false;
		size_t k = br->len - br->pos;
		if (k > n)
			k = n;
		memcpy(out, br->buf + br->pos, k);
		br->pos += k;
		out += k;
		n -= k;
	}
	return true;
}

bool br_byte(img_br_t *br, uint8_t *out)
{
	if (!br_fill(br))
		return false;
	*out = br->buf[br->pos++];
	return true;
}

bool br_skip(img_br_t *br, size_t n)
{
	while (n > 0) {
		if (!br_fill(br))
			return false;
		size_t k = br->len - br->pos;
		if (k > n)
			k = n;
		br->pos += k;
		n -= k;
	}
	return true;
}

size_t br_peek(img_br_t *br, const uint8_t **out, size_t n)
{
	if (!br_fill(br))
		return 0;
	// Detection needs the first few bytes in one piece; move what is left
	// to the front and top the buffer up.
	if (br->len - br->pos < n) {
		memmove(br->buf, br->buf + br->pos, br->len - br->pos);
		br->len -= br->pos;
		br->pos = 0;
		while (br->len < n && !br->eof) {
			int r = br->src->read(br->src->ctx, br->buf + br->len, sizeof br->buf - br->len);
			if (r < 0)
				br->error = true;
			if (r <= 0) {
				br->eof = true;
				break;
			}
			br->len += (size_t)r;
		}
	}
	*out = br->buf + br->pos;
	size_t avail = br->len - br->pos;
	return avail < n ? avail : n;
}

void img_fit_init(img_fit_t *fit, const img_target_t *t, uint32_t orig_w, uint32_t orig_h, uint32_t src_w,
		  uint32_t src_h)
{
	memset(fit, 0, sizeof *fit);
	fit->width = t->width;
	fit->height = t->height;
	fit->src_w = src_w;
	fit->src_h = src_h;
	if (orig_w == 0 || orig_h == 0)
		return;
	double sx = (double)t->width / orig_w, sy = (double)t->height / orig_h;
	double scale = sx < sy ? sx : sy;
	fit->new_w = (int)((double)orig_w * scale);
	fit->new_h = (int)((double)orig_h * scale);
	fit->off_x = (t->width - fit->new_w) / 2;
	fit->off_y = (t->height - fit->new_h) / 2;
}

static uint32_t sample(int d, int dlen, uint32_t slen)
{
	return (uint32_t)(((2 * (uint64_t)d + 1) * slen) / (2 * (uint64_t)dlen));
}

uint32_t img_fit_sx(const img_fit_t *fit, int tx)
{
	return sample(tx - fit->off_x, fit->new_w, fit->src_w);
}

uint32_t img_fit_sy(const img_fit_t *fit, int ty)
{
	return sample(ty - fit->off_y, fit->new_h, fit->src_h);
}

// first_target returns the first d in [0, dlen] whose sample is >= s:
// ceil((2 s D - S) / 2S).
static int first_target(uint32_t s, int dlen, uint32_t slen)
{
	int64_t num = 2 * (int64_t)s * dlen - (int64_t)slen, den = 2 * (int64_t)slen;
	int64_t d = num <= 0 ? 0 : (num + den - 1) / den;
	return d > dlen ? dlen : (int)d;
}

static void span(uint32_t s0, uint32_t s1, int dlen, uint32_t slen, int off, int *t0, int *t1)
{
	if (dlen <= 0 || slen == 0 || s0 >= s1) {
		*t0 = *t1 = 0;
		return;
	}
	*t0 = first_target(s0, dlen, slen) + off;
	*t1 = first_target(s1, dlen, slen) + off;
}

void img_fit_cols(const img_fit_t *fit, uint32_t s0, uint32_t s1, int *t0, int *t1)
{
	span(s0, s1, fit->new_w, fit->src_w, fit->off_x, t0, t1);
}

void img_fit_rows(const img_fit_t *fit, uint32_t s0, uint32_t s1, int *t0, int *t1)
{
	span(s0, s1, fit->new_h, fit->src_h, fit->off_y, t0, t1);
}

void canvas_fill_black(uint8_t *canvas, size_t w, size_t h)
{
	memset(canvas, 0, w * h * 3);
}

void canvas_rotate180(uint8_t *dst, const uint8_t *src, size_t w, size_t h)
{
	size_t n = w * h;
	for (size_t i = 0; i < n; i++)
		memcpy(dst + (n - 1 - i) * 3, src + i * 3, 3);
}

img_format_t img_detect(const uint8_t *head, size_t len)
{
	if (len >= 8 && memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0)
		return IMG_FMT_PNG;
	if (len >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff)
		return IMG_FMT_JPEG;
	if (len >= 6 && (memcmp(head, "GIF87a", 6) == 0 || memcmp(head, "GIF89a", 6) == 0))
		return IMG_FMT_GIF;
	return IMG_FMT_UNKNOWN;
}

img_err_t img_decode(const img_reader_t *r, img_frames_t *frames, img_info_t *info)
{
	img_br_t *br = malloc(sizeof *br);
	if (!br)
		return IMG_ERR_NOMEM;
	br_init(br, r);
	memset(info, 0, sizeof *info);

	const uint8_t *head;
	size_t n = br_peek(br, &head, 8);
	img_err_t err;
	switch (info->format = img_detect(head, n)) {
	case IMG_FMT_GIF:
		err = img_decode_gif(br, frames, info);
		break;
	case IMG_FMT_PNG:
		err = img_decode_png(br, frames, info);
		break;
	case IMG_FMT_JPEG:
		err = img_decode_jpeg(br, frames, info);
		break;
	default:
		err = br->error ? IMG_ERR_IO : IMG_ERR_FORMAT;
	}
	if (err == IMG_ERR_CORRUPT && br->error)
		err = IMG_ERR_IO;
	info->frames = img_frames_count(frames);
	free(br);
	return err;
}

typedef struct {
	const uint8_t *data;
	size_t len, pos;
} mem_src_t;

static int mem_read(void *ctx, uint8_t *buf, size_t len)
{
	mem_src_t *m = ctx;
	size_t k = m->len - m->pos;
	if (k > len)
		k = len;
	memcpy(buf, m->data + m->pos, k);
	m->pos += k;
	return (int)k;
}

img_err_t img_decode_mem(const uint8_t *data, size_t len, img_frames_t *frames, img_info_t *info)
{
	mem_src_t m = {data, len, 0};
	img_reader_t r = {mem_read, &m};
	return img_decode(&r, frames, info);
}
