/*
 * VARIANT conversions (VariantChangeType(Ex) and the Var*From* family)
 * and the date functions, after Wine's variant.c and vartype.c.
 *
 * One engine converts between the types: numbers go through a 64-bit
 * integer, a double or a scaled currency value, with Windows' rules:
 * floating values and currency round to even ("banker's rounding"), a
 * value out of the target's range is DISP_E_OVERFLOW, VARIANT_TRUE is -1
 * (all ones as an unsigned type), and a string that isn't a number is
 * DISP_E_TYPEMISMATCH. Strings are read and written as in the US English
 * locale whatever LCID is given: "1,234.5", "&H1F", "$3", "(4)" read as
 * numbers; dates as "M/D/YYYY h:mm:ss AM" (or ISO "YYYY-MM-DD", or with
 * month names); VT_BOOL as "-1"/"0", or "True"/"False" with
 * VARIANT_ALPHABOOL and from VarBstrFromBool. A VT_DISPATCH converts
 * through its value property (DISPID_VALUE) unless VARIANT_NOVALUEPROP.
 * VT_DECIMAL can be copied but not converted.
 *
 * DATE is days since 30 December 1899, the fraction the time of day; a
 * negative date's fraction still counts forward from its midnight.
 */
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/oleaut.h"

#define DATE_MIN (-657434.0)		/* 1 January 100 */
#define DATE_MAX 2958465.9999999	/* 31 December 9999, 23:59:59 */

/* Dates. */

/* Days from 1 January 1970 of a proleptic Gregorian date (Hinnant's). */
static long days_from_civil(long y, unsigned m, unsigned d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, long *y, unsigned *m, unsigned *d)
{
	z += 719468;
	long era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = (long)yoe + era * 400 + (*m <= 2);
}

static long epoch(void)
{
	return days_from_civil(1899, 12, 30);
}

