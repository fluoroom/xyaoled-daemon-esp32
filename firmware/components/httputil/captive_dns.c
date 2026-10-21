#include "captive_dns.h"

#include <string.h>

size_t captive_dns_answer(uint8_t *buf, size_t n, size_t cap, const uint8_t ip[4])
{
	if (n < 12 || n > cap || (buf[2] & 0x80) || (buf[2] & 0x78)) // a response, or not a standard query
		return 0;
	uint16_t qd = (uint16_t)(buf[4] << 8 | buf[5]);
	if (qd != 1)
		return 0;
	// Walk the name of the question: labels up to a zero byte.
	size_t p = 12;
	while (p < n && buf[p] != 0) {
		if (buf[p] & 0xc0)
			return 0; // compression has no place in a question
		p += (size_t)buf[p] + 1;
	}
	if (p + 5 > n)
		return 0;
	p++; // the zero label
	uint16_t qtype = (uint16_t)(buf[p] << 8 | buf[p + 1]);
	p += 4; // type and class
	// Response, authoritative, recursion desired copied from the query.
	buf[2] = (uint8_t)(0x84 | (buf[2] & 0x01));
	buf[3] = 0x00;
	memset(buf + 6, 0, 6); // no answer, authority or additional records yet
	size_t len = p;
	if ((qtype == 1 || qtype == 255) && len + 16 <= cap) {
		buf[7] = 1;
		static const uint8_t answer[12] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
		memcpy(buf + len, answer, sizeof answer);
		memcpy(buf + len + sizeof answer, ip, 4);
		len += 16;
	}
	return len;
}
