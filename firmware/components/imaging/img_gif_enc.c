// GIF encoder for the frames the panel plays (display.go imageToGif and
// processAnimatedGif): pixels mapped to the shared palette with
// Floyd–Steinberg dithering, LZW compressed.
#include <stdlib.h>
#include <string.h>

#include "img_internal.h"

#define STILL_DELAY_CS 60000 // stills are held for 600 s so the panel never shows its splash
#define DISPOSE_BACKGROUND 2
#define MAX_CODES 4096
#define NONE 0xffff

typedef struct {
	const img_writer_t *w;
	uint8_t buf[512];
	size_t len;
	uint32_t total;
	bool error;
} out_t;

static void out_flush(out_t *o)
{
	if (o->len && !o->error && o->w->write(o->w->ctx, o->buf, o->len) < 0)
		o->error = true;
	o->len = 0;
}

static void out_bytes(out_t *o, const void *data, size_t n)
{
	const uint8_t *p = data;
	o->total += (uint32_t)n;
	while (n > 0) {
		if (o->len == sizeof o->buf)
			out_flush(o);
		size_t k = sizeof o->buf - o->len;
		if (k > n)
			k = n;
		memcpy(o->buf + o->len, p, k);
		o->len += k;
		p += k;
		n -= k;
	}
}

static void out_byte(out_t *o, uint8_t b)
{
	out_bytes(o, &b, 1);
}

static void out_u16(out_t *o, uint32_t v)
{
	uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	out_bytes(o, b, 2);
}

typedef struct {
	out_t *o;
	// Encoder dictionary as a trie: children of a code are a linked list.
	uint16_t first[MAX_CODES], next[MAX_CODES];
	uint8_t ch[MAX_CODES];
	int free_ent;
	// What the decoder's code width and table size are at this point; the
	// decoder adds each entry one code later than the encoder.
	int dec_size, dec_next;
	bool dec_first;
	int min_size, clear, eoi;
	// Bit packer and the current 255-byte sub-block.
	uint32_t bits;
	int nbits;
	uint8_t block[255];
	int block_len;
} lzw_t;

static void lzw_block_byte(lzw_t *z, uint8_t b)
{
	z->block[z->block_len++] = b;
	if (z->block_len == 255) {
		out_byte(z->o, 255);
		out_bytes(z->o, z->block, 255);
		z->block_len = 0;
	}
}

static void lzw_reset(lzw_t *z)
{
	for (int i = 0; i < MAX_CODES; i++)
		z->first[i] = NONE;
	z->free_ent = z->eoi + 1;
	z->dec_size = z->min_size + 1;
	z->dec_next = z->eoi + 1;
	z->dec_first = true;
}

static void lzw_emit(lzw_t *z, int code)
{
	z->bits |= (uint32_t)code << z->nbits;
	z->nbits += z->dec_size;
	while (z->nbits >= 8) {
		lzw_block_byte(z, (uint8_t)z->bits);
		z->bits >>= 8;
		z->nbits -= 8;
	}
	if (code == z->clear) {
		lzw_reset(z);
		return;
	}
	// Mirror the decoder: every code after the first since a clear adds an
	// entry, and the width grows once the table reaches 1 << width.
	if (!z->dec_first && z->dec_next < MAX_CODES) {
		z->dec_next++;
		if (z->dec_next == 1 << z->dec_size && z->dec_size < 12)
			z->dec_size++;
	}
	z->dec_first = false;
}

static void lzw_begin(lzw_t *z, out_t *o, int min_size)
{
	z->o = o;
	z->min_size = min_size;
	z->clear = 1 << min_size;
	z->eoi = z->clear + 1;
	z->bits = 0;
	z->nbits = 0;
	z->block_len = 0;
	lzw_reset(z);
	out_byte(o, (uint8_t)min_size);
	lzw_emit(z, z->clear);
}

static void lzw_pixels(lzw_t *z, const uint8_t *idx, size_t n)
{
	if (n == 0)
		return;
	int prefix = idx[0];
	for (size_t i = 1; i < n; i++) {
		uint8_t p = idx[i];
		int c = z->first[prefix];
		while (c != NONE && z->ch[c] != p)
			c = z->next[c];
		if (c != NONE) {
			prefix = c;
			continue;
		}
		lzw_emit(z, prefix);
		if (z->free_ent < MAX_CODES) {
			int e = z->free_ent++;
			z->ch[e] = p;
			z->first[e] = NONE;
			z->next[e] = z->first[prefix];
			z->first[prefix] = (uint16_t)e;
		} else {
			lzw_emit(z, z->clear);
		}
		prefix = p;
	}
	lzw_emit(z, prefix);
}

static void lzw_end(lzw_t *z)
{
	lzw_emit(z, z->eoi);
	if (z->nbits > 0)
		lzw_block_byte(z, (uint8_t)z->bits);
	if (z->block_len) {
		out_byte(z->o, (uint8_t)z->block_len);
		out_bytes(z->o, z->block, (size_t)z->block_len);
	}
	out_byte(z->o, 0);
}

static int nearest(const uint8_t *pal, size_t n, int r, int g, int b)
{
	int best = 0;
	long best_d = -1;
	for (size_t i = 0; i < n; i++) {
		long dr = r - pal[i * 3], dg = g - pal[i * 3 + 1], db = b - pal[i * 3 + 2];
		long d = dr * dr + dg * dg + db * db;
		if (best_d < 0 || d < best_d) {
			best_d = d;
			best = (int)i;
			if (d == 0)
				break;
		}
	}
	return best;
}

