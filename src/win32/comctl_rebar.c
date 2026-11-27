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
	unsigned uid;		/* the band's identity while indexes move */
};

struct rebar {
	struct rb_band *b;
	int n, cap;
	HIMAGELIST himl;
	COLORREF bk, fg;
	HPALETTE palette;
	HWND notify, tooltips;
	int height;
	BOOL busy, again, moving, unicode;	/* moving: the bar itself */
	BOOL deferred;		/* a WIN_DEFERRED_LAYOUT is posted */
	int width;		/* RB_SIZETORECT's width while it lays out, or 0 */
	unsigned drag;		/* the uid of the band being dragged, or 0 */
	int drag_x;
	int mouse_x;		/* the last mouse position seen, for (DWORD)-1 */
	unsigned next_uid;
	HFONT font;
};

static int find_uid(const struct rebar *s, unsigned uid)
{
	for (int i = 0; uid && i < s->n; i++)
		if (s->b[i].uid == uid)
			return i;
	return -1;
}

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

static int bar_width(HWND h, const struct rebar *s)
{
	RECT rc;
	if (s->width > 0)
		return s->width;
	HWND parent = comctl_parent(h);
	if (!(comctl_style(h) & CCS_NORESIZE) && parent)
		comctl_client_rect(parent, &rc);
	else
		comctl_client_rect(h, &rc);
	return rc.right - rc.left;
}

/* Moving a child sends it messages whose handlers may change the bands
 * (then the layout runs again, s->again) or destroy the rebar (FALSE). */
static BOOL place_children(HWND h, struct rebar *s, BOOL all)
{
	/* A layout asked for from a child's handler stops the round early,
	 * unless this is the last pass (all), which places every child. */
	for (int i = 0; i < s->n && (all || !s->again); i++) {
		const struct rb_band *b = &s->b[i];
		if (!b->child || (b->style & RBBS_HIDDEN))
			continue;
		HWND child = b->child;
		int x = b->rc.left + header_cx(s, b);
		int y = b->rc.top + MARGIN_CY, cx = b->rc.right - x;
		int cy = content_cy(s, b);
		comctl_move(child, x, y, cx, cy);
		if (!comctl_alive(h, s))
			return FALSE;
	}
	return TRUE;
}

/* Show or hide a band's child with the band (RBBS_HIDDEN). */
static void show_child(const struct rb_band *b, UINT old_style)
{
	if (b->child && ((old_style ^ b->style) & RBBS_HIDDEN))
		comctl_show(b->child, (b->style & RBBS_HIDDEN) ? SW_HIDE :
							       SW_SHOWNA);
}

static void compute(HWND h, struct rebar *s, int *height);

/* Lay the bands out and move the children and the bar. A layout asked
 * for from inside (a child's or the parent's handler) makes this one run
 * again with the new bands; afterwards s may be gone (comctl_alive). */
static void layout(HWND h, struct rebar *s)
{
	if (s->busy) {
		s->again = TRUE;
		return;
	}
	s->busy = TRUE;
	int old = s->height, y = 0;
	enum { PASSES = 8 };
	for (int pass = 0; pass < PASSES; pass++) {
		s->again = FALSE;
		compute(h, s, &y);
		if (!place_children(h, s, pass == PASSES - 1))
			return;
		/* Handlers that keep asking for layouts get PASSES of them;
		 * the last one places every child, the height and the bar,
		 * and what they asked for after that is posted for later. */
		if (s->again && pass < PASSES - 1)
			continue;
		s->height = y;
		DWORD st = comctl_style(h);
		HWND parent = comctl_parent(h);
		if (!(st & CCS_NORESIZE) && parent && !s->width) {
			RECT pc;
			comctl_client_rect(parent, &pc);
			int top = (st & CCS_BOTTOM) == CCS_BOTTOM ?
				  pc.bottom - y : 0;
			/* Its own WM_SIZE needs no second pass. */
			s->moving = TRUE;
			comctl_move(h, 0, top, pc.right - pc.left, y);
			if (!comctl_alive(h, s))
				return;
			s->moving = FALSE;
		}
		if (!s->again)
			break;
	}
	s->busy = FALSE;
	/* Marked only once it is queued, so a failed post leaves the next
	 * layout to try again. */
	if (s->again && !s->deferred)
		s->deferred = comctl_post(h, WIN_DEFERRED_LAYOUT, 0, 0);
	s->again = FALSE;
	if (old != s->height)
		comctl_notify_to(target(h, s), h, RBN_HEIGHTCHANGE, NULL);
}

