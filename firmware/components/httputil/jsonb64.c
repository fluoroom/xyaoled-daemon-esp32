#include "jsonb64.h"

#include <string.h>

enum { S_START, S_KEY_OR_END, S_KEY, S_COLON, S_VALUE, S_SCALAR, S_NESTED, S_AFTER_VALUE, S_DONE };

void jb_init(jb_t *jb)
{
	memset(jb, 0, sizeof *jb);
	jb->state = S_START;
}

static bool is_ws(uint8_t c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int b64_value(uint8_t c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static int fail(jb_t *jb, jb_err_t err)
{
	jb->err = err;
	jb->state = S_DONE;
	return err;
}

// b64 decodes one character of the image string into out.
static int b64(jb_t *jb, uint8_t c, uint8_t *out, size_t *w)
{
	if (is_ws(c))
		return 0;
	if (c == '=') {
		if (jb->quad_len < 2 || ++jb->pad > 2)
			return fail(jb, JB_ERR_BASE64);
		jb->quad <<= 6;
		if (++jb->quad_len == 4) {
			out[(*w)++] = (uint8_t)(jb->quad >> 16);
			if (jb->pad == 1)
				out[(*w)++] = (uint8_t)(jb->quad >> 8);
			jb->quad_len = 0;
		}
		return 0;
	}
	int v = b64_value(c);
	if (v < 0 || jb->pad)
		return fail(jb, JB_ERR_BASE64);
	jb->quad = jb->quad << 6 | (uint32_t)v;
	if (++jb->quad_len == 4) {
		out[(*w)++] = (uint8_t)(jb->quad >> 16);
		out[(*w)++] = (uint8_t)(jb->quad >> 8);
		out[(*w)++] = (uint8_t)jb->quad;
		jb->quad_len = 0;
		jb->quad = 0;
	}
	return 0;
}

static int end_image(jb_t *jb)
{
	// Unpadded input is accepted if it ends on a whole byte.
	if (jb->quad_len == 1)
		return fail(jb, JB_ERR_BASE64);
	jb->image_done = jb->quad_len == 0;
	jb->in_image = false;
	return 0;
}

int jb_feed(jb_t *jb, const uint8_t *in, size_t in_len, uint8_t *out, size_t *consumed)
{
	size_t w = 0, i = 0;
	for (; i < in_len && jb->state != S_DONE; i++) {
		uint8_t c = in[i];
		if (jb->in_string) {
			if (jb->escape) {
				jb->escape = false;
				if (jb->in_image) {
					if (c == '/' && b64(jb, '/', out, &w) < 0)
						return jb->err;
					else if (c != '/' && c != 'n' && c != 'r' && c != 't')
						return fail(jb, JB_ERR_BASE64);
				} else if (jb->state == S_KEY && jb->key_len < sizeof jb->key - 1) {
					jb->key[jb->key_len++] = (char)c;
				} else if (jb->in_type && jb->type_len < sizeof jb->type - 1) {
					jb->type[jb->type_len++] = (char)c;
				}
				continue;
			}
			if (c == '\\') {
				jb->escape = true;
				continue;
			}
			if (c == '"') {
				jb->in_string = false;
				if (jb->state == S_KEY) {
					// A key too long to be ours ends up empty.
					jb->key[jb->key_len < sizeof jb->key ? jb->key_len : 0] = 0;
					jb->state = S_COLON;
				} else if (jb->state == S_NESTED) {
					// string inside a skipped value; keep skipping
				} else {
					bool was_image = jb->in_image;
					if (jb->in_image && end_image(jb) < 0)
						return jb->err;
					if (jb->in_type)
						jb->type[jb->type_len] = 0;
					jb->in_type = false;
					jb->state = S_AFTER_VALUE;
					if (was_image) {
						i++;
						break; // let the caller see the end of the image
					}
				}
				continue;
			}
			if (jb->in_image) {
				if (b64(jb, c, out, &w) < 0)
					return jb->err;
			} else if (jb->state == S_KEY) {
				if (jb->key_len < sizeof jb->key - 1)
					jb->key[jb->key_len++] = (char)c;
				else
					jb->key[0] = 0, jb->key_len = sizeof jb->key; // too long to be ours
			} else if (jb->in_type && jb->type_len < sizeof jb->type - 1) {
				jb->type[jb->type_len++] = (char)c;
			}
			continue;
		}
		switch (jb->state) {
		case S_START:
			if (is_ws(c))
				break;
			if (c != '{')
				return fail(jb, JB_ERR_JSON);
			jb->state = S_KEY_OR_END;
			break;
		case S_KEY_OR_END:
		case S_KEY:
			if (is_ws(c))
				break;
			if (c == '}' && jb->state == S_KEY_OR_END) {
				jb->state = S_DONE;
				jb->done = true;
				break;
			}
			if (c != '"')
				return fail(jb, JB_ERR_JSON);
			jb->state = S_KEY;
			jb->in_string = true;
			jb->key_len = 0;
			break;
		case S_COLON:
			if (is_ws(c))
				break;
			if (c != ':')
				return fail(jb, JB_ERR_JSON);
			jb->state = S_VALUE;
			break;
		case S_VALUE:
			if (is_ws(c))
				break;
			if (c == '"') {
				jb->in_string = true;
				if (jb->key_len < sizeof jb->key && strcmp(jb->key, "image") == 0) {
					jb->in_image = true;
					jb->image_seen = true;
					jb->image_done = false;
					jb->quad_len = jb->pad = 0;
				} else if (jb->key_len < sizeof jb->key && strcmp(jb->key, "type") == 0) {
					jb->in_type = true;
					jb->type_len = 0;
				}
				// Other strings are skipped: the string branch drops
				// their characters and S_AFTER_VALUE follows.
			} else if (c == '{' || c == '[') {
				jb->state = S_NESTED;
				jb->depth = 1;
			} else if (c == '}' || c == ',' || c == ']' || c == ':') {
				return fail(jb, JB_ERR_JSON);
			} else {
				jb->state = S_SCALAR;
			}
			break;
		case S_SCALAR:
			if (c == ',') {
				jb->state = S_KEY;
			} else if (c == '}') {
				jb->state = S_DONE;
				jb->done = true;
			}
			break;
		case S_NESTED:
			if (c == '"')
				jb->in_string = true;
			else if (c == '{' || c == '[')
				jb->depth++;
			else if ((c == '}' || c == ']') && --jb->depth == 0)
				jb->state = S_AFTER_VALUE;
			break;
		case S_AFTER_VALUE:
			if (is_ws(c))
				break;
			if (c == ',') {
				jb->state = S_KEY;
			} else if (c == '}') {
				jb->state = S_DONE;
				jb->done = true;
			} else {
				return fail(jb, JB_ERR_JSON);
			}
			break;
		}
	}
	if (consumed)
		*consumed = i;
	return (int)w;
}

bool jb_image_complete(const jb_t *jb)
{
	return jb->done && jb->err == JB_OK && jb->image_seen && jb->image_done;
}

const char *jb_type(const jb_t *jb)
{
	return jb->type;
}
