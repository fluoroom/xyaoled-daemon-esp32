// Image pipeline: decode PNG, JPEG or GIF from a stream, fit it to the
// panel while decoding, and encode the GIF the panel plays. A port of the
// image code in reference/go-daemon/display.go, reshaped for a few hundred
// KB of RAM:
//
//   - The Go resizer is nearest neighbour, so every output pixel depends
//     on exactly one source pixel. Frames are composited directly on a
//     panel-sized canvas, touching only the source pixels that land on it;
//     the result equals compositing at full size and scaling afterwards.
//   - Frames go to a frame store (RAM or a file) while decoding, because
//     the shared palette needs all of them before encoding.
//
// No ESP-IDF dependencies: the host tests build this as is.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	IMG_OK = 0,
	IMG_ERR_FORMAT = -1,  // not PNG, JPEG or GIF
	IMG_ERR_CORRUPT = -2, // truncated or invalid data
	IMG_ERR_TOO_BIG = -3, // source dimensions over IMG_MAX_SOURCE_SIDE
	IMG_ERR_NOMEM = -4,
	IMG_ERR_IO = -5,      // the reader or writer failed
	IMG_ERR_FULL = -6,    // more frames than the frame store holds
	IMG_ERR_UNSUPPORTED = -7, // e.g. progressive JPEG
} img_err_t;

const char *img_strerror(img_err_t err);

#define IMG_MAX_SOURCE_SIDE 8192

// Pull reader: returns the number of bytes read (> 0), 0 at the end of the
// stream, or < 0 on error.
typedef struct {
	int (*read)(void *ctx, uint8_t *buf, size_t len);
	void *ctx;
} img_reader_t;

// Writer: returns 0, or < 0 on error.
typedef struct {
	int (*write)(void *ctx, const uint8_t *buf, size_t len);
	void *ctx;
} img_writer_t;

typedef struct {
	uint16_t width, height; // panel size
	bool upside_down;       // rotate 180°
} img_target_t;

typedef enum { IMG_FMT_UNKNOWN, IMG_FMT_PNG, IMG_FMT_JPEG, IMG_FMT_GIF } img_format_t;

// img_detect returns the format of a file from its first bytes (at least 4).
img_format_t img_detect(const uint8_t *head, size_t len);

// Frame store: panel-sized RGB888 frames with delays in centiseconds, and
// the palette shared by all of them (their first 256 distinct colours).
typedef struct img_frames img_frames_t;

img_frames_t *img_frames_new_mem(const img_target_t *target, size_t max_frames);
// Frames are kept in the file at path (created, truncated); delays in RAM.
img_frames_t *img_frames_new_file(const img_target_t *target, const char *path, size_t max_frames);
void img_frames_free(img_frames_t *f);
img_err_t img_frames_add(img_frames_t *f, const uint8_t *rgb, uint16_t delay_cs);
img_err_t img_frames_get(img_frames_t *f, size_t i, uint8_t *rgb, uint16_t *delay_cs);
size_t img_frames_count(const img_frames_t *f);
const img_target_t *img_frames_target(const img_frames_t *f);

typedef struct {
	img_format_t format;
	uint32_t src_width, src_height;
	size_t frames;
} img_info_t;

// img_decode decodes one image from r into frames: one frame for PNG and
// JPEG, every frame for GIF (composited, disposal honoured, zero delays
// turned into 50 ms). Single images get delay 0; the encoder sets theirs.
img_err_t img_decode(const img_reader_t *r, img_frames_t *frames, img_info_t *info);

// Same, from a memory buffer.
img_err_t img_decode_mem(const uint8_t *data, size_t len, img_frames_t *frames, img_info_t *info);

typedef struct {
	uint32_t frames;
	uint32_t total_ms; // sum of frame delays
	uint32_t bytes;    // GIF size
	bool static_image; // one frame: send with the static trailer
} img_gif_info_t;

// img_encode_gif writes the GIF the panel plays. One frame becomes a still
// held for 600 s with its own colour table, as the Go daemon sends stills;
// several become a looping animation with one global colour table (the
// panel's decoder loses sync on per-frame tables) and background disposal.
img_err_t img_encode_gif(img_frames_t *frames, const img_writer_t *w, img_gif_info_t *out);
// The same with at most levels³ colours (levels 2..IMG_MAX_LEVELS; 0 is
// img_encode_gif): frames with more colours than that are dithered onto a
// fixed uniform cube, the same for every frame so dithering doesn't
// flicker; frames with fewer keep their exact colours. Fewer colours make
// a smaller GIF (narrower LZW codes, shorter colour table).
#define IMG_MAX_LEVELS 5
img_err_t img_encode_gif_levels(img_frames_t *frames, const img_writer_t *w, unsigned levels, img_gif_info_t *out);

// img_black_frame adds one opaque black frame (for clear-screen).
img_err_t img_black_frame(img_frames_t *frames);

// GIF passthrough check: scans a GIF's blocks without decoding pixel data.
typedef struct {
	uint16_t width, height; // logical screen
	uint32_t frames;
	uint32_t total_ms;
	bool local_color_table; // some frame has one
	bool zero_delay;        // some frame has delay 0
	// Some frame does not cover the whole screen, or has transparency.
	// The Go daemon always sent full, opaque frames; whether the panel's
	// decoder composites partial ones is unknown, so they are not passed
	// through.
	bool partial_frame;
	bool transparency;
} img_gif_scan_t;

img_err_t img_gif_scan(const img_reader_t *r, img_gif_scan_t *scan);

// img_gif_passthrough reports whether a scanned GIF can be sent as it is,
// and if not, why (a static string). allow_partial lets partial and
// transparent frames through, to try the panel's decoder on them.
bool img_gif_passthrough(const img_gif_scan_t *scan, const img_target_t *target, bool allow_partial,
			 const char **why);
