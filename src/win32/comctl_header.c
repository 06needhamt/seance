/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's header control (SysHeader32), headless: its items' widths,
 * text, formats, images, order and the rest, with Windows' messages and
 * the HDN_ITEMCHANGING/ITEMCHANGED notifications. Nothing is drawn; the
 * control is a fixed HEADER_HEIGHT pixels high and each item as wide as
 * its cxy, side by side in display order.
 */
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define HEADER_HEIGHT 20
#define DIVIDER_SLOP  4		/* how near a divider counts as on it */

struct hd_item {
	int cxy;
	WCHAR *text;
	BOOL callback;		/* LPSTR_TEXTCALLBACK */
	HBITMAP hbm;
	int fmt;
	LPARAM lParam;
	int iImage;
	UINT type;
	void *filter;
	UINT state;
};

struct header {
	struct hd_item *item;
	int *order;		/* order[position] = index */
	int n, cap;
	HIMAGELIST himl;
	HFONT font;
	int margin, hot, focus;
	UINT filter_timeout;
	BOOL unicode;
};

static int position_of(const struct header *s, int i)
{
	for (int p = 0; p < s->n; p++)
		if (s->order[p] == i)
			return p;
	return -1;
}

/* The item's left and right edges. */
static void edges(const struct header *s, int i, int *l, int *r)
{
	int x = 0, p = position_of(s, i);

	for (int q = 0; q < p; q++)
		x += s->item[s->order[q]].cxy;
	*l = x;
	*r = x + s->item[i].cxy;
}

static WCHAR *text_in(LPARAM p, int wide)
{
	return wide ? comctl_w_dup((const WCHAR *)p) :
		      comctl_a_to_w((const char *)p);
}

/* What one HDITEM says, into it. The A and W forms differ only in the
 * text pointer's type. */
static void set_fields(struct header *s, struct hd_item *it,
		       const HDITEMW *in, int wide)
{
	UINT m = in->mask;

	if (m & HDI_WIDTH)
		it->cxy = in->cxy;
	if (m & HDI_TEXT) {
		free(it->text);
		it->text = NULL;
		it->callback = in->pszText == LPSTR_TEXTCALLBACKW;
		if (in->pszText && !it->callback)
			it->text = text_in((LPARAM)in->pszText, wide);
	}
	if (m & HDI_FORMAT)
		it->fmt = in->fmt;
	if (m & HDI_LPARAM)
		it->lParam = in->lParam;
	if (m & HDI_BITMAP)
		it->hbm = in->hbm;
	if (m & HDI_IMAGE)
		it->iImage = in->iImage;
	if (m & HDI_FILTER) {
		it->type = in->type;
		it->filter = in->pvFilter;
	}
	if (m & HDI_STATE)
		it->state = in->state;
}

static void get_fields(const struct header *s, int i, HDITEMW *out, int wide)
{
	const struct hd_item *it = &s->item[i];
	UINT m = out->mask;

	if (m & HDI_WIDTH)
		out->cxy = it->cxy;
	if (m & HDI_TEXT) {
		if (it->callback)
			out->pszText = LPSTR_TEXTCALLBACKW;
		else
			comctl_copy_text_out(it->text, out->pszText,
					     out->cchTextMax, wide);
	}
	if (m & HDI_FORMAT)
		out->fmt = it->fmt;
	if (m & HDI_LPARAM)
		out->lParam = it->lParam;
	if (m & HDI_BITMAP)
		out->hbm = it->hbm;
	if (m & HDI_IMAGE)
		out->iImage = it->iImage;
	if (m & HDI_ORDER)
		out->iOrder = position_of(s, i);
	if (m & HDI_FILTER) {
		out->type = it->type;
		out->pvFilter = it->filter;
	}
	if (m & HDI_STATE)
		out->state = it->state;
}

/* Moves item i to display position p. */
static void reorder(struct header *s, int i, int p)
{
	int from = position_of(s, i);

	if (p < 0 || p >= s->n || from < 0 || from == p)
		return;
	if (from < p)
		memmove(&s->order[from], &s->order[from + 1],
			(size_t)(p - from) * sizeof(int));
	else
		memmove(&s->order[p + 1], &s->order[p],
			(size_t)(from - p) * sizeof(int));
	s->order[p] = i;
}

