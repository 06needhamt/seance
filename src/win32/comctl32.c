/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32.dll, headless: the common controls, after Wine's comctl32.
 *
 * - InitCommonControls(Ex) registers the control window classes (so do
 *   the Create* helpers). Nothing is drawn and no input arrives, so a
 *   control is its state: trackbars, progress bars, up-down controls and
 *   status bars keep and answer theirs (positions, ranges, parts, text,
 *   an up-down's buddy text); the other classes are plain windows.
 * - Window subclassing (SetWindowSubclass and friends), dynamic arrays
 *   (comctl_dpa.c), image lists (comctl_imagelist.c), and property sheets
 *   and task dialogs (comctl_sheet.c), which answer as MessageBox does.
 *
 * Much of comctl32 is imported by ordinal; the exports carry Windows'
 * ordinals, as Wine's comctl32.spec lists them.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "win32/codepage.h"
#include "win32/comctl.h"
#include "win32/internal.h"

/* user32 and gdi32 entry points, looked up once. */
static struct {
	ATOM (WINAPI *RegisterClassW)(const WNDCLASSW *);
	HWND (WINAPI *CreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
				       int, int, HWND, HMENU, HINSTANCE, LPVOID);
	LRESULT (WINAPI *DefWindowProcW)(HWND, UINT, WPARAM, LPARAM);
	LRESULT (WINAPI *SendMessageW)(HWND, UINT, WPARAM, LPARAM);
	LRESULT (WINAPI *CallWindowProcA)(WNDPROC, HWND, UINT, WPARAM, LPARAM);
	LRESULT (WINAPI *CallWindowProcW)(WNDPROC, HWND, UINT, WPARAM, LPARAM);
	LONG (WINAPI *GetWindowLongW)(HWND, int);
	LONG (WINAPI *SetWindowLongA)(HWND, int, LONG);
	LONG (WINAPI *SetWindowLongW)(HWND, int, LONG);
	BOOL (WINAPI *IsWindowUnicode)(HWND);
	BOOL (WINAPI *ShowWindow)(HWND, int);
	HANDLE (WINAPI *GetPropW)(HWND, LPCWSTR);
	BOOL (WINAPI *SetPropW)(HWND, LPCWSTR, HANDLE);
	HANDLE (WINAPI *RemovePropW)(HWND, LPCWSTR);
	BOOL (WINAPI *SetWindowTextW)(HWND, LPCWSTR);
	int (WINAPI *GetWindowTextW)(HWND, LPWSTR, int);
	HWND (WINAPI *GetParent)(HWND);
	HWND (WINAPI *GetDlgItem)(HWND, int);
	BOOL (WINAPI *GetClientRect)(HWND, LPRECT);
	BOOL (WINAPI *GetWindowRect)(HWND, LPRECT);
	BOOL (WINAPI *ScreenToClient)(HWND, LPPOINT);
	BOOL (WINAPI *SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
	HANDLE (WINAPI *LoadImageW)(HINSTANCE, LPCWSTR, UINT, int, int, UINT);
	DWORD (WINAPI *GetSysColor)(int);
	HDC (WINAPI *CreateCompatibleDC)(HDC);
	BOOL (WINAPI *DeleteDC)(HDC);
	HGDIOBJ (WINAPI *SelectObject)(HDC, HGDIOBJ);
	int (WINAPI *GetObjectW)(HGDIOBJ, int, void *);
	COLORREF (WINAPI *GetPixel)(HDC, int, int);
	COLORREF (WINAPI *SetPixel)(HDC, int, int, COLORREF);
	BOOL (WINAPI *TextOutW)(HDC, int, int, LPCWSTR, int);
	BOOL (WINAPI *ExtTextOutW)(HDC, int, int, UINT, const RECT *, LPCWSTR,
				   UINT, const INT *);
	BOOL (WINAPI *GetTextExtentPoint32W)(HDC, LPCWSTR, int, LPSIZE);
} api;

static pthread_once_t api_once = PTHREAD_ONCE_INIT;

static void api_init(void)
{
#define U(name) api.name = USER32(name)
#define G(name) api.name = GDI32(name)
	U(RegisterClassW); U(CreateWindowExW); U(DefWindowProcW);
	U(SendMessageW); U(CallWindowProcA); U(CallWindowProcW);
	U(GetWindowLongW); U(SetWindowLongA); U(SetWindowLongW);
	U(IsWindowUnicode); U(ShowWindow);
	U(GetPropW); U(SetPropW); U(RemovePropW);
	U(SetWindowTextW); U(GetWindowTextW); U(GetParent); U(GetDlgItem);
	U(GetClientRect); U(GetWindowRect); U(ScreenToClient);
	U(SetWindowPos); U(LoadImageW); U(GetSysColor);
	G(CreateCompatibleDC); G(DeleteDC); G(SelectObject); G(GetObjectW);
	G(GetPixel); G(SetPixel); G(TextOutW); G(ExtTextOutW);
	G(GetTextExtentPoint32W);
#undef U
#undef G
}

#define API() pthread_once(&api_once, api_init)

/* A UTF-16 copy of an ANSI string (malloc()ed). */
static WCHAR *a_to_w(const char *s)
{
	char *u = acp_to_utf8(s);
	WCHAR *w = u ? utf8_to_utf16_dup(u) : NULL;
	free(u);
	return w;
}

static char *w_to_a(const WCHAR *s)
{
	char *u = utf16_to_utf8_dup(s);
	char *a = u ? utf8_to_acp_dup(u) : NULL;
	free(u);
	return a;
}

static WCHAR *w_dup(const WCHAR *s)
{
	size_t n = wstrlen(s) + 1;
	WCHAR *d = malloc(n * sizeof(WCHAR));
	if (d)
		memcpy(d, s, n * sizeof(WCHAR));
	return d;
}

/*
 * Copies text out as ANSI or UTF-16, cut to size characters (bytes for
 * ANSI) with its NUL; returns the length copied.
 */
static int copy_text_out(const WCHAR *text, void *buf, int size, int wide)
{
	if (!buf || size <= 0)
		return 0;
	if (!text)
		text = u"";
	if (wide) {
		int n = (int)wstrlen(text);
		if (n >= size)
			n = size - 1;
		memcpy(buf, text, n * sizeof(WCHAR));
		((WCHAR *)buf)[n] = 0;
		return n;
	}
	char *a = w_to_a(text);
	int n = a ? (int)strlen(a) : 0;
	if (n >= size)
		n = size - 1;
	if (a)
		memcpy(buf, a, n);
	((char *)buf)[n] = '\0';
	free(a);
	return n;
}

static int text_len(const WCHAR *text, int wide)
{
	if (!text)
		return 0;
	if (wide)
		return (int)wstrlen(text);
	char *a = w_to_a(text);
	int n = a ? (int)strlen(a) : 0;
	free(a);
	return n;
}

/* Every control keeps its state behind the first window extra DWORD. */
static void *state(HWND h)
{
	return (void *)(ULONG_PTR)api.GetWindowLongW(h, 0);
}

static void set_state(HWND h, void *s)
{
	api.SetWindowLongW(h, 0, (LONG)(ULONG_PTR)s);
}

static DWORD style_of(HWND h)
{
	return (DWORD)api.GetWindowLongW(h, GWL_STYLE);
}

/* Trackbars. */

struct trackbar {
	LONG min, max, pos, sel_min, sel_max;
	LONG line, page, freq;
	UINT thumb_len, tip_side;
	HWND buddy[2], tooltips;
	DWORD *tics;
	UINT ntics;
	BOOL unicode;
};

static int cmp_long(const void *a, const void *b)
{
	DWORD x = *(const DWORD *)a, y = *(const DWORD *)b;
	return (LONG)x < (LONG)y ? -1 : (LONG)x > (LONG)y;
}

/* With TBS_AUTOTICKS, a tic every freq steps between the ends. */
static void tb_auto_tics(struct trackbar *t, DWORD style)
{
	if (!(style & TBS_AUTOTICKS))
		return;
	free(t->tics);
	t->tics = NULL;
	t->ntics = 0;
	LONG range = t->max - t->min;
	if (t->freq <= 0 || range <= 1)
		return;
	UINT n = (UINT)((range - 1) / t->freq);
	if (!n || !(t->tics = malloc(n * sizeof(DWORD))))
		return;
	for (UINT i = 0; i < n; i++)
		t->tics[i] = (DWORD)(t->min + (LONG)(i + 1) * t->freq);
	t->ntics = n;
}

static LONG tb_clamp(const struct trackbar *t, LONG v)
{
	return v < t->min ? t->min : v > t->max ? t->max : v;
}

/* Geometry, for the messages that ask: a channel across the middle. */
static void tb_channel(HWND h, RECT *rc)
{
	RECT c;
	api.GetClientRect(h, &c);
	if (style_of(h) & TBS_VERT) {
		rc->left = c.right / 2 - 2;
		rc->right = rc->left + 4;
		rc->top = 8;
		rc->bottom = c.bottom > 16 ? c.bottom - 8 : 8;
	} else {
		rc->top = c.bottom / 2 - 2;
		rc->bottom = rc->top + 4;
		rc->left = 8;
		rc->right = c.right > 16 ? c.right - 8 : 8;
	}
}

static LONG tb_offset(HWND h, const struct trackbar *t, LONG v)
{
	RECT rc;
	tb_channel(h, &rc);
	LONG len = style_of(h) & TBS_VERT ? rc.bottom - rc.top :
		   rc.right - rc.left;
	LONG start = style_of(h) & TBS_VERT ? rc.top : rc.left;
	LONG range = t->max - t->min;
	return start + (range > 0 ? (LONG)((long long)(v - t->min) * len /
					   range) : 0);
}

WINFN(LRESULT) trackbar_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct trackbar *t = state(h);
	LONG old;

	if (msg == WM_NCCREATE) {
		if (!(t = calloc(1, sizeof(*t))))
			return FALSE;
		t->max = 100;
		t->line = 1;
		t->page = 20;
		t->freq = 1;
		t->thumb_len = 20;
		set_state(h, t);
		tb_auto_tics(t, ((CREATESTRUCTW *)lp)->style);
	}
	if (!t)
		return api.DefWindowProcW(h, msg, wp, lp);
	switch (msg) {
	case TBM_GETPOS:
		return t->pos;
	case TBM_SETPOS:
		t->pos = tb_clamp(t, (LONG)lp);
		return 0;
	case TBM_GETRANGEMIN:
		return t->min;
	case TBM_GETRANGEMAX:
		return t->max;
	case TBM_SETRANGE:
		t->min = (SHORT)LOWORD(lp);
		t->max = (SHORT)HIWORD(lp);
		t->pos = tb_clamp(t, t->pos);
		tb_auto_tics(t, style_of(h));
		return 0;
	case TBM_SETRANGEMIN:
		t->min = (LONG)lp;
		t->pos = tb_clamp(t, t->pos);
		tb_auto_tics(t, style_of(h));
		return 0;
	case TBM_SETRANGEMAX:
		t->max = (LONG)lp;
		t->pos = tb_clamp(t, t->pos);
		tb_auto_tics(t, style_of(h));
		return 0;
	case TBM_SETSEL:
		if (!(style_of(h) & TBS_ENABLESELRANGE)) {
			t->sel_min = t->sel_max = 0;
			return 0;
		}
		t->sel_min = tb_clamp(t, (SHORT)LOWORD(lp));
		t->sel_max = tb_clamp(t, (SHORT)HIWORD(lp));
		return 0;
	case TBM_SETSELSTART:
		if (style_of(h) & TBS_ENABLESELRANGE)
			t->sel_min = tb_clamp(t, (LONG)lp);
		return 0;
	case TBM_SETSELEND:
		if (style_of(h) & TBS_ENABLESELRANGE)
			t->sel_max = tb_clamp(t, (LONG)lp);
		return 0;
	case TBM_GETSELSTART:
		return t->sel_min;
	case TBM_GETSELEND:
		return t->sel_max;
	case TBM_CLEARSEL:
		t->sel_min = t->sel_max = 0;
		return 0;
	case TBM_SETPAGESIZE:
		old = t->page;
		t->page = (LONG)lp == -1 ? 20 : (LONG)lp;
		return old;
	case TBM_GETPAGESIZE:
		return t->page;
	case TBM_SETLINESIZE:
		old = t->line;
		t->line = (LONG)lp;
		return old;
	case TBM_GETLINESIZE:
		return t->line;
	case TBM_SETTICFREQ:
		t->freq = (LONG)wp;
		tb_auto_tics(t, style_of(h));
		return 0;
	case TBM_SETTIC: {
		if ((LONG)lp < t->min || (LONG)lp > t->max)
			return FALSE;
		DWORD *n = realloc(t->tics, (t->ntics + 1) * sizeof(DWORD));
		if (!n)
			return FALSE;
		t->tics = n;
		t->tics[t->ntics++] = (DWORD)lp;
		qsort(t->tics, t->ntics, sizeof(DWORD), cmp_long);
		return TRUE;
	}
	case TBM_CLEARTICS:
		free(t->tics);
		t->tics = NULL;
		t->ntics = 0;
		return 0;
	case TBM_GETNUMTICS:
		return style_of(h) & TBS_NOTICKS ? 0 : t->ntics + 2;
	case TBM_GETTIC:
		return (INT)wp >= 0 && (UINT)wp < t->ntics ?
		       (LONG)t->tics[wp] : -1;
	case TBM_GETPTICS:
		return (LRESULT)t->tics;
	case TBM_GETTICPOS:
		return (INT)wp >= 0 && (UINT)wp < t->ntics ?
		       tb_offset(h, t, (LONG)t->tics[wp]) : -1;
	case TBM_GETCHANNELRECT:
		if (lp)
			tb_channel(h, (RECT *)lp);
		return 0;
	case TBM_GETTHUMBRECT:
		if (lp) {
			RECT *rc = (RECT *)lp, ch;
			LONG at = tb_offset(h, t, t->pos);
			tb_channel(h, &ch);
			if (style_of(h) & TBS_VERT) {
				rc->top = at - 5;
				rc->bottom = at + 5;
				rc->left = (ch.left + ch.right) / 2 -
					   (LONG)t->thumb_len / 2;
				rc->right = rc->left + t->thumb_len;
			} else {
				rc->left = at - 5;
				rc->right = at + 5;
				rc->top = (ch.top + ch.bottom) / 2 -
					  (LONG)t->thumb_len / 2;
				rc->bottom = rc->top + t->thumb_len;
			}
		}
		return 0;
	case TBM_SETTHUMBLENGTH:
		t->thumb_len = (UINT)wp;
		return 0;
	case TBM_GETTHUMBLENGTH:
		return t->thumb_len;
	case TBM_SETTOOLTIPS:
		t->tooltips = (HWND)wp;
		return 0;
	case TBM_GETTOOLTIPS:
		return (LRESULT)t->tooltips;
	case TBM_SETTIPSIDE:
		old = (LONG)t->tip_side;
		t->tip_side = (UINT)wp;
		return old;
	case TBM_SETBUDDY: {
		HWND prev = t->buddy[!wp];
		t->buddy[!wp] = (HWND)lp;
		return (LRESULT)prev;
	}
	case TBM_GETBUDDY:
		return (LRESULT)t->buddy[!wp];
	case CCM_SETUNICODEFORMAT:
		old = t->unicode;
		t->unicode = (BOOL)wp;
		return old;
	case CCM_GETUNICODEFORMAT:
		return t->unicode;
	case WM_NCDESTROY:
		free(t->tics);
		free(t);
		set_state(h, NULL);
		break;
	}
	return api.DefWindowProcW(h, msg, wp, lp);
}

/* Progress bars. */

struct progress {
	INT min, max, pos, step, state;
	COLORREF bar, bk;
	BOOL unicode;
};

static INT pb_clamp(const struct progress *p, INT v)
{
	return v < p->min ? p->min : v > p->max ? p->max : v;
}

WINFN(LRESULT) progress_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct progress *p = state(h);
	INT old;

	if (msg == WM_NCCREATE) {
		if (!(p = calloc(1, sizeof(*p))))
			return FALSE;
		p->max = 100;
		p->step = 10;
		p->state = PBST_NORMAL;
		p->bar = p->bk = CLR_DEFAULT;
		set_state(h, p);
	}
	if (!p)
		return api.DefWindowProcW(h, msg, wp, lp);
	switch (msg) {
	case PBM_SETRANGE:
	case PBM_SETRANGE32: {
		LRESULT prev = MAKELONG(p->min, p->max);
		if (msg == PBM_SETRANGE) {
			p->min = LOWORD(lp);
			p->max = HIWORD(lp);
		} else {
			p->min = (INT)wp;
			p->max = (INT)lp;
		}
		p->pos = pb_clamp(p, p->pos);
		return prev;
	}
	case PBM_GETRANGE:
		if (lp) {
			((PBRANGE *)lp)->iLow = p->min;
			((PBRANGE *)lp)->iHigh = p->max;
		}
		return wp ? p->min : p->max;
	case PBM_SETPOS:
		old = p->pos;
		p->pos = pb_clamp(p, (INT)wp);
		return old;
	case PBM_DELTAPOS:
		old = p->pos;
		p->pos = pb_clamp(p, p->pos + (INT)wp);
		return old;
	case PBM_GETPOS:
		return p->pos;
	case PBM_SETSTEP:
		old = p->step;
		p->step = (INT)wp;
		return old;
	case PBM_GETSTEP:
		return p->step;
	case PBM_STEPIT: {
		/* Past an end it wraps round, starting over. */
		old = p->pos;
		INT range = p->max - p->min, v = p->pos + p->step;
		if (range <= 0)
			v = p->min;
		else if (v > p->max)
			v = (v - p->min) % range + p->min;
		else if (v < p->min)
			v = (v - p->min) % range + p->max;
		p->pos = v;
		return old;
	}
	case PBM_SETBARCOLOR:
		old = (INT)p->bar;
		p->bar = (COLORREF)lp;
		return (COLORREF)old;
	case PBM_GETBARCOLOR:
		return p->bar;
	case PBM_SETBKCOLOR:
		old = (INT)p->bk;
		p->bk = (COLORREF)lp;
		return (COLORREF)old;
	case PBM_GETBKCOLOR:
		return p->bk;
	case PBM_SETSTATE:
		old = p->state;
		if (wp >= PBST_NORMAL && wp <= PBST_PAUSED)
			p->state = (INT)wp;
		return old;
	case PBM_GETSTATE:
		return p->state;
	case PBM_SETMARQUEE:
		return TRUE;
	case CCM_SETUNICODEFORMAT:
		old = p->unicode;
		p->unicode = (BOOL)wp;
		return old;
	case CCM_GETUNICODEFORMAT:
		return p->unicode;
	case WM_NCDESTROY:
		free(p);
		set_state(h, NULL);
		break;
	}
	return api.DefWindowProcW(h, msg, wp, lp);
}

