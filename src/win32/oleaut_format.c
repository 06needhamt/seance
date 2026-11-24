/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's formatting: VarFormat with Visual Basic's Format strings,
 * VarFormatNumber/Currency/Percent/DateTime, VarWeekdayName/MonthName and
 * the token forms (VarTokenizeFormatString, VarFormatFromTokens), all in
 * US English whatever the LCID, as the conversions are.
 *
 * - Named formats: General Number, Currency, Fixed, Standard, Percent,
 *   Scientific, Yes/No, True/False, On/Off, General/Long/Medium/Short
 *   Date, Long/Medium/Short Time.
 * - Numbers: up to four sections (positive; negative; zero; Null), with
 *   0 and # digits, a decimal point, thousands separators and scaling
 *   commas, %, E+/E-/e+/e- exponents, and literals ("text", \c, and the
 *   characters that stand for themselves). Halves round away from zero,
 *   as Format does (not to even, as Round does).
 * - Dates and times: c, d to dddddd, w, ww, m to mmmm (minutes after h),
 *   q, y, yy, yyyy, h, hh, n, nn, s, ss, ttttt, AM/PM, am/pm, A/P, a/p,
 *   AMPM, : and /.
 * - Strings: @ and & placeholders, < and >, and ! to fill from the left.
 *
 * Tokenized formats are opaque to callers: the buffer holds the format
 * itself with its first day and first week.
 */
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/oleaut.h"

#define US_LCID 0x0409

/* ---- a growing UTF-8 string ---- */

struct out {
	char *s;
	size_t n, cap;
	int bad;
};

static void put_n(struct out *o, const char *s, size_t n)
{
	if (o->n + n + 1 > o->cap) {
		size_t cap = (o->cap ? o->cap * 2 : 64) + n;
		char *p = realloc(o->s, cap);
		if (!p) {
			o->bad = 1;
			return;
		}
		o->s = p;
		o->cap = cap;
	}
	memcpy(o->s + o->n, s, n);
	o->n += n;
	o->s[o->n] = '\0';
}

static void put(struct out *o, const char *s)
{
	put_n(o, s, strlen(s));
}

static void putc_(struct out *o, char c)
{
	put_n(o, &c, 1);
}

static HRESULT finish(struct out *o, BSTR *res)
{
	if (o->bad) {
		free(o->s);
		return E_OUTOFMEMORY;
	}
	*res = oa_bstr_utf8(o->s ? o->s : "");
	free(o->s);
	return *res ? S_OK : E_OUTOFMEMORY;
}

/* ---- names ---- */

static const char *const day_names[] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
	"Saturday",
};
static const char *const month_names[] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December",
};

static void put_name(struct out *o, const char *name, int abbrev)
{
	put_n(o, name, abbrev ? 3 : strlen(name));
}

/* ---- dates ---- */

struct when {
	long year;
	int month, day, hour, minute, second, wday, yday;
};

static long days_from_civil(long y, int m, int d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	long yoe = y - era * 400;
	long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, long *y, int *m, int *d)
{
	z += 719468;
	long era = (z >= 0 ? z : z - 146096) / 146097;
	long doe = z - era * 146097;
	long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	long mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = yoe + era * 400 + (*m <= 2);
}

/* 30 December 1899, day 0 of a DATE, in days since 1970. */
#define DATE_EPOCH (-25569L)

static int split_when(double v, struct when *w)
{
	if (!(v >= -657434.0 && v < 2958466.0))
		return 0;
	long day = (long)v;		/* toward zero: the fraction is the time */
	long secs = lround(fabs(v - (double)day) * 86400.0);
	if (secs >= 86400) {
		secs -= 86400;
		day++;
	}
	long unix_day = day + DATE_EPOCH;
	civil_from_days(unix_day, &w->year, &w->month, &w->day);
	w->hour = (int)(secs / 3600);
	w->minute = (int)(secs / 60 % 60);
	w->second = (int)(secs % 60);
	w->wday = (int)(((unix_day % 7) + 11) % 7);	/* 1970-01-01: Thu */
	w->yday = (int)(unix_day - days_from_civil(w->year, 1, 1)) + 1;
	return 1;
}

/* The day of the week counted from first_day (1 Sunday .. 7 Saturday). */
static int weekday_from(const struct when *w, int first_day)
{
	return (w->wday - (first_day - 1) + 7) % 7 + 1;
}

/*
 * The week of the year: first_week 1 starts it on 1 January, 2 with the
 * first week of four days, 3 with the first whole week.
 */
static int week_of_year(const struct when *w, int first_day, int first_week)
{
	struct when jan1 = *w;
	jan1.wday = (w->wday - (w->yday - 1) % 7 + 7) % 7;
	int lead = weekday_from(&jan1, first_day) - 1;	/* days before 1 Jan */
	int week = (w->yday - 1 + lead) / 7 + 1;
	if (first_week == 2 && lead > 3)
		week--;
	else if (first_week == 3 && lead > 0)
		week--;
	if (week == 0) {		/* the last week of the year before */
		long y = w->year - 1;
		long ld = days_from_civil(y, 12, 31);
		struct when prev = *w;
		prev.year = y;
		prev.month = 12;
		prev.day = 31;
		prev.yday = (int)(ld - days_from_civil(y, 1, 1)) + 1;
		prev.wday = (w->wday - w->yday % 7 + 7) % 7;
		return week_of_year(&prev, first_day, first_week);
	}
	return week;
}

