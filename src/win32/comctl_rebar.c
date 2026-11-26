/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's rebar (ReBarWindow32), headless: its bands (style, colours,
 * text, image, child window and its sizes, width, id, ideal width,
 * lParam, header width), the bar's image list, colours and palette, with
 * Windows' messages and the RBN_ notifications. Nothing is drawn. Bands
 * sit side by side in rows (RBBS_BREAK starts a new one), each as wide as
 * its cx but at least its header and minimum child width, the last band
 * of a row taking what is left of the bar's width. A band's header is its
 * gripper (unless RBBS_NOGRIPPER or RBBS_FIXEDSIZE), image and text, or
 * its cxHeader. A row is as high as its tallest band's content (child,
 * image or text) plus a margin above and below; the child is moved to
 * the band's area after the header. Unless CCS_NORESIZE, the bar takes
 * its parent's width and its rows' height (RBN_HEIGHTCHANGE when that
 * changes).
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define GRIPPER_CX   10
#define HEADER_PAD   4
#define MARGIN_CY    2
#define LINE_CY      16
#define ICON_CX      16

struct rb_band {
	UINT style;
	COLORREF fore, back;
	WCHAR *text;
	int image;
	HWND child;
	UINT cx_min, cy_min, cx;
	HBITMAP bmp;
	UINT id;
	UINT cy_child, cy_max, cy_integral, cx_ideal;
	LPARAM lParam;
	UINT cx_header;
	BOOL header_set;
	RECT rc;
};

struct rebar {
	struct rb_band *b;
	int n, cap;
	HIMAGELIST himl;
	COLORREF bk, fg;
	HPALETTE palette;
	HWND notify, tooltips;
	int height;
	BOOL busy, unicode;
	int drag;		/* the band being dragged, or -1 */
	int drag_x;
	HFONT font;
};

static HWND target(HWND h, const struct rebar *s)
{
	return s->notify ? s->notify : comctl_parent(h);
}

static LRESULT notify_band(HWND h, const struct rebar *s, UINT code, int i)
{
	NMREBAR nm;
	memset(&nm, 0, sizeof(nm));
	nm.uBand = (UINT)i;
	if (i >= 0 && i < s->n) {
		nm.dwMask = RBNM_ID | RBNM_STYLE | RBNM_LPARAM;
		nm.fStyle = s->b[i].style;
		nm.wID = s->b[i].id;
		nm.lParam = s->b[i].lParam;
	}
	return comctl_notify_to(target(h, s), h, code, &nm.hdr);
}

static int header_cx(const struct rebar *s, const struct rb_band *b)
{
	if (b->header_set)
		return (int)b->cx_header;
	int w = 0;
	if (!(b->style & (RBBS_NOGRIPPER | RBBS_FIXEDSIZE)))
		w += GRIPPER_CX;
	if (b->image >= 0 && s->himl)
		w += ICON_CX + HEADER_PAD;
	if (b->text && *b->text && !(b->style & RBBS_HIDETITLE))
		w += comctl_text_width(b->text) + HEADER_PAD;
	return w;
}

static int content_cy(const struct rebar *s, const struct rb_band *b)
{
	int cy = 0;
	if (b->child)
		cy = (int)b->cy_min;
	if ((b->style & RBBS_VARIABLEHEIGHT) && (int)b->cy_child > cy)
		cy = (int)b->cy_child;
	if (b->text && *b->text && cy < LINE_CY)
		cy = LINE_CY;
	if (b->image >= 0 && s->himl && cy < LINE_CY)
		cy = LINE_CY;
	return cy;
}

static int min_cx(const struct rebar *s, const struct rb_band *b)
{
	return header_cx(s, b) + (int)b->cx_min;
}

static int bar_width(HWND h)
{
	RECT rc;
	HWND parent = comctl_parent(h);
	if (!(comctl_style(h) & CCS_NORESIZE) && parent)
		comctl_client_rect(parent, &rc);
	else
		comctl_client_rect(h, &rc);
	return rc.right - rc.left;
}

static void place_children(const struct rebar *s)
{
	for (int i = 0; i < s->n; i++) {
		const struct rb_band *b = &s->b[i];
		if (!b->child || (b->style & RBBS_HIDDEN))
			continue;
		int x = b->rc.left + header_cx(s, b);
		int cy = content_cy(s, b);
		comctl_move(b->child, x, b->rc.top + MARGIN_CY,
			    b->rc.right - x, cy);
	}
}

