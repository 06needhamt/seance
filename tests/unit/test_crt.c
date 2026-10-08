/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt: pe_crt.dll imports math and string functions, the way a codec
 * with a dynamic CRT (/MD) would; the harness's own builtin_msvcrt serves
 * them, forwarding to the host's libm/libc.
 */
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
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

static WCHAR *w16(const char *s)
{
	static WCHAR buf[4][128];
	static int next;
	WCHAR *w = buf[next++ % 4];
	size_t i = 0;

	for (; s[i] && i < 127; i++)
		w[i] = (BYTE)s[i];
	w[i] = 0;
	return w;
}

static int w16_is(const WCHAR *w, const char *s)
{
	while (*s && *w == (BYTE)*s)
		w++, s++;
	return !*s && !*w;
}

static int w16_starts(const WCHAR *w, const char *s)
{
	while (*s && *w == (BYTE)*s)
		w++, s++;
	return !*s;
}

/* msvcrt.dll's printf dialect. */
static void test_printf(void)
{
	int (*sprintf_)(char *, const char *, ...) = get("sprintf");
	int (*snprintf_)(char *, size_t, const char *, ...) = get("_snprintf");
	int (*scprintf)(const char *, ...) = get("_scprintf");
	int (*swprintf_)(WCHAR *, const WCHAR *, ...) = get("swprintf");
	int (*snwprintf)(WCHAR *, size_t, const WCHAR *, ...) = get("_snwprintf");
	int (*scwprintf)(const WCHAR *, ...) = get("_scwprintf");
	char b[128];
	WCHAR w[128];
	static const WCHAR euro_w[] = { 'a', 'b', 0x20ac, 0 };

	/* 64-bit and explicitly sized integers. */
	CHECK_EQ(sprintf_(b, "%I64d|%I64u|%I64x", -5000000000LL,
			  18446744073709551615ULL, 0x123456789abcULL), 45);
	CHECK_STR(b, "-5000000000|18446744073709551615|123456789abc");
	CHECK_EQ(sprintf_(b, "%lld %I32d %Id %hd %hhu", 1LL << 40, -7, 8,
			  70000, 300), 26);
	CHECK_STR(b, "1099511627776 -7 8 4464 44");
	sprintf_(b, "[%08I64X] [%-6I64d] [%+d] [% d] [%#x] [%#010x]",
		 0xabcdULL, 42LL, 3, 3, 255, 255);
	CHECK_STR(b, "[0000ABCD] [42    ] [+3] [ 3] [0xff] [0x000000ff]");
	sprintf_(b, "[%*d] [%-*d] [%.*d] [%05.3d]", 4, 1, 3, 2, 3, 5, 7);
	CHECK_STR(b, "[   1] [2  ] [005] [  007]");
	sprintf_(b, "[%*d]", -4, 9);
	CHECK_STR(b, "[9   ]");

	/* Floats: L is a double, exponents have three digits. */
	sprintf_(b, "%Lf %.2f %e %E %g %G", 1.5, 2.345, 12345.678, 0.00015,
		 1e-10, 1e20);
	CHECK_STR(b, "1.500000 2.35 1.234568e+004 1.500000E-004 1e-010 1E+020");
	sprintf_(b, "[%10.2e] [%-12.1e] [%012.3f] [%+.1f]", 1234.5, -2.0,
		 -3.25, 0.5);
	CHECK_STR(b, "[ 1.23e+003] [-2.0e+000   ] [-0000003.250] [+0.5]");
	sprintf_(b, "%g %g", 100.0, 1e100);
	CHECK_STR(b, "100 1e+100");
	/*
	 * Infinities and NaNs as msvcrt spells them, "#INF" cut (and
	 * rounded) by the precision as if it were digits.
	 */
	double inf = __builtin_inf(), qnan = __builtin_nan(""), ind = -qnan;
	sprintf_(b, "%f %e %E %g %G", inf, inf, inf, inf, inf);
	CHECK_STR(b, "1.#INF00 1.#INF00e+000 1.#INF00E+000 1.#INF 1.#INF");
	sprintf_(b, "%.0f|%.1f|%.2f|%.3f|%.4f|%.10f", inf, inf, inf, inf, inf,
		 inf);
	CHECK_STR(b, "1|1.$|1.#J|1.#IO|1.#INF|1.#INF000000");
	sprintf_(b, "%#.10g|%#g", inf, inf);	/* # keeps the zeros */
	CHECK_STR(b, "1.#INF00000|1.#INF");
	sprintf_(b, "%f %f %f %g %f", -inf, qnan, ind, ind, -qnan);
	CHECK_STR(b, "-1.#INF00 1.#QNAN0 -1.#IND00 -1.#IND -1.#IND00");
	sprintf_(b, "[%10f] [%+f] [%010f] [%.3g] [%-9g] [%e]", inf, inf, inf,
		 inf, inf, ind);
	CHECK_STR(b, "[  1.#INF00] [+1.#INF00] [001.#INF00] [1.#J] "
		  "[1.#INF   ] [-1.#IND00e+000]");
	/* Native msvcrt's own, from Wine's tests. */
	sprintf_(b, "%010.2f|%010.2f|%010.2f", ind, qnan, inf);
	CHECK_STR(b, "-000001.#J|0000001.#R|0000001.#J");

	/* Pointers, strings and characters. */
	sprintf_(b, "%p [%12p]", (void *)0x1234abcd, (void *)0xff);
	CHECK_STR(b, "1234ABCD [    000000FF]");
	sprintf_(b, "%s|%.2s|%5s|%-5s|%hs", "abc", "abc", "ab", "ab", "hs");
	CHECK_STR(b, "abc|ab|   ab|ab   |hs");
	CHECK_EQ(sprintf_(b, "%S|%ls|%ws|%.2S", euro_w, euro_w, euro_w,
			  euro_w), 14);
	CHECK_STR(b, "ab\x80|ab\x80|ab\x80|ab");
	sprintf_(b, "%s %S", (char *)NULL, (WCHAR *)NULL);
	CHECK_STR(b, "(null) (null)");
	/* %Z: an ANSI_STRING, or with w or l a UNICODE_STRING (bytes). */
	struct counted {
		unsigned short len, max;
		const void *buf;
	} as = { 3, 8, "abcdef" }, us = { 4, 8, euro_w }, none = { 0, 0, NULL };
	sprintf_(b, "[%Z] [%5Z] [%.2Z] [%wZ] [%lZ] [%Z] [%Z]", &as, &as, &as,
		 &us, &us, &none, (void *)NULL);
	CHECK_STR(b, "[abc] [  abc] [ab] [ab] [ab] [(null)] [(null)]");
	CHECK_EQ(sprintf_(b, "%c%C%lc%3c", 'x', 0x20ac, 'y', 'z'), 6);
	CHECK_STR(b, "x\x80y  z");
	CHECK_EQ(sprintf_(b, "a%cb", 0), 3);
	CHECK(!memcmp(b, "a\0b", 4));

	/* %n, %%, an unknown conversion, a lone % at the end. */
	int n = 0;
	short h = 0;
	sprintf_(b, "ab%ncd%hn", &n, &h);
	CHECK(n == 2 && h == 4);
	/* Each size writes just its own width. */
	unsigned char guard[4] = { 0x55, 0x55, 0x55, 0x55 };
	long long big = -1, big2 = -1;
	sprintf_(b, "abc%hhn%I64n%lln", &guard[1], &big, &big2);
	CHECK(guard[0] == 0x55 && guard[1] == 3 && guard[2] == 0x55 &&
	      guard[3] == 0x55);
	CHECK(big == 3 && big2 == 3);
	sprintf_(b, "100%% %y %");
	CHECK_STR(b, "100% y ");

	CHECK_EQ(scprintf("%I64d", 1234567890123LL), 13);
	CHECK_EQ(snprintf_(b, 5, "%I64d", 1234567890123LL), -1);
	CHECK(!memcmp(b, "12345", 5));

	/* The wide family: %s is wide, %S narrow. */
	CHECK_EQ(swprintf_(w, w16("%s/%S/%hs/%c/%C/%d"), euro_w, "nar", "h",
			   0x20ac, 'n', 42), 16);
	CHECK(w[0] == 'a' && w[1] == 'b' && w[2] == 0x20ac);
	CHECK(w16_starts(w + 3, "/nar/h/") && w[10] == 0x20ac);
	CHECK(w16_is(w + 11, "/n/42"));
	CHECK_EQ(swprintf_(w, w16("%5.1f|%-4s|%I64x"), 2.25, w16("x"),
			   0xfULL), 12);
	CHECK(w16_is(w, "  2.2|x   |f") || w16_is(w, "  2.3|x   |f"));
	CHECK_EQ(scwprintf(w16("%d%s"), 12, w16("abc")), 5);
	for (int i = 0; i < 8; i++)
		w[i] = 'q';
	CHECK_EQ(snwprintf(w, 3, w16("%s"), w16("abcdef")), -1);
	CHECK(w[0] == 'a' && w[2] == 'c' && w[3] == 'q');
	CHECK_EQ(snwprintf(w, 8, w16("%d"), 7), 1);
	CHECK(w16_is(w, "7"));
}