/* ---- format strings ---- */

/*
 * The format's sections, split at semicolons outside quotes, as UTF-8.
 * Returns how many there are (at most 4).
 */
static int split_sections(const char *f, char *sec[4])
{
	int count = 0, quote = 0;
	const char *start = f;

	for (const char *p = f;; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p == '"')
			quote = !quote;
		if (!*p || (*p == ';' && !quote)) {
			if (count < 4) {
				size_t n = (size_t)(p - start);
				sec[count] = malloc(n + 1);
				if (sec[count]) {
					memcpy(sec[count], start, n);
					sec[count][n] = '\0';
				}
				count++;
			}
			if (!*p)
				break;
			start = p + 1;
		}
	}
	return count;
}

static void free_sections(char *sec[4], int n)
{
	for (int i = 0; i < n; i++)
		free(sec[i]);
}

static int ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
	return !*a && !*b;
}

static int istarts(const char *s, const char *prefix)
{
	for (; *prefix; s++, prefix++)
		if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
			return 0;
	return 1;
}

/* What kind of format a section is. */
enum kind { K_NUMBER, K_DATE, K_STRING, K_TEXT };

static enum kind section_kind(const char *f)
{
	int date = 0, number = 0, string = 0;
	for (const char *p = f; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				;
			if (!*p)
				break;
			continue;
		}
		switch (tolower((unsigned char)*p)) {
		case '0': case '#': case '%':
			number = 1;
			break;
		case 'e':
			if (p[1] == '+' || p[1] == '-')
				number = 1;
			break;
		case '@': case '&': case '<': case '>': case '!':
			string = 1;
			break;
		case 'c': case 'd': case 'w': case 'm': case 'q': case 'y':
		case 'h': case 'n': case 's': case 't': case '/': case ':':
			date = 1;
			break;
		case 'a':
			if (istarts(p, "am/pm") || istarts(p, "a/p") ||
			    istarts(p, "ampm"))
				date = 1;
			break;
		}
	}
	return number ? K_NUMBER : string ? K_STRING : date ? K_DATE : K_TEXT;
}

/* ---- numbers as decimal digits ---- */

struct dnum {
	int neg;
	char d[80];	/* significant digits, no leading zeros */
	int n;
	int exp;	/* the value is 0.d[0]d[1]... times 10^exp */
};

static void dnum_from_text(const char *t, struct dnum *x)
{
	int seen_point = 0, e = 0;

	memset(x, 0, sizeof(*x));
	if (*t == '-') {
		x->neg = 1;
		t++;
	}
	for (; *t; t++) {
		if (*t == '.') {
			seen_point = 1;
		} else if (*t >= '0' && *t <= '9') {
			if (!x->n && *t == '0') {
				if (seen_point)
					x->exp--;
				continue;
			}
			if (x->n < (int)sizeof(x->d) - 1)
				x->d[x->n++] = *t;
			if (!seen_point)
				x->exp++;
		} else if (*t == 'e' || *t == 'E') {
			e = atoi(t + 1);
			break;
		}
	}
	while (x->n && x->d[x->n - 1] == '0')
		x->n--;
	x->exp += e;
	if (!x->n) {
		x->exp = 0;
		x->neg = 0;
	}
}

/* The value's digits: 15 significant for a double, 7 for a single. */
static HRESULT dnum_from_variant(const VARIANT *v, struct dnum *x)
{
	char t[96];
	VARIANT tmp;
	HRESULT hr;

	switch (v->vt) {
	case VT_R4:
		snprintf(t, sizeof(t), "%.6e", v->fltVal);
		break;
	case VT_R8:
	case VT_DATE:
		snprintf(t, sizeof(t), "%.14e",
			 v->vt == VT_R8 ? v->dblVal : v->date);
		break;
	case VT_CY: {
		int64_t c = v->cyVal.int64;
		uint64_t a = c < 0 ? -(uint64_t)c : (uint64_t)c;
		snprintf(t, sizeof(t), "%s%llu.%04u", c < 0 ? "-" : "",
			 (unsigned long long)(a / 10000), (unsigned)(a % 10000));
		break;
	}
	case VT_BOOL:
		strcpy(t, v->boolVal ? "-1" : "0");
		break;
	case VT_EMPTY:
		strcpy(t, "0");
		break;
	case VT_BSTR:
		oa_VariantInit(&tmp);
		if ((hr = oa_VariantChangeTypeEx(&tmp, (VARIANT *)v, US_LCID, 0,
						 VT_R8)) != S_OK)
			return hr;
		snprintf(t, sizeof(t), "%.14e", tmp.dblVal);
		break;
	default:
		oa_VariantInit(&tmp);
		if ((hr = oa_VariantChangeTypeEx(&tmp, (VARIANT *)v, US_LCID, 0,
						 VT_BSTR)) != S_OK)
			return hr;
		char *u = utf16_to_utf8_dup(tmp.bstrVal);
		oa_VariantClear(&tmp);
		if (!u)
			return E_OUTOFMEMORY;
		snprintf(t, sizeof(t), "%s", u);
		free(u);
		break;
	}
	dnum_from_text(t, x);
	return S_OK;
}

