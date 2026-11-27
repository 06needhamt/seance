/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's tooltip control (tooltips_class32), headless: its tools
 * (each a window and an id, a rectangle or the whole window with
 * TTF_IDISHWND, and a text that is a string, a string resource or
 * LPSTR_TEXTCALLBACK answered by TTN_GETDISPINFO), the delay times,
 * colours, margins, width and title, tracking, and the hit test, with
 * Windows' messages. Nothing is drawn and no timer runs: a mouse move
 * relayed over a tool (TTM_RELAYEVENT, or the tool window's own messages
 * with TTF_SUBCLASS) makes it the current tool and shows the tip at once
 * (TTN_SHOW); leaving it, a button press or TTM_POP hides it (TTN_POP).
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define IS_INTRES(p)  (((UINT_PTR)(p) >> 16) == 0)
#define TEXT_MAX      80	/* what TTM_GETTOOLINFO copies, as Windows */
#define BORDER        2
#define TEXT_PAD      4
#define LINE_HEIGHT   16

struct tool {
	UINT flags;
	HWND hwnd;
	UINT_PTR id;
	RECT rect;
	HINSTANCE hinst;
	WCHAR *text;
	UINT_PTR res;		/* a string resource id, with hinst */
	BOOL callback;
	BOOL ansi_owner;	/* the owner answers WM_NOTIFYFORMAT with ANSI */
	LPARAM lParam;
	unsigned uid;		/* stable identity across notifications */
};

struct tooltip {
	struct tool *t;
	int n, cap;
	int current;		/* -1: none */
	unsigned next_uid;
	BOOL active, visible, tracking, unicode;
	int delay[4];		/* by TTDT_* */
	BOOL delay_set[4];
	COLORREF bk, fg;
	int max_width;
	RECT margin;
	WCHAR *title;
	UINT_PTR icon;
	POINT track;
	HFONT font;
};

static int default_delay(const struct tooltip *s, int which)
{
	int initial = s->delay_set[TTDT_INITIAL] ? s->delay[TTDT_INITIAL] :
		      (int)((UINT (WINAPI *)(void))
			    USER32(GetDoubleClickTime))();
	switch (which) {
	case TTDT_AUTOPOP:
		return initial * 10;
	case TTDT_RESHOW:
		return initial / 5;
	}
	return initial;
}

static int delay(const struct tooltip *s, int which)
{
	return s->delay_set[which] ? s->delay[which] : default_delay(s, which);
}

static COLORREF sys_colour(int i)
{
	return ((DWORD (WINAPI *)(int))USER32(GetSysColor))(i);
}

/* TOOLINFO A and W differ only in the text pointer's type; how much of
 * one the caller passed is in its cbSize. */
static BOOL valid_size(const TTTOOLINFOW *ti)
{
	return ti && ti->cbSize >= offsetof(TTTOOLINFOW, lParam);
}

static BOOL has_lparam(const TTTOOLINFOW *ti)
{
	return ti->cbSize >= offsetof(TTTOOLINFOW, lpReserved);
}

static int find(const struct tooltip *s, HWND hwnd, UINT_PTR id)
{
	for (int i = 0; i < s->n; i++)
		if (s->t[i].hwnd == hwnd && s->t[i].id == id)
			return i;
	return -1;
}

static int find_uid(const struct tooltip *s, unsigned uid)
{
	for (int i = 0; i < s->n; i++)
		if (s->t[i].uid == uid)
			return i;
	return -1;
}

static void set_text(struct tool *t, const TTTOOLINFOW *ti, int wide)
{
	free(t->text);
	t->text = NULL;
	t->res = 0;
	t->callback = FALSE;
	t->hinst = ti->hinst;
	if (ti->lpszText == LPSTR_TEXTCALLBACKW)
		t->callback = TRUE;
	else if (ti->lpszText && IS_INTRES(ti->lpszText))
		t->res = (UINT_PTR)ti->lpszText;
	else if (ti->lpszText)
		t->text = wide ? comctl_w_dup(ti->lpszText) :
				 comctl_a_to_w((const char *)ti->lpszText);
}

static void set_fields(struct tool *t, const TTTOOLINFOW *ti, int wide)
{
	t->flags = ti->uFlags;
	t->hwnd = ti->hwnd;
	t->id = ti->uId;
	t->rect = ti->rect;
	set_text(t, ti, wide);
	if (has_lparam(ti))
		t->lParam = ti->lParam;
}

