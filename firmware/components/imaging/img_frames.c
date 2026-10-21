#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "img_internal.h"

#define HASH_SLOTS 512 // power of two, twice the palette size

struct img_frames {
	img_target_t target;
	size_t frame_bytes, max, count, cap;
	uint16_t *delays;
	uint8_t *mem; // RAM store: cap frames
	FILE *file;   // file store
	uint8_t *rotated;

	uint8_t palette[256 * 3];
	size_t palette_len;
	uint16_t hash[HASH_SLOTS]; // palette index + 1, 0 when empty
	uint32_t uses[256];        // pixels of each palette colour, in all frames
	bool overflow;             // some colour did not fit in the palette
};

static img_frames_t *frames_new(const img_target_t *target, size_t max_frames)
{
	img_frames_t *f = calloc(1, sizeof *f);
	if (!f)
		return NULL;
	f->target = *target;
	f->frame_bytes = (size_t)target->width * target->height * 3;
	f->max = max_frames;
	return f;
}

img_frames_t *img_frames_new_mem(const img_target_t *target, size_t max_frames)
{
	return frames_new(target, max_frames);
}

img_frames_t *img_frames_new_file(const img_target_t *target, const char *path, size_t max_frames)
{
	img_frames_t *f = frames_new(target, max_frames);
	if (!f)
		return NULL;
	if (!(f->file = fopen(path, "w+b"))) {
		free(f);
		return NULL;
	}
	return f;
}

void img_frames_free(img_frames_t *f)
{
	if (!f)
		return;
	if (f->file)
		fclose(f->file);
	free(f->mem);
	free(f->delays);
	free(f->rotated);
	free(f);
}

size_t img_frames_count(const img_frames_t *f)
{
	return f->count;
}

const img_target_t *img_frames_target(const img_frames_t *f)
{
	return &f->target;
}

static uint32_t hash_of(uint32_t key)
{
	return (key * 2654435761u) >> (32 - 9);
}

int img_frames_lookup(const img_frames_t *f, uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t key = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
	for (uint32_t h = hash_of(key);; h = (h + 1) & (HASH_SLOTS - 1)) {
		uint16_t slot = f->hash[h];
		if (slot == 0)
			return -1;
		const uint8_t *p = f->palette + (slot - 1) * 3;
		if (p[0] == r && p[1] == g && p[2] == b)
			return slot - 1;
	}
}

// palette_add counts a pixel of colour r, g, b, adding the colour while
// the palette has room.
static void palette_add(img_frames_t *f, uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t key = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
	uint32_t h = hash_of(key);
	for (;; h = (h + 1) & (HASH_SLOTS - 1)) {
		uint16_t slot = f->hash[h];
		if (slot == 0)
			break;
		const uint8_t *p = f->palette + (slot - 1) * 3;
		if (p[0] == r && p[1] == g && p[2] == b) {
			f->uses[slot - 1]++;
			return;
		}
	}
	if (f->palette_len == 256) {
		f->overflow = true;
		return;
	}
	uint8_t *p = f->palette + f->palette_len * 3;
	p[0] = r, p[1] = g, p[2] = b;
	f->uses[f->palette_len] = 1;
	f->hash[h] = (uint16_t)++f->palette_len;
}

const uint32_t *img_frames_uses(const img_frames_t *f, bool *overflow)
{
	*overflow = f->overflow;
	return f->uses;
}

size_t img_frames_palette(const img_frames_t *f, const uint8_t **rgb)
{
	*rgb = f->palette;
	return f->palette_len;
}

img_err_t img_frames_add(img_frames_t *f, const uint8_t *rgb, uint16_t delay_cs)
{
	if (f->count >= f->max)
		return IMG_ERR_FULL;
	if (f->count == f->cap) {
		size_t cap = f->cap ? f->cap * 2 : 4;
		if (cap > f->max)
			cap = f->max;
		uint16_t *d = realloc(f->delays, cap * sizeof *d);
		if (!d)
			return IMG_ERR_NOMEM;
		f->delays = d;
		if (!f->file) {
			uint8_t *m = realloc(f->mem, cap * f->frame_bytes);
			if (!m)
				return IMG_ERR_NOMEM;
			f->mem = m;
		}
		f->cap = cap;
	}
	if (f->file) {
		if (fseek(f->file, (long)(f->count * f->frame_bytes), SEEK_SET) != 0 ||
		    fwrite(rgb, 1, f->frame_bytes, f->file) != f->frame_bytes)
			return IMG_ERR_IO;
	} else {
		memcpy(f->mem + f->count * f->frame_bytes, rgb, f->frame_bytes);
	}
	f->delays[f->count++] = delay_cs;

	// Shared palette: the first 256 distinct colours, in the order met,
	// with how often each is used.
	for (size_t i = 0; i < f->frame_bytes; i += 3)
		palette_add(f, rgb[i], rgb[i + 1], rgb[i + 2]);
	return IMG_OK;
}

img_err_t img_frames_add_canvas(img_frames_t *f, const uint8_t *canvas, uint16_t delay_cs)
{
	if (!f->target.upside_down)
		return img_frames_add(f, canvas, delay_cs);
	if (!f->rotated && !(f->rotated = malloc(f->frame_bytes)))
		return IMG_ERR_NOMEM;
	canvas_rotate180(f->rotated, canvas, f->target.width, f->target.height);
	return img_frames_add(f, f->rotated, delay_cs);
}

img_err_t img_frames_get(img_frames_t *f, size_t i, uint8_t *rgb, uint16_t *delay_cs)
{
	if (i >= f->count)
		return IMG_ERR_CORRUPT;
	if (f->file) {
		if (fseek(f->file, (long)(i * f->frame_bytes), SEEK_SET) != 0 ||
		    fread(rgb, 1, f->frame_bytes, f->file) != f->frame_bytes)
			return IMG_ERR_IO;
	} else {
		memcpy(rgb, f->mem + i * f->frame_bytes, f->frame_bytes);
	}
	if (delay_cs)
		*delay_cs = f->delays[i];
	return IMG_OK;
}

img_err_t img_black_frame(img_frames_t *f)
{
	uint8_t *black = calloc(1, f->frame_bytes);
	if (!black)
		return IMG_ERR_NOMEM;
	img_err_t err = img_frames_add(f, black, 0);
	free(black);
	return err;
}