static int dnum_zero(const struct dnum *x)
{
	return !x->n;
}

/* Rounded, halves away from zero, to keep digits after the point. */
static void dnum_round(struct dnum *x, int keep)
{
	int cut = x->exp + keep;	/* digits that survive */

	if (cut >= x->n)
		return;
	if (cut < 0) {
		x->n = 0;
		x->exp = 0;
		return;
	}
	int up = x->d[cut] >= '5';
	x->n = cut;
	if (up) {
		int i = cut - 1;
		for (; i >= 0 && x->d[i] == '9'; i--)
			x->n--;
		if (i < 0) {
			x->d[0] = '1';
			x->n = 1;
			x->exp++;
		} else {
			x->d[i]++;
		}
	}
	while (x->n && x->d[x->n - 1] == '0')
		x->n--;
	if (!x->n)
		x->exp = 0;
}

/* The digit at 10^pos (0 the units). */
static char dnum_digit(const struct dnum *x, int pos)
{
	int i = x->exp - 1 - pos;
	return i >= 0 && i < x->n ? x->d[i] : '0';
}

/* The number of integer digits (0 if below 1). */
static int dnum_int_digits(const struct dnum *x)
{
	return x->n && x->exp > 0 ? x->exp : 0;
}

/* ---- number sections ---- */

struct numfmt {
	int int_ph;		/* integer placeholders */
	int int_min;		/* digits shown at least (from the first 0) */
	int frac_ph;		/* fraction placeholders */
	int frac_min;		/* fraction digits always shown */
	int point, group, scale, percent;
	int exp;		/* 0, or the E's position: then exp_sign, exp_min */
	int exp_plus, exp_min;
};

static void scan_number(const char *f, struct numfmt *nf)
{
	int in_frac = 0, in_exp = 0, last_was_digit = 0, commas = 0;
	int first_zero = -1, idx = 0;
	int frac_last_zero = 0;

	memset(nf, 0, sizeof(*nf));
	for (const char *p = f; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			last_was_digit = 0;
			continue;
		}
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				;
			if (!*p)
				break;
			last_was_digit = 0;
			continue;
		}
		if (in_exp) {
			if (*p == '0' || *p == '#')
				nf->exp_min += *p == '0';
			continue;
		}
		if (*p == '0' || *p == '#') {
			if (commas && last_was_digit && !in_frac)
				nf->group = 1;
			commas = 0;
			if (in_frac) {
				nf->frac_ph++;
				if (*p == '0')
					frac_last_zero = nf->frac_ph;
			} else {
				if (*p == '0' && first_zero < 0)
					first_zero = idx;
				nf->int_ph++;
				idx++;
			}
			last_was_digit = 1;
		} else if (*p == ',' && !in_frac) {
			commas++;
		} else if (*p == '.' && !in_frac) {
			nf->scale += commas;	/* commas just before the point */
			commas = 0;
			in_frac = 1;
			nf->point = 1;
			last_was_digit = 0;
		} else if ((*p == 'E' || *p == 'e') &&
			   (p[1] == '+' || p[1] == '-')) {
			nf->exp = 1;
			nf->exp_plus = p[1] == '+';
			in_exp = 1;
			p++;
		} else {
			if (*p == '%')
				nf->percent++;
			last_was_digit = 0;
		}
	}
	if (!in_frac)
		nf->scale += commas;	/* commas ending the digits */
	nf->int_min = first_zero < 0 ? 0 : nf->int_ph - first_zero;
	nf->frac_min = frac_last_zero;
}

/*
 * The integer digits one placeholder (the done'th) shows: placeholder k
 * stands for the digit at 10^(P-1-k), and the first one also shows all
 * the digits there are no placeholders for.
 */
static void put_int_digits(struct out *o, const struct dnum *x,
			   const struct numfmt *nf, int total, int *done)
{
	int k = (*done)++, p = nf->int_ph;
	int lo = p - 1 - k, hi = k ? lo : (total > p ? total : p) - 1;

	for (int pos = hi; pos >= lo; pos--) {
		if (pos >= total)
			continue;
		putc_(o, dnum_digit(x, pos));
		if (nf->group && pos > 0 && pos % 3 == 0)
			putc_(o, ',');
	}
}

static void format_number(const char *f, struct dnum x, int show_minus,
			  struct out *o)
{
	struct numfmt nf;
	int e = 0;

	scan_number(f, &nf);
	x.exp += 2 * nf.percent - 3 * nf.scale;
	if (!x.n)
		x.exp = 0;

	if (nf.exp && x.n) {
		int lead = nf.int_ph ? nf.int_ph : 1;
		e = x.exp - lead;
		x.exp = lead;
		dnum_round(&x, nf.frac_ph);
		if (x.exp > lead) {	/* rounding carried: 9.99 to 10.0 */
			x.exp--;
			e++;
		}
	} else {
		dnum_round(&x, nf.frac_ph);
	}
	if (dnum_zero(&x))
		show_minus = 0;

