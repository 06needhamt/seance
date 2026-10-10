/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "common/unicode.h"

/* Appends the UTF-8 encoding of cp if it fits (leaving room for the NUL). */
static size_t put_utf8(unsigned cp, char *out, size_t pos, size_t outsz)
{
	char buf[4];
	size_t len;

	if (cp < 0x80) {
		buf[0] = cp;
		len = 1;
	} else if (cp < 0x800) {
		buf[0] = 0xc0 | (cp >> 6);
		buf[1] = 0x80 | (cp & 0x3f);
		len = 2;
	} else if (cp < 0x10000) {
		buf[0] = 0xe0 | (cp >> 12);
		buf[1] = 0x80 | ((cp >> 6) & 0x3f);
		buf[2] = 0x80 | (cp & 0x3f);
		len = 3;
	} else {
		buf[0] = 0xf0 | (cp >> 18);
		buf[1] = 0x80 | ((cp >> 12) & 0x3f);
		buf[2] = 0x80 | ((cp >> 6) & 0x3f);
		buf[3] = 0x80 | (cp & 0x3f);
		len = 4;
	}
	if (pos + len >= outsz)
		return 0;
	memcpy(out + pos, buf, len);
	return len;
}

size_t utf16_to_utf8(const WCHAR *s, size_t n, char *out, size_t outsz)
{
	size_t pos = 0;

	if (!outsz)
		return 0;
	for (size_t i = 0; i < n && s[i]; i++) {
		unsigned cp = s[i];
		if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n &&
		    s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
			cp = 0x10000 + ((cp - 0xd800) << 10) + (s[i + 1] - 0xdc00);
			i++;
		} else if (cp >= 0xd800 && cp < 0xe000) {
			cp = 0xfffd;
		}
		size_t len = put_utf8(cp, out, pos, outsz);
		if (!len)
			break;
		pos += len;
	}
	out[pos] = '\0';
	return pos;
}

static int ascii_lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

int utf16_ascii_casecmp(const WCHAR *s, size_t n, const char *ascii)
{
	size_t i;

	for (i = 0; i < n && ascii[i]; i++) {
		int a = s[i] < 0x80 ? ascii_lower(s[i]) : 0x100 + s[i];
		int b = ascii_lower((unsigned char)ascii[i]);
		if (a != b)
			return a - b;
	}
	if (i < n)
		return 1;
	return ascii[i] ? -1 : 0;
}

size_t utf16_field_to_utf8(const WCHAR *s, size_t max, char *out,
			   size_t outsz)
{
	const unsigned char *b = (const unsigned char *)s;
	size_t n = 0, k = 0;

	while (n < max && s[n])
		n++;
	while (k < 2 * max && b[k] >= 0x20 && b[k] < 0x7f)
		k++;
	if (k >= 2 && k < 2 * max && !b[k] && b[1]) {
		size_t len = outsz ? (k < outsz ? k : outsz - 1) : 0;
		if (outsz) {
			memcpy(out, b, len);
			out[len] = 0;
		}
		return len;
	}
	return utf16_to_utf8(s, n, out, outsz);
}