/* Up-down controls. */

struct updown {
	INT min, max, pos, base;
	HWND buddy;
	UDACCEL *accel;
	UINT naccel;
	BOOL unicode;
};

static int ud_in_range(const struct updown *u, INT v)
{
	INT lo = u->min < u->max ? u->min : u->max;
	INT hi = u->min < u->max ? u->max : u->min;
	return v >= lo && v <= hi;
}

static INT ud_clamp(const struct updown *u, INT v)
{
	INT lo = u->min < u->max ? u->min : u->max;
	INT hi = u->min < u->max ? u->max : u->min;
	return v < lo ? lo : v > hi ? hi : v;
}

/* UDS_SETBUDDYINT: the position as the buddy's text. */
static void ud_set_buddy_int(HWND h, const struct updown *u)
{
	char buf[32], out[48];
	WCHAR w[48];

	if (!u->buddy || !(style_of(h) & UDS_SETBUDDYINT))
		return;
	if (u->base == 16) {
		snprintf(out, sizeof(out), "0x%04X", (unsigned)u->pos);
	} else {
		snprintf(buf, sizeof(buf), "%d", u->pos);
		const char *d = buf[0] == '-' ? buf + 1 : buf;
		size_t n = strlen(d), k = 0;
		if (buf[0] == '-')
			out[k++] = '-';
		for (size_t i = 0; i < n; i++) {
			if (i && !((n - i) % 3) &&
			    !(style_of(h) & UDS_NOTHOUSANDS))
				out[k++] = ',';
			out[k++] = d[i];
		}
		out[k] = '\0';
	}
	size_t i = 0;
	for (; out[i]; i++)
		w[i] = (BYTE)out[i];
	w[i] = 0;
	api.SetWindowTextW(u->buddy, w);
}

