/*
 * msvcrt: pe_crt.dll imports math and string functions, the way a codec
 * with a dynamic CRT (/MD) would; the harness's own builtin_msvcrt serves
 * them, forwarding to the host's libm/libc.
 */
#include <math.h>
#include <stdint.h>

#include "loader/modules.h"
#include "runtime/crash.h"
#include "runtime/runtime.h"
#include "test.h"

static struct module *crt;

static void *get(const char *name)
{
	void *p = mod_get_proc(crt, name);
	if (!p) {
		fprintf(stderr, "msvcrt has no %s\n", name);
		exit(1);
	}
	return p;
}

/* MSVCRT behaviour that differs from glibc's, called directly. */
static void test_msvcrt_semantics(void)
{
	int (*snprintf_)(char *, size_t, const char *, ...) = get("_snprintf");
	int (*fprintf_)(void *, const char *, ...) = get("fprintf");
	void *(*fopen_)(const char *, const char *) = get("fopen");
	size_t (*fwrite_)(const void *, size_t, size_t, void *) = get("fwrite");
	size_t (*fread_)(void *, size_t, size_t, void *) = get("fread");
	int (*fseek_)(void *, long, int) = get("fseek");
	int (*feof_)(void *) = get("feof");
	int (*fclose_)(void *) = get("fclose");
	int (*isprint_)(int) = get("isprint");
	int (*isgraph_)(int) = get("isgraph");
	unsigned (*control87)(unsigned, unsigned) = get("_control87");
	int (*controlfp_s)(unsigned *, unsigned, unsigned) = get("_controlfp_s");
	void (*srand_)(unsigned) = get("srand");
	int (*rand_)(void) = get("rand");
	BYTE *iob = get("_iob");

	/* _snprintf: -1 and no terminator when the output doesn't fit. */
	char buf[16];
	memset(buf, 'x', sizeof(buf));
	CHECK_EQ(snprintf_(buf, 4, "%s", "abcdef"), -1);
	CHECK(!memcmp(buf, "abcdx", 5));
	memset(buf, 'x', sizeof(buf));
	CHECK_EQ(snprintf_(buf, 3, "abc"), 3);
	CHECK(!memcmp(buf, "abcx", 4));
	CHECK_EQ(snprintf_(buf, 8, "%d", 12), 2);
	CHECK_STR(buf, "12");

	/*
	 * A conversion the host can't do fails cleanly: %ls reads a 32-bit
	 * wchar_t on Linux, so UTF-16 text is out of range for it.
	 */
	static const WCHAR utf16[] = { 'a', 'b', 0x20ac, 0 };
	CHECK_EQ(fprintf_(iob + 32, "%ls", utf16), -1);	/* _iob[1], stdout */

	/* A successful fseek clears end-of-file. */
	void *f = fopen_("C:\\WINDOWS\\TEMP\\test_crt.tmp", "w+b");
	CHECK(f != NULL);
	if (f) {
		char c;
		CHECK_EQ(fwrite_("xy", 1, 2, f), 2);
		CHECK_EQ(fseek_(f, 0, SEEK_SET), 0);
		CHECK_EQ(fread_(buf, 1, sizeof(buf), f), 2);
		CHECK_EQ(fread_(&c, 1, 1, f), 0);
		CHECK(feof_(f));
		CHECK_EQ(fseek_(f, 1, SEEK_SET), 0);
		CHECK(!feof_(f));
		CHECK_EQ(fread_(&c, 1, 1, f), 1);
		CHECK_EQ(c, 'y');
		fclose_(f);
	}

	/* isprint and isgraph as MSVC's table has them. */
	CHECK(isprint_('a') && isprint_(' ') && isprint_('~'));
	CHECK(!isprint_('\t') && !isprint_(0x80) && !isprint_(0xff) && !isprint_(-1));
	CHECK(isgraph_('!') && !isgraph_(' ') && !isgraph_(0xa0));

	/* _control87 reports the word after the change. */
	CHECK_EQ(control87(0, 0), 0x9001f);
	CHECK_EQ(control87(0x300, 0x300), 0x9031f);	/* _RC_CHOP */
	unsigned cur = 0;
	CHECK_EQ(controlfp_s(&cur, 0, 0x300), 0);
	CHECK_EQ(cur, 0x9001f);
	CHECK_EQ(control87(0x20000, 0x30000), 0xa001f);	/* _PC_24 */
	CHECK_EQ(control87(0x9001f, 0xffffffff), 0x9001f);

	/* rand: MSVCRT's sequence. */
	srand_(1);
	static const int want[] = { 41, 18467, 6334, 26500, 19169 };
	for (int i = 0; i < 5; i++)
		CHECK_EQ(rand_(), want[i]);
	srand_(12345);
	int first = rand_();
	srand_(12345);
	CHECK_EQ(rand_(), first);
}

int main(void)
{
	const char *tmpdir = getenv("TMPDIR");
	char root[256];
	snprintf(root, sizeof(root), "%s/vfw-crt-XXXXXX",
		 tmpdir && *tmpdir ? tmpdir : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", root, 1);

	CHECK_EQ(runtime_enter(), 0);
	CHECK_EQ(crash_install(), 0);
	crt = mod_load("msvcrt.dll", NULL);
	CHECK(crt != NULL);
	if (crt) {
		test_msvcrt_semantics();
		mod_free(crt);
	}
	/* The fixture, if it was built (the checks above need no mingw). */
	const char *dir = getenv("VFW_FIXTURES");
	char dll[4096];
	snprintf(dll, sizeof(dll), "%s/pe_crt.dll", dir ? dir : ".");
	struct module *m = dir && !access(dll, R_OK) ? mod_load(dll, NULL) : NULL;
	if (!m)
		printf("test_crt: pe_crt.dll not built, skipping the fixture\n");
	if (m) {
		double (*math_check)(void) = mod_get_proc(m, "crt_math_check");
		int (*string_check)(void) = mod_get_proc(m, "crt_string_check");
		int (*stdio_check)(void) = mod_get_proc(m, "crt_stdio_check");
		int (*startup_check)(void) = mod_get_proc(m, "crt_startup_check");
		CHECK(math_check != NULL);
		CHECK(string_check != NULL);
		CHECK(stdio_check != NULL);
		CHECK(startup_check != NULL);
		if (math_check) {
			double want = sin(0.0) + cos(0.0) + sqrt(4.0) +
				      pow(2.0, 10.0) + fabs(-3.5) + floor(2.7) +
				      ceil(2.1) + atan2(1.0, 1.0) + log(1.0) +
				      exp(0.0) + fmod(10.0, 3.0);
			CHECK(math_check() == want);
		}
		if (string_check) {
			int r = string_check();
			if (r)
				fprintf(stderr, "crt_string_check failed at %d\n", r);
			CHECK_EQ(r, 0);
		}
		if (stdio_check) {
			int r = stdio_check();
			if (r)
				fprintf(stderr, "crt_stdio_check failed at %d\n", r);
			CHECK_EQ(r, 0);
		}
		if (startup_check) {
			int r = startup_check();
			if (r)
				fprintf(stderr, "crt_startup_check failed at %d\n", r);
			CHECK_EQ(r, 0);
		}
		mod_free(m);
	}

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", root);
	return test_summary("test_crt");
}
