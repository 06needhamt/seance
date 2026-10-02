/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * user32: strings - LoadString, the wsprintf family, and the Char*
 * functions (case mapping and classification through kernel32's Latin-1
 * tables, and ANSI <-> OEM conversion).
 *
 * wsprintf has its own formatter because Windows' differs from the C
 * library's: %s is the caller's string width and %S the other one (glibc
 * would take %S and %ls as 32-bit wchar_t), there are no floating-point
 * conversions, and the output stops at 1024 characters.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "loader/pe.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"

#define WSPRINTF_MAX 1024

/* LoadString. */

/*
 * The string resource id in module inst: a pointer to its UTF-16 text
 * (not NUL-terminated) and its length, or NULL.
 */
static const WCHAR *find_string(HINSTANCE inst, UINT id, int *len)
{
	struct module *m = inst ? mod_from_base(inst) : mod_main();
	struct pe_resource res;

	if (!m || !m->img ||
	    pe_find_resource(m->img, RT_STRING, (id >> 4) + 1, PE_RES_ANY_LANG,
			     &res)) {
		win_set_error(ERROR_RESOURCE_NAME_NOT_FOUND);
		return NULL;
	}
	/* A block of 16 counted strings. */
	const WCHAR *p = pe_ptr(m->img, res.data_rva, res.size);
	const WCHAR *end = p ? p + res.size / 2 : NULL;
	for (UINT i = 0; p && p < end; i++) {
		int n = *p++;
		if (p + n > end)
			break;
		if (i == (id & 15)) {
			if (!n)
				break;
			*len = n;
			return p;
		}
		p += n;
	}
	win_set_error(ERROR_RESOURCE_NAME_NOT_FOUND);
	return NULL;
}

/* A size of 0 returns a read-only pointer to the resource itself. */
WINFN(int) user_LoadStringW(HINSTANCE inst, UINT id, LPWSTR buf, int size)
{
	int n = 0;
	const WCHAR *s = find_string(inst, id, &n);

	if (!buf)
		return 0;
	if (!size) {
		*(const WCHAR **)buf = s;
		return s ? n : 0;
	}
	if (!s) {
		buf[0] = 0;
		return 0;
	}
	if (n > size - 1)
		n = size - 1;
	memcpy(buf, s, n * sizeof(WCHAR));
	buf[n] = 0;
	return n;
}

WINFN(int) user_LoadStringA(HINSTANCE inst, UINT id, LPSTR buf, int size)
{
	int n = 0;
	const WCHAR *s = find_string(inst, id, &n);

	if (!buf || size <= 0)
		return 0;
	if (!s) {
		buf[0] = '\0';
		return 0;
	}
	int an = cp_from_utf16(CP_ACP, s, n, NULL, 0, '?', NULL);
	char *a = malloc(an > 0 ? an : 1);
	if (!a) {
		buf[0] = '\0';
		return 0;
	}
	cp_from_utf16(CP_ACP, s, n, a, an, '?', NULL);
	if (an > size - 1)
		an = size - 1;
	memcpy(buf, a, an);
	buf[an] = '\0';
	free(a);
	return an;
}

/* wsprintf. */

struct out {
	WCHAR buf[WSPRINTF_MAX];
	int n;
};

static void put(struct out *o, WCHAR c)
{
	if (o->n < WSPRINTF_MAX - 1)
		o->buf[o->n++] = c;
}

static void pad(struct out *o, int n, WCHAR c)
{
	while (n-- > 0)
		put(o, c);
}

/* Emits a string of len characters with the field's width and justification. */
static void field(struct out *o, const WCHAR *s, int len, int width, int left,
		  WCHAR padc)
{
	if (!left)
		pad(o, width - len, padc);
	for (int i = 0; i < len; i++)
		put(o, s[i]);
	if (left)
		pad(o, width - len, ' ');
}

/* A string argument as UTF-16 (malloc()ed), from ANSI or not. */
static WCHAR *string_arg(const void *s, int wide, int max)
{
	static const char null_a[] = "(null)";
	WCHAR *w;
	int n;

	if (!s) {
		s = null_a;
		wide = 0;
	}
	if (wide) {
		const WCHAR *ws = s;
		for (n = 0; ws[n] && (max < 0 || n < max); n++)
			;
		if ((w = malloc((n + 1) * sizeof(WCHAR)))) {
			memcpy(w, ws, n * sizeof(WCHAR));
			w[n] = 0;
		}
		return w;
	}
	const char *as = s;
	for (n = 0; as[n] && (max < 0 || n < max); n++)
		;
	int wn = cp_to_utf16(CP_ACP, as, n, NULL, 0, 0);
	if ((w = malloc((wn + 1) * sizeof(WCHAR)))) {
		cp_to_utf16(CP_ACP, as, n, w, wn, 0);
		w[wn] = 0;
	}
	return w;
}

