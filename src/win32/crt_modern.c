/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt: the functions of the 64-bit time_t era that FFmpeg-based filters
 * (LAV Filters) import: _time64 and friends, _stat64, _strtoi64,
 * _aligned_malloc, and the locale accessors of the CRT's headers. Also a
 * few others that DLLs in the codec zip import: _i64toa and _ui64toa,
 * memcpy_s and memmove_s (mfc90), _strset and _strnset, and mbtowc.
 */
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/k32.h"
#include "win32/path.h"
#include "win32/internal.h"

#define CRTFN(ret) static ret STUB_CDECL

/* Time. The clock is the harness's (VFW_DATE may set it); the zone is UTC. */

CRTFN(int64_t) crt__time64(int64_t *out)
{
	int64_t t = (int64_t)win_unix_now();

	if (out)
		*out = t;
	return t;
}

CRTFN(int32_t) crt__time32(int32_t *out)
{
	int32_t t = (int32_t)win_unix_now();

	if (out)
		*out = t;
	return t;
}

/* MSVC's struct tm is the nine ints of the C standard. */
static void *tm_out(const struct tm *tm)
{
	static _Thread_local int fields[9];

	fields[0] = tm->tm_sec;
	fields[1] = tm->tm_min;
	fields[2] = tm->tm_hour;
	fields[3] = tm->tm_mday;
	fields[4] = tm->tm_mon;
	fields[5] = tm->tm_year;
	fields[6] = tm->tm_wday;
	fields[7] = tm->tm_yday;
	fields[8] = 0;
	return fields;
}

CRTFN(void *) crt__gmtime64(const int64_t *t)
{
	struct tm tm;
	time_t tt;

	if (!t || *t < 0)
		return NULL;
	tt = (time_t)*t;
	return gmtime_r(&tt, &tm) ? tm_out(&tm) : NULL;
}

/* UTC is the local time here. */
CRTFN(void *) crt__localtime64(const int64_t *t)
{
	return crt__gmtime64(t);
}

CRTFN(int64_t) crt__mktime64(void *tmv)
{
	const int *f = tmv;
	struct tm tm;

	if (!f)
		return -1;
	memset(&tm, 0, sizeof(tm));
	tm.tm_sec = f[0];
	tm.tm_min = f[1];
	tm.tm_hour = f[2];
	tm.tm_mday = f[3];
	tm.tm_mon = f[4];
	tm.tm_year = f[5];
	return (int64_t)timegm(&tm);
}

/* Integers. */

CRTFN(int64_t) crt__strtoi64(const char *s, char **end, int base)
{
	return strtoll(s, end, base);
}

CRTFN(uint64_t) crt__strtoui64(const char *s, char **end, int base)
{
	return strtoull(s, end, base);
}

/* struct _stat64: the 32-bit one's fields, with the size and times 64 bits. */

struct crt_stat64 {
	uint32_t dev;
	uint16_t ino, mode;
	int16_t nlink, uid, gid;
	uint32_t rdev;
	uint32_t pad;		/* MSVC aligns the 64-bit fields to 8 */
	int64_t size, atime, mtime, ctime;
};

_Static_assert(__builtin_offsetof(struct crt_stat64, size) == 24, "_stat64");
_Static_assert(sizeof(struct crt_stat64) == 56, "_stat64");

static void stat64_fill(const struct stat *st, struct crt_stat64 *w)
{
	unsigned owner = st->st_mode & 0700;

	memset(w, 0, sizeof(*w));
	w->dev = (uint32_t)st->st_dev;
	w->ino = (uint16_t)st->st_ino;
	w->mode = (uint16_t)((st->st_mode & 0170000) | owner | owner >> 3 | owner >> 6);
	w->nlink = (int16_t)st->st_nlink;
	w->rdev = (uint32_t)st->st_rdev;
	w->size = st->st_size;
	w->atime = st->st_atime;
	w->mtime = st->st_mtime;
	w->ctime = st->st_ctime;
}

CRTFN(int) crt__fstat64(int fd, struct crt_stat64 *out)
{
	struct stat st;

	if (!out) {
		errno = EINVAL;
		return -1;
	}
	if (fstat(fd, &st))
		return -1;
	stat64_fill(&st, out);
	return 0;
}

CRTFN(int) crt__stat64(const char *name, struct crt_stat64 *out)
{
	char *path = name ? path_to_unix(name) : NULL;
	struct stat st;
	int r = -1;

	if (!out || !path)
		errno = !out ? EINVAL : ENOENT;
	else if (!stat(path, &st)) {
		stat64_fill(&st, out);
		r = 0;
	}
	free(path);
	return r;
}

CRTFN(int) crt__wstat64(const WCHAR *name, struct crt_stat64 *out)
{
	char *u = name ? utf16_to_utf8_dup(name) : NULL;
	int r = crt__stat64(u, out);

	free(u);
	return r;
}

/* Aligned allocation. Freed with _aligned_free, which is libc's free. */

CRTFN(void *) crt__aligned_malloc(size_t size, size_t align)
{
	void *p = NULL;

	if (align < sizeof(void *))
		align = sizeof(void *);
	if (align & (align - 1)) {
		errno = EINVAL;
		return NULL;
	}
	if (posix_memalign(&p, align, size ? size : 1))
		return NULL;
	if (win_zero_memory())
		memset(p, 0, size);
	return p;
}

