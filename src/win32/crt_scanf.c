/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt: sscanf, and ucrtbase's __stdio_common_vsscanf, which its
 * headers make sscanf, sscanf_s and _snscanf from. Formats are read as
 * msvcrt reads them, which glibc's differs from in its sizes:
 *
 * - I64 (and ll) for 64-bit integers, I32 and I (32-bit here), h, hh, l;
 *   L on a floating conversion is a double, since MSVC's long double is.
 * - %s, %c and %[ store the function's kind of string (narrow here), %S
 *   and %C and the l and w sizes UTF-16, converted from the ANSI code page.
 * - ucrtbase's sscanf_s (the SECURECRT option) takes a buffer size, an
 *   unsigned, after each %s, %c and %[ pointer, and stops with an empty
 *   string stored if what it read wouldn't fit.
 *
 * Each conversion is done by the host's sscanf, with the width and size
 * it should have and a %n to see how far it went; the rest is done here.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/internal.h"

#define CRTFN(ret) static ret STUB_CDECL

/* ucrtbase's _CRT_INTERNAL_SCANF_* options. */
#define SCANF_SECURECRT 0x0001

enum size { SZ_NONE, SZ_HH, SZ_H, SZ_L, SZ_64, SZ_W };

static int is_space(char c)
{
	return c == ' ' || (c >= '\t' && c <= '\r');
}

/*
 * Reads one conversion, f being its host format (with %n last), into
 * target (NULL when suppressed): 1 if it matched, and *used how much of in
 * it took; 0 if it didn't, -1 if in ended first.
 */
static int host_scan(const char *in, const char *f, void *target, int *used)
{
	int n = -1, r;

	if (target)
		r = sscanf(in, f, target, &n);
	else
		r = sscanf(in, f, &n);
	if (n >= 0) {
		*used = n;
		return 1;
	}
	return r == EOF ? -1 : 0;
}

/* A narrow string read, stored as UTF-16 (with no terminator for %c). */
static int store_wide(WCHAR *dst, size_t room, const char *a, size_t len,
		      int terminate)
{
	int wn = len ? cp_to_utf16(CP_ACP, a, (int)len, NULL, 0, 0) : 0;

	if (wn < 0 || (size_t)wn + (terminate ? 1 : 0) > room)
		return -1;
	if (wn)
		cp_to_utf16(CP_ACP, a, (int)len, dst, wn, 0);
	if (terminate)
		dst[wn] = 0;
	return wn;
}

static int crt_vscan(unsigned options, const char *in, const char *fmt,
		     va_list ap)
{
	const char *start = in;
	int assigned = 0, converted = 0;

	if (!in || !fmt) {
		errno = EINVAL;
		return EOF;
	}
	while (*fmt) {
		if (is_space(*fmt)) {
			while (is_space(*fmt))
				fmt++;
			while (is_space(*in))
				in++;
			continue;
		}
		if (*fmt != '%' || fmt[1] == '%') {
			if (*fmt == '%') {
				fmt++;
				while (is_space(*in))
					in++;
			}
			if (!*in)
				return converted ? assigned : EOF;
			if (*in != *fmt)
				return assigned;
			in++;
			fmt++;
			continue;
		}
		fmt++;

		int suppress = 0, width = 0;
		enum size size = SZ_NONE;
		if (*fmt == '*') {
			suppress = 1;
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') {
			size = SZ_64;
			fmt += 3;
		} else if (fmt[0] == 'I' && fmt[1] == '3' && fmt[2] == '2') {
			fmt += 3;
		} else if (fmt[0] == 'I' || fmt[0] == 'z' || fmt[0] == 't') {
			fmt++;
		} else if (fmt[0] == 'l' && fmt[1] == 'l') {
			size = SZ_64;
			fmt += 2;
		} else if (fmt[0] == 'h' && fmt[1] == 'h') {
			size = SZ_HH;
			fmt += 2;
		} else if (fmt[0] == 'j') {
			size = SZ_64;
			fmt++;
		} else if (fmt[0] == 'h' || fmt[0] == 'l' || fmt[0] == 'w' ||
			   fmt[0] == 'L') {
			size = fmt[0] == 'h' ? SZ_H : fmt[0] == 'w' ? SZ_W : SZ_L;
			fmt++;
		}
		char conv = *fmt;
		if (!conv)
			break;
		fmt++;

		/* The scan set of a %[, as written. */
		const char *set = NULL;
		size_t set_len = 0;
		if (conv == '[') {
			set = fmt;
			if (*fmt == '^')
				fmt++;
			if (*fmt == ']')
				fmt++;
			while (*fmt && *fmt != ']')
				fmt++;
			if (!*fmt)
				return assigned;
			set_len = (size_t)(fmt - set);
			fmt++;
		}

		if (conv == 'n') {
			if (!suppress) {
				void *p = va_arg(ap, void *);
				long long pos = in - start;
				if (size == SZ_HH)
					*(signed char *)p = (signed char)pos;
				else if (size == SZ_H)
					*(short *)p = (short)pos;
				else if (size == SZ_64)
					*(long long *)p = pos;
				else
					*(int *)p = (int)pos;
			}
			continue;
		}

		char f[64 + 256];
		int used = 0, r;
		const char *at = in;

		if (conv != 'c' && conv != 'C' && conv != '[')
			while (is_space(*at))
				at++;
		if (!*at && conv != 'c' && conv != 'C')
			return converted ? assigned : EOF;

		switch (conv) {
		case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
		case 'p': {
			static const char *const sizes[] = {
				[SZ_NONE] = "", [SZ_HH] = "hh", [SZ_H] = "h",
				[SZ_L] = "l", [SZ_64] = "ll", [SZ_W] = "" };
			char c = conv == 'X' ? 'x' : conv, w[16] = "";
			if (width)
				snprintf(w, sizeof(w), "%d", width);
			snprintf(f, sizeof(f), "%%%s%s%s%c%%n", suppress ? "*" : "",
				 w, c == 'p' ? "" : sizes[size], c);
			r = host_scan(at, f, suppress ? NULL : va_arg(ap, void *),
				      &used);
			break;
		}
		case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
		case 'a': case 'A': {
			/* float, or double for l and for L as MSVC has it. */
			int dbl = size == SZ_L || size == SZ_64;
			char w[16] = "";
			if (width)
				snprintf(w, sizeof(w), "%d", width);
			snprintf(f, sizeof(f), "%%%s%s%sf%%n", suppress ? "*" : "",
				 w, dbl ? "l" : "");
			r = host_scan(at, f, suppress ? NULL : va_arg(ap, void *),
				      &used);
			break;
		}
		case 's': case 'S': case 'c': case 'C': case '[': {
			int is_c = conv == 'c' || conv == 'C';
			int wide = size == SZ_L || size == SZ_W ||
				   (size != SZ_H && (conv == 'S' || conv == 'C'));
			void *dst = suppress ? NULL : va_arg(ap, void *);
			size_t room = SIZE_MAX;
			if (!suppress && (options & SCANF_SECURECRT))
				room = va_arg(ap, unsigned);
			if (is_c && !width)
				width = 1;
			size_t cap = width ? (size_t)width : strlen(at);
			char *tmp = malloc(cap + 1);
			if (!tmp)
				return assigned;
			if (conv == '[' && set_len > 250) {
				free(tmp);
				return assigned;
			}
			if (conv == '[')
				snprintf(f, sizeof(f), "%%%zu[%.*s]%%n", cap,
					 (int)set_len, set);
			else
				snprintf(f, sizeof(f), "%%%zu%c%%n", cap,
					 is_c ? 'c' : 's');
			r = host_scan(at, f, tmp, &used);
			if (r == 1 && dst) {
				size_t len = is_c ? (size_t)used : strlen(tmp);
				int ok;
				if (wide)
					ok = store_wide(dst, room, tmp, len, !is_c) >= 0;
				else if ((ok = len + !is_c <= room)) {
					memcpy(dst, tmp, len);
					if (!is_c)
						((char *)dst)[len] = 0;
				}
				if (!ok) {
					/* sscanf_s: too long for its buffer. */
					if (room) {
						if (wide)
							*(WCHAR *)dst = 0;
						else
							*(char *)dst = 0;
					}
					free(tmp);
					return assigned;
				}
			}
			free(tmp);
			break;
		}
		default:
			return assigned;
		}
		if (r < 0)
			return converted ? assigned : EOF;
		if (r == 0)
			return assigned;
		converted = 1;
		if (!suppress)
			assigned++;
		in = at + used;
	}
	return assigned;
}

CRTFN(int) crt_sscanf(const char *s, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = crt_vscan(0, s, fmt, ap);
	va_end(ap);
	return r;
}

CRTFN(int) crt_vsscanf(const char *s, const char *fmt, va_list ap)
{
	return crt_vscan(0, s, fmt, ap);
}

/* length (size_t)-1: up to the terminator; otherwise that many bytes. */
CRTFN(int) crt___stdio_common_vsscanf(ULONGLONG options, const char *s,
				       size_t length, const char *fmt,
				       void *locale, va_list ap)
{
	(void)locale;
	if (!s || length == (size_t)-1)
		return crt_vscan((unsigned)options, s, fmt, ap);
	char *copy = strndup(s, length);
	if (!copy)
		return EOF;
	int r = crt_vscan((unsigned)options, copy, fmt, ap);
	free(copy);
	return r;
}

#define E(name) { #name, 0, (void *)crt_##name }

const struct builtin_export crt_scanf_exports[] = {
	E(sscanf), E(vsscanf), E(__stdio_common_vsscanf),
	BUILTIN_END
};