static LRESULT insert(struct header *s, int i, const HDITEMW *in, int wide)
{
	if (!in)
		return -1;
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 8;
		struct hd_item *it = realloc(s->item, cap * sizeof(*it));
		if (!it)
			return -1;
		s->item = it;
		int *o = realloc(s->order, cap * sizeof(int));
		if (!o)
			return -1;
		s->order = o;
		s->cap = cap;
	}
	if (i < 0)
		return -1;
	if (i > s->n)
		i = s->n;
	memmove(&s->item[i + 1], &s->item[i],
		(size_t)(s->n - i) * sizeof(*s->item));
	memset(&s->item[i], 0, sizeof(*s->item));
	set_fields(s, &s->item[i], in, wide);
	int p = (in->mask & HDI_ORDER) && in->iOrder >= 0 &&
		in->iOrder <= s->n ? in->iOrder : i;
	for (int q = 0; q < s->n; q++)
		if (s->order[q] >= i)
			s->order[q]++;
	memmove(&s->order[p + 1], &s->order[p],
		(size_t)(s->n - p) * sizeof(int));
	s->order[p] = i;
	s->n++;
	return i;
}

static BOOL delete(struct header *s, int i)
{
	if (i < 0 || i >= s->n)
		return FALSE;
	free(s->item[i].text);
	memmove(&s->item[i], &s->item[i + 1],
		(size_t)(s->n - i - 1) * sizeof(*s->item));
	int p = position_of(s, i);
	memmove(&s->order[p], &s->order[p + 1],
		(size_t)(s->n - p - 1) * sizeof(int));
	s->n--;
	for (int q = 0; q < s->n; q++)
		if (s->order[q] > i)
			s->order[q]--;
	return TRUE;
}

/* HDN_ITEMCHANGING (which can refuse) or HDN_ITEMCHANGED about item i,
 * carrying what the caller passed, in the parent's charset. */
static LRESULT notify_change(HWND h, int i, const HDITEMW *in, int wide,
			     BOOL changed)
{
	BOOL uni = comctl_notify_unicode(h);
	HDITEMW item = *in;
	void *text = NULL;
	NMHEADERW nm;

	if ((in->mask & HDI_TEXT) && in->pszText &&
	    in->pszText != LPSTR_TEXTCALLBACKW && uni != wide) {
		text = uni ? (void *)comctl_a_to_w((const char *)in->pszText) :
			     (void *)comctl_w_to_a(in->pszText);
		item.pszText = text;
	}
	memset(&nm, 0, sizeof(nm));
	nm.iItem = i;
	nm.pitem = &item;
	UINT code = changed ? (uni ? HDN_ITEMCHANGEDW : HDN_ITEMCHANGEDA) :
			      (uni ? HDN_ITEMCHANGINGW : HDN_ITEMCHANGINGA);
	LRESULT r = comctl_notify(h, code, &nm.hdr);
	free(text);
	return r;
}

static int hit(HWND h, const struct header *s, HDHITTESTINFO *ht)
{
	RECT c;
	int x = ht->pt.x, y = ht->pt.y, right = 0;

	comctl_client_rect(h, &c);
	ht->flags = 0;
	ht->iItem = -1;
	if (y < 0)
		ht->flags |= HHT_ABOVE;
	else if (y >= c.bottom)
		ht->flags |= HHT_BELOW;
	if (x < 0)
		ht->flags |= HHT_TOLEFT;
	else if (x >= c.right)
		ht->flags |= HHT_TORIGHT;
	if (ht->flags)
		return -1;
	for (int p = 0; p < s->n; p++) {
		int i = s->order[p], l = right;
		right += s->item[i].cxy;
		if (x >= right - DIVIDER_SLOP && x < right + DIVIDER_SLOP) {
			ht->flags = HHT_ONDIVIDER;
			ht->iItem = i;
			return i;
		}
		if (x >= l && x < right) {
			ht->flags = HHT_ONHEADER;
			ht->iItem = i;
			return i;
		}
	}
	ht->flags = HHT_NOWHERE;
	return -1;
}