CRTFN(void) crt__aligned_free(void *p) { free(p); }

CRTFN(void *) crt__aligned_realloc(void *p, size_t size, size_t align)
{
	void *q;
	size_t have;

	if (!p)
		return crt__aligned_malloc(size, align);
	if (!size) {
		free(p);
		return NULL;
	}
	q = crt__aligned_malloc(size, align);
	if (!q)
		return NULL;
	have = malloc_usable_size(p);
	memcpy(q, p, have < size ? have : size);
	free(p);
	return q;
}

/* The C locale: one byte per character, code page 0 (the "C" locale). */

CRTFN(int *) crt____mb_cur_max_func(void)
{
	static int one = 1;

	return &one;
}

CRTFN(unsigned) crt____lc_codepage_func(void) { return 0; }

/*
 * Restores the guard page after a stack overflow was caught. The stack's
 * guard here is the host's own, which never goes away, so there is
 * nothing to restore: success.
 */
CRTFN(int) crt__resetstkoflw(void) { return 1; }

/*
 * 64-bit integers as text: base 10 signed for _i64toa, else the bits
 * unsigned, as msvcrt gives them (_i64toa(-1, b, 16) is 16 f's).
 */
static char *u64toa(uint64_t u, int neg, char *buf, int radix)
{
	char tmp[66];
	int i = 0, j = 0;

	if (radix < 2 || radix > 36) {
		buf[0] = 0;
		return buf;
	}
	do {
		int d = (int)(u % (unsigned)radix);
		tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		u /= (unsigned)radix;
	} while (u);
	if (neg)
		buf[j++] = '-';
	while (i)
		buf[j++] = tmp[--i];
	buf[j] = 0;
	return buf;
}

CRTFN(char *) crt__i64toa(int64_t v, char *buf, int radix)
{
	int neg = radix == 10 && v < 0;

	return u64toa(neg ? 0 - (uint64_t)v : (uint64_t)v, neg, buf, radix);
}

CRTFN(char *) crt__ui64toa(uint64_t v, char *buf, int radix)
{
	return u64toa(v, 0, buf, radix);
}

/*
 * The bounds-checked copies of msvcr80 on: a copy that wouldn't fit, or
 * from NULL, copies nothing (memcpy_s clears the destination) and fails.
 */
CRTFN(int) crt_memcpy_s(void *dst, size_t size, const void *src, size_t n)
{
	if (!n)
		return 0;
	if (!dst)
		return errno = EINVAL;
	if (!src || size < n) {
		memset(dst, 0, size);
		return errno = src ? ERANGE : EINVAL;
	}
	memcpy(dst, src, n);
	return 0;
}

CRTFN(int) crt_memmove_s(void *dst, size_t size, const void *src, size_t n)
{
	if (!n)
		return 0;
	if (!dst || !src)
		return errno = EINVAL;
	if (size < n)
		return errno = ERANGE;
	memmove(dst, src, n);
	return 0;
}

/* Sets each character (up to n of them) to c, and stops at the end. */
CRTFN(char *) crt__strnset(char *s, int c, size_t n)
{
	for (size_t i = 0; i < n && s[i]; i++)
		s[i] = (char)c;
	return s;
}

CRTFN(char *) crt__strset(char *s, int c)
{
	for (char *p = s; *p; p++)
		*p = (char)c;
	return s;
}

/*
 * One character of the ANSI code page to UTF-16: the bytes it took (a
 * DBCS lead byte takes two), 0 for the terminator, -1 for an invalid one.
 */
CRTFN(int) crt_mbtowc(WCHAR *dst, const char *s, size_t n)
{
	WCHAR w;

	if (!s || !n)
		return s ? -1 : 0;
	if (!*s) {
		if (dst)
			*dst = 0;
		return 0;
	}
	for (int len = 1; len <= 2 && (size_t)len <= n; len++)
		if (cp_to_utf16(CP_ACP, s, len, &w, 1, 1) == 1) {
			if (dst)
				*dst = w;
			return len;
		}
	errno = EILSEQ;
	return -1;
}

#define E(n) { #n + 0, 0, (void *)crt_##n }
#define M(name, fn) { name, 0, (void *)fn }

const struct builtin_export crt_modern_exports[] = {
	M("_time64", crt__time64),
	M("_resetstkoflw", crt__resetstkoflw),
	M("_i64toa", crt__i64toa),
	M("_ui64toa", crt__ui64toa),
	M("memcpy_s", crt_memcpy_s),
	M("memmove_s", crt_memmove_s),
	M("_strnset", crt__strnset),
	M("_strset", crt__strset),
	M("mbtowc", crt_mbtowc),
	M("_time32", crt__time32),
	M("_gmtime64", crt__gmtime64),
	M("_localtime64", crt__localtime64),
	M("_mktime64", crt__mktime64),
	M("_strtoi64", crt__strtoi64),
	M("_strtoui64", crt__strtoui64),
	M("_fstat64", crt__fstat64),
	M("_stat64", crt__stat64),
	M("_wstat64", crt__wstat64),
	M("_aligned_malloc", crt__aligned_malloc),
	M("_aligned_free", crt__aligned_free),
	M("_aligned_realloc", crt__aligned_realloc),
	M("___mb_cur_max_func", crt____mb_cur_max_func),
	M("___lc_codepage_func", crt____lc_codepage_func),
	BUILTIN_END
};
