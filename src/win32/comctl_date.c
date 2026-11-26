/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's month calendar (SysMonthCal32) and date and time picker
 * (SysDateTimePick32), headless: dates checked against the calendar and
 * the control's range, the selection (one day, or a run of up to the
 * maximum with MCS_MULTISELECT), today, the first day of the week,
 * colours, day states (MCN_GETDAYSTATE) and the month delta; the picker's
 * time, its "none" state with DTS_SHOWNONE, range and format, its window
 * text being the time in that format. Nothing is drawn: the calendar
 * shows one month as a title row, a row of day names and six weeks of
 * CELL_CX by CELL_CY cells, which is what MCM_HITTEST and clicks use.
 * A click on a day selects it (MCN_SELCHANGE, MCN_SELECT). The picker
 * has no drop-down calendar (DTM_GETMONTHCAL is NULL).
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define TITLE_CY  20
#define NAMES_CY  16
#define CELL_CX   23
#define CELL_CY   16
#define TODAY_CY  16
#define CAL_CX    (7 * CELL_CX)
#define CAL_CY    (TITLE_CY + NAMES_CY + 6 * CELL_CY + TODAY_CY)

static BOOL leap(int y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in(int y, int m)
{
	static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31,
				   30, 31 };
	return m == 2 && leap(y) ? 29 : d[m - 1];
}

/* Day 0 is 1 January 1601, as FILETIME counts. */
static long day_number(int y, int m, int d)
{
	long n = 0;
	for (int yy = 1601; yy < y; yy++)
		n += leap(yy) ? 366 : 365;
	for (int mm = 1; mm < m; mm++)
		n += days_in(y, mm);
	return n + d - 1;
}

static void from_day_number(long n, SYSTEMTIME *st)
{
	int y = 1601, m = 1;
	while (n >= (leap(y) ? 366 : 365))
		n -= leap(y++) ? 366 : 365;
	while (n >= days_in(y, m))
		n -= days_in(y, m++);
	st->wYear = (WORD)y;
	st->wMonth = (WORD)m;
	st->wDay = (WORD)(n + 1);
	st->wDayOfWeek = (WORD)((day_number(y, m, (int)n + 1) + 1) % 7);
}

static int weekday(int y, int m, int d)
{
	return (int)((day_number(y, m, d) + 1) % 7);	/* 1601-01-01: Monday */
}

static BOOL valid_date(const SYSTEMTIME *st)
{
	return st->wYear >= 1601 && st->wYear <= 30827 && st->wMonth >= 1 &&
	       st->wMonth <= 12 && st->wDay >= 1 &&
	       st->wDay <= days_in(st->wYear, st->wMonth);
}

static BOOL valid_time(const SYSTEMTIME *st)
{
	return valid_date(st) && st->wHour < 24 && st->wMinute < 60 &&
	       st->wSecond < 60 && st->wMilliseconds < 1000;
}

static int cmp_date(const SYSTEMTIME *a, const SYSTEMTIME *b)
{
	long x = day_number(a->wYear, a->wMonth, a->wDay);
	long y = day_number(b->wYear, b->wMonth, b->wDay);
	return x < y ? -1 : x > y;
}

static int cmp_time(const SYSTEMTIME *a, const SYSTEMTIME *b)
{
	int c = cmp_date(a, b);
	if (c)
		return c;
	long x = ((a->wHour * 60L + a->wMinute) * 60 + a->wSecond) * 1000 +
		 a->wMilliseconds;
	long y = ((b->wHour * 60L + b->wMinute) * 60 + b->wSecond) * 1000 +
		 b->wMilliseconds;
	return x < y ? -1 : x > y;
}

static void date_only(SYSTEMTIME *st)
{
	st->wDayOfWeek = (WORD)weekday(st->wYear, st->wMonth, st->wDay);
	st->wHour = st->wMinute = st->wSecond = st->wMilliseconds = 0;
}

static void now(SYSTEMTIME *st)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	memset(st, 0, sizeof(*st));
	st->wYear = (WORD)(tm.tm_year + 1900);
	st->wMonth = (WORD)(tm.tm_mon + 1);
	st->wDay = (WORD)tm.tm_mday;
	st->wDayOfWeek = (WORD)tm.tm_wday;
	st->wHour = (WORD)tm.tm_hour;
	st->wMinute = (WORD)tm.tm_min;
	st->wSecond = (WORD)tm.tm_sec;
}