/* Formats into o; fmt is ANSI or UTF-16 as wide says. */
static void format(struct out *o, const void *fmt, int wide, va_list ap)
{
	const BYTE *fa = fmt;
	const WCHAR *fw = fmt;
	int i = 0;
#define CH(k) (wide ? fw[k] : (WCHAR)fa[k])

	o->n = 0;
	while (CH(i)) {
		WCHAR c = CH(i++);
		if (c != '%') {
			if (wide) {
				put(o, c);
			} else {
				/* One ANSI byte at a time through the code page. */
				WCHAR w;
				char b = (char)c;
				if (cp_to_utf16(CP_ACP, &b, 1, &w, 1, 0) == 1)
					put(o, w);
			}
			continue;
		}
		int left = 0, zero = 0, alt = 0, width = 0, prec = -1, size = 0;
		for (;; i++) {
			if (CH(i) == '-')
				left = 1;
			else if (CH(i) == '0')
				zero = 1;
			else if (CH(i) == '#')
				alt = 1;
			else
				break;
		}
		while (CH(i) >= '0' && CH(i) <= '9')
			width = width * 10 + CH(i++) - '0';
		if (CH(i) == '.') {
			prec = 0;
			i++;
			while (CH(i) >= '0' && CH(i) <= '9')
				prec = prec * 10 + CH(i++) - '0';
		}
		if (CH(i) == 'h' || CH(i) == 'l' || CH(i) == 'w') {
			size = CH(i) == 'h' ? 'h' : 'l';
			i++;
		}
		WCHAR type = CH(i);
		if (!type)
			break;
		i++;

		WCHAR num[40];
		int n = 0;
		switch (type) {
		case 'c':
		case 'C': {
			int char_wide = size ? size == 'l' :
				       (type == 'c') == !!wide;
			WCHAR ch = (WCHAR)va_arg(ap, int);
			if (!char_wide) {
				char b = (char)ch;
				cp_to_utf16(CP_ACP, &b, 1, &ch, 1, 0);
			}
			field(o, &ch, 1, width, left, ' ');
			break;
		}
		case 's':
		case 'S': {
			int str_wide = size ? size == 'l' :
				      (type == 's') == !!wide;
			WCHAR *s = string_arg(va_arg(ap, const void *), str_wide, prec);
			if (s)
				field(o, s, wstrlen(s), width, left, ' ');
			free(s);
			break;
		}
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X':
		case 'p': {
			unsigned long v = va_arg(ap, unsigned long);
			int neg = (type == 'd' || type == 'i') && (long)v < 0;
			unsigned long u = neg ? -(unsigned long)v : v;
			int base = (type == 'x' || type == 'X' || type == 'p') ? 16 : 10;
			const char *digits = type == 'x' ? "0123456789abcdef" :
							   "0123456789ABCDEF";
			WCHAR rev[32];
			int nd = 0;
			do {
				rev[nd++] = digits[u % base];
				u /= base;
			} while (u);
			if (type == 'p')
				prec = 8;
			while (nd < prec && nd < 32)
				rev[nd++] = '0';
			if (neg)
				num[n++] = '-';
			if (alt && type == 'x' && v)
				num[n++] = '0', num[n++] = 'x';
			else if (alt && type == 'X' && v)
				num[n++] = '0', num[n++] = 'X';
			/* Zero padding goes after the sign or prefix. */
			if (zero && !left && prec < 0)
				while (n + nd < width && n + nd < 39)
					num[n++] = '0';
			while (nd)
				num[n++] = rev[--nd];
			field(o, num, n, width, left, ' ');
			break;
		}
		default:
			/* Unknown conversions are copied through, as wsprintf does. */
			put(o, type);
			break;
		}
	}
#undef CH
	o->buf[o->n] = 0;
}