/*
 * Calling the _CI* intrinsics as cl does: arguments pushed on the x87
 * stack, the result left in ST0.
 */
__asm__(
	".text\n"
	/* double ci_call1(void *fn, double x) */
	"ci_call1:\n\t"
	"fldl 8(%esp)\n\t"
	"call *4(%esp)\n\t"
	"ret\n"
	/* double ci_call2(void *fn, double x, double y) */
	"ci_call2:\n\t"
	"fldl 8(%esp)\n\t"
	"fldl 16(%esp)\n\t"
	"call *4(%esp)\n\t"
	"ret\n"
	/* void ci_deep(void *fn, double x, const double in[6], double out[7]):
	 * six values live under x; out gets the result, then them. */
	"ci_deep:\n\t"
	"movl 16(%esp), %eax\n\t"
	"fldl 0(%eax)\n\t"
	"fldl 8(%eax)\n\t"
	"fldl 16(%eax)\n\t"
	"fldl 24(%eax)\n\t"
	"fldl 32(%eax)\n\t"
	"fldl 40(%eax)\n\t"
	"fldl 8(%esp)\n\t"
	"call *4(%esp)\n\t"
	"movl 20(%esp), %eax\n\t"
	"fstpl 0(%eax)\n\t"
	"fstpl 8(%eax)\n\t"
	"fstpl 16(%eax)\n\t"
	"fstpl 24(%eax)\n\t"
	"fstpl 32(%eax)\n\t"
	"fstpl 40(%eax)\n\t"
	"fstpl 48(%eax)\n\t"
	"ret\n"
	/* unsigned ci_cw(void *fn, double x, unsigned cw): the control word
	 * after calling fn with cw set. */
	"ci_cw:\n\t"
	"subl $8, %esp\n\t"
	"fnstcw (%esp)\n\t"
	"fldcw 24(%esp)\n\t"
	"fldl 16(%esp)\n\t"
	"call *12(%esp)\n\t"
	"fstp %st(0)\n\t"
	"fnstcw 4(%esp)\n\t"
	"fldcw (%esp)\n\t"
	"movzwl 4(%esp), %eax\n\t"
	"addl $8, %esp\n\t"
	"ret\n"
);
double ci_call1(void *fn, double x) __asm__("ci_call1");
double ci_call2(void *fn, double x, double y) __asm__("ci_call2");
void ci_deep(void *fn, double x, const double *in, double *out)
	__asm__("ci_deep");
