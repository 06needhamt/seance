/*
 * msvcrt: <string.h>, <stdlib.h>'s string/number conversions, and the
 * handful of MSVC-only spellings (a leading underscore, or "i" for a
 * case-insensitive compare) codecs statically or dynamically linked
 * against it use. Wide-character names use our WCHAR (always UTF-16),
 * not the host's wchar_t.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "runtime/builtin.h"
#include "win32/crt.h"
#include "win32/internal.h"

#define CRTFN(ret) static ret STUB_CDECL

/* <string.h>, straight to glibc: same signature, same semantics. */
CRTFN(void *) crt_memcpy(void *dst, const void *src, size_t n) { return memcpy(dst, src, n); }
CRTFN(void *) crt_memmove(void *dst, const void *src, size_t n) { return memmove(dst, src, n); }
CRTFN(void *) crt_memset(void *dst, int c, size_t n) { return memset(dst, c, n); }
CRTFN(int) crt_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
CRTFN(void *) crt_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }

CRTFN(size_t) crt_strlen(const char *s) { return strlen(s); }
CRTFN(char *) crt_strcpy(char *d, const char *s) { return strcpy(d, s); }
CRTFN(char *) crt_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
CRTFN(char *) crt_strcat(char *d, const char *s) { return strcat(d, s); }
CRTFN(char *) crt_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
CRTFN(int) crt_strcmp(const char *a, const char *b) { return strcmp(a, b); }
CRTFN(int) crt_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
CRTFN(char *) crt_strchr(const char *s, int c) { return strchr(s, c); }
CRTFN(char *) crt_strrchr(const char *s, int c) { return strrchr(s, c); }
CRTFN(char *) crt_strstr(const char *h, const char *n) { return strstr(h, n); }
CRTFN(char *) crt_strpbrk(const char *s, const char *accept) { return strpbrk(s, accept); }
CRTFN(size_t) crt_strspn(const char *s, const char *accept) { return strspn(s, accept); }
CRTFN(size_t) crt_strcspn(const char *s, const char *reject) { return strcspn(s, reject); }
CRTFN(char *) crt_strtok(char *s, const char *delim) { return strtok(s, delim); }
CRTFN(char *) crt_strerror(int errnum) { return strerror(errnum); }

/* MSVC-only names for the same thing (case-insensitive compares, and
 * _s-free/underscore spellings some import libraries use). */
CRTFN(int) crt__stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
CRTFN(int) crt__strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
CRTFN(char *) crt__strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	if (p)
		memcpy(p, s, n);
	return p;
}
CRTFN(char *) crt__strrev(char *s)
{
	size_t n = strlen(s);
	for (size_t i = 0; i < n / 2; i++) {
		char t = s[i];
		s[i] = s[n - 1 - i];
		s[n - 1 - i] = t;
	}
	return s;
}
CRTFN(char *) crt__strlwr(char *s) { for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p); return s; }
CRTFN(char *) crt__strupr(char *s) { for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p); return s; }

/* Wide strings: WCHAR is always UTF-16 here, unlike the host's wchar_t
 * (32-bit on Linux), so these are hand-rolled rather than wcs*-forwarded. */
CRTFN(size_t) crt_wcslen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
CRTFN(WCHAR *) crt_wcscpy(WCHAR *d, const WCHAR *s)
{
	WCHAR *r = d;
	while ((*d++ = *s++))
		;
	return r;
}
CRTFN(WCHAR *) crt_wcscat(WCHAR *d, const WCHAR *s)
{
	WCHAR *r = d;
	while (*d)
		d++;
	while ((*d++ = *s++))
		;
	return r;
}
CRTFN(int) crt_wcscmp(const WCHAR *a, const WCHAR *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (int)*a - (int)*b;
}

/* Number/string conversions. atoi/atol/atof and strtol/strtoul/strtod are
 * shared with the host's exactly; MSVC's own itoa/ltoa/ultoa family has no
 * glibc equivalent, so they go through snprintf. */
CRTFN(int) crt_atoi(const char *s) { return atoi(s); }
CRTFN(long) crt_atol(const char *s) { return atol(s); }
CRTFN(double) crt_atof(const char *s) { return atof(s); }
CRTFN(long) crt_strtol(const char *s, char **end, int base) { return strtol(s, end, base); }
CRTFN(unsigned long) crt_strtoul(const char *s, char **end, int base) { return strtoul(s, end, base); }
CRTFN(double) crt_strtod(const char *s, char **end) { return strtod(s, end); }