struct range {
	DWORD flags;		/* GDTR_MIN | GDTR_MAX */
	SYSTEMTIME lim[2];
};

static BOOL in_range(const struct range *r, const SYSTEMTIME *st,
		     int (*cmp)(const SYSTEMTIME *, const SYSTEMTIME *))
{
	if ((r->flags & GDTR_MIN) && cmp(st, &r->lim[0]) < 0)
		return FALSE;
	if ((r->flags & GDTR_MAX) && cmp(st, &r->lim[1]) > 0)
		return FALSE;
	return TRUE;
}

static BOOL set_range(struct range *r, DWORD flags, const SYSTEMTIME *lim,
		      BOOL (*valid)(const SYSTEMTIME *))
{
	if (!lim && flags)
		return FALSE;
	if (((flags & GDTR_MIN) && !valid(&lim[0])) ||
	    ((flags & GDTR_MAX) && !valid(&lim[1])))
		return FALSE;
	r->flags = flags & (GDTR_MIN | GDTR_MAX);
	if (flags & GDTR_MIN)
		r->lim[0] = lim[0];
	if (flags & GDTR_MAX)
		r->lim[1] = lim[1];
	return TRUE;
}

static DWORD get_range(const struct range *r, SYSTEMTIME *lim)
{
	if (lim) {
		memset(lim, 0, 2 * sizeof(*lim));
		if (r->flags & GDTR_MIN)
			lim[0] = r->lim[0];
		if (r->flags & GDTR_MAX)
			lim[1] = r->lim[1];
	}
	return r->flags;
}

/* Month calendars. */

struct monthcal {
	SYSTEMTIME sel[2];	/* the selection (one day unless multiselect) */
	SYSTEMTIME today;
	BOOL today_set;
	struct range range;
	int max_sel, first_day, delta, view;
	BOOL first_set;
	COLORREF colour[6];
	MONTHDAYSTATE state[3];
	BOOL unicode;
	HFONT font;
};

static void get_today(const struct monthcal *s, SYSTEMTIME *st)
{
	if (s->today_set)
		*st = s->today;
	else {
		now(st);
		date_only(st);
	}
}

/* The first day the grid shows: the start of the week holding the 1st
 * of the selection's month (a whole week earlier when the 1st starts
 * one). */
static long grid_start(const struct monthcal *s)
{
	long first = day_number(s->sel[0].wYear, s->sel[0].wMonth, 1);
	int wd = weekday(s->sel[0].wYear, s->sel[0].wMonth, 1);
	int col = (wd + 6 - s->first_day) % 7;	/* first_day: 0 is Monday */
	return first - (col ? col : 7);
}

static void ask_day_states(HWND h, struct monthcal *s)
{
	if (!(comctl_style(h) & MCS_DAYSTATE))
		return;
	NMDAYSTATE nm;
	memset(&nm, 0, sizeof(nm));
	from_day_number(grid_start(s), &nm.stStart);
	nm.cDayState = 3;
	memset(s->state, 0, sizeof(s->state));
	nm.prgDayState = s->state;
	comctl_notify(h, MCN_GETDAYSTATE, &nm.nmhdr);
}

static void set_selection(HWND h, struct monthcal *s, const SYSTEMTIME *a,
			  const SYSTEMTIME *b)
{
	BOOL month = a->wYear != s->sel[0].wYear ||
		     a->wMonth != s->sel[0].wMonth;
	s->sel[0] = *a;
	s->sel[1] = *b;
	date_only(&s->sel[0]);
	date_only(&s->sel[1]);
	if (month)
		ask_day_states(h, s);
}

static void notify_selection(HWND h, struct monthcal *s, UINT code)
{
	NMSELCHANGE nm;
	memset(&nm, 0, sizeof(nm));
	nm.stSelStart = s->sel[0];
	nm.stSelEnd = s->sel[1];
	comctl_notify(h, code, &nm.nmhdr);
}