static WCHAR *load_res(HINSTANCE inst, UINT_PTR id)
{
	WCHAR buf[512];
	int n = ((int (WINAPI *)(HINSTANCE, UINT, LPWSTR, int))
		 USER32(LoadStringW))(inst, (UINT)id, buf, 512);
	return n > 0 ? comctl_w_dup(buf) : NULL;
}

/* After the owner answers TTN_GETDISPINFO with TTF_DI_SETITEM, the tool
 * keeps the text, if the tooltip and the tool both survived the call. */
static void cache_text(HWND h, struct tooltip *s, unsigned uid,
		       const WCHAR *out)
{
	if (!comctl_alive(h, s))
		return;
	int i = find_uid(s, uid);
	if (i < 0)
		return;
	free(s->t[i].text);
	s->t[i].text = out ? comctl_w_dup(out) : NULL;
	s->t[i].callback = FALSE;
}

/* Tool i's text, asking its owner when it is a callback. The result is
 * the caller's to free. The owner's handler runs synchronously and may
 * change the tools or destroy the tooltip: callers check comctl_alive and
 * find the tool again before using s afterwards. */
static WCHAR *text_of(HWND h, struct tooltip *s, int i)
{
	const struct tool *t = &s->t[i];
	if (t->text)
		return comctl_w_dup(t->text);
	if (t->res)
		return load_res(t->hinst, t->res);
	if (!t->callback)
		return NULL;

	HWND owner = t->hwnd;
	UINT_PTR id = t->id;
	unsigned uid = t->uid;
	WCHAR *out = NULL;
	if (t->ansi_owner) {
		NMTTDISPINFOA d;
		memset(&d, 0, sizeof(d));
		d.hdr.hwndFrom = h;
		d.hdr.idFrom = id;
		d.hdr.code = TTN_GETDISPINFOA;
		d.hinst = t->hinst;
		d.uFlags = t->flags;
		d.lParam = t->lParam;
		comctl_send(owner, WM_NOTIFY, id, (LPARAM)&d);
		if (d.lpszText && IS_INTRES(d.lpszText) && d.hinst)
			out = load_res(d.hinst, (UINT_PTR)d.lpszText);
		else if (d.lpszText && !IS_INTRES(d.lpszText))
			out = comctl_a_to_w(d.lpszText);
		else
			out = (d.szText[sizeof(d.szText) - 1] = 0,
			       comctl_a_to_w(d.szText));
		if (d.uFlags & TTF_DI_SETITEM)
			cache_text(h, s, uid, out);
		return out;
	}
	NMTTDISPINFOW d;
	memset(&d, 0, sizeof(d));
	d.hdr.hwndFrom = h;
	d.hdr.idFrom = id;
	d.hdr.code = TTN_GETDISPINFOW;
	d.hinst = t->hinst;
	d.uFlags = t->flags;
	d.lParam = t->lParam;
	comctl_send(owner, WM_NOTIFY, id, (LPARAM)&d);
	if (d.lpszText && IS_INTRES(d.lpszText) && d.hinst)
		out = load_res(d.hinst, (UINT_PTR)d.lpszText);
	else if (d.lpszText && !IS_INTRES(d.lpszText))
		out = comctl_w_dup(d.lpszText);
	else
		out = (d.szText[TEXT_MAX - 1] = 0, comctl_w_dup(d.szText));
	if (d.uFlags & TTF_DI_SETITEM)
		cache_text(h, s, uid, out);
	return out;
}

/* TTM_GETTOOLINFO and TTM_ENUMTOOLS: the fields, with the text copied
 * into the caller's buffer (up to TEXT_MAX characters). */
static void get_fields(const struct tool *t, TTTOOLINFOW *ti, int wide)
{
	ti->uFlags = t->flags;
	ti->hwnd = t->hwnd;
	ti->uId = t->id;
	ti->rect = t->rect;
	ti->hinst = t->hinst;
	if (has_lparam(ti))
		ti->lParam = t->lParam;
	if (t->callback)
		ti->lpszText = LPSTR_TEXTCALLBACKW;
	else if (t->res)
		ti->lpszText = (LPWSTR)t->res;
	else if (ti->lpszText && !IS_INTRES(ti->lpszText))
		comctl_copy_text_out(t->text ? t->text : u"", ti->lpszText,
				     TEXT_MAX, wide);
}

/* The point, in hwnd's client coordinates, is over which tool? A mouse
 * move never activates a TTF_TRACK tool (mouse is FALSE for TTM_HITTEST). */