static void layout(HWND h, struct rebar *s)
{
	if (s->busy)
		return;
	s->busy = TRUE;
	int width = bar_width(h), y = 0, first = 0;

	while (first < s->n) {
		/* The row: up to the next RBBS_BREAK. */
		int last = first, row_cy = 0, x = 0, end = first;
		for (int i = first; i < s->n; i++) {
			if (i > first && (s->b[i].style & RBBS_BREAK) &&
			    end > first)
				break;
			end = i + 1;
		}
		for (int i = first; i < end; i++) {
			struct rb_band *b = &s->b[i];
			if (b->style & RBBS_HIDDEN) {
				memset(&b->rc, 0, sizeof(b->rc));
				continue;
			}
			int cy = content_cy(s, b) + 2 * MARGIN_CY;
			if (cy > row_cy)
				row_cy = cy;
			int w = (int)b->cx;
			if (w < min_cx(s, b))
				w = min_cx(s, b);
			b->rc.left = x;
			b->rc.right = x + w;
			x += w;
			last = i;
		}
		if (last >= first && !(s->b[last].style & RBBS_HIDDEN) &&
		    s->b[last].rc.right < width)
			s->b[last].rc.right = width;
		for (int i = first; i < end; i++) {
			if (s->b[i].style & RBBS_HIDDEN)
				continue;
			s->b[i].rc.top = y;
			s->b[i].rc.bottom = y + row_cy;
		}
		y += row_cy;
		first = end;
	}
	place_children(s);

	int old = s->height;
	s->height = y;
	DWORD st = comctl_style(h);
	HWND parent = comctl_parent(h);
	if (!(st & CCS_NORESIZE) && parent) {
		RECT pc;
		comctl_client_rect(parent, &pc);
		int top = (st & CCS_BOTTOM) == CCS_BOTTOM ? pc.bottom - y : 0;
		comctl_move(h, 0, top, pc.right - pc.left, y);
	}
	s->busy = FALSE;
	if (old != s->height)
		comctl_notify_to(target(h, s), h, RBN_HEIGHTCHANGE, NULL);
}

static int rows(const struct rebar *s)
{
	int r = 0, last = -1;
	for (int i = 0; i < s->n; i++) {
		if (s->b[i].style & RBBS_HIDDEN)
			continue;
		if (s->b[i].rc.top != last) {
			last = s->b[i].rc.top;
			r++;
		}
	}
	return r;
}

/* REBARBANDINFO's A and W forms differ only in the text pointer's type;
 * cbSize says how much of it the caller has (at least to wID). */
static BOOL valid(const REBARBANDINFOW *bi)
{
	return bi && bi->cbSize >= offsetof(REBARBANDINFOW, cyChild);
}

static BOOL covers(const REBARBANDINFOW *bi, size_t end)
{
	return bi->cbSize >= end;
}

#define HAS(field) covers(bi, offsetof(REBARBANDINFOW, field) + \
			      sizeof(bi->field))

static void set_fields(struct rb_band *b, const REBARBANDINFOW *bi, int wide)
{
	UINT m = bi->fMask;

	if (m & RBBIM_STYLE)
		b->style = bi->fStyle;
	if (m & RBBIM_COLORS) {
		b->fore = bi->clrFore;
		b->back = bi->clrBack;
	}
	if (m & RBBIM_TEXT) {
		free(b->text);
		b->text = !bi->lpText ? NULL :
			  wide ? comctl_w_dup(bi->lpText) :
				 comctl_a_to_w((const char *)bi->lpText);
	}
	if (m & RBBIM_IMAGE)
		b->image = bi->iImage;
	if (m & RBBIM_CHILD)
		b->child = bi->hwndChild;
	if (m & RBBIM_CHILDSIZE) {
		b->cx_min = bi->cxMinChild;
		b->cy_min = bi->cyMinChild;
		if (HAS(cyIntegral)) {
			b->cy_child = bi->cyChild;
			b->cy_max = bi->cyMaxChild;
			b->cy_integral = bi->cyIntegral;
		}
	}
	if (m & RBBIM_SIZE)
		b->cx = bi->cx;
	if (m & RBBIM_BACKGROUND)
		b->bmp = bi->hbmBack;
	if (m & RBBIM_ID)
		b->id = bi->wID;
	if ((m & RBBIM_IDEALSIZE) && HAS(cxIdeal))
		b->cx_ideal = bi->cxIdeal;
	if ((m & RBBIM_LPARAM) && HAS(lParam))
		b->lParam = bi->lParam;
	if ((m & RBBIM_HEADERSIZE) && HAS(cxHeader)) {
		b->cx_header = bi->cxHeader;
		b->header_set = TRUE;
	}
}