static LRESULT month_range(const struct monthcal *s, DWORD which,
			   SYSTEMTIME *out)
{
	SYSTEMTIME a, b;
	int y = s->sel[0].wYear, m = s->sel[0].wMonth;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	if (which == GMR_DAYSTATE) {
		long start = grid_start(s);
		from_day_number(start, &a);
		from_day_number(start + 41, &b);
	} else {
		a.wYear = b.wYear = (WORD)y;
		a.wMonth = b.wMonth = (WORD)m;
		a.wDay = 1;
		b.wDay = (WORD)days_in(y, m);
		date_only(&a);
		date_only(&b);
	}
	if (out) {
		out[0] = a;
		out[1] = b;
	}
	return (b.wYear - a.wYear) * 12 + b.wMonth - a.wMonth + 1;
}

static UINT hit(const struct monthcal *s, POINT pt, SYSTEMTIME *st)
{
	if (pt.x < 0 || pt.x >= CAL_CX || pt.y < 0 || pt.y >= CAL_CY)
		return MCHT_NOWHERE;
	if (pt.y < TITLE_CY) {
		if (pt.x < CELL_CX)
			return MCHT_TITLEBTNPREV;
		if (pt.x >= CAL_CX - CELL_CX)
			return MCHT_TITLEBTNNEXT;
		return MCHT_TITLEBK;
	}
	if (pt.y < TITLE_CY + NAMES_CY)
		return MCHT_CALENDARDAY;
	if (pt.y >= CAL_CY - TODAY_CY)
		return MCHT_TODAYLINK;
	int row = (pt.y - TITLE_CY - NAMES_CY) / CELL_CY, col = pt.x / CELL_CX;
	SYSTEMTIME d;
	memset(&d, 0, sizeof(d));
	from_day_number(grid_start(s) + row * 7 + col, &d);
	if (st)
		*st = d;
	if (d.wMonth != s->sel[0].wMonth)
		return (cmp_date(&d, &s->sel[0]) < 0 ? MCHT_CALENDARDATEPREV :
						      MCHT_CALENDARDATENEXT);
	return MCHT_CALENDARDATE;
}

static void click(HWND h, struct monthcal *s, POINT pt)
{
	SYSTEMTIME d;
	UINT what = hit(s, pt, &d);

	/* A day, this month's or a trailing one. */
	if ((what & ~(DWORD)(MCHT_NEXT | MCHT_PREV)) != MCHT_CALENDARDATE ||
	    !in_range(&s->range, &d, cmp_date))
		return;
	set_selection(h, s, &d, &d);
	notify_selection(h, s, MCN_SELCHANGE);
	notify_selection(h, s, MCN_SELECT);
}

