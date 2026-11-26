/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's tab control (SysTabControl32), headless: its items (text,
 * image, state and the caller's extra bytes, TCM_SETITEMEXTRA long),
 * the selection and focus, button states, sizes and padding, with
 * Windows' messages and the TCN_SELCHANGING/SELCHANGE notifications.
 * Nothing is drawn. Tabs are item-height high and sit side by side from
 * the left (wrapping at the client width with TCS_MULTILINE), each as
 * wide as its text and image plus the padding, or the TCM_SETITEMSIZE
 * width with TCS_FIXEDWIDTH; the display area is the client area less
 * the tab rows and a border (TCM_ADJUSTRECT).
 */
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define ITEM_HEIGHT  20
#define FIXED_WIDTH  96
#define BORDER       2
#define ICON_CX      16

struct tc_item {
	WCHAR *text;
	int image;
	DWORD state;
	BYTE *extra;
	RECT rc;
};

struct tab {
	struct tc_item *it;
	int n, cap;
	int sel, focus;
	int extra;		/* bytes of caller data per item */
	int cx, cy, pad_x, pad_y, min_width;
	HIMAGELIST himl;
	HWND tooltips;
	DWORD ex_style;
	BOOL unicode;
	HFONT font;
};

static int item_width(HWND h, const struct tab *s, const struct tc_item *t)
{
	DWORD st = comctl_style(h);
	int w;

	if (st & TCS_FIXEDWIDTH)
		w = s->cx;
	else
		w = comctl_text_width(t->text) + 2 * s->pad_x +
		    (t->image >= 0 && s->himl ? ICON_CX + s->pad_x : 0);
	if (s->min_width > 0 && w < s->min_width)
		w = s->min_width;
	return w;
}

static void layout(HWND h, struct tab *s)
{
	RECT client;
	DWORD st = comctl_style(h);
	int x = 0, y = 0, width;

	comctl_client_rect(h, &client);
	width = client.right - client.left;
	for (int i = 0; i < s->n; i++) {
		int w = item_width(h, s, &s->it[i]);
		if ((st & TCS_MULTILINE) && x > 0 && width > 0 &&
		    x + w > width) {
			x = 0;
			y += s->cy;
		}
		s->it[i].rc.left = x;
		s->it[i].rc.top = y;
		s->it[i].rc.right = x + w;
		s->it[i].rc.bottom = y + s->cy;
		x += w;
	}
	if (!(st & TCS_BOTTOM))
		return;
	/* Along the bottom, the first row lowest. */
	int bottom = client.bottom - s->cy;
	for (int i = 0; i < s->n; i++) {
		int top = bottom - s->it[i].rc.top;
		s->it[i].rc.top = top;
		s->it[i].rc.bottom = top + s->cy;
	}
}

static int rows(const struct tab *s)
{
	int r = 0, last = -0x7fffffff;

	for (int i = 0; i < s->n; i++)
		if (s->it[i].rc.top != last) {
			last = s->it[i].rc.top;
			r++;
		}
	return r;
}

/* SELCHANGING (which can refuse), the change, SELCHANGE. */
static BOOL select_by_user(HWND h, struct tab *s, int i)
{
	NMHDR hdr;

	if (i == s->sel)
		return TRUE;
	if (comctl_notify(h, TCN_SELCHANGING, &hdr))
		return FALSE;
	s->sel = s->focus = i;
	comctl_notify(h, TCN_SELCHANGE, &hdr);
	return TRUE;
}

static int hit(const struct tab *s, POINT pt, UINT *flags)
{
	for (int i = 0; i < s->n; i++) {
		const RECT *r = &s->it[i].rc;
		if (pt.x < r->left || pt.x >= r->right || pt.y < r->top ||
		    pt.y >= r->bottom)
			continue;
		if (flags) {
			BOOL icon = s->it[i].image >= 0 && s->himl &&
				    pt.x < r->left + s->pad_x + ICON_CX;
			*flags = icon ? TCHT_ONITEMICON : TCHT_ONITEMLABEL;
		}
		return i;
	}
	if (flags)
		*flags = TCHT_NOWHERE;
	return -1;
}

/* TCITEM's A and W forms differ only in the text pointer's type. */
static void set_fields(struct tab *s, struct tc_item *t, const TCITEMW *in,
		       int wide)
{
	if (in->mask & TCIF_TEXT) {
		free(t->text);
		t->text = !in->pszText ? NULL :
			  wide ? comctl_w_dup(in->pszText) :
				 comctl_a_to_w((const char *)in->pszText);
	}
	if (in->mask & TCIF_IMAGE)
		t->image = in->iImage;
	if (in->mask & TCIF_STATE)
		t->state = (t->state & ~in->dwStateMask) |
			   (in->dwState & in->dwStateMask);
	if ((in->mask & TCIF_PARAM) && s->extra)
		memcpy(t->extra, &in->lParam, (size_t)s->extra);
}