static void get_fields(const struct rebar *s, const struct rb_band *b,
		       REBARBANDINFOW *bi, int wide)
{
	UINT m = bi->fMask;

	if (m & RBBIM_STYLE)
		bi->fStyle = b->style;
	if (m & RBBIM_COLORS) {
		bi->clrFore = b->fore;
		bi->clrBack = b->back;
	}
	if ((m & RBBIM_TEXT) && bi->lpText && bi->cch)
		comctl_copy_text_out(b->text ? b->text : u"", bi->lpText,
				     (int)bi->cch, wide);
	if (m & RBBIM_IMAGE)
		bi->iImage = b->image;
	if (m & RBBIM_CHILD)
		bi->hwndChild = b->child;
	if (m & RBBIM_CHILDSIZE) {
		bi->cxMinChild = b->cx_min;
		bi->cyMinChild = b->cy_min;
		if (HAS(cyIntegral)) {
			bi->cyChild = b->cy_child;
			bi->cyMaxChild = b->cy_max;
			bi->cyIntegral = b->cy_integral;
		}
	}
	if (m & RBBIM_SIZE)
		bi->cx = b->cx;
	if (m & RBBIM_BACKGROUND)
		bi->hbmBack = b->bmp;
	if (m & RBBIM_ID)
		bi->wID = b->id;
	if ((m & RBBIM_IDEALSIZE) && HAS(cxIdeal))
		bi->cxIdeal = b->cx_ideal;
	if ((m & RBBIM_LPARAM) && HAS(lParam))
		bi->lParam = b->lParam;
	if ((m & RBBIM_HEADERSIZE) && HAS(cxHeader))
		bi->cxHeader = (UINT)header_cx(s, b);
}

static BOOL insert(HWND h, struct rebar *s, int at,
		   const REBARBANDINFOW *bi, int wide)
{
	if (!valid(bi))
		return FALSE;
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 8;
		void *p = realloc(s->b, (size_t)cap * sizeof(*s->b));
		if (!p)
			return FALSE;
		s->b = p;
		s->cap = cap;
	}
	if (at < 0 || at > s->n)
		at = s->n;
	memmove(&s->b[at + 1], &s->b[at], (size_t)(s->n - at) * sizeof(*s->b));
	struct rb_band *b = &s->b[at];
	memset(b, 0, sizeof(*b));
	b->image = -1;
	b->fore = b->back = CLR_NONE;
	set_fields(b, bi, wide);
	s->n++;
	layout(h, s);
	return TRUE;
}

static BOOL delete(HWND h, struct rebar *s, int i)
{
	if (i < 0 || i >= s->n)
		return FALSE;
	notify_band(h, s, RBN_DELETINGBAND, i);
	UINT id = s->b[i].id;
	free(s->b[i].text);
	memmove(&s->b[i], &s->b[i + 1], (size_t)(s->n - i - 1) * sizeof(*s->b));
	s->n--;
	NMREBAR nm;
	memset(&nm, 0, sizeof(nm));
	nm.dwMask = RBNM_ID;
	nm.uBand = (UINT)i;
	nm.wID = id;
	comctl_notify_to(target(h, s), h, RBN_DELETEDBAND, &nm.hdr);
	if (s->drag == i)
		s->drag = -1;
	layout(h, s);
	return TRUE;
}

static int hit(const struct rebar *s, POINT pt, UINT *flags)
{
	for (int i = 0; i < s->n; i++) {
		const struct rb_band *b = &s->b[i];
		if (b->style & RBBS_HIDDEN)
			continue;
		if (pt.x < b->rc.left || pt.x >= b->rc.right ||
		    pt.y < b->rc.top || pt.y >= b->rc.bottom)
			continue;
		int dx = pt.x - b->rc.left;
		BOOL grip = !(b->style & (RBBS_NOGRIPPER | RBBS_FIXEDSIZE));
		if (grip && dx < GRIPPER_CX)
			*flags = RBHT_GRABBER;
		else if (dx < header_cx(s, b))
			*flags = RBHT_CAPTION;
		else
			*flags = RBHT_CLIENT;
		return i;
	}
	*flags = RBHT_NOWHERE;
	return -1;
}