/*
 * UDS_SETBUDDYINT: reads the position back from the buddy's text; FALSE
 * if it isn't a number in range (the position is kept).
 */
static BOOL ud_get_buddy_int(HWND h, struct updown *u)
{
	WCHAR w[64];
	char s[64], *end;
	size_t k = 0;

	if (!u->buddy || !(style_of(h) & UDS_SETBUDDYINT))
		return TRUE;
	int n = api.GetWindowTextW(u->buddy, w, 64);
	for (int i = 0; i < n && k + 1 < sizeof(s); i++)
		if (w[i] != ',' && w[i] < 128)
			s[k++] = (char)w[i];
	s[k] = '\0';
	if (!k)
		return FALSE;
	long v = strtol(s, &end, u->base == 16 ? 16 : 10);
	if (*end || !ud_in_range(u, (INT)v))
		return FALSE;
	u->pos = (INT)v;
	return TRUE;
}

WINFN(LRESULT) updown_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	static const UDACCEL defaults[] = { { 0, 1 }, { 2, 5 }, { 5, 20 } };
	struct updown *u = state(h);
	INT old;

	if (msg == WM_NCCREATE) {
		if (!(u = calloc(1, sizeof(*u))) ||
		    !(u->accel = malloc(sizeof(defaults)))) {
			free(u);
			return FALSE;
		}
		/* Windows' default range runs backwards, 100 to 0. */
		u->min = 100;
		u->max = 0;
		u->base = 10;
		memcpy(u->accel, defaults, sizeof(defaults));
		u->naccel = 3;
		set_state(h, u);
	}
	if (!u)
		return api.DefWindowProcW(h, msg, wp, lp);
	switch (msg) {
	case UDM_SETRANGE:
		u->max = (SHORT)LOWORD(lp);
		u->min = (SHORT)HIWORD(lp);
		if (u->max < UD_MINVAL)
			u->max = UD_MINVAL;
		if (u->min < UD_MINVAL)
			u->min = UD_MINVAL;
		return 0;
	case UDM_GETRANGE:
		return MAKELONG(u->max, u->min);
	case UDM_SETRANGE32:
		u->min = (INT)wp;
		u->max = (INT)lp;
		return 0;
	case UDM_GETRANGE32:
		if (wp)
			*(INT *)wp = u->min;
		if (lp)
			*(INT *)lp = u->max;
		return 0;
	case UDM_SETPOS:
	case UDM_SETPOS32:
		old = u->pos;
		u->pos = ud_clamp(u, msg == UDM_SETPOS ? (SHORT)LOWORD(lp) :
				  (INT)lp);
		ud_set_buddy_int(h, u);
		return old;
	case UDM_GETPOS: {
		BOOL ok = ud_get_buddy_int(h, u);
		return MAKELONG(u->pos, !ok);
	}
	case UDM_GETPOS32: {
		BOOL ok = ud_get_buddy_int(h, u);
		if (lp)
			*(BOOL *)lp = !ok;
		return u->pos;
	}
	case UDM_SETBUDDY: {
		HWND prev = u->buddy;
		u->buddy = (HWND)wp;
		return (LRESULT)prev;
	}
	case UDM_GETBUDDY:
		return (LRESULT)u->buddy;
	case UDM_SETBASE:
		if (wp != 10 && wp != 16)
			return 0;
		old = u->base;
		u->base = (INT)wp;
		ud_set_buddy_int(h, u);
		return old;
	case UDM_GETBASE:
		return u->base;
	case UDM_SETACCEL: {
		if (!lp || !wp)
			return FALSE;
		UDACCEL *a = malloc(wp * sizeof(UDACCEL));
		if (!a)
			return FALSE;
		memcpy(a, (const void *)lp, wp * sizeof(UDACCEL));
		free(u->accel);
		u->accel = a;
		u->naccel = (UINT)wp;
		return TRUE;
	}
	case UDM_GETACCEL:
		if (lp)
			memcpy((void *)lp, u->accel,
			       (wp < u->naccel ? wp : u->naccel) *
			       sizeof(UDACCEL));
		return u->naccel;
	case CCM_SETUNICODEFORMAT:
		old = u->unicode;
		u->unicode = (BOOL)wp;
		return old;
	case CCM_GETUNICODEFORMAT:
		return u->unicode;
	case WM_NCDESTROY:
		free(u->accel);
		free(u);
		set_state(h, NULL);
		break;
	}
	return api.DefWindowProcW(h, msg, wp, lp);
}

