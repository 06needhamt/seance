/*
 * Minimal unit-test helpers. Each test program includes this, runs CHECKs,
 * and returns test_summary() from main().
 */
#ifndef VFW_TEST_H
#define VFW_TEST_H

#include <stdio.h>

static int test_failures;
static int test_checks;

#define CHECK(cond) \
	do { \
		test_checks++; \
		if (!(cond)) { \
			test_failures++; \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
				__FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		long long a_ = (long long)(a), b_ = (long long)(b); \
		test_checks++; \
		if (a_ != b_) { \
			test_failures++; \
			fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s " \
				"(%lld != %lld)\n", __FILE__, __LINE__, \
				#a, #b, a_, b_); \
		} \
	} while (0)

static inline int test_summary(const char *name)
{
	if (test_failures) {
		fprintf(stderr, "%s: %d of %d checks failed\n",
			name, test_failures, test_checks);
		return 1;
	}
	printf("%s: %d checks passed\n", name, test_checks);
	return 0;
}

#endif /* VFW_TEST_H */