static int hit(const struct tooltip *s, HWND hwnd, POINT pt, BOOL mouse)
{
	for (int i = 0; i < s->n; i++) {
		const struct tool *t = &s->t[i];
		if (mouse && (t->flags & TTF_TRACK))
			continue;
		if (t->flags & TTF_IDISHWND) {
			if ((HWND)t->id != hwnd)
				continue;
			RECT rc;
			comctl_client_rect(hwnd, &rc);
			if (pt.x >= rc.left && pt.x < rc.right &&
			    pt.y >= rc.top && pt.y < rc.bottom)
				return i;
			continue;
		}
		if (t->hwnd == hwnd && pt.x >= t->rect.left &&
		    pt.x < t->rect.right && pt.y >= t->rect.top &&
		    pt.y < t->rect.bottom)
			return i;
	}
	return -1;
}

static void notify_owner(HWND h, HWND owner, UINT_PTR id, UINT code)
{
	NMHDR hdr;
	hdr.hwndFrom = h;
	hdr.idFrom = id;
	hdr.code = code;
	comctl_send(owner, WM_NOTIFY, id, (LPARAM)&hdr);
}

/* Hide the tip. It is marked hidden before TTN_POP goes out, so a
 * TTM_POP from the handler sends no second one. Afterwards s may be gone
 * (check comctl_alive). */
static void pop(HWND h, struct tooltip *s)
{
	BOOL was = s->visible;
	s->visible = FALSE;
	if (was && s->current >= 0 && s->current < s->n)
		notify_owner(h, s->t[s->current].hwnd, s->t[s->current].id,
			     TTN_POP);
}

/* Show tool i's tip: the old one goes (TTN_POP), the owner is asked for
 * a callback text (TTN_GETDISPINFO) and told (TTN_SHOW). Each handler may
 * change the tools or destroy the tooltip, so the tool is found again by
 * its uid after each; if it went, nothing is shown. */
static void show(HWND h, struct tooltip *s, int i)
{
	if (i == s->current && s->visible)
		return;
	unsigned uid = i >= 0 ? s->t[i].uid : 0;
	pop(h, s);
	if (!comctl_alive(h, s))
		return;
	i = i >= 0 ? find_uid(s, uid) : -1;
	s->current = i;
	if (i < 0)
		return;
	free(text_of(h, s, i));
	if (!comctl_alive(h, s))
		return;
	if ((i = find_uid(s, uid)) < 0) {
		s->current = -1;
		return;
	}
	s->current = i;
	s->visible = TRUE;
	notify_owner(h, s->t[i].hwnd, s->t[i].id, TTN_SHOW);
}

static LRESULT STUB_WINAPI relay_subclass(HWND hwnd, UINT msg, WPARAM wp,
					  LPARAM lp, UINT_PTR id,
					  DWORD_PTR ref);

/* Returns FALSE when the TTN_POP handler destroyed the tooltip. */
static BOOL delete_at(HWND h, struct tooltip *s, int i)
{
	if (s->current == i) {
		unsigned uid = s->t[i].uid;
		pop(h, s);
		if (!comctl_alive(h, s))
			return FALSE;
		if ((i = find_uid(s, uid)) < 0)
			return TRUE;	/* the handler deleted it */
	}
	/* The TTN_POP handler may have shown another tool: the tip state
	 * is cleared only when it is still this one's. */
	if (s->current == i) {
		s->current = -1;
		s->tracking = FALSE;
	} else if (s->current > i) {
		s->current--;
	}
	struct tool *t = &s->t[i];
	HWND win = (t->flags & TTF_IDISHWND) ? (HWND)t->id : t->hwnd;
	BOOL sub = (t->flags & TTF_SUBCLASS) != 0;
	free(t->text);
	memmove(t, t + 1, (size_t)(s->n - i - 1) * sizeof(*t));
	s->n--;
	if (!sub)
		return TRUE;
	for (int j = 0; j < s->n; j++) {
		const struct tool *o = &s->t[j];
		HWND ow = (o->flags & TTF_IDISHWND) ? (HWND)o->id : o->hwnd;
		if ((o->flags & TTF_SUBCLASS) && ow == win)
			return TRUE;
	}
	comctl_remove_subclass(win, (SUBCLASSPROC)relay_subclass, (UINT_PTR)h);
	return TRUE;
}