static int leap(long y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static unsigned month_days(long y, unsigned m)
{
	static const unsigned char n[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30,
					   31, 30, 31 };
	return m == 2 && leap(y) ? 29 : n[m - 1];
}

/* A DATE from its parts; seconds past a day roll into the next. */
static double make_date(long y, unsigned mo, unsigned d, long secs)
{
	long day = days_from_civil(y, mo, 1) + (long)d - 1 - epoch();
	day += secs / 86400;
	secs %= 86400;
	double frac = secs / 86400.0;
	return day >= 0 ? day + frac : day - frac;
}

/* A DATE's parts, to the nearest second; FALSE if out of range. */
static BOOL split_date(double v, SYSTEMTIME *st)
{
	if (!(v >= DATE_MIN && v <= DATE_MAX + 0.0000001))
		return FALSE;
	double whole = trunc(v);
	long secs = lround(fabs(v - whole) * 86400.0);
	long day = (long)whole;
	if (secs >= 86400) {
		secs -= 86400;
		day++;
	}
	long y;
	unsigned m, d;
	long z = epoch() + day;
	civil_from_days(z, &y, &m, &d);
	st->wYear = (WORD)y;
	st->wMonth = (WORD)m;
	st->wDay = (WORD)d;
	st->wDayOfWeek = (WORD)(((z % 7) + 11) % 7);	/* 1 Jan 1970: Thursday */
	st->wHour = (WORD)(secs / 3600);
	st->wMinute = (WORD)(secs / 60 % 60);
	st->wSecond = (WORD)(secs % 60);
	st->wMilliseconds = 0;
	return TRUE;
}

/* Two-digit years: 00-29 are 2000-2029, 30-99 are 1930-1999. */
static long full_year(long y)
{
	return y < 30 ? y + 2000 : y < 100 ? y + 1900 : y;
}

WINFN(INT) oa_SystemTimeToVariantTime(SYSTEMTIME *st, double *out)
{
	if (!st || !out || st->wMonth < 1 || st->wMonth > 12 ||
	    st->wDay < 1 || st->wDay > 31 || (short)st->wYear < 0)
		return FALSE;
	long y = full_year(st->wYear);
	double v = make_date(y, st->wMonth, st->wDay,
			     st->wHour * 3600L + st->wMinute * 60L + st->wSecond);
	if (!(v >= DATE_MIN && v <= DATE_MAX + 0.0000001))
		return FALSE;
	*out = v;
	return TRUE;
}

WINFN(INT) oa_VariantTimeToSystemTime(double v, SYSTEMTIME *st)
{
	return st && split_date(v, st);
}

/* DOS: date bits 0-4 day, 5-8 month, 9-15 year - 1980; time 2-second units. */
WINFN(INT) oa_DosDateTimeToVariantTime(USHORT date, USHORT time, double *out)
{
	unsigned d = date & 0x1f, m = (date >> 5) & 0xf, h = time >> 11,
		 mi = (time >> 5) & 0x3f, s = (time & 0x1f) * 2;

	if (!out || h > 23 || mi > 59 || s > 59 || m < 1 || m > 12 || d < 1)
		return FALSE;
	*out = make_date(1980 + (date >> 9), m, d, h * 3600L + mi * 60L + s);
	return TRUE;
}

WINFN(INT) oa_VariantTimeToDosDateTime(double v, USHORT *date, USHORT *time)
{
	SYSTEMTIME st;

	if (!date || !time || !split_date(v, &st) || st.wYear < 1980 ||
	    st.wYear > 2107)
		return FALSE;
	*date = (USHORT)((st.wYear - 1980) << 9 | st.wMonth << 5 | st.wDay);
	*time = (USHORT)(st.wHour << 11 | st.wMinute << 5 | st.wSecond / 2);
	return TRUE;
}

/* Rounding to even, whatever the FPU's rounding mode (codecs change it). */
static double round_even(double x)
{
	double f = floor(x), r = x - f;

	if (r > 0.5)
		return f + 1;
	if (r < 0.5)
		return f;
	return fmod(f, 2.0) == 0 ? f : f + 1;
}

/* Strings. */

static BSTR bstr_ascii(const char *s)
{
	size_t n = strlen(s);
	BSTR b = oa_SysAllocStringLen(NULL, (UINT)n);

	for (size_t i = 0; b && i < n; i++)
		b[i] = (BYTE)s[i];
	return b;
}

static void format_double(double d, int digits, char *out, size_t n)
{
	if (d == 0)
		d = 0;	/* not "-0" */
	snprintf(out, n, "%.*G", digits, d);
}

static void format_cy(int64_t v, char *out, size_t n)
{
	uint64_t a = v < 0 ? -(uint64_t)v : (uint64_t)v;
	int len = snprintf(out, n, "%s%llu.%04u", v < 0 ? "-" : "",
			   (unsigned long long)(a / 10000), (unsigned)(a % 10000));
	while (len > 0 && out[len - 1] == '0')
		out[--len] = '\0';
	if (len > 0 && out[len - 1] == '.')
		out[--len] = '\0';
}

/* US English: "M/D/YYYY", "h:mm:ss AM", or both. */
static int format_date(double v, ULONG flags, char *out, size_t n)
{
	SYSTEMTIME st;

	if (!split_date(v, &st))
		return -1;
	int date_only = (flags & VAR_DATEVALUEONLY) ||
			(!(flags & VAR_TIMEVALUEONLY) && !st.wHour &&
			 !st.wMinute && !st.wSecond && trunc(v) != 0);
	int time_only = (flags & VAR_TIMEVALUEONLY) ||
			(!(flags & VAR_DATEVALUEONLY) && trunc(v) == 0);
	char d[32] = "", t[32] = "";
	if (!time_only)
		snprintf(d, sizeof(d), "%u/%u/%u", st.wMonth, st.wDay, st.wYear);
	if (!date_only)
		snprintf(t, sizeof(t), "%u:%02u:%02u %s",
			 st.wHour % 12 ? st.wHour % 12 : 12, st.wMinute,
			 st.wSecond, st.wHour < 12 ? "AM" : "PM");
	snprintf(out, n, "%s%s%s", d, *d && *t ? " " : "", t);
	return 0;
}

static int is_space(WCHAR c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xa0;
}

/* A number read from a string: an integer if it is one, else a double. */
struct num {
	int kind;	/* N_INT, N_UINT, N_DBL, N_CY (i scaled by 10000), N_BOOL */
	int hex;	/* read as &H or &O: fills the target's width */
	int64_t i;
	uint64_t u;
	double d;
};

enum { N_INT, N_UINT, N_DBL, N_CY, N_BOOL };

/*
 * Reads a number the way VarParseNumFromStr does in US English: blanks
 * around it, a sign or parentheses, a "$", thousands separators, a
 * fraction, an exponent (E or D), or &H hex and &O octal.
 */
static HRESULT parse_num(const WCHAR *s, struct num *out)
{
	const WCHAR *p = s;
	int neg = 0, parens = 0, digits = 0;

	if (!s)
		return DISP_E_TYPEMISMATCH;
	while (is_space(*p))
		p++;
	if (*p == '(') {
		parens = 1;
		p++;
	}
	if (*p == '$')
		p++;
	if (*p == '+' || *p == '-')
		neg = *p++ == '-';
	if (*p == '$')
		p++;
	if (*p == '&' && (p[1] == 'H' || p[1] == 'h' || p[1] == 'O' ||
			  p[1] == 'o')) {
		int base = p[1] == 'H' || p[1] == 'h' ? 16 : 8;
		uint64_t v = 0;
		for (p += 2; *p; p++, digits++) {
			int x = *p >= '0' && *p <= '9' ? *p - '0' :
				*p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
				*p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : 99;
			if (x >= base)
				break;
			if (v > (UINT64_MAX >> (base == 16 ? 4 : 3)))
				return DISP_E_OVERFLOW;
			v = v * base + x;
		}
		while (is_space(*p))
			p++;
		if (!digits || *p || parens)
			return DISP_E_TYPEMISMATCH;
		/* Hex fills the type it's read into: &HFFFF is -1 as an I2. */
		out->hex = 1;
		out->kind = N_UINT;
		out->u = neg ? -v : v;
		out->i = (int64_t)out->u;
		out->d = neg ? -(double)v : (double)v;
		if (!neg && v <= INT64_MAX)
			out->kind = N_INT;
		return S_OK;
	}

	char buf[400];
	size_t n = 0;
	int frac = 0, exp = 0, integral = 1;
	for (; *p; p++) {
		if (*p >= '0' && *p <= '9') {
			if (n < sizeof(buf) - 8)
				buf[n++] = (char)*p;
			digits += !exp;
		} else if (*p == ',' && !frac && !exp && digits) {
			continue;
		} else if (*p == '.' && !frac && !exp) {
			frac = 1;
			buf[n++] = '.';
		} else if ((*p == 'e' || *p == 'E' || *p == 'd' || *p == 'D') &&
			   digits && !exp) {
			exp = 1;
			buf[n++] = 'e';
			if (p[1] == '+' || p[1] == '-')
				buf[n++] = (char)*++p;
			if (!(p[1] >= '0' && p[1] <= '9'))
				return DISP_E_TYPEMISMATCH;
		} else {
			break;
		}
	}
	buf[n] = '\0';
	while (is_space(*p))
		p++;
	if (parens) {
		if (*p != ')' || neg)
			return DISP_E_TYPEMISMATCH;
		neg = 1;
		for (p++; is_space(*p); p++)
			;
	}
	if (!digits || *p)
		return DISP_E_TYPEMISMATCH;
	if (frac || exp)
		integral = 0;
	char *end;
	out->d = strtod(buf, &end);
	if (neg)
		out->d = -out->d;
	if (integral) {
		errno = 0;
		unsigned long long v = strtoull(buf, &end, 10);
		if (!errno) {
			if (!neg && v <= INT64_MAX) {
				out->kind = N_INT;
				out->i = (int64_t)v;
				return S_OK;
			}
			if (neg && v <= (unsigned long long)INT64_MAX + 1) {
				out->kind = N_INT;
				out->i = v == (unsigned long long)INT64_MAX + 1 ?
					 INT64_MIN : -(int64_t)v;
				return S_OK;
			}
			if (!neg) {
				out->kind = N_UINT;
				out->u = v;
				return S_OK;
			}
		}
	}
	out->kind = N_DBL;
	return S_OK;
}

/* Words in dates. */
static int month_word(const char *w)
{
	static const char *const names[] = {
		"january", "february", "march", "april", "may", "june", "july",
		"august", "september", "october", "november", "december",
	};
	size_t n = strlen(w);

	for (int i = 0; i < 12; i++)
		if (n >= 3 && n <= strlen(names[i]) &&
		    !strncasecmp(w, names[i], n))
			return i + 1;
	return 0;
}

/*
 * Reads a date and/or time as US English writes one: "1/2/2000",
 * "2000-01-02", "Jan 2, 2000", "2 January 2000", "13:30", "1:30:15 PM"
 * and combinations; DISP_E_TYPEMISMATCH otherwise.
 */
static HRESULT parse_date(const WCHAR *s, ULONG flags, double *out)
{
	long dn[3] = { 0 }, tn[3] = { 0 };
	int nd = 0, nt = 0, dlen[3] = { 0 }, month = 0, ampm = 0, in_time = 0;

	if (!s)
		return DISP_E_TYPEMISMATCH;
	for (const WCHAR *p = s; *p;) {
		if (is_space(*p) || *p == ',') {
			in_time = 0;
			p++;
		} else if (*p >= '0' && *p <= '9') {
			long v = 0;
			int len = 0;
			for (; *p >= '0' && *p <= '9'; p++, len++)
				if (v < 100000)
					v = v * 10 + (*p - '0');
			if (*p == ':' || in_time) {
				if (nt == 3)
					return DISP_E_TYPEMISMATCH;
				tn[nt++] = v;
				in_time = 1;
			} else {
				if (nd == 3)
					return DISP_E_TYPEMISMATCH;
				dlen[nd] = len;
				dn[nd++] = v;
			}
		} else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
			char w[16];
			size_t n = 0;
			for (; (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z');
			     p++)
				if (n < sizeof(w) - 1)
					w[n++] = (char)*p;
			w[n] = '\0';
			if (!strcasecmp(w, "am") || !strcasecmp(w, "a"))
				ampm = 1;
			else if (!strcasecmp(w, "pm") || !strcasecmp(w, "p"))
				ampm = 2;
			else if (!month && (month = month_word(w)))
				;
			else
				return DISP_E_TYPEMISMATCH;
		} else if (*p == '/' || *p == '-' || *p == '.') {
			if (in_time)
				return DISP_E_TYPEMISMATCH;
			p++;
		} else if (*p == ':') {
			in_time = 1;
			p++;
		} else {
			return DISP_E_TYPEMISMATCH;
		}
	}

	/* "3 PM": an hour on its own. */
	if (ampm && !nt && nd == 1 && !month) {
		tn[nt++] = dn[0];
		nd = 0;
	}
	long y = 1899, m = 12, d = 30;
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	if (month) {
		m = month;
		if (nd == 1) {
			d = dn[0];
			y = tm.tm_year + 1900;
		} else if (nd == 2) {
			d = dn[0];
			y = dn[1];
			if (dlen[0] > 2) {	/* "2000 Jan 2" */
				y = dn[0];
				d = dn[1];
			}
		} else {
			return DISP_E_TYPEMISMATCH;
		}
	} else if (nd == 3) {
		if (dlen[0] > 2 || dn[0] > 31) {	/* Y-M-D */
			y = dn[0];
			m = dn[1];
			d = dn[2];
		} else {				/* M/D/Y */
			m = dn[0];
			d = dn[1];
			y = dn[2];
		}
	} else if (nd == 2) {
		m = dn[0];
		d = dn[1];
		y = tm.tm_year + 1900;
		if (m > 12 && d <= 12) {
			m = dn[1];
			d = dn[0];
		}
	} else if (nd == 1 || (!nd && !nt)) {
		return DISP_E_TYPEMISMATCH;
	}
	if (nd || month) {
		if ((nd == 3 && dlen[2] <= 2 && !(dlen[0] > 2 || dn[0] > 31)) ||
		    (month && nd == 2 && dlen[1] <= 2 && dlen[0] <= 2))
			y = full_year(y);
		if (m < 1 || m > 12 || y < 100 || y > 9999 || d < 1 ||
		    d > (long)month_days(y, (unsigned)m))
			return DISP_E_TYPEMISMATCH;
	}

	long h = tn[0], mi = nt > 1 ? tn[1] : 0, sec = nt > 2 ? tn[2] : 0;
	if (ampm) {
		if (h < 1 || h > 12)
			return DISP_E_TYPEMISMATCH;
		h = h % 12 + (ampm == 2 ? 12 : 0);
	}
	if (h > 23 || mi > 59 || sec > 59)
		return DISP_E_TYPEMISMATCH;
	if (flags & VAR_TIMEVALUEONLY)
		y = 1899, m = 12, d = 30;
	if (flags & VAR_DATEVALUEONLY)
		h = mi = sec = 0;
	*out = make_date(y, (unsigned)m, (unsigned)d, h * 3600 + mi * 60 + sec);
	return S_OK;
}

static int bool_word(const WCHAR *s, VARIANT_BOOL *out)
{
	char *u = s ? utf16_to_utf8_dup(s) : NULL;
	char *p = u, *e;
	int r = 0;

	if (!u)
		return 0;
	while (*p == ' ' || *p == '\t')
		p++;
	e = p + strlen(p);
	while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
	if (!strcasecmp(p, "true") || !strcasecmp(p, "#true#")) {
		*out = VARIANT_TRUE;
		r = 1;
	} else if (!strcasecmp(p, "false") || !strcasecmp(p, "#false#")) {
		*out = VARIANT_FALSE;
		r = 1;
	}
	free(u);
	return r;
}

/* The engine. */

/* The number a VARIANT holds; DISP_E_TYPEMISMATCH if it holds none. */
static HRESULT to_num(const VARIANT *v, struct num *n)
{
	memset(n, 0, sizeof(*n));
	n->kind = N_INT;
	switch (v->vt) {
	case VT_EMPTY: n->i = 0; break;
	case VT_I1:    n->i = (signed char)v->cVal; break;
	case VT_UI1:   n->i = v->bVal; break;
	case VT_I2:    n->i = v->iVal; break;
	case VT_UI2:   n->i = v->uiVal; break;
	case VT_I4:    n->i = v->lVal; break;
	case VT_INT:   n->i = v->intVal; break;
	case VT_UI4:   n->i = v->ulVal; break;
	case VT_UINT:  n->i = v->uintVal; break;
	case VT_I8:    n->i = v->llVal; break;
	case VT_UI8:
		if (v->ullVal > INT64_MAX) {
			n->kind = N_UINT;
			n->u = v->ullVal;
		} else {
			n->i = (int64_t)v->ullVal;
		}
		break;
	case VT_BOOL:
		n->kind = N_BOOL;
		n->i = v->boolVal ? -1 : 0;
		break;
	case VT_R4:
		n->kind = N_DBL;
		n->d = v->fltVal;
		break;
	case VT_R8:
		n->kind = N_DBL;
		n->d = v->dblVal;
		break;
	case VT_DATE:
		n->kind = N_DBL;
		n->d = v->date;
		break;
	case VT_CY:
		n->kind = N_CY;
		n->i = v->cyVal.int64;
		break;
	case VT_BSTR:
		return parse_num(v->bstrVal, n);
	default:
		return DISP_E_TYPEMISMATCH;
	}
	if (n->kind != N_DBL)
		n->d = n->kind == N_UINT ? (double)n->u :
		       n->kind == N_CY ? n->i / 10000.0 : (double)n->i;
	return S_OK;
}

/* The integer part of currency, rounded to even. */
static int64_t cy_round(int64_t v)
{
	int64_t q = v / 10000, r = v % 10000;

	if (r > 5000 || (r == 5000 && (q & 1)))
		q++;
	else if (r < -5000 || (r == -5000 && (q & 1)))
		q--;
	return q;
}

/*
 * The number as an integer of the given range; bits is the target's
 * width, for the unsigned readings of hex and of VARIANT_TRUE.
 */
static HRESULT to_int(const struct num *n, int64_t lo, uint64_t hi, int bits,
		      uint64_t *out)
{
	int unsigned_target = lo == 0;
	uint64_t mask = bits == 64 ? UINT64_MAX : (1ull << bits) - 1;

	switch (n->kind) {
	case N_BOOL:	/* all ones: the target's cast keeps what fits */
		*out = (uint64_t)n->i;
		return S_OK;
	case N_UINT:	/* above INT64_MAX: only a UI8 holds it */
		if (n->u > hi)
			return DISP_E_OVERFLOW;
		*out = n->u;
		return S_OK;
	case N_INT:
	case N_CY: {
		int64_t v = n->kind == N_CY ? cy_round(n->i) : n->i;
		if (n->hex && !unsigned_target && v >= 0 && (uint64_t)v <= mask &&
		    (uint64_t)v > hi) {
			/* &HFFFF is -1 as an I2. */
			uint64_t sign = 1ull << (bits - 1);
			*out = (uint64_t)(int64_t)(((uint64_t)v ^ sign) - sign);
			return S_OK;
		}
		if (v < lo || (v >= 0 && (uint64_t)v > hi))
			return DISP_E_OVERFLOW;
		*out = (uint64_t)v;
		return S_OK;
	}
	case N_DBL: {
		double r = round_even(n->d);
		if (isnan(r))
			return DISP_E_OVERFLOW;
		if (unsigned_target) {
			if (r < 0 || r >= (double)hi + 1.0)
				return DISP_E_OVERFLOW;
			*out = (uint64_t)r;
		} else {
			if (r < (double)lo || r >= (double)hi + 1.0)
				return DISP_E_OVERFLOW;
			*out = (uint64_t)(int64_t)r;
		}
		return S_OK;
	}
	}
	return DISP_E_TYPEMISMATCH;
}

static HRESULT to_cy(const struct num *n, int64_t *out)
{
	switch (n->kind) {
	case N_CY:
		*out = n->i;
		return S_OK;
	case N_INT:
	case N_BOOL:
		if (n->i > INT64_MAX / 10000 || n->i < INT64_MIN / 10000)
			return DISP_E_OVERFLOW;
		*out = n->i * 10000;
		return S_OK;
	case N_UINT:
		return DISP_E_OVERFLOW;
	}
	double r = round_even(n->d * 10000.0);
	if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
		return DISP_E_OVERFLOW;
	*out = (int64_t)r;
	return S_OK;
}

static HRESULT to_bstr(const VARIANT *src, LCID lcid, USHORT flags, BSTR *out)
{
	char buf[64];

	switch (src->vt) {
	case VT_EMPTY:
		buf[0] = '\0';
		break;
	case VT_I1: case VT_I2: case VT_I4: case VT_INT: case VT_I8: {
		struct num n;
		to_num(src, &n);
		snprintf(buf, sizeof(buf), "%lld", (long long)n.i);
		break;
	}
	case VT_UI1: case VT_UI2: case VT_UI4: case VT_UINT: case VT_UI8: {
		struct num n;
		to_num(src, &n);
		snprintf(buf, sizeof(buf), "%llu", (unsigned long long)
			 (n.kind == N_UINT ? n.u : (uint64_t)n.i));
		break;
	}
	case VT_R4:
		format_double(src->fltVal, 7, buf, sizeof(buf));
		break;
	case VT_R8:
		format_double(src->dblVal, 15, buf, sizeof(buf));
		break;
	case VT_CY:
		format_cy(src->cyVal.int64, buf, sizeof(buf));
		break;
	case VT_DATE:
		if (format_date(src->date, flags, buf, sizeof(buf)))
			return DISP_E_OVERFLOW;
		break;
	case VT_BOOL:
		if (flags & (VARIANT_ALPHABOOL | VARIANT_LOCALBOOL))
			strcpy(buf, src->boolVal ? "True" : "False");
		else
			strcpy(buf, src->boolVal ? "-1" : "0");
		break;
	case VT_BSTR:
		*out = src->bstrVal ? oa_SysAllocStringByteLen(
			(LPCSTR)src->bstrVal,
			oa_SysStringByteLen(src->bstrVal)) : NULL;
		return src->bstrVal && !*out ? E_OUTOFMEMORY : S_OK;
	default:
		return DISP_E_TYPEMISMATCH;
	}
	return (*out = bstr_ascii(buf)) ? S_OK : E_OUTOFMEMORY;
}

/* An object's value property. */
static HRESULT dispatch_value(IDispatch *d, LCID lcid, VARIANT *out)
{
	DISPPARAMS none = { NULL, NULL, 0, 0 };

	oa_VariantInit(out);
	if (!d)
		return DISP_E_TYPEMISMATCH;
	return d->lpVtbl->Invoke(d, DISPID_VALUE, &oa_IID_NULL, lcid,
				 DISPATCH_PROPERTYGET, &none, out, NULL, NULL);
}

/*
 * Converts src (not by reference, not an array) to type vt in dst, which
 * holds nothing yet.
 */
static HRESULT coerce(VARIANT *dst, const VARIANT *src, LCID lcid,
		      USHORT flags, VARTYPE vt, int depth)
{
	struct num n;
	HRESULT hr;
	uint64_t u;

	oa_VariantInit(dst);
	if (src->vt == vt)
		return oa_VariantCopy(dst, src);
	if (vt == VT_EMPTY)
		return S_OK;
	if (vt == VT_NULL) {
		if (src->vt != VT_EMPTY)
			return DISP_E_TYPEMISMATCH;
		dst->vt = VT_NULL;
		return S_OK;
	}
	if (src->vt == VT_NULL || src->vt == VT_ERROR ||
	    src->vt == VT_DECIMAL || src->vt == VT_RECORD ||
	    vt == VT_ERROR || vt == VT_DECIMAL || vt == VT_RECORD ||
	    vt == VT_VARIANT)
		return DISP_E_TYPEMISMATCH;

	/* Objects: to each other by QueryInterface, else by their value. */
	if (vt == VT_DISPATCH || vt == VT_UNKNOWN) {
		if (src->vt != VT_DISPATCH && src->vt != VT_UNKNOWN)
			return DISP_E_TYPEMISMATCH;
		void *p = NULL;
		if (src->punkVal &&
		    FAILED(hr = src->punkVal->lpVtbl->QueryInterface(
			    src->punkVal, vt == VT_DISPATCH ? &oa_IID_IDispatch :
			    &oa_IID_IUnknown, &p)))
			return hr;
		dst->vt = vt;
		dst->punkVal = p;
		return S_OK;
	}
	if (src->vt == VT_UNKNOWN)
		return DISP_E_TYPEMISMATCH;
	if (src->vt == VT_DISPATCH) {
		VARIANT val;
		if ((flags & VARIANT_NOVALUEPROP) || depth > 4)
			return DISP_E_TYPEMISMATCH;
		if (FAILED(hr = dispatch_value(src->pdispVal, lcid, &val)))
			return hr;
		VARIANT plain;
		oa_VariantInit(&plain);
		hr = oa_VariantCopyInd(&plain, &val);
		oa_VariantClear(&val);
		if (SUCCEEDED(hr))
			hr = coerce(dst, &plain, lcid, flags, vt, depth + 1);
		oa_VariantClear(&plain);
		return hr;
	}

	if (vt == VT_BSTR) {
		BSTR b = NULL;
		if (FAILED(hr = to_bstr(src, lcid, flags, &b)))
			return hr;
		dst->vt = VT_BSTR;
		dst->bstrVal = b;
		return S_OK;
	}
	if (vt == VT_DATE && src->vt == VT_BSTR) {
		double d;
		if (FAILED(hr = parse_date(src->bstrVal, flags, &d)))
			return hr;
		dst->vt = VT_DATE;
		dst->date = d;
		return S_OK;
	}
	if (vt == VT_BOOL && src->vt == VT_BSTR &&
	    bool_word(src->bstrVal, &dst->boolVal)) {
		dst->vt = VT_BOOL;
		return S_OK;
	}
	if (FAILED(hr = to_num(src, &n)))
		return hr;

	switch (vt) {
#define INT_TARGET(t, field, type, lo, hi, bits) \
	case t: \
		if (FAILED(hr = to_int(&n, lo, hi, bits, &u))) \
			return hr; \
		dst->field = (type)u; \
		break;
	INT_TARGET(VT_I1, cVal, signed char, -128, 127, 8)
	INT_TARGET(VT_UI1, bVal, BYTE, 0, 255, 8)
	INT_TARGET(VT_I2, iVal, SHORT, -32768, 32767, 16)
	INT_TARGET(VT_UI2, uiVal, USHORT, 0, 65535, 16)
	INT_TARGET(VT_I4, lVal, LONG, INT32_MIN, INT32_MAX, 32)
	INT_TARGET(VT_INT, intVal, INT, INT32_MIN, INT32_MAX, 32)
	INT_TARGET(VT_UI4, ulVal, ULONG, 0, UINT32_MAX, 32)
	INT_TARGET(VT_UINT, uintVal, UINT, 0, UINT32_MAX, 32)
	INT_TARGET(VT_I8, llVal, int64_t, INT64_MIN, INT64_MAX, 64)
	INT_TARGET(VT_UI8, ullVal, uint64_t, 0, UINT64_MAX, 64)
#undef INT_TARGET
	case VT_R4:
		if (fabs(n.d) > FLT_MAX && !isinf(n.d))
			return DISP_E_OVERFLOW;
		dst->fltVal = (FLOAT)n.d;
		break;
	case VT_R8:
		dst->dblVal = n.d;
		break;
	case VT_DATE:
		if (!(n.d >= DATE_MIN && n.d <= DATE_MAX + 0.0000001))
			return DISP_E_OVERFLOW;
		dst->date = n.d;
		break;
	case VT_CY: {
		int64_t c;
		if (FAILED(hr = to_cy(&n, &c)))
			return hr;
		dst->cyVal.int64 = c;
		break;
	}
	case VT_BOOL:
		dst->boolVal = (n.kind == N_DBL ? n.d != 0 :
				n.kind == N_UINT ? 1 : n.i != 0) ?
			       VARIANT_TRUE : VARIANT_FALSE;
		break;
	default:
		return DISP_E_TYPEMISMATCH;
	}
	dst->vt = vt;
	return S_OK;
}

/* Arrays: only to the same type, and between BSTRs and byte vectors. */
static HRESULT coerce_array(VARIANT *dst, const VARIANT *src, VARTYPE vt)
{
	oa_VariantInit(dst);
	if (src->vt == vt)
		return oa_VariantCopy(dst, src);
	if (src->vt == (VT_ARRAY | VT_UI1) && vt == VT_BSTR) {
		dst->vt = VT_BSTR;
		return oa_BstrFromVector(src->parray, &dst->bstrVal);
	}
	if (src->vt == VT_BSTR && vt == (VT_ARRAY | VT_UI1)) {
		dst->vt = vt;
		return oa_VectorFromBstr(src->bstrVal, &dst->parray);
	}
	return DISP_E_TYPEMISMATCH;
}

HRESULT STUB_WINAPI oa_VariantChangeTypeEx(VARIANTARG *dst,
					   const VARIANTARG *src, LCID lcid,
					   USHORT flags, VARTYPE vt)
{
	HRESULT hr;
	VARIANT plain, tmp;

	if (!dst || !src)
		return E_INVALIDARG;
	if ((src->vt & VT_BYREF) && !src->byref)
		return DISP_E_TYPEMISMATCH;
	if (FAILED(hr = oa_validate_vt(src->vt)) || FAILED(hr = oa_validate_vt(vt)))
		return hr;
	oa_VariantInit(&plain);
	if (FAILED(hr = oa_VariantCopyInd(&plain, src)))
		return hr;
	if ((plain.vt & VT_ARRAY) || (vt & VT_ARRAY))
		hr = coerce_array(&tmp, &plain, vt);
	else
		hr = coerce(&tmp, &plain, lcid, flags, vt, 0);
	oa_VariantClear(&plain);
	if (FAILED(hr)) {
		oa_VariantClear(&tmp);
		return hr;
	}
	/* dst may be src: plain is a copy, so clearing dst is safe. */
	if (FAILED(hr = oa_VariantClear(dst))) {
		oa_VariantClear(&tmp);
		return hr;
	}
	*dst = tmp;
	return S_OK;
}

/* The Var*From* family, each a VariantChangeType on one value. */

#define IN_UI1  BYTE
#define IN_I1   signed char
#define IN_I2   SHORT
#define IN_UI2  USHORT
#define IN_I4   LONG
#define IN_UI4  ULONG
#define IN_I8   int64_t
#define IN_UI8  uint64_t
#define IN_R4   FLOAT
#define IN_R8   double
#define IN_Date double
#define IN_Cy   int64_t
#define IN_Bool VARIANT_BOOL

#define OUT_UI1  BYTE
#define OUT_I1   signed char
#define OUT_I2   SHORT
#define OUT_UI2  USHORT
#define OUT_I4   LONG
#define OUT_UI4  ULONG
#define OUT_I8   int64_t
#define OUT_UI8  uint64_t
#define OUT_R4   FLOAT
#define OUT_R8   double
#define OUT_Date double
#define OUT_Cy   CY
#define OUT_Bool VARIANT_BOOL

#define VT_OF_UI1  VT_UI1
#define VT_OF_I1   VT_I1
#define VT_OF_I2   VT_I2
#define VT_OF_UI2  VT_UI2
#define VT_OF_I4   VT_I4
#define VT_OF_UI4  VT_UI4
#define VT_OF_I8   VT_I8
#define VT_OF_UI8  VT_UI8
#define VT_OF_R4   VT_R4
#define VT_OF_R8   VT_R8
#define VT_OF_Date VT_DATE
#define VT_OF_Cy   VT_CY
#define VT_OF_Bool VT_BOOL

#define SET_UI1(v, x)  ((v).bVal = (x))
#define SET_I1(v, x)   ((v).cVal = (CHAR)(x))
#define SET_I2(v, x)   ((v).iVal = (x))
#define SET_UI2(v, x)  ((v).uiVal = (x))
#define SET_I4(v, x)   ((v).lVal = (x))
#define SET_UI4(v, x)  ((v).ulVal = (x))
#define SET_I8(v, x)   ((v).llVal = (x))
#define SET_UI8(v, x)  ((v).ullVal = (x))
#define SET_R4(v, x)   ((v).fltVal = (x))
#define SET_R8(v, x)   ((v).dblVal = (x))
#define SET_Date(v, x) ((v).date = (x))
#define SET_Cy(v, x)   ((v).cyVal.int64 = (x))
#define SET_Bool(v, x) ((v).boolVal = (x))

#define GET_UI1(v)  ((v).bVal)
#define GET_I1(v)   ((signed char)(v).cVal)
#define GET_I2(v)   ((v).iVal)
#define GET_UI2(v)  ((v).uiVal)
#define GET_I4(v)   ((v).lVal)
#define GET_UI4(v)  ((v).ulVal)
#define GET_I8(v)   ((v).llVal)
#define GET_UI8(v)  ((v).ullVal)
#define GET_R4(v)   ((v).fltVal)
#define GET_R8(v)   ((v).dblVal)
#define GET_Date(v) ((v).date)
#define GET_Cy(v)   ((v).cyVal)
#define GET_Bool(v) ((v).boolVal)

static HRESULT var_from(VARIANT *s, VARTYPE vt, LCID lcid, ULONG flags,
			VARIANT *d)
{
	return coerce(d, s, lcid, (USHORT)flags, vt, 0);
}

#define DEF(to, from, ord) \
	WINFN(HRESULT) oa_Var##to##From##from(IN_##from in, OUT_##to *out) \
	{ \
		VARIANT s, d; \
		if (!out) \
			return E_INVALIDARG; \
		s.vt = VT_OF_##from; \
		SET_##from(s, in); \
		HRESULT hr = var_from(&s, VT_OF_##to, 0, 0, &d); \
		if (SUCCEEDED(hr)) \
			*out = GET_##to(d); \
		return hr; \
	}
#define DEF_STR(to, ord) \
	WINFN(HRESULT) oa_Var##to##FromStr(OLECHAR *str, LCID lcid, ULONG flags, \
					   OUT_##to *out) \
	{ \
		VARIANT s, d; \
		if (!out) \
			return E_INVALIDARG; \
		s.vt = VT_BSTR; \
		s.bstrVal = str; \
		HRESULT hr = var_from(&s, VT_OF_##to, lcid, flags, &d); \
		if (SUCCEEDED(hr)) \
			*out = GET_##to(d); \
		return hr; \
	}
#define DEF_DISP(to, ord) \
	WINFN(HRESULT) oa_Var##to##FromDisp(IDispatch *disp, LCID lcid, \
					    OUT_##to *out) \
	{ \
		VARIANT s, d; \
		if (!out) \
			return E_INVALIDARG; \
		s.vt = VT_DISPATCH; \
		s.pdispVal = disp; \
		HRESULT hr = var_from(&s, VT_OF_##to, lcid, 0, &d); \
		if (SUCCEEDED(hr)) \
			*out = GET_##to(d); \
		return hr; \
	}
#define DEF_BSTR(from, ord) \
	WINFN(HRESULT) oa_VarBstrFrom##from(IN_##from in, LCID lcid, \
					    ULONG flags, BSTR *out) \
	{ \
		VARIANT s, d; \
		if (!out) \
			return E_INVALIDARG; \
		s.vt = VT_OF_##from; \
		SET_##from(s, in); \
		HRESULT hr = var_from(&s, VT_BSTR, lcid, flags, &d); \
		if (SUCCEEDED(hr)) \
			*out = d.bstrVal; \
		return hr; \
	}

/* Generated from Wine's oleaut32.spec: each conversion and its ordinal. */
#define VAR_FROM_LIST(X, X_STR, X_DISP, X_BSTR) \
	X(I2, UI1, 48) X(I2, I4, 49) X(I2, R4, 50) X(I2, R8, 51) X(I2, Cy, 52) \
	X(I2, Date, 53) X_STR(I2, 54) X_DISP(I2, 55) X(I2, Bool, 56) \
	X(I4, UI1, 58) X(I4, I2, 59) X(I4, R4, 60) X(I4, R8, 61) X(I4, Cy, 62) \
	X(I4, Date, 63) X_STR(I4, 64) X_DISP(I4, 65) X(I4, Bool, 66) \
	X(R4, UI1, 68) X(R4, I2, 69) X(R4, I4, 70) X(R4, R8, 71) X(R4, Cy, 72) \
	X(R4, Date, 73) X_STR(R4, 74) X_DISP(R4, 75) X(R4, Bool, 76) \
	X(R8, UI1, 78) X(R8, I2, 79) X(R8, I4, 80) X(R8, R4, 81) X(R8, Cy, 82) \
	X(R8, Date, 83) X_STR(R8, 84) X_DISP(R8, 85) X(R8, Bool, 86) \
	X(Date, UI1, 88) X(Date, I2, 89) X(Date, I4, 90) X(Date, R4, 91) \
	X(Date, R8, 92) X(Date, Cy, 93) X_STR(Date, 94) X_DISP(Date, 95) \
	X(Date, Bool, 96) X(Cy, UI1, 98) X(Cy, I2, 99) X(Cy, I4, 100) \
	X(Cy, R4, 101) X(Cy, R8, 102) X(Cy, Date, 103) X_STR(Cy, 104) \
	X_DISP(Cy, 105) X(Cy, Bool, 106) X_BSTR(UI1, 108) X_BSTR(I2, 109) \
	X_BSTR(I4, 110) X_BSTR(R4, 111) X_BSTR(R8, 112) X_BSTR(Cy, 113) \
	X_BSTR(Date, 114) X(Bool, UI1, 118) X(Bool, I2, 119) X(Bool, I4, 120) \
	X(Bool, R4, 121) X(Bool, R8, 122) X(Bool, Date, 123) X(Bool, Cy, 124) \
	X_STR(Bool, 125) X_DISP(Bool, 126) X(UI1, I2, 130) X(UI1, I4, 131) \
	X(UI1, R4, 132) X(UI1, R8, 133) X(UI1, Cy, 134) X(UI1, Date, 135) \
	X_STR(UI1, 136) X_DISP(UI1, 137) X(UI1, Bool, 138) X(I2, I1, 205) \
	X(I2, UI2, 206) X(I2, UI4, 207) X(I4, I1, 209) X(I4, UI2, 210) \
	X(I4, UI4, 211) X(R4, I1, 213) X(R4, UI2, 214) X(R4, UI4, 215) \
	X(R8, I1, 217) X(R8, UI2, 218) X(R8, UI4, 219) X(Date, I1, 221) \
	X(Date, UI2, 222) X(Date, UI4, 223) X(Cy, I1, 225) X(Cy, UI2, 226) \
	X(Cy, UI4, 227) X_BSTR(I1, 229) X_BSTR(UI2, 230) X_BSTR(UI4, 231) \
	X(Bool, I1, 233) X(Bool, UI2, 234) X(Bool, UI4, 235) X(UI1, I1, 237) \
	X(UI1, UI2, 238) X(UI1, UI4, 239) X(I1, UI1, 244) X(I1, I2, 245) \
	X(I1, I4, 246) X(I1, R4, 247) X(I1, R8, 248) X(I1, Date, 249) \
	X(I1, Cy, 250) X_STR(I1, 251) X_DISP(I1, 252) X(I1, Bool, 253) \
	X(I1, UI2, 254) X(I1, UI4, 255) X(UI2, UI1, 257) X(UI2, I2, 258) \
	X(UI2, I4, 259) X(UI2, R4, 260) X(UI2, R8, 261) X(UI2, Date, 262) \
	X(UI2, Cy, 263) X_STR(UI2, 264) X_DISP(UI2, 265) X(UI2, Bool, 266) \
	X(UI2, I1, 267) X(UI2, UI4, 268) X(UI4, UI1, 270) X(UI4, I2, 271) \
	X(UI4, I4, 272) X(UI4, R4, 273) X(UI4, R8, 274) X(UI4, Date, 275) \
	X(UI4, Cy, 276) X_STR(UI4, 277) X_DISP(UI4, 278) X(UI4, Bool, 279) \
	X(UI4, I1, 280) X(UI4, UI2, 281) X(I8, UI1, 333) X(I8, I2, 334) \
	X(I8, R4, 335) X(I8, R8, 336) X(I8, Cy, 337) X(I8, Date, 338) \
	X_STR(I8, 339) X_DISP(I8, 340) X(I8, Bool, 341) X(I8, I1, 342) \
	X(I8, UI2, 343) X(I8, UI4, 344) X(I2, I8, 346) X(I2, UI8, 347) \
	X(I4, I8, 348) X(I4, UI8, 349) X(R4, I8, 360) X(R4, UI8, 361) \
	X(R8, I8, 362) X(R8, UI8, 363) X(Date, I8, 364) X(Date, UI8, 365) \
	X(Cy, I8, 366) X(Cy, UI8, 367) X_BSTR(I8, 368) X_BSTR(UI8, 369) \
	X(Bool, I8, 370) X(Bool, UI8, 371) X(UI1, I8, 372) X(UI1, UI8, 373) \
	X(I1, I8, 376) X(I1, UI8, 377) X(UI2, I8, 378) X(UI2, UI8, 379) \
	X(UI4, I8, 425) X(UI4, UI8, 426) X(I8, UI8, 427) X(UI8, I8, 428) \
	X(UI8, UI1, 429) X(UI8, I2, 430) X(UI8, R4, 431) X(UI8, R8, 432) \
	X(UI8, Cy, 433) X(UI8, Date, 434) X_STR(UI8, 435) X_DISP(UI8, 436) \
	X(UI8, Bool, 437) X(UI8, I1, 438) X(UI8, UI2, 439) X(UI8, UI4, 440)

VAR_FROM_LIST(DEF, DEF_STR, DEF_DISP, DEF_BSTR)

/* Always "True" or "False", as with VARIANT_ALPHABOOL. */
WINFN(HRESULT) oa_VarBstrFromBool(VARIANT_BOOL in, LCID lcid, ULONG flags,
				  BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	return (*out = bstr_ascii(in ? "True" : "False")) ? S_OK :
	       E_OUTOFMEMORY;
}

WINFN(HRESULT) oa_VarBstrFromDisp(IDispatch *disp, LCID lcid, ULONG flags,
				  BSTR *out)
{
	VARIANT s, d;

	if (!out)
		return E_INVALIDARG;
	s.vt = VT_DISPATCH;
	s.pdispVal = disp;
	HRESULT hr = var_from(&s, VT_BSTR, lcid, flags, &d);
	if (SUCCEEDED(hr))
		*out = d.bstrVal;
	return hr;
}

#define ENTRY(to, from, ord) \
	{ "Var" #to "From" #from, ord, (void *)oa_Var##to##From##from },
#define ENTRY_STR(to, ord) \
	{ "Var" #to "FromStr", ord, (void *)oa_Var##to##FromStr },
#define ENTRY_DISP(to, ord) \
	{ "Var" #to "FromDisp", ord, (void *)oa_Var##to##FromDisp },
#define ENTRY_BSTR(from, ord) \
	{ "VarBstrFrom" #from, ord, (void *)oa_VarBstrFrom##from },

const struct builtin_export oa_conv_exports[] = {
	VAR_FROM_LIST(ENTRY, ENTRY_STR, ENTRY_DISP, ENTRY_BSTR)
	{ "VarBstrFromBool", 116, (void *)oa_VarBstrFromBool },
	{ "VarBstrFromDisp", 115, (void *)oa_VarBstrFromDisp },
	{ "VariantTimeToDosDateTime", 13, (void *)oa_VariantTimeToDosDateTime },
	{ "DosDateTimeToVariantTime", 14, (void *)oa_DosDateTimeToVariantTime },
	{ "SystemTimeToVariantTime", 184, (void *)oa_SystemTimeToVariantTime },
	{ "VariantTimeToSystemTime", 185, (void *)oa_VariantTimeToSystemTime },
	BUILTIN_END
};