	int total = dnum_int_digits(&x);
	if (total < nf.int_min)
		total = nf.int_min;
	if (!nf.int_ph && total)	/* no room: the integer digits vanish */
		total = 0;
	int frac_shown = nf.frac_ph;
	while (frac_shown > nf.frac_min &&
	       dnum_digit(&x, -frac_shown) == '0')
		frac_shown--;

	if (show_minus)
		putc_(o, '-');
	int done = 0, frac_done = 0, in_frac = 0;
	for (const char *p = f; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			put_n(o, p, 1);
			continue;
		}
		if (*p == '"') {
			const char *q = p + 1;
			while (*q && *q != '"')
				q++;
			put_n(o, p + 1, (size_t)(q - p - 1));
			if (!*q)
				break;
			p = q;
			continue;
		}
		if ((*p == '0' || *p == '#') && !in_frac) {
			put_int_digits(o, &x, &nf, total, &done);
		} else if ((*p == '0' || *p == '#') && in_frac) {
			frac_done++;
			if (frac_done <= frac_shown)
				putc_(o, dnum_digit(&x, -frac_done));
		} else if (*p == '.' && !in_frac) {
			in_frac = 1;
			putc_(o, '.');
		} else if (*p == ',' && !in_frac) {
			/* grouping and scaling: shown by the digits */
		} else if ((*p == 'E' || *p == 'e') &&
			   (p[1] == '+' || p[1] == '-')) {
			char buf[16];
			putc_(o, *p);
			if (e < 0)
				putc_(o, '-');
			else if (nf.exp_plus)
				putc_(o, '+');
			snprintf(buf, sizeof(buf), "%0*d",
				 nf.exp_min > 4 ? 4 : nf.exp_min,
				 e < 0 ? -e : e);
			put(o, buf);
			/* skip the sign and the exponent's placeholders */
			for (p += 2; *p == '0' || *p == '#'; p++)
				;
			p--;
		} else {
			put_n(o, p, 1);
		}
	}
	(void)frac_done;
}

/* ---- date sections ---- */

struct datefmt {
	int first_day, first_week;
};

static void put_num(struct out *o, long v, int width)
{
	char buf[24];
	snprintf(buf, sizeof(buf), "%0*ld", width > 4 ? 4 : width, v);
	put(o, buf);
}

static void put_date_part(struct out *o, const char *f,
			  const struct when *w, const struct datefmt *df);

static void put_short_date(struct out *o, const struct when *w)
{
	put_num(o, w->month, 1);
	putc_(o, '/');
	put_num(o, w->day, 1);
	putc_(o, '/');
	put_num(o, w->year, 4);
}

static void put_long_date(struct out *o, const struct when *w)
{
	put(o, day_names[w->wday]);
	put(o, ", ");
	put(o, month_names[w->month - 1]);
	putc_(o, ' ');
	put_num(o, w->day, 1);
	put(o, ", ");
	put_num(o, w->year, 4);
}

static void put_long_time(struct out *o, const struct when *w)
{
	put_num(o, w->hour % 12 ? w->hour % 12 : 12, 1);
	putc_(o, ':');
	put_num(o, w->minute, 2);
	putc_(o, ':');
	put_num(o, w->second, 2);
	put(o, w->hour < 12 ? " AM" : " PM");
}

static void put_general(struct out *o, double v, const struct when *w)
{
	long day = (long)v;
	int time = w->hour || w->minute || w->second;
	if (day || !time)
		put_short_date(o, w);
	if (day && time)
		putc_(o, ' ');
	if (time)
		put_long_time(o, w);
}

/* Whether the section has an AM/PM marker, which makes hours 12-hour. */
static int has_ampm(const char *f)
{
	for (const char *p = f; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				;
			if (!*p)
				break;
			continue;
		}
		if (istarts(p, "am/pm") || istarts(p, "a/p") ||
		    istarts(p, "ampm"))
			return 1;
	}
	return 0;
}

static int run_of(const char *p, char c)
{
	int n = 0;
	while (tolower((unsigned char)p[n]) == c)
		n++;
	return n;
}

static void format_date(const char *f, double v, const struct datefmt *df,
			struct out *o)
{
	struct when w;

	if (!split_when(v, &w)) {
		o->bad = 1;
		return;
	}
	put_date_part(o, f, &w, df);
	(void)v;
}