/* Status bars. */

#define SB_MAXPARTS 256
#define SB_HEIGHT   20

struct sb_part {
	INT right;		/* -1: to the edge */
	WCHAR *text;
	LPARAM data;		/* SBT_OWNERDRAW's */
	WORD type;
	HICON icon;
};

struct status {
	int nparts;
	struct sb_part part[SB_MAXPARTS];
	struct sb_part simple_part;
	BOOL simple, unicode, placing;
	int min_height;
};

/* Along the bottom of the parent, unless the styles say not to. */
static void sb_place(HWND h, struct status *s)
{
	RECT pc;
	HWND parent = api.GetParent(h);

	if (s->placing || !parent ||
	    (style_of(h) & (CCS_NORESIZE | CCS_NOPARENTALIGN)) ||
	    !api.GetClientRect(parent, &pc))
		return;
	int height = s->min_height > SB_HEIGHT ? s->min_height : SB_HEIGHT;
	s->placing = TRUE;
	api.SetWindowPos(h, NULL, 0, pc.bottom - height, pc.right, height,
			 SWP_NOZORDER | SWP_NOACTIVATE);
	s->placing = FALSE;
}

static struct sb_part *sb_part(struct status *s, WPARAM wp)
{
	int i = (int)(wp & 0xff);
	return i == SB_SIMPLEID ? &s->simple_part :
	       i < s->nparts ? &s->part[i] : NULL;
}