static int out_a(const struct out *o, LPSTR buf)
{
	int n = cp_from_utf16(CP_ACP, o->buf, o->n, NULL, 0, '?', NULL);

	if (n > WSPRINTF_MAX - 1)
		n = WSPRINTF_MAX - 1;
	char *a = malloc(n + 1);
	if (!a) {
		buf[0] = '\0';
		return 0;
	}
	cp_from_utf16(CP_ACP, o->buf, o->n, a, n, '?', NULL);
	memcpy(buf, a, n);
	buf[n] = '\0';
	free(a);
	return n;
}

static int out_w(const struct out *o, LPWSTR buf)
{
	memcpy(buf, o->buf, (o->n + 1) * sizeof(WCHAR));
	return o->n;
}

WINFN(int) user_wvsprintfA(LPSTR buf, LPCSTR fmt, va_list ap)
{
	struct out o;

	if (!buf || !fmt)
		return 0;
	format(&o, fmt, 0, ap);
	return out_a(&o, buf);
}

WINFN(int) user_wvsprintfW(LPWSTR buf, LPCWSTR fmt, va_list ap)
{
	struct out o;

	if (!buf || !fmt)
		return 0;
	format(&o, fmt, 1, ap);
	return out_w(&o, buf);
}

static int STUB_CDECL user_wsprintfA(LPSTR buf, LPCSTR fmt, ...)
{
	struct out o;
	va_list ap;

	if (!buf || !fmt)
		return 0;
	va_start(ap, fmt);
	format(&o, fmt, 0, ap);
	va_end(ap);
	return out_a(&o, buf);
}

static int STUB_CDECL user_wsprintfW(LPWSTR buf, LPCWSTR fmt, ...)
{
	struct out o;
	va_list ap;

	if (!buf || !fmt)
		return 0;
	va_start(ap, fmt);
	format(&o, fmt, 1, ap);
	va_end(ap);
	return out_w(&o, buf);
}

/* Char*: ANSI characters go through the code page to UTF-16 and back. */

static WCHAR a_to_w(char c)
{
	WCHAR w = 0;
	cp_to_utf16(CP_ACP, &c, 1, &w, 1, 0);
	return w;
}

static char w_to_a(WCHAR w, char fallback)
{
	char c;
	int used = 0;
	if (cp_from_utf16(CP_ACP, &w, 1, &c, 1, 0, &used) != 1 || used)
		return fallback;
	return c;
}

static char map_a(char c, int upper)
{
	WCHAR w = a_to_w(c);
	return w_to_a(upper ? k32_toupper_w(w) : k32_tolower_w(w), c);
}

/* A pointer whose high word is 0 is a single character. */
static LPSTR case_a(LPSTR s, int upper)
{
	if (!((ULONG_PTR)s >> 16))
		return (LPSTR)(ULONG_PTR)(BYTE)map_a((char)(ULONG_PTR)s, upper);
	for (char *p = s; *p; p++)
		*p = map_a(*p, upper);
	return s;
}

static LPWSTR case_w(LPWSTR s, int upper)
{
	if (!((ULONG_PTR)s >> 16)) {
		WCHAR c = (WCHAR)(ULONG_PTR)s;
		return (LPWSTR)(ULONG_PTR)(upper ? k32_toupper_w(c) : k32_tolower_w(c));
	}
	for (WCHAR *p = s; *p; p++)
		*p = upper ? k32_toupper_w(*p) : k32_tolower_w(*p);
	return s;
}

WINFN(LPSTR) user_CharUpperA(LPSTR s) { return case_a(s, 1); }
WINFN(LPSTR) user_CharLowerA(LPSTR s) { return case_a(s, 0); }
WINFN(LPWSTR) user_CharUpperW(LPWSTR s) { return case_w(s, 1); }
WINFN(LPWSTR) user_CharLowerW(LPWSTR s) { return case_w(s, 0); }

WINFN(DWORD) user_CharUpperBuffA(LPSTR s, DWORD n)
{
	for (DWORD i = 0; s && i < n; i++)
		s[i] = map_a(s[i], 1);
	return s ? n : 0;
}

WINFN(DWORD) user_CharLowerBuffA(LPSTR s, DWORD n)
{
	for (DWORD i = 0; s && i < n; i++)
		s[i] = map_a(s[i], 0);
	return s ? n : 0;
}

WINFN(DWORD) user_CharUpperBuffW(LPWSTR s, DWORD n)
{
	for (DWORD i = 0; s && i < n; i++)
		s[i] = k32_toupper_w(s[i]);
	return s ? n : 0;
}

WINFN(DWORD) user_CharLowerBuffW(LPWSTR s, DWORD n)
{
	for (DWORD i = 0; s && i < n; i++)
		s[i] = k32_tolower_w(s[i]);
	return s ? n : 0;
}