static void put_date_part(struct out *o, const char *f,
			  const struct when *w, const struct datefmt *df)
{
	int twelve = has_ampm(f);
	int after_hour = 0;
	double v = (double)(days_from_civil(w->year, w->month, w->day) -
			    DATE_EPOCH) +
		   (w->hour * 3600 + w->minute * 60 + w->second) / 86400.0;

	for (const char *p = f; *p;) {
		char c = (char)tolower((unsigned char)*p);
		int n;

		if (*p == '\\' && p[1]) {
			put_n(o, p + 1, 1);
			p += 2;
			continue;
		}
		if (*p == '"') {
			const char *q = p + 1;
			while (*q && *q != '"')
				q++;
			put_n(o, p + 1, (size_t)(q - p - 1));
			p = *q ? q + 1 : q;
			continue;
		}
		if (istarts(p, "am/pm") || istarts(p, "a/p")) {
			int full = istarts(p, "am/pm");
			int pm = w->hour >= 12;
			/* The case of the marker written. */
			const char *src = pm ? p + (full ? 3 : 2) : p;
			put_n(o, src, full ? 2 : 1);
			p += full ? 5 : 3;
			continue;
		}
		if (istarts(p, "ampm")) {
			put(o, w->hour < 12 ? "AM" : "PM");
			p += 4;
			continue;
		}
		if (istarts(p, "ttttt")) {
			put_long_time(o, w);
			p += 5;
			after_hour = 0;
			continue;
		}
		switch (c) {
		case 'c':
			put_general(o, v, w);
			p++;
			continue;
		case 'd':
			n = run_of(p, 'd');
			p += n;
			if (n == 1 || n == 2)
				put_num(o, w->day, n);
			else if (n == 3 || n == 4)
				put_name(o, day_names[w->wday], n == 3);
			else if (n == 5)
				put_short_date(o, w);
			else
				put_long_date(o, w);
			continue;
		case 'w':
			n = run_of(p, 'w');
			p += n;
			if (n == 1)
				put_num(o, weekday_from(w, df->first_day), 1);
			else
				put_num(o, week_of_year(w, df->first_day,
							df->first_week), 1);
			continue;
		case 'm':
			n = run_of(p, 'm');
			p += n;
			if (after_hour && n <= 2) {	/* minutes */
				put_num(o, w->minute, n);
				after_hour = 0;
			} else if (n <= 2) {
				put_num(o, w->month, n);
			} else {
				put_name(o, month_names[w->month - 1], n == 3);
			}
			continue;
		case 'q':
			p++;
			put_num(o, (w->month - 1) / 3 + 1, 1);
			continue;
		case 'y':
			n = run_of(p, 'y');
			p += n;
			if (n == 1)
				put_num(o, w->yday, 1);
			else if (n == 2)
				put_num(o, w->year % 100, 2);
			else
				put_num(o, w->year, 4);
			continue;
		case 'h': {
			n = run_of(p, 'h');
			p += n;
			int h = w->hour;
			if (twelve)
				h = h % 12 ? h % 12 : 12;
			put_num(o, h, n >= 2 ? 2 : 1);
			after_hour = 1;
			continue;
		}
		case 'n':
			n = run_of(p, 'n');
			p += n;
			put_num(o, w->minute, n >= 2 ? 2 : 1);
			after_hour = 0;
			continue;
		case 's':
			n = run_of(p, 's');
			p += n;
			put_num(o, w->second, n >= 2 ? 2 : 1);
			continue;
		}
		if (*p != ':' && *p != ' ')
			after_hour = 0;
		put_n(o, p, 1);
		p++;
	}
}

/* ---- string sections ---- */

static HRESULT format_string(const char *f, const WCHAR *s, BSTR *res)
{
	WCHAR *fw = utf8_to_utf16_dup(f);
	size_t slen = s ? wstrlen(s) : 0, ph = 0;
	int lower = 0, upper = 0, left = 0;
	HRESULT hr = E_OUTOFMEMORY;

	if (!fw)
		return E_OUTOFMEMORY;
	for (WCHAR *p = fw; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				;
			if (!*p)
				break;
			continue;
		}
		if (*p == '@' || *p == '&')
			ph++;
		else if (*p == '<')
			lower = 1;
		else if (*p == '>')
			upper = 1;
		else if (*p == '!')
			left = 1;
	}
	size_t cap = slen + wstrlen(fw) + 1;
	WCHAR *buf = malloc(cap * sizeof(WCHAR)), *o = buf;
	if (!buf) {
		free(fw);
		return hr;
	}
	/* Characters for each placeholder: the excess at the first (or the
	 * last, filling from the left). */
	size_t next = 0, seen = 0;
	size_t blank = ph > slen ? ph - slen : 0;	/* placeholders with none */
	for (WCHAR *p = fw; *p; p++) {
		if (*p == '\\' && p[1]) {
			*o++ = *++p;
			continue;
		}
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				*o++ = *p;
			if (!*p)
				break;
			continue;
		}
		if (*p == '<' || *p == '>' || *p == '!')
			continue;
		if (*p == '@' || *p == '&') {
			size_t take;
			seen++;
			if (!left) {
				if (seen <= blank) {
					take = 0;
				} else if (seen == blank + 1 && slen > ph) {
					take = slen - ph + 1;
				} else {
					take = 1;
				}
			} else {
				if (seen > slen)
					take = 0;
				else if (seen == ph && slen > ph)
					take = slen - ph + 1;
				else
					take = 1;
			}
			if (!take && *p == '@')
				*o++ = ' ';
			for (size_t i = 0; i < take && next < slen; i++)
				*o++ = s[next++];
			continue;
		}
		*o++ = *p;
	}
	if (!ph)		/* no placeholders: the string after the text */
		while (next < slen && (size_t)(o - buf) < cap - 1)
			*o++ = s[next++];
	*o = 0;
	for (WCHAR *q = buf; q < o; q++) {
		if (upper)
			*q = k32_toupper_w(*q);
		else if (lower)
			*q = k32_tolower_w(*q);
	}
	*res = oa_SysAllocStringLen(buf, (UINT)(o - buf));
	hr = *res ? S_OK : E_OUTOFMEMORY;
	free(buf);
	free(fw);
	return hr;
}

