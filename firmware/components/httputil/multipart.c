#include "multipart.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ci_find is a case-insensitive strstr.
static const char *ci_find(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (strncasecmp(hay, needle, n) == 0)
			return hay;
	return NULL;
}

bool mp_boundary(const char *ct, char *out, size_t out_len)
{
	if (!ct || strncasecmp(ct, "multipart/form-data", 19) != 0)
		return false;
	const char *b = ci_find(ct, "boundary=");
	if (!b)
		return false;
	b += 9;
	bool quoted = *b == '"';
	if (quoted)
		b++;
	size_t n = 0;
	while (b[n] && (quoted ? b[n] != '"' : (b[n] != ';' && b[n] != ' ' && b[n] != '\t')))
		n++;
	if (n == 0 || n > MP_BOUNDARY_MAX || n >= out_len)
		return false;
	memcpy(out, b, n);
	out[n] = 0;
	return true;
}

void mp_init(mp_t *mp, const char *boundary, mp_src_fn src, void *ctx)
{
	memset(mp, 0, sizeof *mp);
	mp->src = src;
	mp->ctx = ctx;
	// The first boundary has no CRLF before it; parse as if it had one.
	mp->delim_len = (size_t)snprintf(mp->delim, sizeof mp->delim, "\r\n--%s", boundary);
	mp->buf[0] = '\r';
	mp->buf[1] = '\n';
	mp->len = 2;
	mp->state = MP_PREAMBLE;
}

// fill tops the buffer up, keeping unread bytes; false when nothing new came.
static bool fill(mp_t *mp)
{
	if (mp->src_end || mp->error)
		return false;
	if (mp->pos > 0) {
		memmove(mp->buf, mp->buf + mp->pos, mp->len - mp->pos);
		mp->len -= mp->pos;
		mp->pos = 0;
	}
	if (mp->len == sizeof mp->buf)
		return false;
	int n = mp->src(mp->ctx, mp->buf + mp->len, sizeof mp->buf - mp->len);
	if (n < 0)
		mp->error = true;
	if (n <= 0) {
		mp->src_end = true;
		return false;
	}
	mp->len += (size_t)n;
	return true;
}

static const uint8_t *find(const uint8_t *hay, size_t hn, const char *needle, size_t nn)
{
	if (nn == 0 || hn < nn)
		return NULL;
	for (size_t i = 0; i + nn <= hn; i++)
		if (hay[i] == (uint8_t)needle[0] && memcmp(hay + i, needle, nn) == 0)
			return hay + i;
	return NULL;
}

// body_chunk returns how many bytes from pos are part body for sure, and
// sets *at_delim when the delimiter starts right there.
static size_t body_chunk(mp_t *mp, bool *at_delim)
{
	for (;;) {
		const uint8_t *start = mp->buf + mp->pos;
		size_t avail = mp->len - mp->pos;
		const uint8_t *d = find(start, avail, mp->delim, mp->delim_len);
		if (d) {
			*at_delim = d == start;
			return (size_t)(d - start);
		}
		// The delimiter may begin in the last delim_len - 1 bytes.
		size_t safe = avail >= mp->delim_len ? avail - (mp->delim_len - 1) : 0;
		if (safe > 0 || !fill(mp)) {
			*at_delim = false;
			if (safe == 0 && mp->src_end)
				mp->error = true; // ended inside a part
			return safe;
		}
	}
}

int mp_read(mp_t *mp, uint8_t *out, size_t len)
{
	if (mp->state != MP_BODY)
		return 0;
	bool at_delim;
	size_t n = body_chunk(mp, &at_delim);
	if (mp->error)
		return -1;
	if (n == 0 && at_delim) {
		mp->pos += mp->delim_len;
		mp->state = MP_PART_END;
		return 0;
	}
	if (n > len)
		n = len;
	memcpy(out, mp->buf + mp->pos, n);
	mp->pos += n;
	return (int)n;
}

// read_line reads a header line (CRLF excluded) into line.
static int read_line(mp_t *mp, char *line, size_t cap)
{
	for (;;) {
		const uint8_t *start = mp->buf + mp->pos;
		size_t avail = mp->len - mp->pos;
		const uint8_t *e = find(start, avail, "\r\n", 2);
		if (e) {
			size_t n = (size_t)(e - start);
			if (n >= cap)
				n = cap - 1; // overlong header lines are truncated
			memcpy(line, start, n);
			line[n] = 0;
			mp->pos += (size_t)(e - start) + 2;
			return 0;
		}
		if (!fill(mp)) {
			if (mp->len - mp->pos == sizeof mp->buf) {
				mp->pos = mp->len; // a header line longer than the buffer: drop it
				continue;
			}
			return -1;
		}
	}
}

// param copies the value of key="..." from a header line.
static void param(const char *line, const char *key, char *out, size_t cap)
{
	out[0] = 0;
	size_t kl = strlen(key);
	for (const char *p = line; (p = ci_find(p, key)) != NULL; p += kl) {
		// Whole word only: "name" must not match inside "filename".
		if (p > line && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
			continue;
		const char *v = p + kl;
		while (*v == ' ')
			v++;
		if (*v != '=')
			continue;
		v++;
		while (*v == ' ')
			v++;
		bool quoted = *v == '"';
		if (quoted)
			v++;
		size_t n = 0;
		while (v[n] && (quoted ? v[n] != '"' : v[n] != ';'))
			n++;
		if (n >= cap)
			n = cap - 1;
		memcpy(out, v, n);
		out[n] = 0;
		return;
	}
}

int mp_next_part(mp_t *mp, mp_part_t *part)
{
	uint8_t skip[128];
	if (mp->state == MP_DONE)
		return 0;
	// Skip the preamble or the rest of the current part.
	if (mp->state == MP_PREAMBLE || mp->state == MP_BODY) {
		mp->state = MP_BODY;
		int n;
		while ((n = mp_read(mp, skip, sizeof skip)) > 0) {
		}
		if (n < 0)
			return -1;
	}
	// After a delimiter: "--" ends the body, CRLF starts headers.
	while (mp->len - mp->pos < 2)
		if (!fill(mp))
			return -1;
	if (mp->buf[mp->pos] == '-' && mp->buf[mp->pos + 1] == '-') {
		mp->state = MP_DONE;
		return 0;
	}
	char line[256];
	if (read_line(mp, line, sizeof line) < 0) // rest of the delimiter line
		return -1;
	memset(part, 0, sizeof *part);
	for (;;) {
		if (read_line(mp, line, sizeof line) < 0)
			return -1;
		if (line[0] == 0)
			break;
		if (strncasecmp(line, "Content-Disposition:", 20) == 0) {
			param(line, "name", part->name, sizeof part->name);
			part->is_file = ci_find(line, "filename") != NULL;
			param(line, "filename", part->filename, sizeof part->filename);
		} else if (strncasecmp(line, "Content-Type:", 13) == 0) {
			const char *v = line + 13;
			while (*v == ' ')
				v++;
			strlcpy(part->content_type, v, sizeof part->content_type);
		}
	}
	mp->state = MP_BODY;
	return 1;
}