/* The band before i in its row, or -1 when i starts the row. */
static int previous_in_row(const struct rebar *s, int i)
{
	for (int j = i - 1; j >= 0; j--) {
		if (s->b[j].style & RBBS_HIDDEN)
			continue;
		return s->b[j].rc.top == s->b[i].rc.top ? j : -1;
	}
	return -1;
}

static void maximize(HWND h, struct rebar *s, int i, BOOL ideal)
{
	struct rb_band *b = &s->b[i];
	if (ideal && b->cx_ideal) {
		b->cx = (UINT)header_cx(s, b) + b->cx_ideal;
	} else {
		/* The others in its row shrink to their minimum. */
		int width = bar_width(h), used = 0;
		for (int j = 0; j < s->n; j++) {
			if (j == i || (s->b[j].style & RBBS_HIDDEN) ||
			    s->b[j].rc.top != b->rc.top)
				continue;
			s->b[j].cx = (UINT)min_cx(s, &s->b[j]);
			used += (int)s->b[j].cx;
		}
		b->cx = (UINT)(width - used > min_cx(s, b) ? width - used :
							      min_cx(s, b));
	}
	layout(h, s);
}

LRESULT STUB_WINAPI comctl_rebar_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct rebar *s = comctl_state(h);
	int wide = 1;

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->bk = s->fg = CLR_NONE;
		s->drag = -1;
		s->unicode = TRUE;
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	REBARBANDINFOW *bi = (REBARBANDINFOW *)lp;
	switch (msg) {
	case RB_INSERTBANDA:
		wide = 0;
		/* fall through */
	case RB_INSERTBANDW:
		return insert(h, s, (int)wp, bi, wide);
	case RB_DELETEBAND:
		return delete(h, s, (int)wp);
	case RB_GETBANDINFOA:
		wide = 0;
		/* fall through */
	case RB_GETBANDINFOW:
		if ((int)wp < 0 || (int)wp >= s->n || !valid(bi))
			return FALSE;
		get_fields(s, &s->b[wp], bi, wide);
		return TRUE;
	case RB_SETBANDINFOA:
		wide = 0;
		/* fall through */
	case RB_SETBANDINFOW:
		if ((int)wp < 0 || (int)wp >= s->n || !valid(bi))
			return FALSE;
		set_fields(&s->b[wp], bi, wide);
		layout(h, s);
		return TRUE;
	case RB_GETBARINFO: {
		REBARINFO *ri = (REBARINFO *)lp;
		if (!ri || ri->cbSize < sizeof(*ri))
			return FALSE;
		if (ri->fMask & RBIM_IMAGELIST)
			ri->himl = s->himl;
		return TRUE;
	}
	case RB_SETBARINFO: {
		const REBARINFO *ri = (const REBARINFO *)lp;
		if (!ri || ri->cbSize < sizeof(*ri))
			return FALSE;
		if (ri->fMask & RBIM_IMAGELIST)
			s->himl = ri->himl;
		layout(h, s);
		return TRUE;
	}
	case RB_SETPARENT: {
		HWND old = target(h, s);
		s->notify = (HWND)wp;
		return (LRESULT)old;
	}
	case RB_HITTEST: {
		RBHITTESTINFO *hi = (RBHITTESTINFO *)lp;
		if (!hi)
			return -1;
		hi->iBand = hit(s, hi->pt, &hi->flags);
		return hi->iBand;
	}
	case RB_GETRECT:
		if ((int)wp < 0 || (int)wp >= s->n || !lp)
			return FALSE;
		*(RECT *)lp = s->b[wp].rc;
		return TRUE;
	case RB_GETBANDCOUNT:
		return s->n;
	case RB_GETROWCOUNT:
		return rows(s);
	case RB_GETROWHEIGHT:
		if ((int)wp < 0 || (int)wp >= s->n)
			return 0;
		return s->b[wp].rc.bottom - s->b[wp].rc.top;
	case RB_GETBARHEIGHT:
		return s->height;
	case RB_IDTOINDEX:
		for (int i = 0; i < s->n; i++)
			if (s->b[i].id == (UINT)wp)
				return i;
		return -1;
	case RB_GETTOOLTIPS:
		return (LRESULT)s->tooltips;
	case RB_SETTOOLTIPS:
		s->tooltips = (HWND)wp;
		return 0;
	case RB_SETBKCOLOR: {
		COLORREF old = s->bk;
		s->bk = (COLORREF)lp;
		return old;
	}
	case RB_GETBKCOLOR:
		return s->bk;
	case RB_SETTEXTCOLOR: {
		COLORREF old = s->fg;
		s->fg = (COLORREF)lp;
		return old;
	}
	case RB_GETTEXTCOLOR:
		return s->fg;
	case RB_SIZETORECT: {
		const RECT *rc = (const RECT *)lp;
		if (!rc)
			return FALSE;
		int old = s->height;
		comctl_move(h, rc->left, rc->top, rc->right - rc->left,
			    rc->bottom - rc->top);
		layout(h, s);
		return old != s->height;
	}
	case RB_BEGINDRAG:
		if ((int)wp < 0 || (int)wp >= s->n ||
		    notify_band(h, s, RBN_BEGINDRAG, (int)wp))
			return 0;
		s->drag = (int)wp;
		s->drag_x = lp == (LPARAM)-1 ? s->b[wp].rc.left :
					      (short)LOWORD(lp);
		return 0;
	case RB_DRAGMOVE: {
		if (s->drag < 0)
			return 0;
		int x = (short)LOWORD(lp), dx = x - s->drag_x;
		int p = previous_in_row(s, s->drag);
		if (p < 0 || !dx)
			return 0;
		int w = s->b[p].rc.right - s->b[p].rc.left + dx;
		if (w < min_cx(s, &s->b[p]))
			w = min_cx(s, &s->b[p]);
		s->b[p].cx = (UINT)w;
		s->drag_x = x;
		layout(h, s);
		return 0;
	}
	case RB_ENDDRAG:
		if (s->drag >= 0)
			notify_band(h, s, RBN_ENDDRAG, s->drag);
		s->drag = -1;
		return 0;
	case RB_MINIMIZEBAND:
		if ((int)wp < 0 || (int)wp >= s->n)
			return 0;
		s->b[wp].cx = (UINT)min_cx(s, &s->b[wp]);
		layout(h, s);
		return 0;
	case RB_MAXIMIZEBAND:
		if ((int)wp >= 0 && (int)wp < s->n)
			maximize(h, s, (int)wp, (BOOL)lp);
		return 0;
	case RB_GETBANDBORDERS: {
		RECT *rc = (RECT *)lp;
		if ((int)wp < 0 || (int)wp >= s->n || !rc)
			return 0;
		rc->left = header_cx(s, &s->b[wp]);
		rc->top = rc->bottom = MARGIN_CY;
		rc->right = 0;
		return 0;
	}
	case RB_SHOWBAND:
		if ((int)wp < 0 || (int)wp >= s->n)
			return FALSE;
		if (lp)
			s->b[wp].style &= ~(UINT)RBBS_HIDDEN;
		else
			s->b[wp].style |= RBBS_HIDDEN;
		layout(h, s);
		return TRUE;
	case RB_MOVEBAND: {
		int from = (int)wp, to = (int)lp;
		if (from < 0 || from >= s->n || to < 0 || to >= s->n)
			return FALSE;
		struct rb_band t = s->b[from];
		if (from < to)
			memmove(&s->b[from], &s->b[from + 1],
				(size_t)(to - from) * sizeof(*s->b));
		else
			memmove(&s->b[to + 1], &s->b[to],
				(size_t)(from - to) * sizeof(*s->b));
		s->b[to] = t;
		layout(h, s);
		return TRUE;
	}
	case RB_SETPALETTE: {
		HPALETTE old = s->palette;
		s->palette = (HPALETTE)lp;
		return (LRESULT)old;
	}
	case RB_GETPALETTE:
		return (LRESULT)s->palette;
	case RB_GETBANDMARGINS:
	case RB_PUSHCHEVRON:
		return 0;
	case RB_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case RB_GETUNICODEFORMAT:
		return s->unicode;
	case WM_SIZE:
	case WM_STYLECHANGED:
		layout(h, s);
		break;
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		for (int i = 0; i < s->n; i++)
			free(s->b[i].text);
		free(s->b);
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}