/* ---- VarFormat ---- */

static const struct {
	const char *name, *format;
} named[] = {
	{ "Currency", "$#,##0.00;($#,##0.00)" },
	{ "Fixed", "0.00" },
	{ "Standard", "#,##0.00" },
	{ "Percent", "0.00%" },
	{ "Scientific", "0.00E+00" },
	{ "Long Date", "dddd, mmmm d, yyyy" },
	{ "Medium Date", "dd-mmm-yy" },
	{ "Short Date", "m/d/yyyy" },
	{ "Long Time", "h:nn:ss AM/PM" },
	{ "Medium Time", "hh:nn AM/PM" },
	{ "Short Time", "hh:nn" },
	{ "General Date", "c" },
};

static HRESULT plain_text(const VARIANT *v, BSTR *res)
{
	VARIANT t;
	HRESULT hr;

	if (v->vt == VT_NULL || v->vt == VT_EMPTY) {
		*res = oa_SysAllocStringLen(NULL, 0);
		return *res ? S_OK : E_OUTOFMEMORY;
	}
	oa_VariantInit(&t);
	hr = oa_VariantChangeTypeEx(&t, (VARIANT *)v, US_LCID,
				    v->vt == VT_BOOL ? VARIANT_ALPHABOOL : 0,
				    VT_BSTR);
	if (hr == S_OK)
		*res = t.bstrVal;
	return hr;
}

static int is_number_vt(VARTYPE vt)
{
	switch (vt) {
	case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4:
	case VT_UI4: case VT_INT: case VT_UINT: case VT_I8: case VT_UI8:
	case VT_R4: case VT_R8: case VT_CY: case VT_DECIMAL: case VT_BOOL:
	case VT_EMPTY:
		return 1;
	}
	return 0;
}

static HRESULT format_value(const VARIANT *in, const WCHAR *fmt,
			    int first_day, int first_week, BSTR *res)
{
	VARIANT v;
	HRESULT hr;
	char *f8 = NULL, *sec[4] = { 0 };
	int nsec = 0;
	struct out o = { 0 };
	struct datefmt df = { first_day ? first_day : 1,
			      first_week ? first_week : 1 };

	if (!res)
		return E_INVALIDARG;
	*res = NULL;
	if (!in || first_day < 0 || first_day > 7 || first_week < 0 ||
	    first_week > 3)
		return E_INVALIDARG;
	oa_VariantInit(&v);
	if ((hr = oa_VariantCopyInd(&v, in)) != S_OK)
		return hr;
	if (v.vt == VT_DISPATCH) {
		VARIANT t;
		oa_VariantInit(&t);
		hr = oa_VariantChangeTypeEx(&t, &v, US_LCID, 0, VT_BSTR);
		oa_VariantClear(&v);
		if (hr != S_OK)
			return hr;
		v = t;
	}
	if (v.vt & (VT_ARRAY | VT_VECTOR) || v.vt == VT_UNKNOWN ||
	    v.vt == VT_ERROR) {
		oa_VariantClear(&v);
		return DISP_E_TYPEMISMATCH;
	}
	if (!fmt || !*fmt) {
		hr = plain_text(&v, res);
		goto done;
	}
	f8 = utf16_to_utf8_dup(fmt);
	if (!f8) {
		hr = E_OUTOFMEMORY;
		goto done;
	}

	/* Named formats. */
	if (ieq(f8, "General Number")) {
		if (v.vt == VT_DATE || v.vt == VT_BOOL) {
			VARIANT t;
			oa_VariantInit(&t);
			if ((hr = oa_VariantChangeTypeEx(&t, &v, US_LCID, 0,
							 VT_R8)) != S_OK)
				goto done;
			v = t;
		}
		hr = plain_text(&v, res);
		goto done;
	}
	if (ieq(f8, "Yes/No") || ieq(f8, "True/False") || ieq(f8, "On/Off")) {
		VARIANT t;
		static const char *const words[3][2] = {
			{ "No", "Yes" }, { "False", "True" }, { "Off", "On" },
		};
		int which = ieq(f8, "Yes/No") ? 0 : ieq(f8, "True/False") ? 1 : 2;
		if (v.vt == VT_NULL || v.vt == VT_EMPTY) {
			hr = plain_text(&v, res);
			goto done;
		}
		oa_VariantInit(&t);
		if ((hr = oa_VariantChangeTypeEx(&t, &v, US_LCID, 0,
						 VT_BOOL)) != S_OK)
			goto done;
		*res = oa_bstr_utf8(words[which][t.boolVal != 0]);
		hr = *res ? S_OK : E_OUTOFMEMORY;
		goto done;
	}
	for (size_t i = 0; i < sizeof(named) / sizeof(*named); i++)
		if (ieq(f8, named[i].name)) {
			char *copy = strdup(named[i].format);
			if (!copy) {
				hr = E_OUTOFMEMORY;
				goto done;
			}
			free(f8);
			f8 = copy;
			break;
		}

	nsec = split_sections(f8, sec);
	for (int i = 0; i < nsec; i++)
		if (!sec[i]) {
			hr = E_OUTOFMEMORY;
			goto done;
		}
	enum kind kind = section_kind(sec[0]);

	if (v.vt == VT_NULL) {
		if (nsec >= 4)
			format_number(sec[3], (struct dnum){ 0 }, 0, &o);
		hr = finish(&o, res);
		goto done;
	}

	/* A string: as text if the format is for text, else read. */
	if (v.vt == VT_BSTR) {
		VARIANT t;
		if (kind == K_STRING || kind == K_TEXT) {
			hr = format_string(sec[0], v.bstrVal, res);
			goto done;
		}
		oa_VariantInit(&t);
		hr = oa_VariantChangeTypeEx(&t, &v, US_LCID, 0,
					    kind == K_DATE ? VT_DATE : VT_R8);
		if (hr != S_OK) {
			/* Not a number or date: the string as it is. */
			*res = oa_SysAllocStringLen(v.bstrVal,
						    oa_SysStringLen(v.bstrVal));
			hr = *res ? S_OK : E_OUTOFMEMORY;
			goto done;
		}
		oa_VariantClear(&v);
		v = t;
	}

	if (kind == K_STRING) {
		BSTR text;
		if ((hr = plain_text(&v, &text)) != S_OK)
			goto done;
		hr = format_string(sec[0], text, res);
		oa_SysFreeString(text);
		goto done;
	}
	if (kind == K_DATE || (kind == K_TEXT && v.vt == VT_DATE)) {
		VARIANT t;
		oa_VariantInit(&t);
		if ((hr = oa_VariantChangeTypeEx(&t, &v, US_LCID, 0,
						 VT_DATE)) != S_OK)
			goto done;
		format_date(sec[0], t.date, &df, &o);
		if (o.bad && !o.s) {
			hr = DISP_E_OVERFLOW;
			goto done;
		}
		hr = finish(&o, res);
		goto done;
	}

	/* A number. */
	struct dnum x;
	if (!is_number_vt(v.vt) && v.vt != VT_DATE) {
		hr = DISP_E_TYPEMISMATCH;
		goto done;
	}
	if ((hr = dnum_from_variant(&v, &x)) != S_OK)
		goto done;
	const char *use = sec[0];
	int minus = x.neg;
	if (dnum_zero(&x) && nsec >= 3 && *sec[2]) {
		use = sec[2];
	} else if (x.neg && nsec >= 2 && *sec[1]) {
		use = sec[1];
		minus = 0;
	}
	x.neg = 0;
	format_number(use, x, minus, &o);
	hr = finish(&o, res);
done:
	free_sections(sec, nsec);
	free(f8);
	oa_VariantClear(&v);
	return hr;
}