/* The ANSI code page is single-byte, so a character is a byte. */
WINFN(LPSTR) user_CharNextA(LPCSTR s) { return (LPSTR)(s && *s ? s + 1 : s); }
WINFN(LPWSTR) user_CharNextW(LPCWSTR s) { return (LPWSTR)(s && *s ? s + 1 : s); }
WINFN(LPSTR) user_CharPrevA(LPCSTR start, LPCSTR s) { return (LPSTR)(s > start ? s - 1 : start); }
WINFN(LPWSTR) user_CharPrevW(LPCWSTR start, LPCWSTR s) { return (LPWSTR)(s > start ? s - 1 : start); }

WINFN(BOOL) user_IsCharAlphaW(WCHAR c) { return (k32_ctype1(c) & C1_ALPHA) != 0; }
WINFN(BOOL) user_IsCharAlphaNumericW(WCHAR c) { return (k32_ctype1(c) & (C1_ALPHA | C1_DIGIT)) != 0; }
WINFN(BOOL) user_IsCharUpperW(WCHAR c) { return (k32_ctype1(c) & C1_UPPER) != 0; }
WINFN(BOOL) user_IsCharLowerW(WCHAR c) { return (k32_ctype1(c) & C1_LOWER) != 0; }
WINFN(BOOL) user_IsCharAlphaA(CHAR c) { return (k32_ctype1(a_to_w(c)) & C1_ALPHA) != 0; }
WINFN(BOOL) user_IsCharAlphaNumericA(CHAR c) { return (k32_ctype1(a_to_w(c)) & (C1_ALPHA | C1_DIGIT)) != 0; }
WINFN(BOOL) user_IsCharUpperA(CHAR c) { return (k32_ctype1(a_to_w(c)) & C1_UPPER) != 0; }
WINFN(BOOL) user_IsCharLowerA(CHAR c) { return (k32_ctype1(a_to_w(c)) & C1_LOWER) != 0; }

/* ANSI <-> OEM, n bytes (in place allowed). */
static BOOL recode(const char *src, char *dst, DWORD n, UINT from, UINT to)
{
	if (!src || !dst)
		return FALSE;
	for (DWORD i = 0; i < n; i++) {
		WCHAR w;
		char c = src[i];
		if (cp_to_utf16(from, &c, 1, &w, 1, 0) != 1 ||
		    cp_from_utf16(to, &w, 1, &dst[i], 1, '?', NULL) != 1)
			dst[i] = '?';
	}
	return TRUE;
}

WINFN(BOOL) user_CharToOemBuffA(LPCSTR src, LPSTR dst, DWORD n)
{
	return recode(src, dst, n, CP_ACP, CP_OEMCP);
}

WINFN(BOOL) user_OemToCharBuffA(LPCSTR src, LPSTR dst, DWORD n)
{
	return recode(src, dst, n, CP_OEMCP, CP_ACP);
}

WINFN(BOOL) user_CharToOemA(LPCSTR src, LPSTR dst)
{
	return src && recode(src, dst, strlen(src) + 1, CP_ACP, CP_OEMCP);
}

WINFN(BOOL) user_OemToCharA(LPCSTR src, LPSTR dst)
{
	return src && recode(src, dst, strlen(src) + 1, CP_OEMCP, CP_ACP);
}

#define E(name) { #name, 0, (void *)user_##name }

const struct builtin_export u32_string_exports[] = {
	E(CharLowerA),
	E(CharLowerBuffA),
	E(CharLowerBuffW),
	E(CharLowerW),
	E(CharNextA),
	E(CharNextW),
	E(CharPrevA),
	E(CharPrevW),
	E(CharToOemA),
	E(CharToOemBuffA),
	E(CharUpperA),
	E(CharUpperBuffA),
	E(CharUpperBuffW),
	E(CharUpperW),
	E(IsCharAlphaA),
	E(IsCharAlphaNumericA),
	E(IsCharAlphaNumericW),
	E(IsCharAlphaW),
	E(IsCharLowerA),
	E(IsCharLowerW),
	E(IsCharUpperA),
	E(IsCharUpperW),
	E(LoadStringA),
	E(LoadStringW),
	E(OemToCharA),
	E(OemToCharBuffA),
	E(wsprintfA),
	E(wsprintfW),
	E(wvsprintfA),
	E(wvsprintfW),
	BUILTIN_END
};