static BOOL get_fields(const struct tab *s, int i, TCITEMW *out, int wide)
{
	if (i < 0 || i >= s->n || !out)
		return FALSE;
	const struct tc_item *t = &s->it[i];
	if (out->mask & TCIF_TEXT)
		comctl_copy_text_out(t->text ? t->text : u"", out->pszText,
				     out->cchTextMax, wide);
	if (out->mask & TCIF_IMAGE)
		out->iImage = t->image;
	if (out->mask & TCIF_STATE)
		out->dwState = t->state & out->dwStateMask;
	if ((out->mask & TCIF_PARAM) && s->extra)
		memcpy(&out->lParam, t->extra, (size_t)s->extra);
	return TRUE;
}

static int insert(HWND h, struct tab *s, int at, const TCITEMW *in, int wide)
{
	if (!in)
		return -1;
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 8;
		void *p = realloc(s->it, (size_t)cap * sizeof(*s->it));
		if (!p)
			return -1;
		s->it = p;
		s->cap = cap;
	}
	BYTE *extra = calloc(1, s->extra ? (size_t)s->extra : 1);
	if (!extra)
		return -1;
	if (at < 0)
		return free(extra), -1;
	if (at > s->n)
		at = s->n;
	memmove(&s->it[at + 1], &s->it[at], (size_t)(s->n - at) * sizeof(*s->it));
	struct tc_item *t = &s->it[at];
	memset(t, 0, sizeof(*t));
	t->image = -1;
	t->extra = extra;
	set_fields(s, t, in, wide);
	s->n++;
	if (s->n == 1)
		s->sel = s->focus = 0;
	else {
		if (s->sel >= at)
			s->sel++;
		if (s->focus >= at)
			s->focus++;
	}
	layout(h, s);
	return at;
}

static void clear(struct tc_item *t)
{
	free(t->text);
	free(t->extra);
}

static BOOL delete(HWND h, struct tab *s, int i)
{
	if (i < 0 || i >= s->n)
		return FALSE;
	clear(&s->it[i]);
	memmove(&s->it[i], &s->it[i + 1], (size_t)(s->n - i - 1) * sizeof(*s->it));
	s->n--;
	if (s->sel == i)
		s->sel = -1;
	else if (s->sel > i)
		s->sel--;
	if (s->focus == i)
		s->focus = -1;
	else if (s->focus > i)
		s->focus--;
	layout(h, s);
	return TRUE;
}

static void adjust(HWND h, const struct tab *s, BOOL larger, RECT *rc)
{
	DWORD st = comctl_style(h);
	int tabs = s->n ? rows(s) * s->cy : 0;
	int border = (st & TCS_BUTTONS) ? 0 : BORDER;
	int sign = larger ? -1 : 1;

	rc->left += sign * border;
	rc->right -= sign * border;
	if (st & TCS_BOTTOM) {
		rc->top += sign * border;
		rc->bottom -= sign * (border + tabs);
	} else {
		rc->top += sign * (border + tabs);
		rc->bottom -= sign * border;
	}
}

