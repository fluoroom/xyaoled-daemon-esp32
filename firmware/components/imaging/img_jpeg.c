// JPEG through TJpgDec (baseline only). The decoder pre-scales by 1/2,
// 1/4 or 1/8 while the result stays at least as large as the fitted
// image, so a large photo costs a fraction of a full decode; sampling then
// runs on the pre-scaled grid.
#include <stdlib.h>
#include <string.h>

#include "img_internal.h"
#include "tjpgd.h"

#define POOL_SIZE 4096

typedef struct {
	img_br_t *br;
	img_fit_t fit;
	uint8_t *canvas;
} jpeg_t;

static size_t in_func(JDEC *jd, uint8_t *buf, size_t n)
{
	jpeg_t *s = jd->device;
	if (!buf)
		return br_skip(s->br, n) ? n : 0;
	size_t done = 0;
	while (done < n && br_fill(s->br)) {
		size_t k = s->br->len - s->br->pos;
		if (k > n - done)
			k = n - done;
		memcpy(buf + done, s->br->buf + s->br->pos, k);
		s->br->pos += k;
		done += k;
	}
	return done;
}

static int out_func(JDEC *jd, void *bitmap, JRECT *rect)
{
	jpeg_t *s = jd->device;
	int tx0, tx1, ty0, ty1;
	img_fit_cols(&s->fit, rect->left, rect->right + 1u, &tx0, &tx1);
	img_fit_rows(&s->fit, rect->top, rect->bottom + 1u, &ty0, &ty1);
	unsigned bw = rect->right - rect->left + 1u;
	const uint8_t *px = bitmap;
	for (int ty = ty0; ty < ty1; ty++) {
		uint32_t sy = img_fit_sy(&s->fit, ty) - rect->top;
		for (int tx = tx0; tx < tx1; tx++) {
			uint32_t sx = img_fit_sx(&s->fit, tx) - rect->left;
			memcpy(canvas_px(s->canvas, &s->fit, tx, ty), px + (sy * bw + sx) * 3, 3);
		}
	}
	return 1;
}

img_err_t img_decode_jpeg(img_br_t *br, img_frames_t *frames, img_info_t *info)
{
	const img_target_t *t = img_frames_target(frames);
	jpeg_t s = {.br = br};
	JDEC *jd = malloc(sizeof *jd);
	void *pool = malloc(POOL_SIZE);
	s.canvas = calloc((size_t)t->width * t->height, 3);
	img_err_t err = IMG_OK;
	if (!jd || !pool || !s.canvas) {
		err = IMG_ERR_NOMEM;
		goto out;
	}

	JRESULT r = jd_prepare(jd, in_func, pool, POOL_SIZE, &s);
	if (r == JDR_FMT3) {
		err = IMG_ERR_UNSUPPORTED; // progressive or other non-baseline JPEG
		goto out;
	}
	if (r == JDR_MEM1 || r == JDR_MEM2) {
		err = IMG_ERR_NOMEM;
		goto out;
	}
	if (r != JDR_OK) {
		err = IMG_ERR_CORRUPT;
		goto out;
	}
	info->src_width = jd->width;
	info->src_height = jd->height;
	if (jd->width > IMG_MAX_SOURCE_SIDE || jd->height > IMG_MAX_SOURCE_SIDE) {
		err = IMG_ERR_TOO_BIG;
		goto out;
	}

	img_fit_init(&s.fit, t, jd->width, jd->height, jd->width, jd->height);
	uint8_t scale = 0;
	while (scale < 3 && (int)(jd->width >> (scale + 1)) >= s.fit.new_w &&
	       (int)(jd->height >> (scale + 1)) >= s.fit.new_h)
		scale++;
	img_fit_init(&s.fit, t, jd->width, jd->height, jd->width >> scale, jd->height >> scale);

	r = jd_decomp(jd, out_func, scale);
	if (r != JDR_OK) {
		err = IMG_ERR_CORRUPT;
		goto out;
	}
	err = img_frames_add_canvas(frames, s.canvas, 0);
out:
	free(s.canvas);
	free(pool);
	free(jd);
	return err;
}