WINFN(HRESULT) oa_VarFormat(LPVARIANT in, LPOLESTR fmt, int first_day,
			    int first_week, ULONG flags, BSTR *out)
{
	return format_value(in, fmt, first_day, first_week, out);
}

/* The token buffer: a tag, the first day and week, then the format. */
#define TOKEN_TAG 0x544d4653u	/* "SFMT" */

struct tokens {
	DWORD tag;
	BYTE first_day, first_week;
	WORD len;	/* characters */
	WCHAR text[];
};

WINFN(HRESULT) oa_VarTokenizeFormatString(LPOLESTR fmt, LPBYTE buf, int cb,
					  int first_day, int first_week,
					  LCID lcid, int *used)
{
	size_t len = fmt ? wstrlen(fmt) : 0;
	size_t need = sizeof(struct tokens) + (len + 1) * sizeof(WCHAR);

	if (!buf || cb < 0 || first_day < 0 || first_day > 7 ||
	    first_week < 0 || first_week > 3 || len > 0xffff)
		return E_INVALIDARG;
	if ((size_t)cb < need)
		return DISP_E_BUFFERTOOSMALL;
	struct tokens *t = (struct tokens *)buf;
	t->tag = TOKEN_TAG;
	t->first_day = (BYTE)first_day;
	t->first_week = (BYTE)first_week;
	t->len = (WORD)len;
	if (len)
		memcpy(t->text, fmt, len * sizeof(WCHAR));
	t->text[len] = 0;
	if (used)
		*used = (int)need;
	return S_OK;
}

WINFN(HRESULT) oa_VarFormatFromTokens(LPVARIANT in, LPOLESTR fmt,
				      LPBYTE buf, ULONG flags, BSTR *out,
				      LCID lcid)
{
	const struct tokens *t = (const struct tokens *)buf;

	if (!t || t->tag != TOKEN_TAG)
		return E_INVALIDARG;
	return format_value(in, t->text, t->first_day, t->first_week, out);
}

/* ---- the fixed formats ---- */

/* A tristate: -1 yes, 0 no, -2 the locale's default. */
static int tristate(int v, int dflt)
{
	return v == -2 ? dflt : v != 0;
}