static BOOL sb_set_text(struct status *s, WPARAM wp, LPARAM lp, int wide)
{
	struct sb_part *p = sb_part(s, wp);

	if (!p)
		return FALSE;
	WORD type = (WORD)(wp & 0xff00);
	free(p->text);
	p->text = NULL;
	p->data = 0;
	p->type = type;
	if (type & SBT_OWNERDRAW)
		p->data = lp;
	else if (lp)
		p->text = wide ? w_dup((const WCHAR *)lp) :
			  a_to_w((const char *)lp);
	return TRUE;
}

WINFN(LRESULT) status_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct status *s = state(h);
	struct sb_part *p;

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->nparts = 1;
		s->part[0].right = -1;
		set_state(h, s);
	}
	if (!s)
		return api.DefWindowProcW(h, msg, wp, lp);
	switch (msg) {
	case WM_CREATE:
		/* A resource id isn't text (there are no string tables here). */
		if ((ULONG_PTR)((CREATESTRUCTW *)lp)->lpszName >> 16)
			sb_set_text(s, 0, (LPARAM)((CREATESTRUCTW *)lp)->lpszName, 1);
		sb_place(h, s);
		break;
	case WM_SIZE:
		sb_place(h, s);
		return 0;
	case WM_SETTEXT:
		sb_set_text(s, 0, lp, 1);
		break;
	case SB_SETPARTS: {
		int n = (int)wp;
		if (n <= 0 || n > SB_MAXPARTS || !lp)
			return FALSE;
		for (int i = n; i < s->nparts; i++) {
			free(s->part[i].text);
			memset(&s->part[i], 0, sizeof(s->part[i]));
		}
		for (int i = 0; i < n; i++)
			s->part[i].right = ((const INT *)lp)[i];
		s->nparts = n;
		return TRUE;
	}
	case SB_GETPARTS:
		for (int i = 0; lp && i < (int)wp && i < s->nparts; i++)
			((INT *)lp)[i] = s->part[i].right;
		return s->nparts;
	case SB_SETTEXTA:
	case SB_SETTEXTW:
		return sb_set_text(s, wp, lp, msg == SB_SETTEXTW);
	case SB_GETTEXTA:
	case SB_GETTEXTW:
		if (!(p = sb_part(s, wp)))
			return 0;
		if (p->type & SBT_OWNERDRAW)
			return p->data;
		if (lp) {
			/* The caller's buffer is assumed big enough, as on Windows. */
			int n = text_len(p->text, msg == SB_GETTEXTW) + 1;
			copy_text_out(p->text, (void *)lp, n, msg == SB_GETTEXTW);
		}
		return MAKELONG(text_len(p->text, msg == SB_GETTEXTW), p->type);
	case SB_GETTEXTLENGTHA:
	case SB_GETTEXTLENGTHW:
		if (!(p = sb_part(s, wp)))
			return 0;
		return MAKELONG(text_len(p->text, msg == SB_GETTEXTLENGTHW),
				p->type);
	case SB_SIMPLE:
		s->simple = (BOOL)wp;
		return 0;
	case SB_ISSIMPLE:
		return s->simple;
	case SB_GETBORDERS:
		if (!lp)
			return FALSE;
		((INT *)lp)[0] = 0;
		((INT *)lp)[1] = 2;
		((INT *)lp)[2] = 2;
		return TRUE;
	case SB_SETMINHEIGHT:
		s->min_height = (int)wp;
		return 0;
	case SB_GETRECT: {
		RECT c, *rc = (RECT *)lp;
		int i = (int)wp;
		if (!rc || i < 0 || i >= (s->simple ? 1 : s->nparts))
			return FALSE;
		api.GetClientRect(h, &c);
		if (s->simple) {
			*rc = c;
			return TRUE;
		}
		rc->left = i ? s->part[i - 1].right + 2 : 0;
		rc->right = s->part[i].right == -1 ? c.right : s->part[i].right;
		rc->top = 2;
		rc->bottom = c.bottom > 2 ? c.bottom - 2 : c.bottom;
		return TRUE;
	}
	case SB_SETICON:
		if ((int)wp == -1)
			s->simple_part.icon = (HICON)lp;
		else if ((p = sb_part(s, wp)))
			p->icon = (HICON)lp;
		else
			return FALSE;
		return TRUE;
	case SB_GETICON:
		if ((int)wp == -1)
			return (LRESULT)s->simple_part.icon;
		return (p = sb_part(s, wp)) ? (LRESULT)p->icon : 0;
	case CCM_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case CCM_GETUNICODEFORMAT:
		return s->unicode;
	case WM_NCDESTROY:
		for (int i = 0; i < SB_MAXPARTS; i++)
			free(s->part[i].text);
		free(s->simple_part.text);
		free(s);
		set_state(h, NULL);
		break;
	}
	return api.DefWindowProcW(h, msg, wp, lp);
}

/* Window classes. */

static const struct control_class {
	const char *name;
	WNDPROC proc;		/* NULL: a plain window */
} control_classes[] = {
	{ "msctls_trackbar32", trackbar_proc },
	{ "msctls_progress32", progress_proc },
	{ "msctls_updown32", updown_proc },
	{ "msctls_statusbar32", status_proc },
	{ "ToolbarWindow32", NULL },
	{ "tooltips_class32", NULL },
	{ "SysListView32", NULL },
	{ "SysHeader32", NULL },
	{ "SysTreeView32", NULL },
	{ "SysTabControl32", NULL },
	{ "msctls_hotkey32", NULL },
	{ "SysAnimate32", NULL },
	{ "SysDateTimePick32", NULL },
	{ "SysMonthCal32", NULL },
	{ "ComboBoxEx32", NULL },
	{ "ReBarWindow32", NULL },
	{ "SysIPAddress32", NULL },
	{ "SysPager", NULL },
	{ "NativeFontCtl", NULL },
	{ "SysLink", NULL },
};

static pthread_once_t classes_once = PTHREAD_ONCE_INIT;

static void register_classes(void)
{
	API();
	for (size_t i = 0; i < sizeof(control_classes) /
			       sizeof(control_classes[0]); i++) {
		WCHAR name[32];
		WNDCLASSW wc;
		size_t j = 0;
		for (; control_classes[i].name[j]; j++)
			name[j] = (BYTE)control_classes[i].name[j];
		name[j] = 0;
		memset(&wc, 0, sizeof(wc));
		wc.style = CS_GLOBALCLASS;
		wc.lpfnWndProc = control_classes[i].proc ?
				 control_classes[i].proc : api.DefWindowProcW;
		wc.cbWndExtra = sizeof(void *);
		wc.lpszClassName = name;
		api.RegisterClassW(&wc);
	}
}

