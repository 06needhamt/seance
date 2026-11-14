/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * More of msvcrt: wide-string conversions and classes (wcstol, wcstod,
 * _wtoi, _itow, wcstok, isw*), and odds and ends codecs import (_ftime,
 * _filelength, _strtime, _strdate, _swab, perror, _stricoll, _HUGE,
 * __CppXcptFilter). Windows' WCHAR is 16 bits and its long 32, so nothing
 * here goes through the host's wchar_t or long.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "runtime/builtin.h"
#include "win32/internal.h"
#include "win32/k32.h"

#define CRTFN(ret) static ret STUB_CDECL

/* The ASCII prefix of s a number could be read from, one char per WCHAR,
 * so offsets into it are offsets into s. */
static char *num_prefix(const WCHAR *s)
{
	size_t n = 0;

	while (s[n] && s[n] < 0x80 && n < 512)
		n++;
	char *a = malloc(n + 1);
	if (!a)
		return NULL;
	for (size_t i = 0; i < n; i++)
		a[i] = (char)s[i];
	a[n] = 0;
	return a;
}

CRTFN(LONG) crt_wcstol(const WCHAR *s, WCHAR **end, int base)
{
	char *a = num_prefix(s), *e;

	if (!a) {
		if (end)
			*end = (WCHAR *)s;
		return 0;
	}
	errno = 0;
	long long v = strtoll(a, &e, base);
	if (end)
		*end = (WCHAR *)s + (e - a);
	free(a);
	if (v > INT32_MAX) {
		errno = ERANGE;
		return INT32_MAX;
	}
	if (v < INT32_MIN) {
		errno = ERANGE;
		return INT32_MIN;
	}
	return (LONG)v;
}

CRTFN(ULONG) crt_wcstoul(const WCHAR *s, WCHAR **end, int base)
{
	char *a = num_prefix(s), *e;

	if (!a) {
		if (end)
			*end = (WCHAR *)s;
		return 0;
	}
	errno = 0;
	unsigned long long v = strtoull(a, &e, base);
	int neg = strchr(a, '-') && strchr(a, '-') < e;
	if (end)
		*end = (WCHAR *)s + (e - a);
	free(a);
	/* As strtoul: a negated value wraps, one too big saturates. */
	if (!neg && v > UINT32_MAX) {
		errno = ERANGE;
		return UINT32_MAX;
	}
	return (ULONG)v;
}

CRTFN(double) crt_wcstod(const WCHAR *s, WCHAR **end)
{
	char *a = num_prefix(s), *e;

	if (!a) {
		if (end)
			*end = (WCHAR *)s;
		return 0;
	}
	double v = strtod(a, &e);
	if (end)
		*end = (WCHAR *)s + (e - a);
	free(a);
	return v;
}

CRTFN(int) crt__wtoi(const WCHAR *s) { return (int)crt_wcstol(s, NULL, 10); }
CRTFN(LONG) crt__wtol(const WCHAR *s) { return crt_wcstol(s, NULL, 10); }