static HRESULT fixed_format(LPVARIANT in, int digits, int lead, int parens,
			    int group, const char *prefix, const char *suffix,
			    int percent, int dflt_parens, BSTR *out)
{
	VARIANT v;
	struct dnum x;
	struct out o = { 0 };
	HRESULT hr;

	if (!in || !out || digits < -1 || digits > 99 || lead < -2 ||
	    lead > 0 || parens < -2 || parens > 0 || group < -2 || group > 0)
		return E_INVALIDARG;
	if (digits == -1)
		digits = 2;
	oa_VariantInit(&v);
	if ((hr = oa_VariantCopyInd(&v, in)) != S_OK)
		return hr;
	if (v.vt == VT_NULL) {
		*out = oa_SysAllocStringLen(NULL, 0);
		return *out ? S_OK : E_OUTOFMEMORY;
	}
	if (v.vt == VT_BSTR || v.vt == VT_DATE || v.vt == VT_DISPATCH) {
		VARIANT t;
		oa_VariantInit(&t);
		hr = oa_VariantChangeTypeEx(&t, &v, US_LCID, 0, VT_R8);
		oa_VariantClear(&v);
		if (hr != S_OK)
			return hr;
		v = t;
	}
	if (!is_number_vt(v.vt)) {
		oa_VariantClear(&v);
		return DISP_E_TYPEMISMATCH;
	}
	hr = dnum_from_variant(&v, &x);
	oa_VariantClear(&v);
	if (hr != S_OK)
		return hr;
	x.exp += 2 * percent;
	if (!x.n)
		x.exp = 0;
	dnum_round(&x, digits);
	int neg = x.neg && !dnum_zero(&x);
	int use_parens = neg && tristate(parens, dflt_parens);
	int grouping = tristate(group, 1);

	if (use_parens)
		putc_(&o, '(');
	else if (neg)
		putc_(&o, '-');
	put(&o, prefix);
	int total = dnum_int_digits(&x);
	if (!total && tristate(lead, 1))
		total = 1;
	for (int pos = total - 1; pos >= 0; pos--) {
		putc_(&o, dnum_digit(&x, pos));
		if (grouping && pos > 0 && pos % 3 == 0)
			putc_(&o, ',');
	}
	if (digits) {
		putc_(&o, '.');
		for (int i = 1; i <= digits; i++)
			putc_(&o, dnum_digit(&x, -i));
	}
	put(&o, suffix);
	if (use_parens)
		putc_(&o, ')');
	return finish(&o, out);
}

WINFN(HRESULT) oa_VarFormatNumber(LPVARIANT in, int digits, int lead,
				  int parens, int group, ULONG flags,
				  BSTR *out)
{
	return fixed_format(in, digits, lead, parens, group, "", "", 0, 0,
			    out);
}

WINFN(HRESULT) oa_VarFormatPercent(LPVARIANT in, int digits, int lead,
				   int parens, int group, ULONG flags,
				   BSTR *out)
{
	return fixed_format(in, digits, lead, parens, group, "", "%", 1, 0,
			    out);
}

WINFN(HRESULT) oa_VarFormatCurrency(LPVARIANT in, int digits, int lead,
				    int parens, int group, ULONG flags,
				    BSTR *out)
{
	return fixed_format(in, digits, lead, parens, group, "$", "", 0, 1,
			    out);
}

WINFN(HRESULT) oa_VarFormatDateTime(LPVARIANT in, int how, ULONG flags,
				    BSTR *out)
{
	static const char *const formats[] = {
		"General Date", "Long Date", "Short Date", "Long Time",
		"Short Time",
	};

	if (how < 0 || how > 4)
		return E_INVALIDARG;
	WCHAR *f = utf8_to_utf16_dup(formats[how]);
	if (!f)
		return E_OUTOFMEMORY;
	HRESULT hr;
	if (in && in->vt == VT_BSTR) {	/* read as a date first */
		VARIANT t;
		oa_VariantInit(&t);
		hr = oa_VariantChangeTypeEx(&t, in, US_LCID, 0, VT_DATE);
		if (hr == S_OK)
			hr = format_value(&t, f, 0, 0, out);
	} else {
		hr = format_value(in, f, 0, 0, out);
	}
	free(f);
	return hr;
}

WINFN(HRESULT) oa_VarWeekdayName(int day, int abbrev, int first_day,
				 ULONG flags, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	if (day < 1 || day > 7 || first_day < 0 || first_day > 7)
		return E_INVALIDARG;
	if (!first_day)
		first_day = 1;
	int d = (first_day - 1 + day - 1) % 7;
	char buf[16];
	snprintf(buf, sizeof(buf), "%.*s", abbrev ? 3 : 15, day_names[d]);
	*out = oa_bstr_utf8(buf);
	return *out ? S_OK : E_OUTOFMEMORY;
}

WINFN(HRESULT) oa_VarMonthName(int month, int abbrev, ULONG flags,
			       BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	if (month < 1 || month > 12)
		return E_INVALIDARG;
	char buf[16];
	snprintf(buf, sizeof(buf), "%.*s", abbrev ? 3 : 15,
		 month_names[month - 1]);
	*out = oa_bstr_utf8(buf);
	return *out ? S_OK : E_OUTOFMEMORY;
}

#define E(name, ord) { #name, ord, (void *)oa_##name }

const struct builtin_export oa_format_exports[] = {
	E(VarFormat, 87),
	E(VarFormatDateTime, 97),
	E(VarFormatNumber, 107),
	E(VarFormatPercent, 117),
	E(VarFormatCurrency, 127),
	E(VarWeekdayName, 128),
	E(VarMonthName, 129),
	E(VarFormatFromTokens, 139),
	E(VarTokenizeFormatString, 140),
	BUILTIN_END
};
