#include "test.h"

int test_failures;

int run_tests(const char *suite, const struct test_case *cases, size_t n)
{
	int failed = 0;
	for (size_t i = 0; i < n; i++) {
		int before = test_failures;
		cases[i].fn();
		if (test_failures != before) {
			printf("FAIL %s/%s\n", suite, cases[i].name);
			failed++;
		}
	}
	printf("%s: %zu tests, %d failed\n", suite, n, failed);
	return failed;
}