unsigned ci_cw(void *fn, double x, unsigned cw) __asm__("ci_cw");

static void test_intrinsics(void)
{
	static const struct {
		const char *name;
		double (*ref)(double);
		double x;
	} one[] = {
		{ "_CIacos", acos, 0.25 }, { "_CIasin", asin, 0.25 },
		{ "_CIatan", atan, 2.0 }, { "_CIcos", cos, 1.0 },
		{ "_CIcosh", cosh, 1.5 }, { "_CIexp", exp, 2.5 },
		{ "_CIlog", log, 10.0 }, { "_CIlog10", log10, 1234.0 },
		{ "_CIsin", sin, 1.0 }, { "_CIsinh", sinh, 1.5 },
		{ "_CIsqrt", sqrt, 2.0 }, { "_CItan", tan, 0.5 },
		{ "_CItanh", tanh, 0.5 },
	};
	for (size_t i = 0; i < sizeof(one) / sizeof(*one); i++) {
		double r = ci_call1(get(one[i].name), one[i].x);
		if (fabs(r - one[i].ref(one[i].x)) > 1e-12) {
			fprintf(stderr, "%s(%g) = %.17g\n", one[i].name,
				one[i].x, r);
			CHECK(0);
		}
	}
	CHECK(fabs(ci_call2(get("_CIpow"), 2.0, 10.0) - 1024.0) < 1e-9);
	CHECK(fabs(ci_call2(get("_CIpow"), 9.0, 0.5) - 3.0) < 1e-12);
	CHECK(fabs(ci_call2(get("_CIatan2"), 1.0, -1.0) - atan2(1.0, -1.0)) <
	      1e-12);
	CHECK(fabs(ci_call2(get("_CIfmod"), 7.5, 2.0) - 1.5) < 1e-12);

	/* What the caller keeps on the x87 stack survives. */
	static const double in[6] = { 1, 2, 3, 4, 5, 6 };
	double out[7] = { 0 };
	ci_deep(get("_CIsin"), 0.5, in, out);
	CHECK(fabs(out[0] - sin(0.5)) < 1e-12);
	for (int i = 0; i < 6; i++)
		CHECK(out[1 + i] == in[5 - i]);

	/* And its control word, Windows' default here. */
	CHECK_EQ(ci_cw(get("_CIexp"), 1.0, 0x27f), 0x27f);
	CHECK_EQ(ci_cw(get("_CIsqrt"), 2.0, 0x7f), 0x7f);
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
	int (*sscanf_)(const char *, const char *, ...) = get("sscanf");
	int (*access_)(const char *, int) = get("_access");
	char *(*getenv_)(const char *) = get("getenv");
	time_t (*time_)(time_t *) = get("time");
	BYTE *iob = get("_iob");

	int parsed = 0;
	char word[8];
	CHECK_EQ(sscanf_("42 codec", "%d %7s", &parsed, word), 2);
	CHECK_EQ(parsed, 42);
	CHECK_STR(word, "codec");
	CHECK_EQ(access_("C:\\WINDOWS", 0), 0);
	CHECK_STR(getenv_("VFW_ROOT"), getenv("VFW_ROOT"));
	time_t now = 0;
	CHECK(time_(&now) == now && now > 0);

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

	/* A null pointer with a length of zero, which codecs pass. */
	void *(*memcpy_)(void *, const void *, size_t) = get("memcpy");
	void *(*memmove_)(void *, const void *, size_t) = get("memmove");
	void *(*memset_)(void *, int, size_t) = get("memset");
	int (*memcmp_)(const void *, const void *, size_t) = get("memcmp");
	void *(*memchr_)(const void *, int, size_t) = get("memchr");
	CHECK(memcpy_(buf, NULL, 0) == buf);
	CHECK(memmove_(buf, NULL, 0) == buf);
	CHECK(memset_(NULL, 0, 0) == NULL);
	CHECK_EQ(memcmp_(NULL, buf, 0), 0);
	CHECK(memchr_(NULL, 'x', 0) == NULL);

	/* An empty write to stdout through _iob[1]. */
	CHECK_EQ(fprintf_(iob + 32, "%s", ""), 0);

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

/* ucrtbase's __stdio_common_* functions, called as its headers do. */
typedef int (*common_sprintf_fn)(unsigned long long, void *, size_t,
				 const void *, void *, va_list);
typedef int (*common_snprintf_s_fn)(unsigned long long, void *, size_t,
				    size_t, const void *, void *, va_list);
typedef int (*common_sscanf_fn)(unsigned long long, const char *, size_t,
				const char *, void *, va_list);

static int csp(common_sprintf_fn fn, unsigned long long options, void *buf,
	       size_t n, const void *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = fn(options, buf, n, fmt, NULL, ap);
	va_end(ap);
	return r;
}

static int csnp_s(common_snprintf_s_fn fn, void *buf, size_t n, size_t count,
		  const void *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = fn(0, buf, n, count, fmt, NULL, ap);
	va_end(ap);
	return r;
}

static int css(common_sscanf_fn fn, unsigned long long options,
	       const char *in, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = fn(options, in, len, fmt, NULL, ap);
	va_end(ap);
	return r;
}

#define LEGACY_NULL 0x1ull	/* _CRT_INTERNAL_PRINTF_* */
#define STANDARD_SNPRINTF 0x2ull
#define LEGACY_WIDE 0x4ull
#define LEGACY_MSVCRT 0x8ull
#define LEGACY_EXPONENT 0x10ull
#define SECURECRT 0x1ull	/* _CRT_INTERNAL_SCANF_SECURECRT */

static void test_ucrt(void)
{
	common_sprintf_fn vsprintf_ = get("__stdio_common_vsprintf");
	common_sprintf_fn vswprintf_ = get("__stdio_common_vswprintf");
	common_sprintf_fn vsprintf_s = get("__stdio_common_vsprintf_s");
	common_snprintf_s_fn vsnprintf_s = get("__stdio_common_vsnprintf_s");
	int (*vfprintf_)(unsigned long long, void *, const char *, void *,
			 va_list) = get("__stdio_common_vfprintf");
	void *(*iob_func)(unsigned) = get("__acrt_iob_func");
	common_sscanf_fn vsscanf_ = get("__stdio_common_vsscanf");
	BYTE *iob = get("_iob");
	char b[64];
	WCHAR w[32];

	/* The three ways of a buffer too small, and none. */
	CHECK_EQ(csp(vsprintf_, 0, b, sizeof(b), "%d", 12), 2);
	CHECK_STR(b, "12");
	memset(b, 'x', sizeof(b));
	CHECK_EQ(csp(vsprintf_, 0, b, 4, "abcdef"), -2);
	CHECK_STR(b, "abc");
	CHECK_EQ(csp(vsprintf_, 0, b, 0, "abcdef"), -1);
	CHECK_EQ(csp(vsprintf_, STANDARD_SNPRINTF, b, 4, "abcdef"), 6);
	CHECK_STR(b, "abc");
	memset(b, 'x', sizeof(b));
	CHECK_EQ(csp(vsprintf_, LEGACY_NULL, b, 4, "abcdef"), -1);
	CHECK(!memcmp(b, "abcdx", 5));
	memset(b, 'x', sizeof(b));
	CHECK_EQ(csp(vsprintf_, LEGACY_NULL, b, 3, "abc"), 3);
	CHECK(!memcmp(b, "abcx", 4));
	CHECK_EQ(csp(vsprintf_, 0, NULL, 0, "%s!", "four"), 5);

	/* C99's ways, unless an option keeps msvcrt's. */
	double inf = INFINITY, ind = -NAN;
	csp(vsprintf_, 0, b, sizeof(b), "%e %g %E", 12345.678, 1e100, 0.00015);
	CHECK_STR(b, "1.234568e+04 1e+100 1.500000E-04");
	csp(vsprintf_, LEGACY_EXPONENT, b, sizeof(b), "%e", 12345.678);
	CHECK_STR(b, "1.234568e+004");
	csp(vsprintf_, 0, b, sizeof(b), "%f %f %F %+f [%6f] %f %G", inf, -inf,
	    inf, inf, inf, NAN, ind);
	CHECK_STR(b, "inf -inf INF +inf [   inf] nan -NAN(IND)");
	csp(vsprintf_, LEGACY_MSVCRT, b, sizeof(b), "%f", inf);
	CHECK_STR(b, "1.#INF00");
	csp(vsprintf_, 0, b, sizeof(b), "%zu %td %jd %.1F", (size_t)7,
	    (ptrdiff_t)-3, (long long)1 << 40, 2.5);
	CHECK_STR(b, "7 -3 1099511627776 2.5");

	/* In the wide forms %s is narrow, unless legacy; %S is wide. */
	CHECK_EQ(csp(vswprintf_, 0, w, 32, w16("%s|%S|%c"), "nar", w16("wid"),
		     'c'), 9);
	CHECK(w16_is(w, "nar|wid|c"));
	CHECK_EQ(csp(vswprintf_, LEGACY_WIDE, w, 32, w16("%s|%S"), w16("wid"),
		     "nar"), 7);
	CHECK(w16_is(w, "wid|nar"));

	/* The _s forms: -1 and an empty buffer when it doesn't fit. */
	CHECK_EQ(csp(vsprintf_s, 0, b, 8, "%d", 1234), 4);
	CHECK_EQ(csp(vsprintf_s, 0, b, 4, "%d", 1234), -1);
	CHECK_STR(b, "");
	CHECK_EQ(csnp_s(vsnprintf_s, b, 4, (size_t)-1, "abcdef"), -1);
	CHECK_STR(b, "abc");
	CHECK_EQ(csnp_s(vsnprintf_s, b, 8, 2, "abcdef"), -1);
	CHECK_STR(b, "ab");
	CHECK_EQ(csnp_s(vsnprintf_s, b, 4, 10, "abcdef"), -1);
	CHECK_STR(b, "");
	CHECK_EQ(csnp_s(vsnprintf_s, b, 8, 10, "abc"), 3);
	CHECK_STR(b, "abc");

	/* stdin, stdout and stderr are _iob's. */
	CHECK(iob_func(0) == iob && iob_func(1) == iob + 32 &&
	      iob_func(2) == iob + 64 && !iob_func(3));
	va_list none;
	memset(&none, 0, sizeof(none));
	CHECK_EQ(vfprintf_(0, iob_func(1), "", NULL, none), 0);

	/* sscanf_s: a size after each string; one too small stops it. */
	char word[4];
	int n = 0;
	CHECK_EQ(css(vsscanf_, SECURECRT, "ab 12", (size_t)-1, "%s %d", word,
		     (unsigned)sizeof(word), &n), 2);
	CHECK_STR(word, "ab");
	CHECK_EQ(n, 12);
	CHECK_EQ(css(vsscanf_, SECURECRT, "abcd 12", (size_t)-1, "%s %d", word,
		     (unsigned)sizeof(word), &n), 0);
	CHECK_STR(word, "");
	/* A length: no further than it, terminator or not. */
	CHECK_EQ(css(vsscanf_, 0, "12345", 3, "%d", &n), 1);
	CHECK_EQ(n, 123);
}

/* msvcrt's sscanf: its sizes, sets, %n, and EOF. */
static void test_scanf(void)
{
	int (*sscanf_)(const char *, const char *, ...) = get("sscanf");
	long long big = 0;
	unsigned long long hex = 0;
	signed char tiny = 0;
	short sh = 0;
	double d = 0, ld = 0;
	float f = 0;
	char set[8], three[4] = "zzz";
	WCHAR wide[8];
	int n = -1, m = -1;

	CHECK_EQ(sscanf_("-5000000000 ffffffffff 200 -7", "%I64d %I64x %hhd %hd",
			 &big, &hex, &tiny, &sh), 4);
	CHECK(big == -5000000000LL && hex == 0xffffffffffULL);
	CHECK_EQ(tiny, -56);
	CHECK_EQ(sh, -7);
	CHECK_EQ(sscanf_("1.5 2.25 3.125", "%f %lf %Lf", &f, &d, &ld), 3);
	CHECK(f == 1.5f && d == 2.25 && ld == 3.125);
	CHECK_EQ(sscanf_("abc123", "%[a-z]%n", set, &n), 1);
	CHECK_STR(set, "abc");
	CHECK_EQ(n, 3);
	CHECK_EQ(sscanf_("xyzw", "%3c", three), 1);
	CHECK(!memcmp(three, "xyz", 3));
	CHECK_EQ(sscanf_("  42 rest", "%*d %n%S", &m, wide), 1);
	CHECK_EQ(m, 5);
	CHECK(w16_is(wide, "rest"));
	CHECK_EQ(sscanf_("a=5", "a=%d", &n), 1);
	CHECK_EQ(n, 5);
	CHECK_EQ(sscanf_("b=5", "a=%d", &n), 0);
	CHECK_EQ(sscanf_("", "%d", &n), -1);
	CHECK_EQ(sscanf_("   ", "%d", &n), -1);
	CHECK_EQ(sscanf_("x", "%d", &n), 0);
	CHECK_EQ(sscanf_("7", "%d %d", &n, &m), 1);
	CHECK_EQ(sscanf_("100%", "%d%%", &n), 1);
}

/* ntdll's own _vsnprintf and friends: msvcrt's. */
static int nt_vsn(int (*fn)(char *, size_t, const char *, va_list),
		  char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = fn(buf, n, fmt, ap);
	va_end(ap);
	return r;
}

static void test_ntdll_printf(void)
{
	struct module *nt = mod_load("ntdll.dll", NULL);
	CHECK(nt != NULL);
	if (!nt)
		return;
	int (*vsn)(char *, size_t, const char *, va_list) =
		mod_get_proc(nt, "_vsnprintf");
	int (*sn)(char *, size_t, const char *, ...) =
		mod_get_proc(nt, "_snprintf");
	int (*snw)(WCHAR *, size_t, const WCHAR *, ...) =
		mod_get_proc(nt, "_snwprintf");
	CHECK(vsn && sn && snw && mod_get_proc(nt, "_vsnwprintf"));
	if (vsn && sn && snw) {
		char b[8];
		WCHAR w[8];
		memset(b, 'x', sizeof(b));
		CHECK_EQ(nt_vsn(vsn, b, 4, "%I64d", 123456LL), -1);
		CHECK(!memcmp(b, "1234x", 5));
		CHECK_EQ(sn(b, sizeof(b), "%s", "ok"), 2);
		CHECK_STR(b, "ok");
		CHECK_EQ(snw(w, 8, w16("%d"), 42), 2);
		CHECK(w16_is(w, "42"));
	}
	mod_free(nt);
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
		test_printf();
		test_intrinsics();
		test_ucrt();
		test_scanf();
		mod_free(crt);
	}
	test_ntdll_printf();
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
		int (*setjmp_check)(void) = mod_get_proc(m, "crt_setjmp_check");
		CHECK(setjmp_check != NULL);
		if (setjmp_check) {
			int r = setjmp_check();
			if (r)
				fprintf(stderr, "crt_setjmp_check failed at %d\n", r);
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