static char *itoa_base(long v, char *buf, int radix, int is_unsigned)
{
	if (radix == 10 && is_unsigned)
		snprintf(buf, 34, "%lu", (unsigned long)v);
	else if (radix == 10)
		snprintf(buf, 34, "%ld", v);
	else if (radix == 16)
		snprintf(buf, 34, "%lx", (unsigned long)v);
	else if (radix == 8)
		snprintf(buf, 34, "%lo", (unsigned long)v);
	else {
		/* An uncommon base: build it up a digit at a time. */
		unsigned long u = is_unsigned || v >= 0 ? (unsigned long)v : (unsigned long)-v;
		char tmp[34];
		int i = 0;
		if (!u)
			tmp[i++] = '0';
		while (u) {
			int d = (int)(u % (unsigned)radix);
			tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
			u /= (unsigned)radix;
		}
		int j = 0;
		if (!is_unsigned && v < 0)
			buf[j++] = '-';
		while (i)
			buf[j++] = tmp[--i];
		buf[j] = 0;
	}
	return buf;
}

CRTFN(char *) crt__itoa(int v, char *buf, int radix) { return itoa_base(v, buf, radix, 0); }
CRTFN(char *) crt__ltoa(long v, char *buf, int radix) { return itoa_base(v, buf, radix, 0); }
CRTFN(char *) crt__ultoa(unsigned long v, char *buf, int radix)
{
	return itoa_base((long)v, buf, radix, 1);
}

/*
 * rand/srand: msvcrt's own linear congruential generator, so a seeded
 * sequence matches Windows'. Its state is per thread and starts at 1.
 */
static _Thread_local unsigned int rand_state = 1;
CRTFN(void) crt_srand(unsigned int seed) { rand_state = seed; }
CRTFN(int) crt_rand(void)
{
	rand_state = rand_state * 214013u + 2531011u;
	return (rand_state >> 16) & 0x7fff;
}

CRTFN(void) crt_qsort(void *base, size_t n, size_t size,
		      int (STUB_CDECL *cmp)(const void *, const void *))
{
	/*
	 * glibc's comparator is plain cdecl too (the C standard's qsort has
	 * no calling-convention distinction), so the codec's stdcall-free
	 * cdecl comparator (STUB_CDECL matches what mingw-built code emits
	 * for a plain int(*)(const void*,const void*)) can be handed to it
	 * directly.
	 */
	qsort(base, n, size, (int (*)(const void *, const void *))cmp);
}

CRTFN(void *) crt_bsearch(const void *key, const void *base, size_t n, size_t size,
			  int (STUB_CDECL *cmp)(const void *, const void *))
{
	return bsearch(key, base, n, size, (int (*)(const void *, const void *))cmp);
}

CRTFN(int) crt_abs(int v) { return abs(v); }
CRTFN(long) crt_labs(long v) { return labs(v); }

/* The CRT heap: its own malloc/free/calloc/realloc, independent of
 * kernel32's HeapAlloc/GlobalAlloc family - a codec may use either. On
 * failure each asks the new handler under _set_new_mode(1). */
CRTFN(void *) crt_malloc(size_t n)
{
	void *p;
	while (!(p = malloc(n)) && crt_malloc_retry(n))
		;
	return p;
}

CRTFN(void *) crt_calloc(size_t n, size_t size)
{
	void *p;
	if (size && n > SIZE_MAX / size)
		return NULL;	/* overflow: no handler would help */
	while (!(p = calloc(n, size)) && crt_malloc_retry(n * size))
		;
	return p;
}

CRTFN(void *) crt_realloc(void *p, size_t n)
{
	void *q;
	/* Size 0 frees p and has nothing to retry. */
	while (!(q = realloc(p, n)) && n && crt_malloc_retry(n))
		;
	return q;
}
CRTFN(void) crt_free(void *p) { free(p); }

#define E(name) { #name, 0, (void *)crt_##name }

const struct builtin_export crt_string_exports[] = {
	E(memcpy), E(memmove), E(memset), E(memcmp), E(memchr),
	E(strlen), E(strcpy), E(strncpy), E(strcat), E(strncat),
	E(strcmp), E(strncmp), E(strchr), E(strrchr), E(strstr),
	E(strpbrk), E(strspn), E(strcspn), E(strtok), E(strerror),
	E(_stricmp), E(_strnicmp), E(_strdup), E(_strrev), E(_strlwr), E(_strupr),
	E(wcslen), E(wcscpy), E(wcscat), E(wcscmp),
	E(atoi), E(atol), E(atof), E(strtol), E(strtoul), E(strtod),
	E(_itoa), E(_ltoa), E(_ultoa),
	E(srand), E(rand), E(qsort), E(bsearch), E(abs), E(labs),
	E(malloc), E(calloc), E(realloc), E(free),
	BUILTIN_END
};
