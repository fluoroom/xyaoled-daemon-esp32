// GIF passthrough: walk the blocks of a GIF, skipping the LZW data, to
// decide whether it can go to the panel unchanged and to compute its
// animation trailer.
#include <stdlib.h>

#include "img_internal.h"

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

static img_err_t scan(img_br_t *br, img_gif_scan_t *s)
{
	uint8_t h[13];
	if (!br_read(br, h, sizeof h) || img_detect(h, sizeof h) != IMG_FMT_GIF)
		return IMG_ERR_FORMAT;
	s->width = (uint16_t)(h[6] | h[7] << 8);
	s->height = (uint16_t)(h[8] | h[9] << 8);
	if ((h[10] & 0x80) && !br_skip(br, (size_t)(2 << (h[10] & 7)) * 3))
		return IMG_ERR_CORRUPT;

	uint32_t delay = 0;
	bool have_gce = false;
	for (;;) {
		uint8_t b;
		if (!br_byte(br, &b))
			return s->frames ? IMG_OK : IMG_ERR_CORRUPT;
		if (b == 0x3b)
			return s->frames ? IMG_OK : IMG_ERR_CORRUPT;
		if (b == 0x21) {
			uint8_t label;
			if (!br_byte(br, &label))
				return IMG_ERR_CORRUPT;
			if (label == 0xf9) {
				uint8_t e[6];
				if (!br_read(br, e, sizeof e) || e[0] != 4)
					return IMG_ERR_CORRUPT;
				delay = e[2] | (uint32_t)e[3] << 8;
				if (e[1] & 1)
					s->transparency = true;
				have_gce = true;
			} else if (!skip_sub_blocks(br)) {
				return IMG_ERR_CORRUPT;
			}
		} else if (b == 0x2c) {
			uint8_t d[9];
			if (!br_read(br, d, sizeof d))
				return IMG_ERR_CORRUPT;
			uint32_t x = d[0] | (uint32_t)d[1] << 8, y = d[2] | (uint32_t)d[3] << 8;
			uint32_t w = d[4] | (uint32_t)d[5] << 8, fh = d[6] | (uint32_t)d[7] << 8;
			if (x || y || w != s->width || fh != s->height)
				s->partial_frame = true;
			if (d[8] & 0x80) {
				s->local_color_table = true;
				if (!br_skip(br, (size_t)(2 << (d[8] & 7)) * 3))
					return IMG_ERR_CORRUPT;
			}
			if (!have_gce || delay == 0)
				s->zero_delay = true;
			s->frames++;
			s->total_ms += delay * 10;
			// LZW minimum code size, then the data.
			if (!br_skip(br, 1) || !skip_sub_blocks(br))
				return IMG_ERR_CORRUPT;
			delay = 0;
			have_gce = false;
		} else {
			return IMG_ERR_CORRUPT;
		}
	}
}

img_err_t img_gif_scan(const img_reader_t *r, img_gif_scan_t *s)
{
	img_br_t *br = malloc(sizeof *br);
	if (!br)
		return IMG_ERR_NOMEM;
	br_init(br, r);
	*s = (img_gif_scan_t){0};
	img_err_t err = scan(br, s);
	if (err != IMG_OK && br->error)
		err = IMG_ERR_IO;
	free(br);
	return err;
}

bool img_gif_passthrough(const img_gif_scan_t *s, const img_target_t *t, bool allow_partial, const char **why)
{
	const char *reason = NULL;
	if (s->width != t->width || s->height != t->height)
		reason = "size differs from the panel";
	else if (t->upside_down)
		reason = "upside-down is on";
	else if (s->frames < 2)
		reason = "single frame (sent as a still)";
	else if (s->local_color_table)
		reason = "a frame has a local colour table";
	else if (s->zero_delay)
		reason = "a frame has no delay";
	else if (s->partial_frame && !allow_partial)
		reason = "a frame does not cover the whole screen";
	else if (s->transparency && !allow_partial)
		reason = "a frame has transparency";
	if (why)
		*why = reason;
	return reason == NULL;
}
