// Streaming multipart/form-data parser: parts are read one at a time
// straight from the request, never buffered whole.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Source of request body bytes: returns > 0 bytes read, 0 at the end, < 0
// on error.
typedef int (*mp_src_fn)(void *ctx, uint8_t *buf, size_t len);

#define MP_BOUNDARY_MAX 72 // RFC 2046 allows 70
#define MP_NAME_MAX 64
#define MP_BUF 1024

typedef struct {
	char name[MP_NAME_MAX];
	char filename[MP_NAME_MAX];
	char content_type[MP_NAME_MAX];
	bool is_file; // the part has a filename
} mp_part_t;

typedef struct {
	mp_src_fn src;
	void *ctx;
	char delim[MP_BOUNDARY_MAX + 4]; // "\r\n--" boundary
	size_t delim_len;
	uint8_t buf[MP_BUF];
	size_t pos, len;
	bool src_end, error;
	enum { MP_PREAMBLE, MP_HEADERS, MP_BODY, MP_PART_END, MP_DONE } state;
} mp_t;

// mp_boundary extracts the boundary from a Content-Type header value;
// false when it is not multipart/form-data or has none.
bool mp_boundary(const char *content_type, char *out, size_t out_len);

void mp_init(mp_t *mp, const char *boundary, mp_src_fn src, void *ctx);

// mp_next_part skips what is left of the current part and reads the next
// part's headers. Returns 1 with *part filled, 0 after the last part, < 0
// on malformed input or a source error.
int mp_next_part(mp_t *mp, mp_part_t *part);

// mp_read reads the current part's body: > 0 bytes, 0 at its end, < 0 on
// error.
int mp_read(mp_t *mp, uint8_t *out, size_t len);