static void init_classes(void)
{
	pthread_once(&classes_once, register_classes);
}

WINFN(void) cc_InitCommonControls(void)
{
	init_classes();
}

WINFN(BOOL) cc_InitCommonControlsEx(const INITCOMMONCONTROLSEX *icc)
{
	if (!icc || icc->dwSize != sizeof(*icc))
		return FALSE;
	init_classes();
	return TRUE;
}

/* Whether name is one of the classes; registers them all either way. */
WINFN(BOOL) cc_RegisterClassNameW(LPCWSTR name)
{
	init_classes();
	char *u = name ? utf16_to_utf8_dup(name) : NULL;
	BOOL found = FALSE;
	for (size_t i = 0; u && i < sizeof(control_classes) /
				    sizeof(control_classes[0]); i++)
		if (!strcasecmp(u, control_classes[i].name))
			found = TRUE;
	free(u);
	return found;
}

/* Window subclassing, as Wine's commctrl.c does it. */

static const WCHAR subclass_prop[] = u"CC32SubclassInfo";

struct subclass {
	struct subclass *next;
	SUBCLASSPROC proc;
	UINT_PTR id;
	DWORD_PTR ref;
};

struct subclass_info {
	struct subclass *procs;	/* the newest first */
	struct subclass *stackpos;	/* the next to call */
	WNDPROC orig;
	int unicode;		/* the window's charset when subclassed */
	int running;
};

static struct subclass_info *subclass_info(HWND h)
{
	API();
	return api.GetPropW(h, subclass_prop);
}

/* Once the last subclass goes, the window gets its procedure back. */
static void subclass_cleanup(HWND h, struct subclass_info *s)
{
	if (s->procs || s->running)
		return;
	if (s->unicode)
		api.SetWindowLongW(h, GWL_WNDPROC, (LONG)(ULONG_PTR)s->orig);
	else
		api.SetWindowLongA(h, GWL_WNDPROC, (LONG)(ULONG_PTR)s->orig);
	api.RemovePropW(h, subclass_prop);
	free(s);
}

WINFN(LRESULT) cc_DefSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct subclass_info *s = subclass_info(h);

	if (!s)
		return 0;
	if (!s->stackpos)
		return s->unicode ?
		       api.CallWindowProcW(s->orig, h, msg, wp, lp) :
		       api.CallWindowProcA(s->orig, h, msg, wp, lp);
	struct subclass *p = s->stackpos;
	s->stackpos = p->next;
	return p->proc(h, msg, wp, lp, p->id, p->ref);
}

WINFN(LRESULT) subclass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct subclass_info *s = subclass_info(h);

	if (!s)
		return 0;
	/* Nested messages each walk the chain from the top. */
	struct subclass *saved = s->stackpos;
	s->stackpos = s->procs;
	s->running++;
	LRESULT r = cc_DefSubclassProc(h, msg, wp, lp);
	s->running--;
	s->stackpos = saved;
	if (msg == WM_NCDESTROY && !s->running) {
		/* The window is gone: nothing is left to call them. */
		while (s->procs) {
			struct subclass *next = s->procs->next;
			free(s->procs);
			s->procs = next;
		}
	}
	subclass_cleanup(h, s);
	return r;
}

WINFN(BOOL) cc_SetWindowSubclass(HWND h, SUBCLASSPROC proc, UINT_PTR id,
				 DWORD_PTR ref)
{
	if (!h || !proc)
		return FALSE;
	struct subclass_info *s = subclass_info(h);
	if (!s) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->unicode = api.IsWindowUnicode(h);
		if (!api.SetPropW(h, subclass_prop, s)) {
			free(s);
			return FALSE;
		}
		LONG old = s->unicode ?
			   api.SetWindowLongW(h, GWL_WNDPROC,
					      (LONG)(ULONG_PTR)subclass_proc) :
			   api.SetWindowLongA(h, GWL_WNDPROC,
					      (LONG)(ULONG_PTR)subclass_proc);
		s->orig = (WNDPROC)(ULONG_PTR)old;
	}
	for (struct subclass *p = s->procs; p; p = p->next)
		if (p->proc == proc && p->id == id) {
			p->ref = ref;
			return TRUE;
		}
	struct subclass *p = calloc(1, sizeof(*p));
	if (!p) {
		subclass_cleanup(h, s);
		return FALSE;
	}
	p->proc = proc;
	p->id = id;
	p->ref = ref;
	p->next = s->procs;
	s->procs = p;
	return TRUE;
}

WINFN(BOOL) cc_GetWindowSubclass(HWND h, SUBCLASSPROC proc, UINT_PTR id,
				 DWORD_PTR *ref)
{
	struct subclass_info *s = h ? subclass_info(h) : NULL;

	for (struct subclass *p = s ? s->procs : NULL; p; p = p->next)
		if (p->proc == proc && p->id == id) {
			if (ref)
				*ref = p->ref;
			return TRUE;
		}
	if (ref)
		*ref = 0;
	return FALSE;
}

WINFN(BOOL) cc_RemoveWindowSubclass(HWND h, SUBCLASSPROC proc, UINT_PTR id)
{
	struct subclass_info *s = h ? subclass_info(h) : NULL;

	if (!s)
		return FALSE;
	for (struct subclass **pp = &s->procs; *pp; pp = &(*pp)->next) {
		struct subclass *p = *pp;
		if (p->proc != proc || p->id != id)
			continue;
		*pp = p->next;
		if (s->stackpos == p)
			s->stackpos = p->next;
		free(p);
		subclass_cleanup(h, s);
		return TRUE;
	}
	return FALSE;
}

/* The Create* helpers. */

static HWND create(LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y,
		   int cx, int cy, HWND parent, UINT id)
{
	init_classes();
	return api.CreateWindowExW(0, cls, text, style, x, y, cx, cy, parent,
				   (HMENU)(ULONG_PTR)id, NULL, NULL);
}

WINFN(HWND) cc_CreateStatusWindowW(LONG style, LPCWSTR text, HWND parent,
				   UINT id)
{
	return create(u"msctls_statusbar32", text, (DWORD)style, 0, 0, 0, 0,
		      parent, id);
}

WINFN(HWND) cc_CreateStatusWindowA(LONG style, LPCSTR text, HWND parent,
				   UINT id)
{
	WCHAR *w = text ? a_to_w(text) : NULL;
	HWND h = cc_CreateStatusWindowW(style, w, parent, id);
	free(w);
	return h;
}

