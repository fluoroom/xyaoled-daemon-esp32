// Streaming GIF decoder. Frames are composited on the panel-sized canvas
// as they are decoded, one LZW row at a time, as display.go's
// processAnimatedGif does at full size: disposal none / background
// (black, as the Go code fills) / previous, transparency, interlacing.
#include <stdlib.h>
#include <string.h>

#include "img_internal.h"

#define FALLBACK_DELAY_CS 5 // zero delays become 50 ms so the panel never flashes its splash
#define MAX_CODES 4096

enum { DISPOSE_NONE = 1, DISPOSE_BACKGROUND = 2, DISPOSE_PREVIOUS = 3 };

typedef struct {
	img_br_t *br;
	img_frames_t *frames;
	img_fit_t fit;
	bool fit_ready;
	uint32_t screen_w, screen_h;

	uint8_t gct[256 * 3];
	int gct_len;
	uint8_t lct[256 * 3];

	// Graphic control extension for the next image.
	uint16_t delay_cs;
	int disposal;
	int transparent;

	uint8_t *canvas, *saved;

	// LZW state.
	uint16_t prefix[MAX_CODES];
	uint8_t suffix[MAX_CODES];
	uint8_t stack[MAX_CODES + 1];

	// Sub-block bit reader.
	uint32_t bits;
	int nbits;
	int block_left;
	bool data_end;
} gif_t;

static bool skip_sub_blocks(img_br_t *br)
{
	for (;;) {
		uint8_t n;
		if (!br_byte(br, &n))
			return false;
		if (n == 0)
			return true;
		if (!br_skip(br, n))
			return false;
	}
}

// read_code returns the next size-bit code, or -1 at the end of the data.
static int read_code(gif_t *g, int size)
{
	while (g->nbits < size) {
		if (g->data_end)
			return -1;
		if (g->block_left == 0) {
			uint8_t n;
			if (!br_byte(g->br, &n) || n == 0) {
				g->data_end = true;
				return -1;
			}
			g->block_left = n;
		}
		uint8_t b;
		if (!br_byte(g->br, &b)) {
			g->data_end = true;
			return -1;
		}
		g->block_left--;
		g->bits |= (uint32_t)b << g->nbits;
		g->nbits += 8;
	}
	int code = (int)(g->bits & ((1u << size) - 1));
	g->bits >>= size;
	g->nbits -= size;
	return code;
}

typedef struct {
	uint32_t x, y, w, h;
	bool interlaced;
	const uint8_t *palette;
	int palette_len;
	uint8_t *row;
	uint32_t row_fill, rows_done;
} frame_t;

// row_y maps the n-th decoded row to its image row.
static uint32_t row_y(const frame_t *f, uint32_t n)
{
	if (!f->interlaced)
		return n;
	uint32_t h = f->h, p1 = (h + 7) / 8, p2 = (h + 3) / 8, p3 = (h + 1) / 4;
	if (n < p1)
		return n * 8;
	n -= p1;
	if (n < p2)
		return n * 8 + 4;
	n -= p2;
	if (n < p3)
		return n * 4 + 2;
	n -= p3;
	return n * 2 + 1;
}

// draw_row composites one decoded row onto the canvas, at the panel
// pixels that sample it.
static void draw_row(gif_t *g, frame_t *f)
{
	uint32_t y = f->y + row_y(f, f->rows_done);
	int ty0, ty1, tx0, tx1;
	img_fit_rows(&g->fit, y, y + 1, &ty0, &ty1);
	if (ty0 >= ty1)
		return;
	img_fit_cols(&g->fit, f->x, f->x + f->w, &tx0, &tx1);
	for (int tx = tx0; tx < tx1; tx++) {
		uint32_t sx = img_fit_sx(&g->fit, tx) - f->x;
		int idx = f->row[sx];
		if (idx == g->transparent)
			continue;
		static const uint8_t black[3];
		const uint8_t *c = idx < f->palette_len ? f->palette + idx * 3 : black;
		for (int ty = ty0; ty < ty1; ty++)
			memcpy(canvas_px(g->canvas, &g->fit, tx, ty), c, 3);
	}
}

