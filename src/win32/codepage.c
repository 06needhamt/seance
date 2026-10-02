/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdlib.h>
#include <string.h>

#include "win32/codepage.h"
#include "win32/internal.h"

/* Windows-1252 0x80-0x9f; the unassigned bytes map to C1 controls. */
static const WCHAR cp1252_high[32] = {
	0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
	0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
	0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
	0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

/* Code page 437 (the DOS/OEM code page) 0x80-0xff. */
static const WCHAR cp437_high[128] = {
	0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
	0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
	0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
	0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
	0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
	0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
	0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
	0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
	0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
	0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
	0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
	0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
	0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
	0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

UINT cp_resolve(UINT cp)
{
	switch (cp) {
	case CP_ACP:
	case CP_THREAD_ACP:
	case CP_MACCP:
		return CODEPAGE_ANSI;
	case CP_OEMCP:
		return CODEPAGE_OEM;
	default:
		return cp;
	}
}

int cp_valid(UINT cp)
{
	switch (cp_resolve(cp)) {
	case 1252: case 437: case 28591: case 20127: case CP_UTF8:
		return 1;
	default:
		return 0;
	}
}

/* A single-byte code page's character for b, or -1 if it has none. */
static int sb_decode(UINT cp, BYTE b)
{
	if (b < 0x80)
		return b;
	switch (cp) {
	case 1252:
		return b < 0xa0 ? cp1252_high[b - 0x80] : b;
	case 437:
		return cp437_high[b - 0x80];
	case 28591:
		return b;
	default:		/* 20127, ASCII */
		return -1;
	}
}

/* The byte for c in a single-byte code page, or -1. */
static int sb_encode(UINT cp, WCHAR c)
{
	if (c < 0x80)
		return c;
	switch (cp) {
	case 1252:
		if (c >= 0xa0 && c <= 0xff)
			return c;
		for (int i = 0; i < 32; i++)
			if (cp1252_high[i] == c)
				return 0x80 + i;
		return -1;
	case 437:
		for (int i = 0; i < 128; i++)
			if (cp437_high[i] == c)
				return 0x80 + i;
		return -1;
	case 28591:
		return c <= 0xff ? c : -1;
	default:
		return -1;
	}
}

/* Decodes one UTF-8 sequence; returns its length, and U+FFFD if invalid. */
static int utf8_decode(const BYTE *s, int len, unsigned *cp, int *bad)
{
	static const unsigned min[5] = { 0, 0, 0x80, 0x800, 0x10000 };
	BYTE b = s[0];
	int n;

	*bad = 0;
	if (b < 0x80) {
		*cp = b;
		return 1;
	}
	n = b >= 0xf0 && b < 0xf5 ? 4 : b >= 0xe0 ? (b < 0xf0 ? 3 : 0) :
	    b >= 0xc2 ? 2 : 0;
	if (!n || n > len)
		goto invalid;
	unsigned c = b & (0x7f >> n);
	for (int i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			n = i;
			goto invalid_n;
		}
		c = (c << 6) | (s[i] & 0x3f);
	}
	if (c < min[n] || c > 0x10ffff || (c >= 0xd800 && c < 0xe000))
		goto invalid_n;
	*cp = c;
	return n;

invalid:
	n = 1;
invalid_n:
	*cp = 0xfffd;
	*bad = 1;
	return n;
}

int cp_to_utf16(UINT cp, const char *src, int srclen, WCHAR *dst, int dstlen,
		int strict)
{
	const BYTE *s = (const BYTE *)src;
	int out = 0;

	cp = cp_resolve(cp);
	if (srclen < 0)
		srclen = strlen(src) + 1;

	for (int i = 0; i < srclen;) {
		unsigned c;
		int bad = 0;

		if (cp == CP_UTF8) {
			i += utf8_decode(s + i, srclen - i, &c, &bad);
		} else {
			int d = sb_decode(cp, s[i++]);
			bad = d < 0;
			c = bad ? '?' : (unsigned)d;
		}
		if (bad && strict)
			return -1;

		int units = c >= 0x10000 ? 2 : 1;
		if (dstlen) {
			if (out + units > dstlen)
				return -1;
			if (units == 2) {
				dst[out] = 0xd800 + ((c - 0x10000) >> 10);
				dst[out + 1] = 0xdc00 + ((c - 0x10000) & 0x3ff);
			} else {
				dst[out] = c;
			}
		}
		out += units;
	}
	return out;
}

int cp_from_utf16(UINT cp, const WCHAR *src, int srclen, char *dst,
		  int dstlen, char defchar, int *used_default)
{
	int out = 0;

	cp = cp_resolve(cp);
	if (used_default)
		*used_default = 0;
	if (srclen < 0)
		srclen = wstrlen(src) + 1;

	for (int i = 0; i < srclen; i++) {
		BYTE buf[4];
		int n;
		unsigned c = src[i];

		if (cp == CP_UTF8) {
			if (c >= 0xd800 && c < 0xdc00 && i + 1 < srclen &&
			    src[i + 1] >= 0xdc00 && src[i + 1] < 0xe000) {
				c = 0x10000 + ((c - 0xd800) << 10) + (src[i + 1] - 0xdc00);
				i++;
			} else if (c >= 0xd800 && c < 0xe000) {
				c = 0xfffd;
			}
			if (c < 0x80) {
				buf[0] = c;
				n = 1;
			} else if (c < 0x800) {
				buf[0] = 0xc0 | (c >> 6);
				buf[1] = 0x80 | (c & 0x3f);
				n = 2;
			} else if (c < 0x10000) {
				buf[0] = 0xe0 | (c >> 12);
				buf[1] = 0x80 | ((c >> 6) & 0x3f);
				buf[2] = 0x80 | (c & 0x3f);
				n = 3;
			} else {
				buf[0] = 0xf0 | (c >> 18);
				buf[1] = 0x80 | ((c >> 12) & 0x3f);
				buf[2] = 0x80 | ((c >> 6) & 0x3f);
				buf[3] = 0x80 | (c & 0x3f);
				n = 4;
			}
		} else {
			int b = sb_encode(cp, c);
			if (b < 0) {
				b = (BYTE)defchar;
				if (used_default)
					*used_default = 1;
			}
			buf[0] = b;
			n = 1;
		}

		if (dstlen) {
			if (out + n > dstlen)
				return -1;
			memcpy(dst + out, buf, n);
		}
		out += n;
	}
	return out;
}

static char *convert_to_utf8(UINT cp, const char *s)
{
	if (!s)
		return NULL;
	int n = cp_to_utf16(cp, s, -1, NULL, 0, 0);
	WCHAR *w = malloc(n * sizeof(WCHAR));
	if (!w)
		return NULL;
	cp_to_utf16(cp, s, -1, w, n, 0);
	char *r = utf16_to_utf8_dup(w);
	free(w);
	return r;
}

char *acp_to_utf8(const char *s)
{
	return convert_to_utf8(CP_ACP, s);
}

char *utf16_to_utf8_dup(const WCHAR *s)
{
	if (!s)
		return NULL;
	int n = cp_from_utf16(CP_UTF8, s, -1, NULL, 0, '?', NULL);
	char *r = malloc(n);
	if (r)
		cp_from_utf16(CP_UTF8, s, -1, r, n, '?', NULL);
	return r;
}

WCHAR *utf8_to_utf16_dup(const char *s)
{
	if (!s)
		return NULL;
	int n = cp_to_utf16(CP_UTF8, s, -1, NULL, 0, 0);
	WCHAR *w = malloc(n * sizeof(WCHAR));
	if (w)
		cp_to_utf16(CP_UTF8, s, -1, w, n, 0);
	return w;
}

char *utf8_to_acp_dup(const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	if (!w)
		return NULL;
	int n = cp_from_utf16(CP_ACP, w, -1, NULL, 0, '?', NULL);
	char *r = malloc(n);
	if (r)
		cp_from_utf16(CP_ACP, w, -1, r, n, '?', NULL);
	free(w);
	return r;
}