static void compute(HWND h, struct rebar *s, int *height)
{
	int width = bar_width(h, s), y = 0, first = 0;

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
	*height = y;
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

/* FALSE, changing nothing, when the new title cannot be copied. */
static BOOL set_fields(struct rb_band *b, const REBARBANDINFOW *bi, int wide)
{
	UINT m = bi->fMask;
	WCHAR *text = NULL;

	if ((m & RBBIM_TEXT) && bi->lpText) {
		text = wide ? comctl_w_dup(bi->lpText) :
			      comctl_a_to_w((const char *)bi->lpText);
		if (!text)
			return FALSE;
	}
	if (m & RBBIM_STYLE)
		b->style = bi->fStyle;
	if (m & RBBIM_COLORS) {
		b->fore = bi->clrFore;
		b->back = bi->clrBack;
	}
	if (m & RBBIM_TEXT) {
		free(b->text);
		b->text = text;
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
	return TRUE;
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
	struct rb_band nb;
	memset(&nb, 0, sizeof(nb));
	nb.image = -1;
	nb.fore = nb.back = CLR_NONE;
	if (!set_fields(&nb, bi, wide))
		return FALSE;
	nb.uid = ++s->next_uid ? s->next_uid : ++s->next_uid;
	memmove(&s->b[at + 1], &s->b[at], (size_t)(s->n - at) * sizeof(*s->b));
	s->b[at] = nb;
	s->n++;
	if (nb.child && (nb.style & RBBS_HIDDEN)) {
		comctl_show(nb.child, SW_HIDE);
		/* The child's handlers may have destroyed the rebar; the
		 * band was inserted all the same. */
		if (!comctl_alive(h, s))
			return TRUE;
	}
	layout(h, s);
	return TRUE;
}

static BOOL delete(HWND h, struct rebar *s, int i)
{
	if (i < 0 || i >= s->n)
		return FALSE;
	/* The handlers run synchronously: the band is found again by its
	 * identity after RBN_DELETINGBAND, and nothing more is done if the
	 * rebar went. */
	unsigned uid = s->b[i].uid;
	notify_band(h, s, RBN_DELETINGBAND, i);
	if (!comctl_alive(h, s))
		return FALSE;
	if ((i = find_uid(s, uid)) < 0)
		return TRUE;	/* the handler deleted it */
	UINT id = s->b[i].id;
	free(s->b[i].text);
	memmove(&s->b[i], &s->b[i + 1], (size_t)(s->n - i - 1) * sizeof(*s->b));
	s->n--;
	if (s->drag == uid)
		s->drag = 0;
	/* The new layout is in place before RBN_DELETEDBAND. */
	layout(h, s);
	if (!comctl_alive(h, s))
		return TRUE;
	NMREBAR nm;
	memset(&nm, 0, sizeof(nm));
	nm.dwMask = RBNM_ID;
	nm.uBand = (UINT)i;
	nm.wID = id;
	comctl_notify_to(target(h, s), h, RBN_DELETEDBAND, &nm.hdr);
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
		int width = bar_width(h, s), used = 0;
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
		s->unicode = TRUE;
		s->mouse_x = -1;
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
		{
			UINT old_style = s->b[wp].style;
			if (!set_fields(&s->b[wp], bi, wide))
				return FALSE;
			show_child(&s->b[wp], old_style);
		}
		if (comctl_alive(h, s))
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
		/* The bands are laid out in the rectangle's width (not the
		 * parent's), and the bar takes the rectangle. The result:
		 * whether any band moved or changed size. */
		const RECT *rc = (const RECT *)lp;
		if (!rc)
			return FALSE;
		RECT want = *rc;
		RECT *before = s->n ? malloc((size_t)s->n * sizeof(RECT)) : NULL;
		int n = s->n, old = s->height, old_width = s->width;
		for (int i = 0; before && i < n; i++)
			before[i] = s->b[i].rc;
		s->width = want.right - want.left > 0 ?
			   want.right - want.left : 1;
		comctl_move(h, want.left, want.top, want.right - want.left,
			    want.bottom - want.top);
		if (comctl_alive(h, s))
			layout(h, s);
		if (!comctl_alive(h, s)) {
			free(before);
			return FALSE;
		}
		/* An RB_SIZETORECT nested in this one's handlers restores
		 * the width it found, so the outer one's stands until it
		 * returns. */
		s->width = old_width;
		BOOL changed = old != s->height || n != s->n || (n && !before);
		for (int i = 0; before && !changed && i < n; i++)
			changed = memcmp(&before[i], &s->b[i].rc,
					 sizeof(RECT)) != 0;
		free(before);
		return changed;
	}
	case RB_BEGINDRAG: {
		if ((int)wp < 0 || (int)wp >= s->n)
			return 0;
		unsigned uid = s->b[wp].uid;
		if (notify_band(h, s, RBN_BEGINDRAG, (int)wp) ||
		    !comctl_alive(h, s))
			return 0;
		int i = find_uid(s, uid);
		if (i < 0)
			return 0;
		s->drag = uid;
		/* (DWORD)-1: where the mouse last was, else the band's left */
		s->drag_x = (DWORD)lp == (DWORD)-1 ?
			    (s->mouse_x >= 0 ? s->mouse_x : s->b[i].rc.left) :
			    (short)LOWORD(lp);
		return 0;
	}
	case RB_DRAGMOVE: {
		int d = find_uid(s, s->drag);
		if (d < 0)
			return 0;
		if ((DWORD)lp == (DWORD)-1 && s->mouse_x < 0)
			return 0;
		int x = (DWORD)lp == (DWORD)-1 ? s->mouse_x : (short)LOWORD(lp);
		int dx = x - s->drag_x;
		int p = previous_in_row(s, d);
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
	case RB_ENDDRAG: {
		int d = find_uid(s, s->drag);
		s->drag = 0;
		if (d >= 0)
			notify_band(h, s, RBN_ENDDRAG, d);
		return 0;
	}
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
	case RB_SHOWBAND: {
		if ((int)wp < 0 || (int)wp >= s->n)
			return FALSE;
		UINT old_style = s->b[wp].style;
		if (lp)
			s->b[wp].style &= ~(UINT)RBBS_HIDDEN;
		else
			s->b[wp].style |= RBBS_HIDDEN;
		show_child(&s->b[wp], old_style);
		if (comctl_alive(h, s))
			layout(h, s);
		return TRUE;
	}
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
	case WIN_DEFERRED_LAYOUT:
		s->deferred = FALSE;
		layout(h, s);
		return 0;
	case WM_SIZE:
		if (s->moving)
			break;
		/* fall through */
	case WM_STYLECHANGED:
		layout(h, s);
		if (!comctl_alive(h, s))
			return 0;
		break;
	case WM_COMMAND:
	case WM_NOTIFY: {
		/* The bands' children talk to the rebar's notify window. */
		HWND to = target(h, s);
		return to ? comctl_send(to, msg, wp, lp) : 0;
	}
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
		s->mouse_x = (short)LOWORD(lp);
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