static BOOL add(HWND h, struct tooltip *s, const TTTOOLINFOW *ti, int wide)
{
	if (!valid_size(ti))
		return FALSE;
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 8;
		void *p = realloc(s->t, (size_t)cap * sizeof(*s->t));
		if (!p)
			return FALSE;
		s->t = p;
		s->cap = cap;
	}
	/* The tool is in the list before its owner is asked for its
	 * notification format, so a tool added from that handler goes
	 * after it; afterwards the tool is found again by its uid. */
	int i = s->n++;
	struct tool *t = &s->t[i];
	memset(t, 0, sizeof(*t));
	set_fields(t, ti, wide);
	t->uid = ++s->next_uid;
	unsigned uid = t->uid;
	HWND owner = t->hwnd;
	LRESULT f = owner ? comctl_send(owner, WM_NOTIFYFORMAT, (WPARAM)h,
					NF_QUERY) : NFR_UNICODE;
	if (!comctl_alive(h, s))
		return FALSE;
	/* A handler that deleted the tool again leaves nothing to relay
	 * for; the add itself succeeded. */
	if ((i = find_uid(s, uid)) < 0)
		return TRUE;
	t = &s->t[i];
	t->ansi_owner = f == NFR_ANSI;
	if (t->flags & TTF_SUBCLASS) {
		UINT_PTR id = t->id;
		owner = t->hwnd;
		HWND win = (t->flags & TTF_IDISHWND) ? (HWND)id : owner;
		comctl_set_subclass(win, (SUBCLASSPROC)relay_subclass,
				    (UINT_PTR)h, 0);
	}
	return TRUE;
}

static void relay(HWND h, struct tooltip *s, const MSG *m)
{
	if (!m)
		return;
	switch (m->message) {
	case WM_MOUSEMOVE: {
		if (!s->active || s->tracking)
			return;
		POINT pt = { (short)LOWORD(m->lParam),
			     (short)HIWORD(m->lParam) };
		show(h, s, hit(s, m->hwnd, pt, TRUE));
		break;
	}
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
		if (!s->tracking)
			pop(h, s);
		break;
	}
}

static LRESULT STUB_WINAPI relay_subclass(HWND hwnd, UINT msg, WPARAM wp,
					  LPARAM lp, UINT_PTR id,
					  DWORD_PTR ref)
{
	if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN ||
	    msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN ||
	    msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN ||
	    msg == WM_MBUTTONUP) {
		MSG m;
		memset(&m, 0, sizeof(m));
		m.hwnd = hwnd;
		m.message = msg;
		m.wParam = wp;
		m.lParam = lp;
		comctl_send((HWND)id, TTM_RELAYEVENT, 0, (LPARAM)&m);
	}
	return comctl_def_subclass(hwnd, msg, wp, lp);
}

/* The tip's size for a text: the text's extent, padded, with the
 * margins, wrapped at the maximum width when one is set. */
static LRESULT bubble(const struct tooltip *s, const WCHAR *text)
{
	int w = comctl_text_width(text), lines = 1;
	int title = s->title ? LINE_HEIGHT : 0;

	if (s->max_width > 0 && w > s->max_width) {
		lines = (w + s->max_width - 1) / s->max_width;
		w = s->max_width;
	}
	int tw = s->title ? comctl_text_width(s->title) : 0;
	if (tw > w)
		w = tw;
	int cx = w + 2 * (BORDER + TEXT_PAD) + s->margin.left + s->margin.right;
	int cy = lines * LINE_HEIGHT + title + 2 * BORDER + s->margin.top +
		 s->margin.bottom;
	return MAKELONG(cx, cy);
}

static void destroy(HWND h, struct tooltip *s)
{
	/* The window is going: no TTN_POP for a tip nobody sees. */
	s->visible = FALSE;
	while (s->n)
		delete_at(h, s, s->n - 1);
	free(s->t);
	free(s->title);
	free(s);
	comctl_set_state(h, NULL);
}

static LRESULT get_text(HWND h, struct tooltip *s, WPARAM wp,
			TTTOOLINFOW *ti, int wide)
{
	if (!ti || !ti->lpszText)
		return 0;
	int i = find(s, ti->hwnd, ti->uId);
	int size = wp ? (int)wp : TEXT_MAX;
	if (i < 0) {
		if (wide)
			ti->lpszText[0] = 0;
		else
			((char *)ti->lpszText)[0] = 0;
		return 0;
	}
	WCHAR *t = text_of(h, s, i);
	comctl_copy_text_out(t ? t : u"", ti->lpszText, size, wide);
	free(t);
	return 0;
}