LRESULT STUB_WINAPI comctl_monthcal_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct monthcal *s = comctl_state(h);

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		get_today(s, &s->sel[0]);
		s->sel[1] = s->sel[0];
		s->max_sel = 7;
		s->first_day = 6;	/* Sunday, as the US locale has it */
		s->unicode = TRUE;
		static const int sys[6] = { COLOR_WINDOW, COLOR_WINDOWTEXT,
					    COLOR_ACTIVECAPTION,
					    COLOR_CAPTIONTEXT, COLOR_WINDOW,
					    COLOR_GRAYTEXT };
		for (int i = 0; i < 6; i++)
			s->colour[i] = ((DWORD (WINAPI *)(int))
					USER32(GetSysColor))(sys[i]);
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	BOOL multi = (comctl_style(h) & MCS_MULTISELECT) != 0;
	SYSTEMTIME *st = (SYSTEMTIME *)lp;
	switch (msg) {
	case WM_CREATE:
		ask_day_states(h, s);
		break;
	case MCM_GETCURSEL:
		if (multi || !st)
			return FALSE;
		*st = s->sel[0];
		return TRUE;
	case MCM_SETCURSEL:
		if (multi || !st || !valid_date(st) ||
		    !in_range(&s->range, st, cmp_date))
			return FALSE;
		set_selection(h, s, st, st);
		return TRUE;
	case MCM_GETMAXSELCOUNT:
		return s->max_sel;
	case MCM_SETMAXSELCOUNT:
		if (!multi || (int)wp < 1)
			return FALSE;
		s->max_sel = (int)wp;
		return TRUE;
	case MCM_GETSELRANGE:
		if (!multi || !st)
			return FALSE;
		st[0] = s->sel[0];
		st[1] = s->sel[1];
		return TRUE;
	case MCM_SETSELRANGE: {
		if (!multi || !st || !valid_date(&st[0]) || !valid_date(&st[1]))
			return FALSE;
		const SYSTEMTIME *a = &st[0], *b = &st[1];
		if (cmp_date(a, b) > 0) {
			const SYSTEMTIME *t = a;
			a = b;
			b = t;
		}
		long span = day_number(b->wYear, b->wMonth, b->wDay) -
			    day_number(a->wYear, a->wMonth, a->wDay) + 1;
		if (span > s->max_sel || !in_range(&s->range, a, cmp_date) ||
		    !in_range(&s->range, b, cmp_date))
			return FALSE;
		set_selection(h, s, a, b);
		return TRUE;
	}
	case MCM_GETMONTHRANGE:
		return month_range(s, (DWORD)wp, st);
	case MCM_SETDAYSTATE: {
		if (!(comctl_style(h) & MCS_DAYSTATE) || !lp)
			return FALSE;
		int n = (int)wp < 3 ? (int)wp : 3;
		memcpy(s->state, (const void *)lp,
		       (size_t)n * sizeof(MONTHDAYSTATE));
		return TRUE;
	}
	case MCM_GETMINREQRECT: {
		RECT *rc = (RECT *)lp;
		if (!rc)
			return FALSE;
		rc->left = rc->top = 0;
		rc->right = CAL_CX;
		rc->bottom = CAL_CY;
		return TRUE;
	}
	case MCM_SETCOLOR: {
		if ((int)wp < 0 || (int)wp > MCSC_TRAILINGTEXT)
			return -1;
		COLORREF old = s->colour[wp];
		s->colour[wp] = (COLORREF)lp;
		return old;
	}
	case MCM_GETCOLOR:
		if ((int)wp < 0 || (int)wp > MCSC_TRAILINGTEXT)
			return -1;
		return s->colour[wp];
	case MCM_SETTODAY:
		if (st && valid_date(st)) {
			s->today = *st;
			date_only(&s->today);
			s->today_set = TRUE;
		} else if (!st) {
			s->today_set = FALSE;
		}
		return 0;
	case MCM_GETTODAY:
		if (!st)
			return FALSE;
		get_today(s, st);
		return TRUE;
	case MCM_HITTEST: {
		MCHITTESTINFO *hi = (MCHITTESTINFO *)lp;
		if (!hi || hi->cbSize < MCHITTESTINFO_V1_SIZE)
			return -1;
		memset(&hi->st, 0, sizeof(hi->st));
		hi->uHit = hit(s, hi->pt, &hi->st);
		return hi->uHit;
	}
	case MCM_SETFIRSTDAYOFWEEK: {
		LRESULT old = MAKELONG(s->first_day, s->first_set);
		if ((int)lp < 0 || (int)lp > 6) {
			s->first_day = 6;
			s->first_set = FALSE;
		} else {
			s->first_day = (int)lp;
			s->first_set = TRUE;
		}
		return old;
	}
	case MCM_GETFIRSTDAYOFWEEK:
		return MAKELONG(s->first_day, s->first_set);
	case MCM_GETRANGE:
		return get_range(&s->range, st);
	case MCM_SETRANGE:
		if (!set_range(&s->range, (DWORD)wp, st, valid_date))
			return FALSE;
		/* The selection moves inside. */
		if ((s->range.flags & GDTR_MIN) &&
		    cmp_date(&s->sel[0], &s->range.lim[0]) < 0)
			set_selection(h, s, &s->range.lim[0], &s->range.lim[0]);
		if ((s->range.flags & GDTR_MAX) &&
		    cmp_date(&s->sel[1], &s->range.lim[1]) > 0)
			set_selection(h, s, &s->range.lim[1], &s->range.lim[1]);
		return TRUE;
	case MCM_GETMONTHDELTA:
		return s->delta ? s->delta : 1;
	case MCM_SETMONTHDELTA: {
		int old = s->delta;
		s->delta = (int)wp;
		return old;
	}
	case MCM_GETMAXTODAYWIDTH:
		return comctl_text_width(u"Today: 00/00/0000");
	case MCM_GETCURRENTVIEW:
		return s->view;
	case MCM_SETCURRENTVIEW:
		if ((int)lp < MCMV_MONTH || (int)lp > MCMV_CENTURY)
			return FALSE;
		s->view = (int)lp;
		return TRUE;
	case MCM_GETCALENDARCOUNT:
		return 1;
	case MCM_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case MCM_GETUNICODEFORMAT:
		return s->unicode;
	case WM_LBUTTONDOWN: {
		POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
		click(h, s, pt);
		return 0;
	}
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}