LRESULT STUB_WINAPI comctl_header_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct header *s = comctl_state(h);
	int wide = 1;

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->margin = 6;
		s->hot = s->focus = -1;
		s->filter_timeout = 1000;
		s->unicode = TRUE;
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case HDM_GETITEMCOUNT:
		return s->n;
	case HDM_INSERTITEMA:
		wide = 0;
		/* fall through */
	case HDM_INSERTITEMW:
		return insert(s, (int)wp, (const HDITEMW *)lp, wide);
	case HDM_DELETEITEM:
		return delete(s, (int)wp);
	case HDM_GETITEMA:
		wide = 0;
		/* fall through */
	case HDM_GETITEMW:
		if (!lp || (int)wp < 0 || (int)wp >= s->n)
			return FALSE;
		get_fields(s, (int)wp, (HDITEMW *)lp, wide);
		return TRUE;
	case HDM_SETITEMA:
		wide = 0;
		/* fall through */
	case HDM_SETITEMW: {
		const HDITEMW *in = (const HDITEMW *)lp;
		int i = (int)wp;
		if (!in || i < 0 || i >= s->n)
			return FALSE;
		if (notify_change(h, i, in, wide, FALSE))
			return FALSE;
		set_fields(s, &s->item[i], in, wide);
		if (in->mask & HDI_ORDER)
			reorder(s, i, in->iOrder);
		notify_change(h, i, in, wide, TRUE);
		return TRUE;
	}
	case HDM_LAYOUT: {
		HDLAYOUT *l = (HDLAYOUT *)lp;
		if (!l || !l->prc || !l->pwpos)
			return FALSE;
		int height = comctl_style(h) & HDS_HIDDEN ? 0 : HEADER_HEIGHT;
		l->pwpos->hwnd = h;
		l->pwpos->hwndInsertAfter = NULL;
		l->pwpos->x = l->prc->left;
		l->pwpos->y = l->prc->top;
		l->pwpos->cx = l->prc->right - l->prc->left;
		l->pwpos->cy = height;
		l->pwpos->flags = SWP_NOZORDER;
		l->prc->top += height;
		return TRUE;
	}
	case HDM_HITTEST:
		return lp ? hit(h, s, (HDHITTESTINFO *)lp) : -1;
	case HDM_GETITEMRECT: {
		RECT *rc = (RECT *)lp, c;
		if (!rc || (int)wp < 0 || (int)wp >= s->n)
			return FALSE;
		comctl_client_rect(h, &c);
		edges(s, (int)wp, (int *)&rc->left, (int *)&rc->right);
		rc->top = 0;
		rc->bottom = c.bottom;
		return TRUE;
	}
	case HDM_SETIMAGELIST: {
		HIMAGELIST old = s->himl;
		s->himl = (HIMAGELIST)lp;
		return (LRESULT)old;
	}
	case HDM_GETIMAGELIST:
		return (LRESULT)s->himl;
	case HDM_ORDERTOINDEX:
		return (int)wp >= 0 && (int)wp < s->n ? s->order[wp] : (int)wp;
	case HDM_GETORDERARRAY:
		if (!lp || (int)wp != s->n)
			return FALSE;
		memcpy((int *)lp, s->order, (size_t)s->n * sizeof(int));
		return TRUE;
	case HDM_SETORDERARRAY: {
		const int *o = (const int *)lp;
		if (!o || (int)wp != s->n)
			return FALSE;
		/* A permutation of the items, or nothing changes. */
		for (int p = 0; p < s->n; p++) {
			if (o[p] < 0 || o[p] >= s->n)
				return FALSE;
			for (int q = 0; q < p; q++)
				if (o[q] == o[p])
					return FALSE;
		}
		memcpy(s->order, o, (size_t)s->n * sizeof(int));
		return TRUE;
	}
	case HDM_SETHOTDIVIDER:
		if (wp) {
			HDHITTESTINFO ht;
			ht.pt.x = (short)LOWORD(lp);
			ht.pt.y = 0;
			int i = hit(h, s, &ht);
			if (i < 0)
				s->hot = ht.flags & HHT_TOLEFT ? 0 : s->n;
			else {
				int l, r;
				edges(s, i, &l, &r);
				int p = position_of(s, i);
				s->hot = ht.pt.x < (l + r) / 2 ? p : p + 1;
			}
		} else {
			s->hot = (int)lp;
		}
		return s->hot;
	case HDM_SETBITMAPMARGIN: {
		int old = s->margin;
		s->margin = (int)wp;
		return old;
	}
	case HDM_GETBITMAPMARGIN:
		return s->margin;
	case HDM_SETFILTERCHANGETIMEOUT: {
		UINT old = s->filter_timeout;
		s->filter_timeout = (UINT)lp;
		return old;
	}
	case HDM_EDITFILTER:
		return (int)wp >= 0 && (int)wp < s->n;
	case HDM_CLEARFILTER:
		for (int i = 0; i < s->n; i++)
			if ((int)wp == -1 || (int)wp == i)
				s->item[i].filter = NULL;
		return TRUE;
	case HDM_CREATEDRAGIMAGE:
		return 0;
	case HDM_GETFOCUSEDITEM:
		return s->focus;
	case HDM_SETFOCUSEDITEM:
		if ((int)lp < -1 || (int)lp >= s->n)
			return FALSE;
		s->focus = (int)lp;
		return TRUE;
	case HDM_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case HDM_GETUNICODEFORMAT:
		return s->unicode;
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		for (int i = 0; i < s->n; i++)
			free(s->item[i].text);
		free(s->item);
		free(s->order);
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}
