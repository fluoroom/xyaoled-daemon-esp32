// Captive-portal DNS: every A query is answered with one address.
#pragma once

#include <stddef.h>
#include <stdint.h>

// captive_dns_answer turns the query in buf (n bytes) into its response in
// place, answering A and ANY questions with ip and others with no records.
// buf must hold cap bytes. Returns the response length, or 0 when the
// packet is not a single-question query and gets no answer.
size_t captive_dns_answer(uint8_t *buf, size_t n, size_t cap, const uint8_t ip[4]);