/* Date and time pickers. */

struct datetime {
	SYSTEMTIME t;
	BOOL none;
	struct range range;
	WCHAR *format;
	COLORREF mc_colour[6];
	HFONT mc_font, font;
	DWORD mc_style;
};

static const char *const day_names[7] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
	"Saturday",
};
static const char *const month_names[12] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December",
};

static void put(WCHAR *out, int *n, int size, const char *s, int len)
{
	for (int i = 0; s[i] && (len < 0 || i < len); i++)
		if (*n < size - 1)
			out[(*n)++] = (WCHAR)(BYTE)s[i];
}

static void put_num(WCHAR *out, int *n, int size, int v, int width)
{
	char b[12];
	int k = 0;
	char d[12];
	do {
		d[k++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && k < 11);
	while (k < width && k < 11)
		d[k++] = '0';
	for (int i = 0; i < k; i++)
		b[i] = d[k - 1 - i];
	b[k] = 0;
	put(out, n, size, b, -1);
}

/* The time in a picture format (d, dd, ddd, dddd, M ... MMMM, y, yy,
 * yyyy, h, hh, H, HH, m, mm, s, ss, t, tt, 'quoted text'). */
static int format_time(const WCHAR *f, const SYSTEMTIME *t, WCHAR *out,
		       int size)
{
	int n = 0;

	while (*f) {
		WCHAR c = *f;
		int run = 1;
		if (c == '\'') {
			f++;
			while (*f && *f != '\'') {
				if (n < size - 1)
					out[n++] = *f;
				f++;
			}
			if (*f)
				f++;
			continue;
		}
		while (f[run] == c)
			run++;
		int h12 = t->wHour % 12 ? t->wHour % 12 : 12;
		switch (c) {
		case 'd':
			if (run >= 4)
				put(out, &n, size, day_names[t->wDayOfWeek % 7],
				    -1);
			else if (run == 3)
				put(out, &n, size, day_names[t->wDayOfWeek % 7],
				    3);
			else
				put_num(out, &n, size, t->wDay, run);
			break;
		case 'M':
			if (run >= 4)
				put(out, &n, size, month_names[t->wMonth - 1],
				    -1);
			else if (run == 3)
				put(out, &n, size, month_names[t->wMonth - 1], 3);
			else
				put_num(out, &n, size, t->wMonth, run);
			break;
		case 'y':
			if (run <= 2)
				put_num(out, &n, size, t->wYear % 100, run);
			else
				put_num(out, &n, size, t->wYear, 4);
			break;
		case 'h':
			put_num(out, &n, size, h12, run > 2 ? 2 : run);
			break;
		case 'H':
			put_num(out, &n, size, t->wHour, run > 2 ? 2 : run);
			break;
		case 'm':
			put_num(out, &n, size, t->wMinute, run > 2 ? 2 : run);
			break;
		case 's':
			put_num(out, &n, size, t->wSecond, run > 2 ? 2 : run);
			break;
		case 't':
			put(out, &n, size, t->wHour < 12 ? "AM" : "PM",
			    run >= 2 ? 2 : 1);
			break;
		default:
			for (int i = 0; i < run; i++)
				if (n < size - 1)
					out[n++] = c;
		}
		f += run;
	}
	if (size > 0)
		out[n] = 0;
	return n;
}

static const WCHAR *format_of(HWND h, const struct datetime *s)
{
	DWORD st = comctl_style(h);

	if (s->format)
		return s->format;
	if ((st & DTS_TIMEFORMAT) == DTS_TIMEFORMAT)
		return u"h:mm:ss tt";
	if ((st & DTS_SHORTDATECENTURYFORMAT) == DTS_SHORTDATECENTURYFORMAT)
		return u"M/d/yyyy";
	if (st & DTS_LONGDATEFORMAT)
		return u"dddd, MMMM dd, yyyy";
	return u"M/d/yyyy";
}

static int datetime_text(HWND h, const struct datetime *s, WCHAR *out,
			 int size)
{
	WCHAR buf[256];
	int n = s->none ? 0 : format_time(format_of(h, s), &s->t, buf, 256);
	buf[n] = 0;
	if (!out || size <= 0)
		return n;
	if (n > size - 1)
		n = size - 1;
	memcpy(out, buf, (size_t)n * sizeof(WCHAR));
	out[n] = 0;
	return n;
}

LRESULT STUB_WINAPI comctl_datetime_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct datetime *s = comctl_state(h);

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		now(&s->t);
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	SYSTEMTIME *st = (SYSTEMTIME *)lp;
	switch (msg) {
	case DTM_GETSYSTEMTIME:
		if (!st)
			return GDT_ERROR;
		if (s->none)
			return GDT_NONE;
		*st = s->t;
		return GDT_VALID;
	case DTM_SETSYSTEMTIME:
		if (wp == GDT_NONE) {
			if (!(comctl_style(h) & DTS_SHOWNONE))
				return FALSE;
			s->none = TRUE;
			return TRUE;
		}
		if (wp != GDT_VALID || !st || !valid_time(st) ||
		    !in_range(&s->range, st, cmp_time))
			return FALSE;
		s->t = *st;
		s->t.wDayOfWeek = (WORD)weekday(st->wYear, st->wMonth,
						st->wDay);
		s->none = FALSE;
		return TRUE;
	case DTM_GETRANGE:
		return get_range(&s->range, st);
	case DTM_SETRANGE:
		if (!set_range(&s->range, (DWORD)wp, st, valid_time))
			return FALSE;
		if ((s->range.flags & GDTR_MIN) &&
		    cmp_time(&s->t, &s->range.lim[0]) < 0)
			s->t = s->range.lim[0];
		if ((s->range.flags & GDTR_MAX) &&
		    cmp_time(&s->t, &s->range.lim[1]) > 0)
			s->t = s->range.lim[1];
		return TRUE;
	case DTM_SETFORMATA:
	case DTM_SETFORMATW:
		free(s->format);
		s->format = !lp ? NULL : msg == DTM_SETFORMATW ?
			    comctl_w_dup((const WCHAR *)lp) :
			    comctl_a_to_w((const char *)lp);
		return TRUE;
	case DTM_SETMCCOLOR: {
		if ((int)wp < 0 || (int)wp > MCSC_TRAILINGTEXT)
			return -1;
		COLORREF old = s->mc_colour[wp];
		s->mc_colour[wp] = (COLORREF)lp;
		return old;
	}
	case DTM_GETMCCOLOR:
		if ((int)wp < 0 || (int)wp > MCSC_TRAILINGTEXT)
			return -1;
		return s->mc_colour[wp];
	case DTM_GETMONTHCAL:
		return 0;
	case DTM_SETMCFONT:
		s->mc_font = (HFONT)wp;
		return 0;
	case DTM_GETMCFONT:
		return (LRESULT)s->mc_font;
	case DTM_SETMCSTYLE: {
		DWORD old = s->mc_style;
		s->mc_style = (DWORD)lp;
		return old;
	}
	case DTM_GETMCSTYLE:
		return s->mc_style;
	case DTM_CLOSEMONTHCAL:
		return 0;
	case DTM_GETIDEALSIZE: {
		SIZE *sz = (SIZE *)lp;
		WCHAR buf[256];
		if (!sz)
			return FALSE;
		datetime_text(h, s, buf, 256);
		sz->cx = comctl_text_width(buf) + 30;
		sz->cy = 20;
		return TRUE;
	}
	case WM_GETTEXT:
		return datetime_text(h, s, (WCHAR *)lp, (int)wp);
	case WM_GETTEXTLENGTH:
		return datetime_text(h, s, NULL, 0);
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		free(s->format);
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}
