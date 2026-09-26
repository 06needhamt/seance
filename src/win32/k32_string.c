/*
 * kernel32: code pages, lstr*, string comparison and mapping, character
 * types and locale information. The locale is always en-US (0x409).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"

#define LCID_EN_US 0x0409

/* Code pages. */

WINFN(UINT) k32_GetACP(void) { return CODEPAGE_ANSI; }
WINFN(UINT) k32_GetOEMCP(void) { return CODEPAGE_OEM; }
WINFN(BOOL) k32_IsValidCodePage(UINT cp) { return cp_valid(cp) && cp > CP_THREAD_ACP; }
WINFN(BOOL) k32_IsDBCSLeadByte(BYTE b) { return FALSE; }
WINFN(BOOL) k32_IsDBCSLeadByteEx(UINT cp, BYTE b) { return FALSE; }

WINFN(BOOL) k32_GetCPInfo(UINT cp, LPCPINFO info)
{
	if (!cp_valid(cp) || !info) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	memset(info, 0, sizeof(*info));
	info->MaxCharSize = cp_resolve(cp) == CP_UTF8 ? 4 : 1;
	info->DefaultChar[0] = '?';
	return TRUE;
}

WINFN(int) k32_MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srclen,
				   LPWSTR dst, int dstlen)
{
	int strict = (flags & MB_ERR_INVALID_CHARS) != 0;

	if (!cp_valid(cp) || !src || !srclen || dstlen < 0 ||
	    (dstlen && !dst)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	int n = cp_to_utf16(cp, src, srclen, NULL, 0, strict);
	if (n < 0) {
		win_set_error(ERROR_NO_UNICODE_TRANSLATION);
		return 0;
	}
	if (!dstlen)
		return n;
	if (n > dstlen) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	return cp_to_utf16(cp, src, srclen, dst, dstlen, strict);
}

WINFN(int) k32_WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src,
				   int srclen, LPSTR dst, int dstlen,
				   LPCSTR defchar, LPBOOL used_default)
{
	int used = 0;

	if (!cp_valid(cp) || !src || !srclen || dstlen < 0 ||
	    (dstlen && !dst) ||
	    (cp_resolve(cp) == CP_UTF8 && (defchar || used_default))) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char def = defchar ? *defchar : '?';
	int n = cp_from_utf16(cp, src, srclen, NULL, 0, def, &used);
	if (dstlen) {
		if (n > dstlen) {
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		cp_from_utf16(cp, src, srclen, dst, dstlen, def, &used);
	}
	if (used_default)
		*used_default = used;
	return n;
}

/* Case mapping and classification cover Latin-1; other characters are
 * left unmapped (and classed as letters). */

static WCHAR wupper(WCHAR c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
		return c - 32;
	if (c == 0xff)
		return 0x178;
	return c;
}

static WCHAR wlower(WCHAR c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		return c + 32;
	if (c == 0x178)
		return 0xff;
	return c;
}

static WORD ctype1(WCHAR c)
{
	if (c == ' ' || c == 0xa0)
		return C1_SPACE | C1_BLANK;
	if (c == '\t')
		return C1_SPACE | C1_BLANK | C1_CNTRL;
	if (c >= '\n' && c <= '\r')
		return C1_SPACE | C1_CNTRL;
	if (c < 0x20 || (c >= 0x7f && c < 0xa0))
		return C1_CNTRL;
	if (c >= '0' && c <= '9')
		return C1_DIGIT | C1_XDIGIT;
	if (c == 0x178)
		return C1_ALPHA | C1_UPPER;
	if (c >= 0x100)
		return C1_ALPHA;
	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		return C1_ALPHA | C1_UPPER |
		       ((c >= 'A' && c <= 'F') ? C1_XDIGIT : 0);
	if ((c >= 'a' && c <= 'z') || (c >= 0xdf && c != 0xf7) ||
	    c == 0xaa || c == 0xb5 || c == 0xba)
		return C1_ALPHA | C1_LOWER |
		       ((c >= 'a' && c <= 'f') ? C1_XDIGIT : 0);
	return C1_PUNCT;
}

WINFN(BOOL) k32_GetStringTypeW(DWORD type, LPCWSTR src, int count, LPWORD out)
{
	if (!src || !out) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (count < 0)
		count = wstrlen(src) + 1;
	for (int i = 0; i < count; i++)
		out[i] = type == CT_CTYPE1 ? ctype1(src[i]) : 0;
	return TRUE;
}

WINFN(BOOL) k32_GetStringTypeExW(LCID lcid, DWORD type, LPCWSTR src, int count,
				 LPWORD out)
{
	return k32_GetStringTypeW(type, src, count, out);
}

WINFN(BOOL) k32_GetStringTypeA(LCID lcid, DWORD type, LPCSTR src, int count,
			       LPWORD out)
{
	if (!src || !out) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (count < 0)
		count = strlen(src) + 1;
	for (int i = 0; i < count; i++) {
		WCHAR w;
		cp_to_utf16(CP_ACP, src + i, 1, &w, 1, 0);
		out[i] = type == CT_CTYPE1 ? ctype1(w) : 0;
	}
	return TRUE;
}

WINFN(BOOL) k32_GetStringTypeExA(LCID lcid, DWORD type, LPCSTR src, int count,
				 LPWORD out)
{
	return k32_GetStringTypeA(lcid, type, src, count, out);
}

/* Comparison. */

static int compare_w(DWORD flags, const WCHAR *a, int na, const WCHAR *b, int nb)
{
	int tie = 0;

	if (na < 0)
		na = wstrlen(a);
	if (nb < 0)
		nb = wstrlen(b);
	for (int i = 0; i < na && i < nb; i++) {
		WCHAR x = wlower(a[i]), y = wlower(b[i]);
		if (x != y)
			return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
		/* Case only breaks ties: lower case sorts first. */
		if (!tie && !(flags & NORM_IGNORECASE) && a[i] != b[i])
			tie = a[i] == x ? -1 : 1;
	}
	if (na != nb)
		return na < nb ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
	return tie < 0 ? CSTR_LESS_THAN : tie > 0 ? CSTR_GREATER_THAN : CSTR_EQUAL;
}

static WCHAR *to_wide(const char *s, int n, int *out_len)
{
	if (n < 0)
		n = strlen(s);
	int wn = cp_to_utf16(CP_ACP, s, n, NULL, 0, 0);
	WCHAR *w = malloc((wn + 1) * sizeof(WCHAR));
	if (w) {
		cp_to_utf16(CP_ACP, s, n, w, wn, 0);
		w[wn] = 0;
	}
	*out_len = wn;
	return w;
}

WINFN(int) k32_CompareStringW(LCID lcid, DWORD flags, LPCWSTR a, int na,
			      LPCWSTR b, int nb)
{
	if (!a || !b) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return compare_w(flags, a, na, b, nb);
}

WINFN(int) k32_CompareStringA(LCID lcid, DWORD flags, LPCSTR a, int na,
			      LPCSTR b, int nb)
{
	int wa, wb;

	if (!a || !b) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	WCHAR *x = to_wide(a, na, &wa), *y = to_wide(b, nb, &wb);
	int r = x && y ? compare_w(flags, x, wa, y, wb) : 0;
	free(x);
	free(y);
	return r;
}

WINFN(int) k32_lstrcmpA(LPCSTR a, LPCSTR b)
{
	return k32_CompareStringA(LCID_EN_US, 0, a ? a : "", -1, b ? b : "", -1) - 2;
}

WINFN(int) k32_lstrcmpiA(LPCSTR a, LPCSTR b)
{
	return k32_CompareStringA(LCID_EN_US, NORM_IGNORECASE, a ? a : "", -1,
				  b ? b : "", -1) - 2;
}

WINFN(int) k32_lstrcmpW(LPCWSTR a, LPCWSTR b)
{
	static const WCHAR empty[1];
	return compare_w(0, a ? a : empty, -1, b ? b : empty, -1) - 2;
}

WINFN(int) k32_lstrcmpiW(LPCWSTR a, LPCWSTR b)
{
	static const WCHAR empty[1];
	return compare_w(NORM_IGNORECASE, a ? a : empty, -1, b ? b : empty, -1) - 2;
}

/* LCMapString: case mapping, and a simple sort key. */

static int map_w(DWORD flags, const WCHAR *src, int n, WCHAR *out)
{
	for (int i = 0; i < n; i++) {
		WCHAR c = src[i];
		if (flags & LCMAP_LOWERCASE)
			c = wlower(c);
		else if (flags & LCMAP_UPPERCASE)
			c = wupper(c);
		out[i] = c;
	}
	return n;
}

#define LCMAP_SORTKEY 0x400

WINFN(int) k32_LCMapStringW(LCID lcid, DWORD flags, LPCWSTR src, int n,
			    LPWSTR dst, int dstlen)
{
	if (!src || dstlen < 0) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (n < 0)
		n = wstrlen(src) + 1;
	if (flags & LCMAP_SORTKEY) {
		/* Case-folded characters, big-endian, then a terminator. */
		int need = n * 2 + 1;
		BYTE *out = (BYTE *)dst;
		if (!dstlen)
			return need;
		if (dstlen < need) {
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		for (int i = 0; i < n; i++) {
			WCHAR c = wlower(src[i]);
			out[2 * i] = c >> 8;
			out[2 * i + 1] = c & 0xff;
		}
		out[2 * n] = 0;
		return need;
	}
	if (!dstlen)
		return n;
	if (dstlen < n) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	return map_w(flags, src, n, dst);
}

WINFN(int) k32_LCMapStringA(LCID lcid, DWORD flags, LPCSTR src, int n,
			    LPSTR dst, int dstlen)
{
	if (!src || dstlen < 0) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (n < 0)
		n = strlen(src) + 1;
	int wn;
	WCHAR *w = to_wide(src, n, &wn);
	if (!w)
		return 0;
	int r;
	if (flags & LCMAP_SORTKEY) {
		r = k32_LCMapStringW(lcid, flags, w, wn, (LPWSTR)dst, dstlen);
	} else {
		map_w(flags, w, wn, w);
		int need = cp_from_utf16(CP_ACP, w, wn, NULL, 0, '?', NULL);
		if (!dstlen) {
			r = need;
		} else if (dstlen < need) {
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			r = 0;
		} else {
			r = cp_from_utf16(CP_ACP, w, wn, dst, dstlen, '?', NULL);
		}
	}
	free(w);
	return r;
}

/* lstr*: NULL-tolerant string helpers. */

WINFN(int) k32_lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
WINFN(int) k32_lstrlenW(LPCWSTR s) { return s ? (int)wstrlen(s) : 0; }

WINFN(LPSTR) k32_lstrcpyA(LPSTR dst, LPCSTR src)
{
	if (!dst || !src)
		return NULL;
	return strcpy(dst, src);
}

WINFN(LPWSTR) k32_lstrcpyW(LPWSTR dst, LPCWSTR src)
{
	if (!dst || !src)
		return NULL;
	memcpy(dst, src, (wstrlen(src) + 1) * sizeof(WCHAR));
	return dst;
}

WINFN(LPSTR) k32_lstrcpynA(LPSTR dst, LPCSTR src, int n)
{
	if (!dst || !src || n <= 0)
		return dst;
	int i;
	for (i = 0; i < n - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
	return dst;
}

WINFN(LPWSTR) k32_lstrcpynW(LPWSTR dst, LPCWSTR src, int n)
{
	if (!dst || !src || n <= 0)
		return dst;
	int i;
	for (i = 0; i < n - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = 0;
	return dst;
}

WINFN(LPSTR) k32_lstrcatA(LPSTR dst, LPCSTR src)
{
	if (!dst || !src)
		return NULL;
	return strcat(dst, src);
}

WINFN(LPWSTR) k32_lstrcatW(LPWSTR dst, LPCWSTR src)
{
	if (!dst || !src)
		return NULL;
	k32_lstrcpyW(dst + wstrlen(dst), src);
	return dst;
}

/* Locale. */

static const struct {
	DWORD type;
	const char *value;
} locale_info[] = {
	{ LOCALE_ILANGUAGE, "0409" },
	{ LOCALE_SLANGUAGE, "English (United States)" },
	{ LOCALE_SABBREVLANGNAME, "ENU" },
	{ LOCALE_SNATIVELANGNAME, "English" },
	{ LOCALE_ICOUNTRY, "1" },
	{ LOCALE_SCOUNTRY, "United States" },
	{ LOCALE_SABBREVCTRYNAME, "USA" },
	{ LOCALE_IDEFAULTLANGUAGE, "0409" },
	{ LOCALE_IDEFAULTCOUNTRY, "1" },
	{ LOCALE_IDEFAULTCODEPAGE, "437" },
	{ LOCALE_SLIST, "," },
	{ LOCALE_IMEASURE, "1" },
	{ LOCALE_SDECIMAL, "." },
	{ LOCALE_STHOUSAND, "," },
	{ LOCALE_SGROUPING, "3;0" },
	{ LOCALE_IDIGITS, "2" },
	{ LOCALE_ILZERO, "1" },
	{ LOCALE_SNATIVEDIGITS, "0123456789" },
	{ LOCALE_SCURRENCY, "$" },
	{ LOCALE_SINTLSYMBOL, "USD" },
	{ LOCALE_SMONDECIMALSEP, "." },
	{ LOCALE_SMONTHOUSANDSEP, "," },
	{ LOCALE_SMONGROUPING, "3;0" },
	{ LOCALE_ICURRDIGITS, "2" },
	{ LOCALE_ICURRENCY, "0" },
	{ LOCALE_INEGCURR, "0" },
	{ LOCALE_SDATE, "/" },
	{ LOCALE_STIME, ":" },
	{ LOCALE_SSHORTDATE, "M/d/yyyy" },
	{ LOCALE_SLONGDATE, "dddd, MMMM dd, yyyy" },
	{ LOCALE_SENGLANGUAGE, "English" },
	{ LOCALE_SENGCOUNTRY, "United States" },
	{ LOCALE_IDEFAULTANSICODEPAGE, "1252" },
	{ LOCALE_SISO639LANGNAME, "en" },
	{ LOCALE_SISO3166CTRYNAME, "US" },
};

static const char *locale_value(DWORD type)
{
	type &= ~(LOCALE_NOUSEROVERRIDE | LOCALE_USE_CP_ACP | LOCALE_RETURN_NUMBER);
	for (size_t i = 0; i < sizeof(locale_info) / sizeof(locale_info[0]); i++)
		if (locale_info[i].type == type)
			return locale_info[i].value;
	return NULL;
}

WINFN(int) k32_GetLocaleInfoA(LCID lcid, DWORD type, LPSTR buf, int size)
{
	const char *v = locale_value(type);

	if (!v) {
		win_set_error(ERROR_INVALID_FLAGS);
		return 0;
	}
	if (type & LOCALE_RETURN_NUMBER) {
		if (size < (int)sizeof(DWORD)) {
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		DWORD n = strtoul(v, NULL, (type & 0xff) == LOCALE_ILANGUAGE ||
				  (type & 0xff) == LOCALE_IDEFAULTLANGUAGE ? 16 : 10);
		memcpy(buf, &n, sizeof(n));
		return sizeof(DWORD);
	}
	int len = strlen(v) + 1;
	if (!size)
		return len;
	if (size < len) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	memcpy(buf, v, len);
	return len;
}

WINFN(int) k32_GetLocaleInfoW(LCID lcid, DWORD type, LPWSTR buf, int size)
{
	const char *v = locale_value(type);

	if (!v) {
		win_set_error(ERROR_INVALID_FLAGS);
		return 0;
	}
	if (type & LOCALE_RETURN_NUMBER) {
		if (size < 2) {
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		DWORD n = strtoul(v, NULL, (type & 0xff) == LOCALE_ILANGUAGE ||
				  (type & 0xff) == LOCALE_IDEFAULTLANGUAGE ? 16 : 10);
		memcpy(buf, &n, sizeof(n));
		return 2;
	}
	int len = strlen(v) + 1;
	if (!size)
		return len;
	if (size < len) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	for (int i = 0; i < len; i++)
		buf[i] = (BYTE)v[i];
	return len;
}

WINFN(LCID) k32_GetUserDefaultLCID(void) { return LCID_EN_US; }
WINFN(LCID) k32_GetSystemDefaultLCID(void) { return LCID_EN_US; }
WINFN(LCID) k32_GetThreadLocale(void) { return LCID_EN_US; }
WINFN(BOOL) k32_SetThreadLocale(LCID lcid) { return TRUE; }
WINFN(LANGID) k32_GetUserDefaultLangID(void) { return LCID_EN_US; }
WINFN(LANGID) k32_GetSystemDefaultLangID(void) { return LCID_EN_US; }
WINFN(LANGID) k32_GetUserDefaultUILanguage(void) { return LCID_EN_US; }
WINFN(LANGID) k32_GetSystemDefaultUILanguage(void) { return LCID_EN_US; }

WINFN(LCID) k32_ConvertDefaultLocale(LCID lcid)
{
	if (lcid == 0 || lcid == LOCALE_USER_DEFAULT || lcid == LOCALE_SYSTEM_DEFAULT)
		return LCID_EN_US;
	return lcid;
}

WINFN(BOOL) k32_IsValidLocale(LCID lcid, DWORD flags)
{
	lcid = k32_ConvertDefaultLocale(lcid);
	return lcid == LCID_EN_US || lcid == LOCALE_INVARIANT;
}

typedef BOOL (WINAPI *LOCALE_ENUMPROCA)(LPSTR name);

WINFN(BOOL) k32_EnumSystemLocalesA(LOCALE_ENUMPROCA fn, DWORD flags)
{
	static char name[] = "00000409";

	if (!fn) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	fn(name);
	return TRUE;
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_string_exports[] = {
	E(CompareStringA),
	E(CompareStringW),
	E(ConvertDefaultLocale),
	E(EnumSystemLocalesA),
	E(GetACP),
	E(GetCPInfo),
	E(GetLocaleInfoA),
	E(GetLocaleInfoW),
	E(GetOEMCP),
	E(GetStringTypeA),
	E(GetStringTypeExA),
	E(GetStringTypeExW),
	E(GetStringTypeW),
	E(GetSystemDefaultLCID),
	E(GetSystemDefaultLangID),
	E(GetSystemDefaultUILanguage),
	E(GetThreadLocale),
	E(GetUserDefaultLCID),
	E(GetUserDefaultLangID),
	E(GetUserDefaultUILanguage),
	E(IsDBCSLeadByte),
	E(IsDBCSLeadByteEx),
	E(IsValidCodePage),
	E(IsValidLocale),
	E(LCMapStringA),
	E(LCMapStringW),
	E(MultiByteToWideChar),
	E(SetThreadLocale),
	E(WideCharToMultiByte),
	E(lstrcatA),
	E(lstrcatW),
	E(lstrcmpA),
	E(lstrcmpW),
	E(lstrcmpiA),
	E(lstrcmpiW),
	E(lstrcpyA),
	E(lstrcpyW),
	E(lstrcpynA),
	E(lstrcpynW),
	E(lstrlenA),
	E(lstrlenW),
	BUILTIN_END
};
