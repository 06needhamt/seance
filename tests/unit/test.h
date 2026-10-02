/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal unit-test helpers. Each test program includes this, runs CHECKs,
 * and returns test_summary() from main().
 */
#ifndef VFW_TEST_H
#define VFW_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define CHECK_STR(a, b) \
	do { \
		const char *a_ = (a), *b_ = (b); \
		test_checks++; \
		if (!a_ || !b_ || strcmp(a_, b_)) { \
			test_failures++; \
			fprintf(stderr, "%s:%d: CHECK_STR failed: %s == %s " \
				"(\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
				#a, #b, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
		} \
	} while (0)

/* Exit status telling CTest that a test was skipped. */
#define TEST_SKIP 77

/*
 * The path of a fixture DLL in $VFW_FIXTURES. Exits with TEST_SKIP if the
 * fixtures were not built (no mingw-w64).
 */
static inline const char *fixture(const char *name)
{
	static char paths[8][4096];
	static int next;
	const char *dir = getenv("VFW_FIXTURES");
	char *p = paths[next++ % 8];

	snprintf(p, sizeof(paths[0]), "%s/%s", dir ? dir : ".", name);
	if (!dir || access(p, R_OK)) {
		printf("fixture %s not available\n", p);
		exit(TEST_SKIP);
	}
	return p;
}

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
