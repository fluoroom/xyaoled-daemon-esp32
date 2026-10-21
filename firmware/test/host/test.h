// Minimal test harness for the host tests: each test is a function that
// reports failures with CHECK; main runs them all.
#pragma once

#include <stdio.h>
#include <string.h>

extern int test_failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			test_failures++;                                       \
			printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

typedef void (*test_fn)(void);

struct test_case {
	const char *name;
	test_fn fn;
};

#define TEST(name) {#name, name}

int run_tests(const char *suite, const struct test_case *cases, size_t n);
