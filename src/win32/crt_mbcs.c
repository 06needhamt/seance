/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The msvcrt multibyte-string functions (_mbs*, _mbc*, _ismbc*) and the
 * other entries mfc42.dll imports: _fullpath, _expand, mktime, __doserrno,
 * __p___argc and __p___argv.
 *
 * The ANSI code page here is single-byte (IsDBCSLeadByte says no lead
 * bytes), so a multibyte string is a byte string and every character is
 * one byte: these are the str* functions under their _mbs names, with case
 * mapping for ASCII letters, as in the "C" locale.
 */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"

#define CRTFN(ret) static ret STUB_CDECL

typedef unsigned char uchar;

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int low(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

CRTFN(uchar *) crt__mbschr(const uchar *s, unsigned c) { return (uchar *)strchr((const char *)s, (int)c); }
CRTFN(uchar *) crt__mbsrchr(const uchar *s, unsigned c) { return (uchar *)strrchr((const char *)s, (int)c); }
CRTFN(uchar *) crt__mbsstr(const uchar *s, const uchar *t) { return (uchar *)strstr((const char *)s, (const char *)t); }
CRTFN(uchar *) crt__mbspbrk(const uchar *s, const uchar *t) { return (uchar *)strpbrk((const char *)s, (const char *)t); }
CRTFN(size_t) crt__mbsspn(const uchar *s, const uchar *t) { return strspn((const char *)s, (const char *)t); }
CRTFN(size_t) crt__mbscspn(const uchar *s, const uchar *t) { return strcspn((const char *)s, (const char *)t); }
CRTFN(int) crt__mbscmp(const uchar *a, const uchar *b) { return strcmp((const char *)a, (const char *)b); }
CRTFN(size_t) crt__mbslen(const uchar *s) { return strlen((const char *)s); }
CRTFN(uchar *) crt__mbscpy(uchar *d, const uchar *s) { return (uchar *)strcpy((char *)d, (const char *)s); }
CRTFN(uchar *) crt__mbscat(uchar *d, const uchar *s) { return (uchar *)strcat((char *)d, (const char *)s); }

static int cmp_n(const uchar *a, const uchar *b, size_t n, int fold)
{
	for (size_t i = 0; i < n; i++) {
		int x = fold ? low(a[i]) : a[i], y = fold ? low(b[i]) : b[i];
		if (x != y)
			return x < y ? -1 : 1;
		if (!x)
			break;
	}
	return 0;
}

CRTFN(int) crt__mbsicmp(const uchar *a, const uchar *b) { return cmp_n(a, b, (size_t)-1, 1); }
CRTFN(int) crt__mbsncmp(const uchar *a, const uchar *b, size_t n) { return cmp_n(a, b, n, 0); }
CRTFN(int) crt__mbsnicmp(const uchar *a, const uchar *b, size_t n) { return cmp_n(a, b, n, 1); }
CRTFN(int) crt__mbsnbcmp(const uchar *a, const uchar *b, size_t n) { return cmp_n(a, b, n, 0); }
CRTFN(int) crt__mbsnbicmp(const uchar *a, const uchar *b, size_t n) { return cmp_n(a, b, n, 1); }

CRTFN(uchar *) crt__mbslwr(uchar *s)
{
	for (uchar *p = s; p && *p; p++)
		*p = (uchar)low(*p);
	return s;
}

CRTFN(uchar *) crt__mbsupr(uchar *s)
{
	for (uchar *p = s; p && *p; p++)
		*p = (uchar)up(*p);
	return s;
}

CRTFN(uchar *) crt__mbsrev(uchar *s)
{
	size_t n = s ? strlen((char *)s) : 0;

	for (size_t i = 0; i < n / 2; i++) {
		uchar t = s[i];
		s[i] = s[n - 1 - i];
		s[n - 1 - i] = t;
	}
	return s;
}

/* strncpy's contract: n bytes, padded with NULs, not terminated if full. */
CRTFN(uchar *) crt__mbsnbcpy(uchar *d, const uchar *s, size_t n) { return (uchar *)strncpy((char *)d, (const char *)s, n); }
CRTFN(uchar *) crt__mbsncpy(uchar *d, const uchar *s, size_t n) { return (uchar *)strncpy((char *)d, (const char *)s, n); }
CRTFN(uchar *) crt__mbsnbcat(uchar *d, const uchar *s, size_t n) { return (uchar *)strncat((char *)d, (const char *)s, n); }
CRTFN(uchar *) crt__mbsncat(uchar *d, const uchar *s, size_t n) { return (uchar *)strncat((char *)d, (const char *)s, n); }
CRTFN(size_t) crt__mbsnbcnt(const uchar *s, size_t n) { size_t l = strlen((const char *)s); return l < n ? l : n; }
CRTFN(size_t) crt__mbsnccnt(const uchar *s, size_t n) { size_t l = strlen((const char *)s); return l < n ? l : n; }

/* The first character of s not in set, or NULL if there is none. */
CRTFN(uchar *) crt__mbsspnp(const uchar *s, const uchar *set)
{
	s += strspn((const char *)s, (const char *)set);
	return *s ? (uchar *)s : NULL;
}

CRTFN(uchar *) crt__mbstok(uchar *s, const uchar *delim)
{
	static __thread char *save;
	return (uchar *)strtok_r((char *)s, (const char *)delim, &save);
}

CRTFN(uchar *) crt__mbsinc(const uchar *s) { return (uchar *)(s + 1); }

CRTFN(uchar *) crt__mbsdec(const uchar *start, const uchar *cur)
{
	return start && cur && cur > start ? (uchar *)(cur - 1) : NULL;
}

CRTFN(size_t) crt__mbclen(const uchar *c) { return 1; }
CRTFN(int) crt__ismbblead(unsigned c) { return 0; }
CRTFN(int) crt__ismbbtrail(unsigned c) { return 0; }
CRTFN(int) crt__ismbcspace(unsigned c) { return c < 0x80 && isspace((int)c); }
CRTFN(int) crt__ismbcdigit(unsigned c) { return c >= '0' && c <= '9'; }
CRTFN(int) crt__ismbcalpha(unsigned c) { return c < 0x80 && isalpha((int)c); }
CRTFN(unsigned) crt__mbctoupper(unsigned c) { return (unsigned)up((int)c); }
CRTFN(unsigned) crt__mbctolower(unsigned c) { return (unsigned)low((int)c); }

/* _mbctype[c + 1]: no lead or trail bytes; _SBUP and _SBLOW for letters. */
static uchar mbctype[257];

__attribute__((constructor)) static void mbctype_init(void)
{
	for (int c = 'A'; c <= 'Z'; c++) {
		mbctype[c + 1] = 0x10;
		mbctype[c + 32 + 1] = 0x20;
	}
}

static int argc = 1;
static char exe_name[] = "seance.exe";
static char *argv_storage[] = { exe_name, NULL };
static char **argv = argv_storage;

CRTFN(int *) crt___p___argc(void) { return &argc; }
CRTFN(char ***) crt___p___argv(void) { return &argv; }

static __thread unsigned long doserrno;

CRTFN(unsigned long *) crt___doserrno(void) { return &doserrno; }

/* Grows or shrinks in place only: the block never moves. */
CRTFN(void *) crt__expand(void *p, size_t size)
{
	if (!p)
		return NULL;
	return size <= win_usable(p) ? p : NULL;
}

CRTFN(char *) crt__fullpath(char *abs, const char *rel, size_t max)
{
	char *u = rel && *rel ? acp_to_utf8(rel) : strdup(".");
	char *full = u ? k32_full_path(u) : NULL;
	char *a = full ? utf8_to_acp_dup(full) : NULL;

	free(u);
	free(full);
	if (!a) {
		errno = ENOMEM;
		return NULL;
	}
	size_t need = strlen(a) + 1;
	if (!abs) {
		if (max < need)
			max = need;
		if (!(abs = malloc(max))) {
			free(a);
			errno = ENOMEM;
			return NULL;
		}
	} else if (max < need) {
		free(a);
		errno = ERANGE;
		return NULL;
	}
	memcpy(abs, a, need);
	free(a);
	return abs;
}

/* struct tm is nine ints, as glibc's starts; local time is UTC here. The
 * fields are normalised in place, as mktime does. 32-bit time_t on i386. */
CRTFN(LONG) crt_mktime(int *f)
{
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
	time_t t = timegm(&tm);
	if (t == (time_t)-1 || t < 0 || t > 0x7fffffff)
		return -1;
	f[0] = tm.tm_sec;
	f[1] = tm.tm_min;
	f[2] = tm.tm_hour;
	f[3] = tm.tm_mday;
	f[4] = tm.tm_mon;
	f[5] = tm.tm_year;
	f[6] = tm.tm_wday;
	f[7] = tm.tm_yday;
	f[8] = 0;
	return (LONG)t;
}

#define E(name) { #name, 0, (void *)crt_##name }

const struct builtin_export crt_mbcs_exports[] = {
	E(__doserrno),
	E(__p___argc),
	E(__p___argv),
	E(_expand),
	E(_fullpath),
	E(_ismbblead),
	E(_ismbbtrail),
	E(_ismbcalpha),
	E(_ismbcdigit),
	E(_ismbcspace),
	E(_mbclen),
	E(_mbctolower),
	E(_mbctoupper),
	{ "_mbctype", 0, (void *)mbctype },
	E(_mbscat),
	E(_mbschr),
	E(_mbscmp),
	E(_mbscpy),
	E(_mbscspn),
	E(_mbsdec),
	E(_mbsicmp),
	E(_mbsinc),
	E(_mbslen),
	E(_mbslwr),
	E(_mbsnbcmp),
	E(_mbsnbicmp),
	E(_mbsnbcat),
	E(_mbsnbcnt),
	E(_mbsnbcpy),
	E(_mbsncat),
	E(_mbsnccnt),
	E(_mbsncmp),
	E(_mbsncpy),
	E(_mbsnicmp),
	E(_mbspbrk),
	E(_mbsrchr),
	E(_mbsrev),
	E(_mbsspn),
	E(_mbsspnp),
	E(_mbsstr),
	E(_mbstok),
	E(_mbsupr),
	E(mktime),
	BUILTIN_END
};