LRESULT STUB_WINAPI comctl_tab_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct tab *s = comctl_state(h);
	int wide = 1;

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->sel = s->focus = -1;
		s->extra = sizeof(LPARAM);
		s->cx = FIXED_WIDTH;
		s->cy = ITEM_HEIGHT;
		s->pad_x = 6;
		s->pad_y = 3;
		s->min_width = -1;
		s->unicode = TRUE;
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case TCM_GETITEMCOUNT:
		return s->n;
	case TCM_INSERTITEMA:
		wide = 0;
		/* fall through */
	case TCM_INSERTITEMW:
		return insert(h, s, (int)wp, (const TCITEMW *)lp, wide);
	case TCM_GETITEMA:
		wide = 0;
		/* fall through */
	case TCM_GETITEMW:
		return get_fields(s, (int)wp, (TCITEMW *)lp, wide);
	case TCM_SETITEMA:
		wide = 0;
		/* fall through */
	case TCM_SETITEMW:
		if ((int)wp < 0 || (int)wp >= s->n || !lp)
			return FALSE;
		set_fields(s, &s->it[wp], (const TCITEMW *)lp, wide);
		layout(h, s);
		return TRUE;
	case TCM_DELETEITEM:
		return delete(h, s, (int)wp);
	case TCM_DELETEALLITEMS:
		while (s->n)
			delete(h, s, s->n - 1);
		return TRUE;
	case TCM_SETITEMEXTRA:
		if (s->n || (int)wp < 0)
			return FALSE;
		s->extra = (int)wp;
		return TRUE;
	case TCM_GETITEMRECT:
		if ((int)wp < 0 || (int)wp >= s->n || !lp)
			return FALSE;
		*(RECT *)lp = s->it[wp].rc;
		return TRUE;
	case TCM_GETCURSEL:
		return s->sel;
	case TCM_SETCURSEL: {
		int old = s->sel;
		if ((int)wp < -1 || (int)wp >= s->n)
			return -1;
		s->sel = (int)wp;
		if (wp != (WPARAM)-1)
			s->focus = (int)wp;
		return old;
	}
	case TCM_GETCURFOCUS:
		return s->focus;
	case TCM_SETCURFOCUS:
		if ((int)wp < -1 || (int)wp >= s->n)
			return 0;
		if ((comctl_style(h) & TCS_BUTTONS) || (int)wp < 0)
			s->focus = (int)wp;
		else
			select_by_user(h, s, (int)wp);
		return 0;
	case TCM_HITTEST: {
		TCHITTESTINFO *hi = (TCHITTESTINFO *)lp;
		if (!hi)
			return -1;
		return hit(s, hi->pt, &hi->flags);
	}
	case TCM_ADJUSTRECT:
		if (lp)
			adjust(h, s, (BOOL)wp, (RECT *)lp);
		return 0;
	case TCM_SETITEMSIZE: {
		LRESULT old = MAKELONG(s->cx, s->cy);
		if ((short)LOWORD(lp) > 0)
			s->cx = (short)LOWORD(lp);
		if ((short)HIWORD(lp) > 0)
			s->cy = (short)HIWORD(lp);
		layout(h, s);
		return old;
	}
	case TCM_SETPADDING:
		s->pad_x = LOWORD(lp);
		s->pad_y = HIWORD(lp);
		layout(h, s);
		return 0;
	case TCM_SETMINTABWIDTH: {
		int old = s->min_width;
		s->min_width = (int)lp;
		layout(h, s);
		return old;
	}
	case TCM_GETROWCOUNT:
		return rows(s);
	case TCM_GETIMAGELIST:
		return (LRESULT)s->himl;
	case TCM_SETIMAGELIST: {
		HIMAGELIST old = s->himl;
		s->himl = (HIMAGELIST)lp;
		layout(h, s);
		return (LRESULT)old;
	}
	case TCM_REMOVEIMAGE:
		for (int i = 0; i < s->n; i++) {
			if (s->it[i].image == (int)wp)
				s->it[i].image = -1;
			else if (s->it[i].image > (int)wp)
				s->it[i].image--;
		}
		layout(h, s);
		return 0;
	case TCM_GETTOOLTIPS:
		return (LRESULT)s->tooltips;
	case TCM_SETTOOLTIPS:
		s->tooltips = (HWND)wp;
		return 0;
	case TCM_DESELECTALL:
		for (int i = 0; i < s->n; i++)
			if (!wp || i != s->focus)
				s->it[i].state &= ~(DWORD)TCIS_BUTTONPRESSED;
		return 0;
	case TCM_HIGHLIGHTITEM:
		if ((int)wp < 0 || (int)wp >= s->n)
			return FALSE;
		if (LOWORD(lp))
			s->it[wp].state |= TCIS_HIGHLIGHTED;
		else
			s->it[wp].state &= ~(DWORD)TCIS_HIGHLIGHTED;
		return TRUE;
	case TCM_SETEXTENDEDSTYLE: {
		DWORD old = s->ex_style;
		s->ex_style = wp ? (old & ~(DWORD)wp) | ((DWORD)lp & (DWORD)wp)
				 : (DWORD)lp;
		return old;
	}
	case TCM_GETEXTENDEDSTYLE:
		return s->ex_style;
	case TCM_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case TCM_GETUNICODEFORMAT:
		return s->unicode;
	case WM_LBUTTONDOWN: {
		POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
		int i = hit(s, pt, NULL);
		DWORD st = comctl_style(h);
		if (i < 0)
			return 0;
		/* Ctrl+click on multiselect buttons toggles one. */
		if ((st & (TCS_BUTTONS | TCS_MULTISELECT)) ==
		    (TCS_BUTTONS | TCS_MULTISELECT) && (wp & MK_CONTROL)) {
			s->it[i].state ^= TCIS_BUTTONPRESSED;
			s->focus = i;
			return 0;
		}
		select_by_user(h, s, i);
		return 0;
	}
	case WM_SIZE:
		layout(h, s);
		break;
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
			clear(&s->it[i]);
		free(s->it);
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}