WINFN(HWND) cc_CreateUpDownControl(DWORD style, INT x, INT y, INT cx, INT cy,
				   HWND parent, INT id, HINSTANCE inst,
				   HWND buddy, INT upper, INT lower, INT pos)
{
	HWND h = create(u"msctls_updown32", NULL, style, x, y, cx, cy, parent,
			(UINT)id);
	if (h) {
		api.SendMessageW(h, UDM_SETBUDDY, (WPARAM)buddy, 0);
		api.SendMessageW(h, UDM_SETRANGE, 0, MAKELONG(upper, lower));
		api.SendMessageW(h, UDM_SETPOS, 0, MAKELONG(pos, 0));
	}
	return h;
}

/* A toolbar is a plain window here: its buttons are not kept. */
WINFN(HWND) cc_CreateToolbarEx(HWND parent, DWORD style, UINT id, INT nbitmaps,
			       HINSTANCE bm_inst, UINT_PTR bm_id,
			       LPCTBBUTTON buttons, INT nbuttons, INT dx_button,
			       INT dy_button, INT dx_bitmap, INT dy_bitmap,
			       UINT struct_size)
{
	return create(u"ToolbarWindow32", NULL, WS_CHILD | style, 0, 0, 100,
		      30, parent, id);
}

WINFN(HWND) cc_CreateToolbar(HWND parent, DWORD style, UINT id, INT nbitmaps,
			     HINSTANCE bm_inst, UINT bm_id, LPCTBBUTTON buttons,
			     INT nbuttons)
{
	return cc_CreateToolbarEx(parent, style | CCS_TOP, id, nbitmaps,
				  bm_inst, bm_id, buttons, nbuttons, 0, 0, 0, 0,
				  12);
}

/* A bitmap resource with its colours mapped (by default to the 3-D ones). */
WINFN(HBITMAP) cc_CreateMappedBitmap(HINSTANCE inst, INT_PTR id, UINT flags,
				     LPCOLORMAP map, INT nmaps)
{
	API();
	COLORMAP defaults[4] = {
		{ RGB(0, 0, 0), api.GetSysColor(COLOR_BTNTEXT) },
		{ RGB(128, 128, 128), api.GetSysColor(COLOR_BTNSHADOW) },
		{ RGB(192, 192, 192), api.GetSysColor(COLOR_BTNFACE) },
		{ RGB(255, 255, 255), api.GetSysColor(COLOR_BTNHIGHLIGHT) },
	};
	if (!map) {
		map = defaults;
		nmaps = 4;
	}
	HBITMAP bmp = api.LoadImageW(inst, (LPCWSTR)id, IMAGE_BITMAP, 0, 0,
				     LR_CREATEDIBSECTION);
	BITMAP bm;
	if (!bmp || api.GetObjectW(bmp, sizeof(bm), &bm) != sizeof(bm))
		return bmp;
	HDC dc = api.CreateCompatibleDC(NULL);
	HGDIOBJ old = api.SelectObject(dc, bmp);
	for (int y = 0; y < bm.bmHeight; y++)
		for (int x = 0; x < bm.bmWidth; x++) {
			COLORREF c = api.GetPixel(dc, x, y);
			for (int i = 0; i < nmaps; i++)
				if (c == map[i].from) {
					api.SetPixel(dc, x, y, map[i].to);
					break;
				}
		}
	api.SelectObject(dc, old);
	api.DeleteDC(dc);
	return bmp;
}

/* The rest. */

static LANGID mui_language;

WINFN(void) cc_InitMUILanguage(LANGID lang) { mui_language = lang; }
WINFN(LANGID) cc_GetMUILanguage(void) { return mui_language; }

/* Version 5.82, as Windows XP's comctl32 reports without a manifest. */
WINFN(HRESULT) cc_DllGetVersion(DLLVERSIONINFO *v)
{
	if (!v || (v->cbSize != sizeof(DLLVERSIONINFO) &&
		   v->cbSize != sizeof(DLLVERSIONINFO2)))
		return E_INVALIDARG;
	v->dwMajorVersion = 5;
	v->dwMinorVersion = 82;
	v->dwBuildNumber = 2900;
	v->dwPlatformID = DLLVER_PLATFORM_NT;
	if (v->cbSize == sizeof(DLLVERSIONINFO2)) {
		DLLVERSIONINFO2 *v2 = (DLLVERSIONINFO2 *)v;
		v2->dwFlags = 0;
		v2->ullVersion = (5ull << 48) | (82ull << 32) | (2900ull << 16);
	}
	return S_OK;
}

WINFN(HRESULT) cc_DllInstall(BOOL install, LPCWSTR cmdline) { return S_OK; }

/* No mouse: nothing is ever tracked. */
WINFN(BOOL) cc__TrackMouseEvent(TRACKMOUSEEVENT *t)
{
	if (!t || t->cbSize != sizeof(*t))
		return FALSE;
	if (t->dwFlags & TME_QUERY) {
		t->dwFlags = 0;
		t->hwndTrack = NULL;
		t->dwHoverTime = 0;
	}
	return TRUE;
}

/* No menus to help with. */
WINFN(void) cc_MenuHelp(UINT msg, WPARAM wp, LPARAM lp, HMENU menu,
			HINSTANCE inst, HWND status, UINT *ids)
{
}

/*
 * lpInfo: two ints (the second the menu), then (menu item, control id)
 * pairs ending with 0. Shows or hides the control paired with item.
 */
WINFN(BOOL) cc_ShowHideMenuCtl(HWND h, UINT_PTR item, INT *info)
{
	if (!info)
		return FALSE;
	API();
	for (INT *p = info + 2; *p; p += 2) {
		if ((UINT_PTR)(UINT)*p != item)
			continue;
		HWND ctl = api.GetDlgItem(h, p[1]);
		if (!ctl)
			return FALSE;
		api.ShowWindow(ctl, style_of(ctl) & WS_VISIBLE ? SW_HIDE : SW_SHOW);
		return TRUE;
	}
	return FALSE;
}

/* SubtractRect: only a band covering a whole side comes off. */
static void subtract(RECT *r, const RECT *c)
{
	if (c->left >= r->right || c->right <= r->left ||
	    c->top >= r->bottom || c->bottom <= r->top)
		return;
	if (c->left <= r->left && c->right >= r->right) {
		if (c->top <= r->top)
			r->top = c->bottom < r->bottom ? c->bottom : r->bottom;
		else if (c->bottom >= r->bottom)
			r->bottom = c->top;
	} else if (c->top <= r->top && c->bottom >= r->bottom) {
		if (c->left <= r->left)
			r->left = c->right < r->right ? c->right : r->right;
		else if (c->right >= r->right)
			r->right = c->left;
	}
}