static void emit(gif_t *g, frame_t *f, const uint8_t *px, int n)
{
	while (n > 0 && f->rows_done < f->h) {
		uint32_t k = f->w - f->row_fill;
		if ((uint32_t)n < k)
			k = (uint32_t)n;
		memcpy(f->row + f->row_fill, px, k);
		f->row_fill += k;
		px += k;
		n -= (int)k;
		if (f->row_fill == f->w) {
			draw_row(g, f);
			f->rows_done++;
			f->row_fill = 0;
		}
	}
}

static img_err_t decode_lzw(gif_t *g, frame_t *f)
{
	uint8_t min_size;
	if (!br_byte(g->br, &min_size))
		return IMG_ERR_CORRUPT;
	if (min_size < 1 || min_size > 11)
		return IMG_ERR_CORRUPT;

	int clear = 1 << min_size, eoi = clear + 1;
	int next = eoi + 1, size = min_size + 1, old = -1;
	uint8_t first = 0;
	for (int i = 0; i < clear; i++) {
		g->prefix[i] = 0;
		g->suffix[i] = (uint8_t)i;
	}
	g->bits = 0;
	g->nbits = 0;
	g->block_left = 0;
	g->data_end = false;

	for (;;) {
		int code = read_code(g, size);
		if (code < 0 || code == eoi)
			break;
		if (code == clear) {
			next = eoi + 1;
			size = min_size + 1;
			old = -1;
			continue;
		}
		if (old < 0) {
			if (code >= clear)
				return IMG_ERR_CORRUPT;
			first = (uint8_t)code;
			emit(g, f, &first, 1);
			old = code;
			continue;
		}
		int in = code, sp = 0;
		if (code >= next) {
			if (code > next)
				return IMG_ERR_CORRUPT;
			g->stack[sp++] = first;
			code = old;
		}
		while (code >= clear) {
			g->stack[sp++] = g->suffix[code];
			code = g->prefix[code];
		}
		first = g->suffix[code];
		g->stack[sp++] = first;
		// The stack holds the string backwards.
		for (int i = 0; i < sp / 2; i++) {
			uint8_t t = g->stack[i];
			g->stack[i] = g->stack[sp - 1 - i];
			g->stack[sp - 1 - i] = t;
		}
		emit(g, f, g->stack, sp);
		if (next < MAX_CODES) {
			g->prefix[next] = (uint16_t)old;
			g->suffix[next] = first;
			next++;
			if (next == 1 << size && size < 12)
				size++;
		}
		old = in;
	}
	// Whatever follows the end code up to the block terminator.
	if (!g->data_end) {
		if (g->block_left > 0 && !br_skip(g->br, g->block_left))
			return IMG_ERR_CORRUPT;
		if (!skip_sub_blocks(g->br))
			return IMG_ERR_CORRUPT;
	}
	return IMG_OK;
}

static img_err_t start_canvas(gif_t *g, img_info_t *info, uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0)
		return IMG_ERR_CORRUPT;
	if (w > IMG_MAX_SOURCE_SIDE || h > IMG_MAX_SOURCE_SIDE)
		return IMG_ERR_TOO_BIG;
	const img_target_t *t = img_frames_target(g->frames);
	img_fit_init(&g->fit, t, w, h, w, h);
	g->screen_w = w;
	g->screen_h = h;
	info->src_width = w;
	info->src_height = h;
	size_t bytes = (size_t)t->width * t->height * 3;
	if (!(g->canvas = malloc(bytes)) || !(g->saved = malloc(bytes)))
		return IMG_ERR_NOMEM;
	canvas_fill_black(g->canvas, t->width, t->height);
	g->fit_ready = true;
	return IMG_OK;
}