LRESULT STUB_WINAPI comctl_tooltip_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct tooltip *s = comctl_state(h);
	int wide = 1;

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->current = -1;
		s->active = TRUE;
		s->unicode = TRUE;
		s->max_width = -1;
		s->bk = sys_colour(COLOR_INFOBK);
		s->fg = sys_colour(COLOR_INFOTEXT);
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	TTTOOLINFOW *ti = (TTTOOLINFOW *)lp;
	switch (msg) {
	case TTM_ADDTOOLA:
		wide = 0;
		/* fall through */
	case TTM_ADDTOOLW:
		return add(h, s, ti, wide);
	case TTM_DELTOOLA:
	case TTM_DELTOOLW: {
		if (!valid_size(ti))
			return 0;
		int i = find(s, ti->hwnd, ti->uId);
		if (i >= 0)
			delete_at(h, s, i);
		return 0;
	}
	case TTM_GETTOOLINFOA:
		wide = 0;
		/* fall through */
	case TTM_GETTOOLINFOW: {
		if (!valid_size(ti))
			return FALSE;
		int i = find(s, ti->hwnd, ti->uId);
		if (i < 0)
			return FALSE;
		get_fields(&s->t[i], ti, wide);
		return TRUE;
	}
	case TTM_SETTOOLINFOA:
		wide = 0;
		/* fall through */
	case TTM_SETTOOLINFOW: {
		if (!valid_size(ti))
			return 0;
		int i = find(s, ti->hwnd, ti->uId);
		if (i >= 0)
			set_fields(&s->t[i], ti, wide);
		return 0;
	}
	case TTM_NEWTOOLRECTA:
	case TTM_NEWTOOLRECTW: {
		if (!valid_size(ti))
			return 0;
		int i = find(s, ti->hwnd, ti->uId);
		if (i >= 0)
			s->t[i].rect = ti->rect;
		return 0;
	}
	case TTM_UPDATETIPTEXTA:
		wide = 0;
		/* fall through */
	case TTM_UPDATETIPTEXTW: {
		if (!valid_size(ti))
			return 0;
		int i = find(s, ti->hwnd, ti->uId);
		if (i >= 0)
			set_text(&s->t[i], ti, wide);
		return 0;
	}
	case TTM_GETTEXTA:
		wide = 0;
		/* fall through */
	case TTM_GETTEXTW:
		return get_text(h, s, wp, ti, wide);
	case TTM_GETTOOLCOUNT:
		return s->n;
	case TTM_ENUMTOOLSA:
		wide = 0;
		/* fall through */
	case TTM_ENUMTOOLSW:
		if ((int)wp < 0 || (int)wp >= s->n || !valid_size(ti))
			return FALSE;
		get_fields(&s->t[wp], ti, wide);
		return TRUE;
	case TTM_GETCURRENTTOOLA:
		wide = 0;
		/* fall through */
	case TTM_GETCURRENTTOOLW:
		if (s->current < 0 || !s->visible)
			return FALSE;
		if (ti) {
			if (!valid_size(ti))
				return FALSE;
			get_fields(&s->t[s->current], ti, wide);
		}
		return TRUE;
	case TTM_HITTESTA:
		wide = 0;
		/* fall through */
	case TTM_HITTESTW: {
		TTHITTESTINFOW *hi = (TTHITTESTINFOW *)lp;
		if (!hi)
			return FALSE;
		int i = hit(s, hi->hwnd, hi->pt, FALSE);
		if (i < 0)
			return FALSE;
		if (hi->ti.cbSize < offsetof(TTTOOLINFOW, lParam))
			hi->ti.cbSize = offsetof(TTTOOLINFOW, lParam);
		get_fields(&s->t[i], &hi->ti, wide);
		return TRUE;
	}
	case TTM_RELAYEVENT:
		relay(h, s, (const MSG *)lp);
		return 0;
	case TTM_WINDOWFROMPOINT:
		return 0;
	case TTM_ACTIVATE:
		s->active = (BOOL)wp;
		if (!s->active)
			pop(h, s);
		return 0;
	case TTM_SETDELAYTIME: {
		int ms = (short)LOWORD(lp);
		if (wp == TTDT_AUTOMATIC) {
			for (int k = 1; k < 4; k++)
				s->delay_set[k] = FALSE;
			if (ms >= 0) {
				s->delay[TTDT_INITIAL] = ms;
				s->delay[TTDT_AUTOPOP] = ms * 10;
				s->delay[TTDT_RESHOW] = ms / 5;
				for (int k = 1; k < 4; k++)
					s->delay_set[k] = TRUE;
			}
		} else if (wp < 4) {
			s->delay[wp] = ms;
			s->delay_set[wp] = ms >= 0;
		}
		return 0;
	}
	case TTM_GETDELAYTIME:
		return wp >= 1 && wp < 4 ? delay(s, (int)wp) : 0;
	case TTM_SETTIPBKCOLOR:
		s->bk = (COLORREF)wp;
		return 0;
	case TTM_GETTIPBKCOLOR:
		return s->bk;
	case TTM_SETTIPTEXTCOLOR:
		s->fg = (COLORREF)wp;
		return 0;
	case TTM_GETTIPTEXTCOLOR:
		return s->fg;
	case TTM_SETMAXTIPWIDTH: {
		int old = s->max_width;
		s->max_width = (int)lp;
		return old;
	}
	case TTM_GETMAXTIPWIDTH:
		return s->max_width;
	case TTM_SETMARGIN:
		if (lp)
			s->margin = *(const RECT *)lp;
		return 0;
	case TTM_GETMARGIN:
		if (lp)
			*(RECT *)lp = s->margin;
		return 0;
	case TTM_POP:
		pop(h, s);
		return 0;
	case TTM_POPUP:
	case TTM_UPDATE:
		if (s->current >= 0 && !s->visible && s->active)
			show(h, s, s->current);
		return 0;
	case TTM_TRACKACTIVATE: {
		if (!wp) {
			pop(h, s);
			if (!comctl_alive(h, s))
				return 0;
			s->tracking = FALSE;
			return 0;
		}
		if (!valid_size(ti))
			return 0;
		int i = find(s, ti->hwnd, ti->uId);
		if (i < 0 || !(s->t[i].flags & TTF_TRACK))
			return 0;
		s->tracking = TRUE;
		show(h, s, i);
		return 0;
	}
	case TTM_TRACKPOSITION:
		s->track.x = (short)LOWORD(lp);
		s->track.y = (short)HIWORD(lp);
		if (s->tracking && s->current >= 0) {
			WCHAR *t = text_of(h, s, s->current);
			if (!comctl_alive(h, s)) {
				free(t);
				return 0;
			}
			LRESULT sz = bubble(s, t);
			free(t);
			comctl_move(h, s->track.x, s->track.y, LOWORD(sz),
				    HIWORD(sz));
		}
		return 0;
	case TTM_GETBUBBLESIZE: {
		if (!valid_size(ti))
			return 0;
		int i = find(s, ti->hwnd, ti->uId);
		if (i < 0)
			return 0;
		WCHAR *t = text_of(h, s, i);
		LRESULT sz = comctl_alive(h, s) ? bubble(s, t) : 0;
		free(t);
		return sz;
	}
	case TTM_ADJUSTRECT: {
		RECT *rc = (RECT *)lp;
		if (!rc)
			return FALSE;
		int dl = BORDER + TEXT_PAD + s->margin.left;
		int dt = BORDER + s->margin.top;
		int dr = BORDER + TEXT_PAD + s->margin.right;
		int db = BORDER + s->margin.bottom;
		if (wp) {
			rc->left -= dl, rc->top -= dt;
			rc->right += dr, rc->bottom += db;
		} else {
			rc->left += dl, rc->top += dt;
			rc->right -= dr, rc->bottom -= db;
		}
		return TRUE;
	}
	case TTM_SETTITLEA:
		wide = 0;
		/* fall through */
	case TTM_SETTITLEW:
		free(s->title);
		s->title = NULL;
		s->icon = wp;
		if (lp)
			s->title = wide ? comctl_w_dup((const WCHAR *)lp) :
					  comctl_a_to_w((const char *)lp);
		return TRUE;
	case TTM_GETTITLE: {
		TTGETTITLE *g = (TTGETTITLE *)lp;
		if (!g || g->dwSize < sizeof(*g))
			return 0;
		g->uTitleBitmap = (UINT)s->icon;
		if (g->pszTitle && g->cch)
			comctl_copy_text_out(s->title ? s->title : u"",
					     g->pszTitle, (int)g->cch, 1);
		return 0;
	}
	case CCM_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case CCM_GETUNICODEFORMAT:
		return s->unicode;
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		destroy(h, s);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}