/*
 * The client area less the visible controls in info (laid out as for
 * ShowHideMenuCtl), such as a toolbar and a status bar.
 */
WINFN(void) cc_GetEffectiveClientRect(HWND h, LPRECT rc, const INT *info)
{
	API();
	if (!rc || !api.GetClientRect(h, rc) || !info)
		return;
	for (const INT *p = info + 2; *p; p += 2) {
		HWND ctl = api.GetDlgItem(h, p[1]);
		RECT c;
		if (!ctl || !(style_of(ctl) & WS_VISIBLE) ||
		    !api.GetWindowRect(ctl, &c))
			continue;
		api.ScreenToClient(h, (POINT *)&c.left);
		api.ScreenToClient(h, (POINT *)&c.right);
		subtract(rc, &c);
	}
}

WINFN(void) cc_DrawStatusTextW(HDC dc, const RECT *rc, LPCWSTR text, UINT flags)
{
	API();
	if (dc && rc && text)
		api.TextOutW(dc, rc->left + 2, rc->top + 1, text,
			     (int)wstrlen(text));
}

WINFN(void) cc_DrawStatusTextA(HDC dc, const RECT *rc, LPCSTR text, UINT flags)
{
	WCHAR *w = text ? a_to_w(text) : NULL;
	cc_DrawStatusTextW(dc, rc, w, flags);
	free(w);
}

/* Drag lists: there is no mouse to drag with. */
WINFN(BOOL) cc_MakeDragList(HWND list) { return list != NULL; }
WINFN(INT) cc_LBItemFromPt(HWND list, POINT pt, BOOL scroll) { return -1; }
WINFN(void) cc_DrawInsert(HWND parent, HWND list, INT item) { }

/* WM_NOTIFY from hwndFrom (or none) to hwndTo (or hwndFrom's parent). */
static LRESULT notify(HWND to, HWND from, UINT code, NMHDR *hdr)
{
	NMHDR local;

	API();
	if (!hdr)
		hdr = &local;
	hdr->hwndFrom = from;
	hdr->idFrom = from ? (UINT_PTR)(UINT)api.GetWindowLongW(from, GWL_ID) : 0;
	hdr->code = code;
	if (!to && from)
		to = api.GetParent(from);
	return to ? api.SendMessageW(to, WM_NOTIFY, hdr->idFrom, (LPARAM)hdr) : 0;
}

WINFN(LRESULT) cc_SendNotify(HWND to, HWND from, UINT code, NMHDR *hdr)
{
	return notify(to, from, code, hdr);
}

WINFN(LRESULT) cc_SendNotifyEx(HWND to, HWND from, UINT code, NMHDR *hdr,
			       DWORD param)
{
	return notify(to, from, code, hdr);
}

/* Icons are handles with nothing behind them in this user32. */
static HRESULT load_icon(HINSTANCE inst, LPCWSTR name, int cx, int cy,
			 HICON *out)
{
	API();
	if (!out)
		return E_INVALIDARG;
	*out = api.LoadImageW(inst, name, IMAGE_ICON, cx, cy, 0);
	return *out ? S_OK : E_FAIL;
}

WINFN(HRESULT) cc_LoadIconMetric(HINSTANCE inst, LPCWSTR name, int metric,
				 HICON *out)
{
	if (metric != LIM_SMALL && metric != LIM_LARGE)
		return E_INVALIDARG;
	int size = metric == LIM_SMALL ? 16 : 32;
	return load_icon(inst, name, size, size, out);
}

WINFN(HRESULT) cc_LoadIconWithScaleDown(HINSTANCE inst, LPCWSTR name, int cx,
					int cy, HICON *out)
{
	return load_icon(inst, name, cx, cy, out);
}

/* The Unicode wrappers comctl32 exports for shell code: straight to gdi32. */
WINFN(BOOL) cc_ExtTextOutWrap(HDC dc, INT x, INT y, UINT flags,
			      const RECT *rc, LPCWSTR s, UINT n, const INT *dx)
{
	API();
	return api.ExtTextOutW(dc, x, y, flags, rc, s, n, dx);
}

WINFN(BOOL) cc_GetTextExtentPoint32Wrap(HDC dc, LPCWSTR s, INT n, LPSIZE sz)
{
	API();
	return api.GetTextExtentPoint32W(dc, s, n, sz);
}

WINFN(BOOL) cc_TextOutWrap(HDC dc, INT x, INT y, LPCWSTR s, INT n)
{
	API();
	return api.TextOutW(dc, x, y, s, n);
}

#define E(name, ord) { #name, ord, (void *)cc_##name }

static const struct builtin_export exports[] = {
	E(MenuHelp, 2),
	E(ShowHideMenuCtl, 3),
	E(GetEffectiveClientRect, 4),
	E(DrawStatusTextA, 5),
	E(CreateStatusWindowA, 6),
	E(CreateToolbar, 7),
	E(CreateMappedBitmap, 8),
	E(MakeDragList, 13),
	E(LBItemFromPt, 14),
	E(DrawInsert, 15),
	E(CreateUpDownControl, 16),
	E(InitCommonControls, 17),
	E(SendNotify, 341),
	E(SendNotifyEx, 342),
	E(LoadIconMetric, 380),
	E(LoadIconWithScaleDown, 381),
	E(SetWindowSubclass, 410),
	E(GetWindowSubclass, 411),
	E(RemoveWindowSubclass, 412),
	E(DefSubclassProc, 413),
	E(ExtTextOutWrap, 417),
	E(GetTextExtentPoint32Wrap, 420),
	E(TextOutWrap, 421),
	{ "CreateStatusWindow", 0, (void *)cc_CreateStatusWindowA },
	E(CreateStatusWindowW, 0),
	E(CreateToolbarEx, 0),
	E(DllGetVersion, 0),
	E(DllInstall, 0),
	{ "DrawStatusText", 0, (void *)cc_DrawStatusTextA },
	E(DrawStatusTextW, 0),
	E(GetMUILanguage, 0),
	E(InitCommonControlsEx, 0),
	E(InitMUILanguage, 0),
	E(RegisterClassNameW, 0),
	E(_TrackMouseEvent, 0),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = {
	exports, comctl_dpa_exports, comctl_imagelist_exports,
	comctl_sheet_exports, NULL
};

static const char *const aliases[] = { "comctl32.ocx", NULL };

const struct builtin_dll builtin_comctl32 = { "comctl32.dll", aliases, tables, 0 };
