// PNG through pngle, sampled onto the canvas pixel by pixel. Alpha is
// composited over black, which is what the Go code's draw.Src resize
// followed by draw.Over onto black amounts to.
#include <stdlib.h>
#include <string.h>

#include "img_internal.h"
#include "pngle.h"

typedef struct {
	img_frames_t *frames;
	img_info_t *info;
	img_fit_t fit;
	uint8_t *canvas;
	img_err_t err;
} png_t;

static void on_init(pngle_t *p, uint32_t w, uint32_t h)
{
	png_t *s = pngle_get_user_data(p);
	s->info->src_width = w;
	s->info->src_height = h;
	if (w > IMG_MAX_SOURCE_SIDE || h > IMG_MAX_SOURCE_SIDE) {
		s->err = IMG_ERR_TOO_BIG;
		return;
	}
	const img_target_t *t = img_frames_target(s->frames);
	img_fit_init(&s->fit, t, w, h, w, h);
	if (!(s->canvas = calloc((size_t)t->width * t->height, 3)))
		s->err = IMG_ERR_NOMEM;
}

static void on_draw(pngle_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4])
{
	png_t *s = pngle_get_user_data(p);
	if (!s->canvas)
		return;
	int tx0, tx1, ty0, ty1;
	img_fit_cols(&s->fit, x, x + w, &tx0, &tx1);
	if (tx0 >= tx1)
		return;
	img_fit_rows(&s->fit, y, y + h, &ty0, &ty1);
	unsigned a = rgba[3];
	uint8_t c[3] = {(uint8_t)((rgba[0] * a + 127) / 255), (uint8_t)((rgba[1] * a + 127) / 255),
			(uint8_t)((rgba[2] * a + 127) / 255)};
	for (int ty = ty0; ty < ty1; ty++)
		for (int tx = tx0; tx < tx1; tx++)
			memcpy(canvas_px(s->canvas, &s->fit, tx, ty), c, 3);
}

img_err_t img_decode_png(img_br_t *br, img_frames_t *frames, img_info_t *info)
{
	png_t s = {.frames = frames, .info = info};
	pngle_t *p = pngle_new();
	// pngle may leave the tail of a chunk header unconsumed; it is fed
	// again with the next bytes.
	uint8_t *buf = malloc(1024);
	if (!p || !buf) {
		pngle_destroy(p);
		free(buf);
		return IMG_ERR_NOMEM;
	}
	pngle_set_user_data(p, &s);
	pngle_set_init_callback(p, on_init);
	pngle_set_draw_callback(p, on_draw);

	size_t pending = 0;
	img_err_t err = IMG_OK;
	for (;;) {
		bool more = br_fill(br);
		if (more) {
			size_t k = br->len - br->pos;
			if (k > 1024 - pending)
				k = 1024 - pending;
			memcpy(buf + pending, br->buf + br->pos, k);
			br->pos += k;
			pending += k;
		}
		if (pending == 0)
			break;
		int eaten = pngle_feed(p, buf, pending);
		if (eaten < 0) {
			err = IMG_ERR_CORRUPT;
			break;
		}
		if (s.err != IMG_OK) {
			err = s.err;
			break;
		}
		memmove(buf, buf + eaten, pending - (size_t)eaten);
		pending -= (size_t)eaten;
		if (!more)
			break;
	}
	if (err == IMG_OK && !s.canvas)
		err = IMG_ERR_CORRUPT;
	if (err == IMG_OK)
		err = img_frames_add_canvas(frames, s.canvas, 0);
	free(s.canvas);
	free(buf);
	pngle_destroy(p);
	return err;
}
