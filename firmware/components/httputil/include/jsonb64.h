// Streaming parser for the /display body, {"image": "<base64>", "type":
// "..."}: the image is base64-decoded as bytes arrive, so a large image
// never sits in RAM as JSON text. Other keys are skipped whatever their
// value; "type" is kept if it is a short string.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	JB_OK = 0,
	JB_ERR_JSON = -1,   // not a JSON object
	JB_ERR_BASE64 = -2, // invalid base64 in "image"
} jb_err_t;

typedef struct {
	int state, depth, skip_state;
	bool in_string, escape, in_image, in_type, key_image, key_type, after_value;
	char key[8];
	size_t key_len;
	char type[16];
	size_t type_len;
	// Base64 quartet being decoded.
	uint32_t quad;
	int quad_len, pad;
	bool image_seen, image_done, done;
	jb_err_t err;
} jb_t;

void jb_init(jb_t *jb);

// A quartet completed by the last byte fed decodes to 3 bytes, so out needs
// this much room beyond in_len.
#define JB_OUT_SLACK 3

// jb_feed parses in[0..in_len), writing decoded image bytes to out (which
// must hold in_len + JB_OUT_SLACK bytes). Returns the number of image bytes written, or
// < 0 on error. *consumed is how much of in was used: feeding stops at the
// end of the image string so the caller can see the image end.
int jb_feed(jb_t *jb, const uint8_t *in, size_t in_len, uint8_t *out, size_t *consumed);

// After the whole body: "image" present, complete and valid.
bool jb_image_complete(const jb_t *jb);
const char *jb_type(const jb_t *jb);
