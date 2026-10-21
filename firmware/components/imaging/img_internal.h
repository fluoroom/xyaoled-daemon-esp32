#pragma once

#include "img.h"

// Buffered reader over an img_reader_t, with a pushback of the bytes used
// for format detection.
typedef struct {
	const img_reader_t *src;
	uint8_t buf[512];
	size_t pos, len;
	bool eof, error;
} img_br_t;

void br_init(img_br_t *br, const img_reader_t *src);
// br_fill makes at least one byte available; false at the end or on error.
bool br_fill(img_br_t *br);
// br_read reads exactly n bytes; false if the stream ends first.
bool br_read(img_br_t *br, uint8_t *out, size_t n);
bool br_byte(img_br_t *br, uint8_t *out);
bool br_skip(img_br_t *br, size_t n);
// br_peek returns up to n buffered bytes without consuming them.
size_t br_peek(img_br_t *br, const uint8_t **out, size_t n);

// Contain fit of a src_w × src_h source on the panel, matching the Go
// resizeImage: scaled by min(W/src_w, H/src_h), centred on black, and
// sampled with golang.org/x/image/draw NearestNeighbor, where target pixel
// d of a D-pixel span samples source pixel (2d + 1) S / 2D.
typedef struct {
	uint16_t width, height; // panel
	uint32_t src_w, src_h;  // coordinates the decoder reports pixels in
	int new_w, new_h;       // scaled size
	int off_x, off_y;       // where it sits on the panel
} img_fit_t;

// img_fit_init computes the fit for a source of orig_w × orig_h whose
// decoder reports pixels in a src_w × src_h grid (smaller when the decoder
// pre-scales, as TJpgDec does).
void img_fit_init(img_fit_t *fit, const img_target_t *t, uint32_t orig_w, uint32_t orig_h, uint32_t src_w,
		  uint32_t src_h);
// Source coordinate sampled by panel pixel tx / ty (inside the scaled area).
uint32_t img_fit_sx(const img_fit_t *fit, int tx);
uint32_t img_fit_sy(const img_fit_t *fit, int ty);
// Panel pixels whose samples fall in source columns [s0, s1): [*t0, *t1),
// already offset onto the panel. Empty when *t0 >= *t1.
void img_fit_cols(const img_fit_t *fit, uint32_t s0, uint32_t s1, int *t0, int *t1);
void img_fit_rows(const img_fit_t *fit, uint32_t s0, uint32_t s1, int *t0, int *t1);

// Canvas: panel-sized RGB888.
static inline uint8_t *canvas_px(uint8_t *canvas, const img_fit_t *fit, int x, int y)
{
	return canvas + ((size_t)y * fit->width + x) * 3;
}

void canvas_fill_black(uint8_t *canvas, size_t w, size_t h);
void canvas_rotate180(uint8_t *dst, const uint8_t *src, size_t w, size_t h);

// Per-format decoders, called by img_decode after detection.
img_err_t img_decode_gif(img_br_t *br, img_frames_t *frames, img_info_t *info);
img_err_t img_decode_png(img_br_t *br, img_frames_t *frames, img_info_t *info);
img_err_t img_decode_jpeg(img_br_t *br, img_frames_t *frames, img_info_t *info);

// img_frames_add_canvas adds a composited canvas, rotating it first when
// the target is upside down.
img_err_t img_frames_add_canvas(img_frames_t *f, const uint8_t *canvas, uint16_t delay_cs);

// Shared palette of the stored frames.
size_t img_frames_palette(const img_frames_t *f, const uint8_t **rgb);
// Pixels using each palette colour, and whether some colour did not fit
// (more than 256 in all).
const uint32_t *img_frames_uses(const img_frames_t *f, bool *overflow);
// Index of an exact palette colour, or -1.
int img_frames_lookup(const img_frames_t *f, uint8_t r, uint8_t g, uint8_t b);