static int clamp255(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

// quantize maps rgb to palette indices with Floyd–Steinberg dithering, as
// draw.FloydSteinberg does in the Go code. When every colour is in the
// palette (256 or fewer in all) no error arises and the mapping is exact.
// err holds two rows of (w + 2) × 3 errors scaled by 16.
static void quantize(const img_frames_t *f, const uint8_t *rgb, uint8_t *idx, int w, int h, int *err)
{
	const uint8_t *pal;
	size_t n = img_frames_palette(f, &pal);
	int *cur = err, *nxt = err + (w + 2) * 3;
	memset(err, 0, sizeof(int) * (size_t)(w + 2) * 3 * 2);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t *s = rgb + ((size_t)y * w + x) * 3;
			int *e = cur + (x + 1) * 3;
			int r = clamp255(s[0] + (e[0] + 8) / 16);
			int g = clamp255(s[1] + (e[1] + 8) / 16);
			int b = clamp255(s[2] + (e[2] + 8) / 16);
			int i = img_frames_lookup(f, (uint8_t)r, (uint8_t)g, (uint8_t)b);
			if (i < 0)
				i = nearest(pal, n, r, g, b);
			idx[(size_t)y * w + x] = (uint8_t)i;
			int q[3] = {r - pal[i * 3], g - pal[i * 3 + 1], b - pal[i * 3 + 2]};
			for (int c = 0; c < 3; c++) {
				if (!q[c])
					continue;
				cur[(x + 2) * 3 + c] += q[c] * 7;
				nxt[x * 3 + c] += q[c] * 3;
				nxt[(x + 1) * 3 + c] += q[c] * 5;
				nxt[(x + 2) * 3 + c] += q[c];
			}
		}
		int *t = cur;
		cur = nxt;
		nxt = t;
		memset(nxt, 0, sizeof(int) * (size_t)(w + 2) * 3);
	}
}

img_err_t img_encode_gif(img_frames_t *frames, const img_writer_t *w, img_gif_info_t *info)
{
	size_t count = img_frames_count(frames);
	if (count == 0)
		return IMG_ERR_CORRUPT;
	const img_target_t *t = img_frames_target(frames);
	int width = t->width, height = t->height;
	bool still = count == 1;

	const uint8_t *pal;
	size_t pal_len = img_frames_palette(frames, &pal);
	int table_bits = 1;
	while ((size_t)1 << table_bits < pal_len)
		table_bits++;
	size_t table_len = (size_t)1 << table_bits;
	int min_size = table_bits < 2 ? 2 : table_bits;

	size_t px = (size_t)width * height;
	uint8_t *rgb = malloc(px * 3), *idx = malloc(px);
	int *err = malloc(sizeof(int) * (size_t)(width + 2) * 3 * 2);
	lzw_t *z = malloc(sizeof *z);
	out_t *o = calloc(1, sizeof *o);
	img_err_t res = IMG_OK;
	if (!rgb || !idx || !err || !z || !o) {
		res = IMG_ERR_NOMEM;
		goto done;
	}
	o->w = w;

	// Header and logical screen; animations get the shared palette as a
	// global colour table, stills carry it as a local one.
	out_bytes(o, "GIF89a", 6);
	out_u16(o, (uint32_t)width);
	out_u16(o, (uint32_t)height);
	out_byte(o, still ? 0 : (uint8_t)(0x80 | (table_bits - 1)));
	out_byte(o, 0);
	out_byte(o, 0);
	uint8_t zero[3] = {0};
	if (!still) {
		out_bytes(o, pal, pal_len * 3);
		for (size_t i = pal_len; i < table_len; i++)
			out_bytes(o, zero, 3);
		// Loop forever (NETSCAPE2.0, count 0).
		out_bytes(o, "\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19);
	}

	uint32_t total_cs = 0;
	for (size_t i = 0; i < count && !o->error; i++) {
		uint16_t delay;
		if ((res = img_frames_get(frames, i, rgb, &delay)) != IMG_OK)
			goto done;
		if (still)
			delay = STILL_DELAY_CS;
		total_cs += delay;

		uint8_t gce[8] = {0x21, 0xf9, 0x04, still ? 0 : DISPOSE_BACKGROUND << 2, (uint8_t)delay,
				  (uint8_t)(delay >> 8), 0x00, 0x00};
		out_bytes(o, gce, sizeof gce);
		out_byte(o, 0x2c);
		out_u16(o, 0);
		out_u16(o, 0);
		out_u16(o, (uint32_t)width);
		out_u16(o, (uint32_t)height);
		if (still) {
			out_byte(o, (uint8_t)(0x80 | (table_bits - 1)));
			out_bytes(o, pal, pal_len * 3);
			for (size_t k = pal_len; k < table_len; k++)
				out_bytes(o, zero, 3);
		} else {
			out_byte(o, 0);
		}

		quantize(frames, rgb, idx, width, height, err);
		lzw_begin(z, o, min_size);
		lzw_pixels(z, idx, px);
		lzw_end(z);
	}
	out_byte(o, 0x3b);
	out_flush(o);
	if (o->error) {
		res = IMG_ERR_IO;
		goto done;
	}
	if (info) {
		info->frames = (uint32_t)count;
		info->total_ms = total_cs * 10;
		info->bytes = o->total;
		info->static_image = still;
	}
done:
	free(rgb);
	free(idx);
	free(err);
	free(z);
	free(o);
	return res;
}