CRTFN(WCHAR *) crt__itow(int v, WCHAR *buf, int radix)
{
	char a[40];
	unsigned long long u;
	int neg = radix == 10 && v < 0;
	size_t n = 0;

	if (radix < 2 || radix > 36) {
		buf[0] = 0;
		return buf;
	}
	u = neg ? (unsigned long long)(-(long long)v) : (unsigned)v;
	do {
		unsigned d = (unsigned)(u % (unsigned)radix);
		a[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		u /= (unsigned)radix;
	} while (u);
	size_t o = 0;
	if (neg)
		buf[o++] = '-';
	while (n)
		buf[o++] = (BYTE)a[--n];
	buf[o] = 0;
	return buf;
}

static int in_set(WCHAR c, const WCHAR *set)
{
	for (; *set; set++)
		if (*set == c)
			return 1;
	return 0;
}

/* msvcrt's two-argument wcstok, with its position per thread. */
CRTFN(WCHAR *) crt_wcstok(WCHAR *s, const WCHAR *delim)
{
	static __thread WCHAR *next;

	if (!s)
		s = next;
	if (!s)
		return NULL;
	while (*s && in_set(*s, delim))
		s++;
	if (!*s) {
		next = NULL;
		return NULL;
	}
	WCHAR *tok = s;
	while (*s && !in_set(*s, delim))
		s++;
	if (*s)
		*s++ = 0;
	next = *s ? s : NULL;
	return tok;
}

#define ISW(name, mask) \
CRTFN(int) crt_##name(unsigned short c) { return (k32_ctype1(c) & (mask)) != 0; }
ISW(iswupper, C1_UPPER)
ISW(iswlower, C1_LOWER)
ISW(iswdigit, C1_DIGIT)
ISW(iswspace, C1_SPACE)
ISW(iswpunct, C1_PUNCT)
ISW(iswcntrl, C1_CNTRL)
ISW(iswxdigit, C1_XDIGIT)
ISW(iswalpha, C1_ALPHA)
ISW(iswalnum, C1_ALPHA | C1_DIGIT)

/* struct _timeb: time_t (32-bit on i386, 64 on x64), milliseconds, the
 * zone's minutes west of UTC and a DST flag; local time is UTC here. */
struct crt_timeb {
#if defined(__x86_64__)
	int64_t time;
#else
	int32_t time;
#endif
	unsigned short millitm;
	short timezone;
	short dstflag;
};

CRTFN(void) crt__ftime(struct crt_timeb *tb)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	tb->time = tv.tv_sec;
	tb->millitm = (unsigned short)(tv.tv_usec / 1000);
	tb->timezone = 0;
	tb->dstflag = 0;
}

CRTFN(LONG) crt__filelength(int fd)
{
	struct stat st;

	if (fstat(fd, &st)) {
		errno = EBADF;
		return -1;
	}
	return st.st_size > INT32_MAX ? -1 : (LONG)st.st_size;
}

CRTFN(char *) crt__strtime(char *buf)
{
	time_t t = time(NULL);
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(buf, 9, "%H:%M:%S", &tm);
	return buf;
}

CRTFN(char *) crt__strdate(char *buf)
{
	time_t t = time(NULL);
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(buf, 9, "%m/%d/%y", &tm);
	return buf;
}

CRTFN(void) crt__swab(const char *src, char *dst, int n)
{
	for (int i = 0; i + 1 < n; i += 2) {
		char a = src[i], b = src[i + 1];
		dst[i] = b;
		dst[i + 1] = a;
	}
}

CRTFN(void) crt_perror(const char *s)
{
	if (s && *s)
		fprintf(stderr, "%s: %s\n", s, strerror(errno));
	else
		fprintf(stderr, "%s\n", strerror(errno));
}

/* Collation is byte order here, as in the "C" locale. */
CRTFN(int) crt__stricoll(const char *a, const char *b) { return strcasecmp(a, b); }

/* The filter a C++ catch(...) in a DLL's entry point uses: handle the
 * Microsoft C++ exception, let everything else go on. */
CRTFN(int) crt___CppXcptFilter(unsigned code, void *ep)
{
	return code == 0xE06D7363 ? 1 : 0;
}

static const double huge_val = HUGE_VAL;

#define E(name) { #name, 0, (void *)crt_##name }

const struct builtin_export crt_wide_exports[] = {
	E(__CppXcptFilter),
	{ "_HUGE", 0, (void *)&huge_val },
	E(_filelength),
	E(_ftime),
	{ "_ftime32", 0, (void *)crt__ftime },
	E(_itow),
	E(_stricoll),
	E(_strdate),
	E(_strtime),
	E(_swab),
	E(_wtoi),
	E(_wtol),
	E(iswalnum),
	E(iswalpha),
	E(iswcntrl),
	E(iswdigit),
	E(iswlower),
	E(iswpunct),
	E(iswspace),
	E(iswupper),
	E(iswxdigit),
	E(perror),
	E(wcstod),
	E(wcstok),
	E(wcstol),
	E(wcstoul),
	BUILTIN_END
};