static img_err_t read_image(gif_t *g, img_info_t *info)
{
	uint8_t d[9];
	if (!br_read(g->br, d, sizeof d))
		return IMG_ERR_CORRUPT;
	frame_t f = {
		.x = d[0] | (uint32_t)d[1] << 8,
		.y = d[2] | (uint32_t)d[3] << 8,
		.w = d[4] | (uint32_t)d[5] << 8,
		.h = d[6] | (uint32_t)d[7] << 8,
		.interlaced = d[8] & 0x40,
		.palette = g->gct,
		.palette_len = g->gct_len,
	};
	if (d[8] & 0x80) {
		f.palette_len = 2 << (d[8] & 7);
		if (!br_read(g->br, g->lct, (size_t)f.palette_len * 3))
			return IMG_ERR_CORRUPT;
		f.palette = g->lct;
	}
	if (f.w > IMG_MAX_SOURCE_SIDE || f.h > IMG_MAX_SOURCE_SIDE)
		return IMG_ERR_TOO_BIG;
	img_err_t err;
	if (!g->fit_ready && (err = start_canvas(g, info, f.x + f.w, f.y + f.h)) != IMG_OK)
		return err;

	const img_target_t *t = img_frames_target(g->frames);
	size_t bytes = (size_t)t->width * t->height * 3;
	if (g->disposal == DISPOSE_PREVIOUS)
		memcpy(g->saved, g->canvas, bytes);

	if (f.w > 0 && !(f.row = malloc(f.w)))
		return IMG_ERR_NOMEM;
	err = decode_lzw(g, &f);
	free(f.row);
	if (err != IMG_OK)
		return err;

	uint16_t delay = g->delay_cs ? g->delay_cs : FALLBACK_DELAY_CS;
	if ((err = img_frames_add_canvas(g->frames, g->canvas, delay)) != IMG_OK)
		return err;

	if (g->disposal == DISPOSE_BACKGROUND) {
		int tx0, tx1, ty0, ty1;
		img_fit_cols(&g->fit, f.x, f.x + f.w, &tx0, &tx1);
		img_fit_rows(&g->fit, f.y, f.y + f.h, &ty0, &ty1);
		for (int ty = ty0; ty < ty1; ty++)
			for (int tx = tx0; tx < tx1; tx++)
				memset(canvas_px(g->canvas, &g->fit, tx, ty), 0, 3);
	} else if (g->disposal == DISPOSE_PREVIOUS) {
		memcpy(g->canvas, g->saved, bytes);
	}
	// A graphic control extension applies to the next image only.
	g->delay_cs = 0;
	g->disposal = 0;
	g->transparent = -1;
	return IMG_OK;
}

img_err_t img_decode_gif(img_br_t *br, img_frames_t *frames, img_info_t *info)
{
	gif_t *g = calloc(1, sizeof *g);
	if (!g)
		return IMG_ERR_NOMEM;
	g->br = br;
	g->frames = frames;
	g->transparent = -1;

	img_err_t err = IMG_OK;
	uint8_t h[13];
	if (!br_read(br, h, sizeof h)) {
		err = IMG_ERR_CORRUPT;
		goto out;
	}
	uint32_t w = h[6] | (uint32_t)h[7] << 8, ht = h[8] | (uint32_t)h[9] << 8;
	if (h[10] & 0x80) {
		g->gct_len = 2 << (h[10] & 7);
		if (!br_read(br, g->gct, (size_t)g->gct_len * 3)) {
			err = IMG_ERR_CORRUPT;
			goto out;
		}
	}
	// A zero logical screen takes the first frame's size, as Go does.
	if (w && ht && (err = start_canvas(g, info, w, ht)) != IMG_OK)
		goto out;

	for (;;) {
		uint8_t b;
		if (!br_byte(br, &b))
			break; // tolerate a missing trailer once frames were read
		if (b == 0x3b)
			break;
		if (b == 0x2c) {
			if ((err = read_image(g, info)) != IMG_OK)
				goto out;
		} else if (b == 0x21) {
			uint8_t label;
			if (!br_byte(br, &label)) {
				err = IMG_ERR_CORRUPT;
				goto out;
			}
			if (label == 0xf9) {
				uint8_t e[6];
				if (!br_read(br, e, sizeof e) || e[0] != 4) {
					err = IMG_ERR_CORRUPT;
					goto out;
				}
				g->disposal = (e[1] >> 2) & 7;
				g->delay_cs = (uint16_t)(e[2] | e[3] << 8);
				g->transparent = (e[1] & 1) ? e[4] : -1;
				// e[5] is the block terminator.
			} else if (!skip_sub_blocks(br)) {
				err = IMG_ERR_CORRUPT;
				goto out;
			}
		} else {
			err = IMG_ERR_CORRUPT;
			goto out;
		}
	}
	if (img_frames_count(frames) == 0)
		err = IMG_ERR_CORRUPT;
out:
	free(g->canvas);
	free(g->saved);
	free(g);
	return err;
}
