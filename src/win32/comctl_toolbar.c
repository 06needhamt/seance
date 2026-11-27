/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's toolbar (ToolbarWindow32), headless: its buttons, their
 * states and styles, the string pool, bitmap and image list bookkeeping,
 * sizes, rows, the hot item and the insert mark, with Windows' messages.
 * Nothing is drawn. Buttons sit side by side from the indent, each
 * buttons-size wide (separators SEPARATOR_WIDTH, or their iBitmap when
 * that is set, BTNS_AUTOSIZE buttons as wide as their text needs), and a
 * row ends after a TBSTATE_WRAP button or, with TBSTYLE_WRAPABLE, where
 * the next button would cross the client width. A click (WM_LBUTTONDOWN
 * then WM_LBUTTONUP on an enabled button) toggles BTNS_CHECK buttons,
 * keeps one button of a BTNS_GROUP run checked and sends WM_COMMAND to
 * the notify window; BTNS_DROPDOWN buttons send TBN_DROPDOWN instead.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define SEPARATOR_WIDTH 8
#define IS_INTRES(p)    (((UINT_PTR)(p) >> 16) == 0)

struct tb_button {
	int iBitmap, idCommand;
	BYTE fsState, fsStyle;
	DWORD_PTR dwData;
	INT_PTR iString;	/* as given */
	WCHAR *text;		/* when iString pointed at a string */
	WORD cx;		/* TBBUTTONINFO's, 0 for the default */
	RECT rc;
};

struct tb_bitmap {
	HINSTANCE inst;
	UINT_PTR id;
	int first, count;
};

struct toolbar {
	struct tb_button *b;
	int n, cap;
	WCHAR **str;
	int nstr;
	struct tb_bitmap *bm;
	int nbm, nimages;
	UINT struct_size;
	int bm_cx, bm_cy, cx, cy, pad_cx, pad_cy;
	int min_cx, max_cx, indent, max_rows;
	HIMAGELIST himl[3];	/* normal, hot, disabled */
	DWORD ex_style, dt_mask, dt_flags;
	int hot, pressed;
	BOOL anchor, unicode, list;
	TBINSERTMARK mark;
	COLORREF mark_colour;
	HWND notify, tooltips;
	HFONT font;
};

static const WCHAR *button_text(const struct toolbar *s,
				const struct tb_button *b)
{
	if (b->text)
		return b->text;
	if (b->iString >= 0 && b->iString < s->nstr)
		return s->str[b->iString];
	return NULL;
}

static int button_width(const struct toolbar *s, const struct tb_button *b)
{
	if (b->fsStyle & BTNS_SEP)
		return b->iBitmap > 0 ? b->iBitmap : SEPARATOR_WIDTH;
	if (b->cx)
		return b->cx;
	int w = s->cx;
	if (b->fsStyle & BTNS_AUTOSIZE) {
		const WCHAR *t = button_text(s, b);
		int bw = b->iBitmap >= 0 ? s->bm_cx : 0;
		int tw = comctl_text_width(t);
		/* TBSTYLE_LIST puts the text beside the image, otherwise
		 * it goes below. */
		w = s->list ? bw + tw + s->pad_cx :
			      (bw > tw ? bw : tw) + s->pad_cx;
	}
	if (s->min_cx && w < s->min_cx)
		w = s->min_cx;
	if (s->max_cx && w > s->max_cx)
		w = s->max_cx;
	if ((b->fsStyle & BTNS_DROPDOWN) &&
	    (s->ex_style & TBSTYLE_EX_DRAWDDARROWS))
		w += 14;
	return w;
}

/* Every visible button's rectangle, row by row. */
static void layout(HWND h, struct toolbar *s)
{
	RECT client;
	int x = s->indent, y = 0, width;
	BOOL wrapable = (comctl_style(h) & TBSTYLE_WRAPABLE) != 0;

	s->list = (comctl_style(h) & TBSTYLE_LIST) != 0;
	comctl_client_rect(h, &client);
	width = client.right - client.left;
	for (int i = 0; i < s->n; i++) {
		struct tb_button *b = &s->b[i];
		if (b->fsState & TBSTATE_HIDDEN) {
			memset(&b->rc, 0, sizeof(b->rc));
			continue;
		}
		int w = button_width(s, b);
		if (wrapable && width > 0 && x > s->indent && x + w > width) {
			x = s->indent;
			y += s->cy;
		}
		b->rc.left = x;
		b->rc.top = y;
		b->rc.right = x + w;
		b->rc.bottom = y + s->cy;
		x += w;
		if (b->fsState & TBSTATE_WRAP) {
			x = s->indent;
			y += s->cy;
		}
	}
}

static int rows(const struct toolbar *s)
{
	int r = 0, last = -1;

	for (int i = 0; i < s->n; i++) {
		if (s->b[i].fsState & TBSTATE_HIDDEN)
			continue;
		if (s->b[i].rc.top != last) {
			last = s->b[i].rc.top;
			r++;
		}
	}
	return r ? r : 1;
}

static void bounds(const struct toolbar *s, SIZE *sz)
{
	sz->cx = sz->cy = 0;
	for (int i = 0; i < s->n; i++) {
		const struct tb_button *b = &s->b[i];
		if (b->fsState & TBSTATE_HIDDEN)
			continue;
		if (b->rc.right > sz->cx)
			sz->cx = b->rc.right;
		if (b->rc.bottom > sz->cy)
			sz->cy = b->rc.bottom;
	}
}

static int index_of(const struct toolbar *s, int id)
{
	for (int i = 0; i < s->n; i++)
		if (s->b[i].idCommand == id)
			return i;
	return -1;
}

static HWND target(HWND h, const struct toolbar *s)
{
	return s->notify ? s->notify : comctl_parent(h);
}

static void clear(struct tb_button *b)
{
	free(b->text);
	b->text = NULL;
}

/* One TBBUTTON from the caller's array, as wide as TB_BUTTONSTRUCTSIZE
 * says: an older, shorter struct has no iString. */
static void read_button(const struct toolbar *s, const BYTE *p,
			TBBUTTON *out)
{
	size_t n = s->struct_size < sizeof(*out) ? s->struct_size :
						   sizeof(*out);
	memset(out, 0, sizeof(*out));
	memcpy(out, p, n);
	if (n < offsetof(TBBUTTON, iString) + sizeof(out->iString))
		out->iString = -1;
}

static BOOL insert(HWND h, struct toolbar *s, int at, const TBBUTTON *in,
		   int wide)
{
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
	struct tb_button *b = &s->b[at];
	memset(b, 0, sizeof(*b));
	b->iBitmap = in->iBitmap;
	b->idCommand = in->idCommand;
	b->fsState = in->fsState;
	b->fsStyle = in->fsStyle;
	b->dwData = in->dwData;
	b->iString = in->iString;
	if (in->iString != -1 && !IS_INTRES(in->iString) &&
	    !(in->iString >= 0 && in->iString < s->nstr))
		b->text = wide ? comctl_w_dup((const WCHAR *)in->iString) :
				 comctl_a_to_w((const char *)in->iString);
	s->n++;
	if (s->hot >= at && s->hot >= 0)
		s->hot++;
	if (s->pressed >= at && s->pressed >= 0)
		s->pressed++;
	layout(h, s);
	return TRUE;
}

static BOOL delete(HWND h, struct toolbar *s, int i)
{
	if (i < 0 || i >= s->n)
		return FALSE;
	NMTOOLBARW nm;
	memset(&nm, 0, sizeof(nm));
	nm.iItem = s->b[i].idCommand;
	nm.tbButton.iBitmap = s->b[i].iBitmap;
	nm.tbButton.idCommand = s->b[i].idCommand;
	nm.tbButton.fsState = s->b[i].fsState;
	nm.tbButton.fsStyle = s->b[i].fsStyle;
	nm.tbButton.dwData = s->b[i].dwData;
	nm.tbButton.iString = s->b[i].iString;
	int n = s->n;
	comctl_notify_to(target(h, s), h, TBN_DELETINGBUTTON, &nm.hdr);
	/* The handler may have destroyed the toolbar or changed its
	 * buttons; then this deletion is dropped. */
	if (!comctl_alive(h, s) || s->n != n)
		return FALSE;
	clear(&s->b[i]);
	memmove(&s->b[i], &s->b[i + 1], (size_t)(s->n - i - 1) * sizeof(*s->b));
	s->n--;
	if (s->hot == i)
		s->hot = -1;
	else if (s->hot > i)
		s->hot--;
	if (s->pressed == i)
		s->pressed = -1;
	else if (s->pressed > i)
		s->pressed--;
	layout(h, s);
	return TRUE;
}

/* Checking a BTNS_GROUP button unchecks the rest of its run. */
static void check(struct toolbar *s, int i, BOOL on)
{
	struct tb_button *b = &s->b[i];

	if (!on) {
		b->fsState &= (BYTE)~TBSTATE_CHECKED;
		return;
	}
	b->fsState |= TBSTATE_CHECKED;
	if (!(b->fsStyle & BTNS_GROUP))
		return;
	for (int j = i - 1; j >= 0 && (s->b[j].fsStyle & BTNS_GROUP) &&
			    !(s->b[j].fsStyle & BTNS_SEP); j--)
		s->b[j].fsState &= (BYTE)~TBSTATE_CHECKED;
	for (int j = i + 1; j < s->n && (s->b[j].fsStyle & BTNS_GROUP) &&
			    !(s->b[j].fsStyle & BTNS_SEP); j++)
		s->b[j].fsState &= (BYTE)~TBSTATE_CHECKED;
}

static LRESULT set_flag(HWND h, struct toolbar *s, int id, BYTE flag, BOOL on)
{
	int i = index_of(s, id);
	if (i < 0)
		return FALSE;
	if (flag == TBSTATE_CHECKED) {
		check(s, i, on);
		return TRUE;
	}
	BYTE old = s->b[i].fsState;
	if (on)
		s->b[i].fsState |= flag;
	else
		s->b[i].fsState &= (BYTE)~flag;
	if (flag == TBSTATE_ENABLED && !on && s->pressed == i) {
		s->pressed = -1;
		s->b[i].fsState &= (BYTE)~TBSTATE_PRESSED;
	}
	if ((old ^ s->b[i].fsState) & (TBSTATE_HIDDEN | TBSTATE_WRAP))
		layout(h, s);
	return TRUE;
}

static LRESULT get_flag(const struct toolbar *s, int id, BYTE flag)
{
	int i = index_of(s, id);
	return i < 0 ? -1 : (s->b[i].fsState & flag) != 0;
}

/* The strings of a TB_ADDSTRING: a run of NUL-terminated strings ended
 * by an empty one, or a string resource whose first character separates
 * them. */
static LRESULT add_strings(struct toolbar *s, WPARAM wp, LPARAM lp, int wide)
{
	int first = s->nstr;
	WCHAR *res = NULL;
	const void *p = (const void *)lp;
	WCHAR sep = 0;

	if (wp && IS_INTRES(lp)) {
		WCHAR buf[512];
		int n = ((int (WINAPI *)(HINSTANCE, UINT, LPWSTR, int))
			 USER32(LoadStringW))((HINSTANCE)wp, (UINT)lp, buf,
					      512);
		if (n <= 0)
			return -1;
		sep = buf[0];
		if (!(res = calloc((size_t)n + 2, sizeof(WCHAR))))
			return -1;
		for (int i = 1; i < n; i++)
			res[i - 1] = buf[i] == sep ? 0 : buf[i];
		p = res;
		wide = 1;
	} else if (!lp) {
		return -1;
	}
	for (;;) {
		WCHAR *w = wide ? comctl_w_dup(p) : comctl_a_to_w(p);
		size_t len;
		if (!w || !*w) {
			free(w);
			break;
		}
		void *q = realloc(s->str, (size_t)(s->nstr + 1) * sizeof(*s->str));
		if (!q) {
			free(w);
			break;
		}
		s->str = q;
		s->str[s->nstr++] = w;
		len = wide ? wstrlen(p) : strlen(p);
		p = (const BYTE *)p + (len + 1) * (wide ? sizeof(WCHAR) : 1);
	}
	free(res);
	return s->nstr > first ? first : -1;
}

/* How many images a standard comctl32 bitmap holds. */
static int standard_count(UINT_PTR id)
{
	switch (id) {
	case IDB_STD_SMALL_COLOR: case IDB_STD_LARGE_COLOR:
		return 15;
	case IDB_VIEW_SMALL_COLOR: case IDB_VIEW_LARGE_COLOR:
		return 12;
	case IDB_HIST_SMALL_COLOR: case IDB_HIST_LARGE_COLOR:
		return 5;
	}
	return 0;
}

static LRESULT add_bitmap(struct toolbar *s, int count, const TBADDBITMAP *a)
{
	if (!a)
		return -1;
	if (a->hInst == HINST_COMMCTRL)
		count = standard_count(a->nID);
	for (int i = 0; i < s->nbm; i++)
		if (s->bm[i].inst == a->hInst && s->bm[i].id == a->nID)
			return s->bm[i].first;
	if (count <= 0)
		return -1;
	void *p = realloc(s->bm, (size_t)(s->nbm + 1) * sizeof(*s->bm));
	if (!p)
		return -1;
	s->bm = p;
	s->bm[s->nbm].inst = a->hInst;
	s->bm[s->nbm].id = a->nID;
	s->bm[s->nbm].first = s->nimages;
	s->bm[s->nbm].count = count;
	s->nbm++;
	s->nimages += count;
	return s->nimages - count;
}

static BOOL replace_bitmap(struct toolbar *s, const TBREPLACEBITMAP *r)
{
	if (!r)
		return FALSE;
	for (int i = 0; i < s->nbm; i++) {
		if (s->bm[i].inst != r->hInstOld || s->bm[i].id != r->nIDOld)
			continue;
		int delta = r->nButtons - s->bm[i].count;
		s->bm[i].inst = r->hInstNew;
		s->bm[i].id = r->nIDNew;
		s->bm[i].count = r->nButtons;
		for (int j = i + 1; j < s->nbm; j++)
			s->bm[j].first += delta;
		s->nimages += delta;
		return TRUE;
	}
	return FALSE;
}

/* TBBUTTONINFO's A and W forms differ only in the text pointer's type. */
static LRESULT get_info(struct toolbar *s, int id, TBBUTTONINFOW *bi, int wide)
{
	if (!bi || bi->cbSize != sizeof(*bi))
		return -1;
	int i = (bi->dwMask & TBIF_BYINDEX) ? id : index_of(s, id);
	if (i < 0 || i >= s->n)
		return -1;
	struct tb_button *b = &s->b[i];
	if (bi->dwMask & TBIF_COMMAND)
		bi->idCommand = b->idCommand;
	if (bi->dwMask & TBIF_IMAGE)
		bi->iImage = b->iBitmap;
	if (bi->dwMask & TBIF_STATE)
		bi->fsState = b->fsState;
	if (bi->dwMask & TBIF_STYLE)
		bi->fsStyle = b->fsStyle;
	if (bi->dwMask & TBIF_LPARAM)
		bi->lParam = b->dwData;
	if (bi->dwMask & TBIF_SIZE)
		bi->cx = (WORD)(b->rc.right - b->rc.left);
	if ((bi->dwMask & TBIF_TEXT) && bi->pszText)
		comctl_copy_text_out(button_text(s, b), bi->pszText,
				     bi->cchText, wide);
	return i;
}

static LRESULT set_info(HWND h, struct toolbar *s, int id,
			const TBBUTTONINFOW *bi, int wide)
{
	if (!bi || bi->cbSize != sizeof(*bi))
		return FALSE;
	int i = (bi->dwMask & TBIF_BYINDEX) ? id : index_of(s, id);
	if (i < 0 || i >= s->n)
		return FALSE;
	struct tb_button *b = &s->b[i];
	if (bi->dwMask & TBIF_COMMAND)
		b->idCommand = bi->idCommand;
	if (bi->dwMask & TBIF_IMAGE)
		b->iBitmap = bi->iImage;
	if (bi->dwMask & TBIF_STATE)
		b->fsState = bi->fsState;
	if (bi->dwMask & TBIF_STYLE)
		b->fsStyle = bi->fsStyle;
	if (bi->dwMask & TBIF_LPARAM)
		b->dwData = bi->lParam;
	if (bi->dwMask & TBIF_SIZE)
		b->cx = bi->cx;
	if (bi->dwMask & TBIF_TEXT) {
		clear(b);
		if (bi->pszText)
			b->text = wide ? comctl_w_dup(bi->pszText) :
					 comctl_a_to_w((const char *)bi->pszText);
		b->iString = -1;
	}
	layout(h, s);
	return TRUE;
}

static int hit(const struct toolbar *s, POINT pt)
{
	for (int i = 0; i < s->n; i++) {
		const struct tb_button *b = &s->b[i];
		if (b->fsState & TBSTATE_HIDDEN)
			continue;
		if (pt.x >= b->rc.left && pt.x < b->rc.right &&
		    pt.y >= b->rc.top && pt.y < b->rc.bottom)
			return i;
	}
	return -1;
}

static void set_hot(HWND h, struct toolbar *s, int i, DWORD why)
{
	if (i == s->hot)
		return;
	NMTBHOTITEM nm;
	memset(&nm, 0, sizeof(nm));
	nm.idOld = s->hot >= 0 ? s->b[s->hot].idCommand : 0;
	nm.idNew = i >= 0 ? s->b[i].idCommand : 0;
	nm.dwFlags = why | (s->hot < 0 ? HICF_ENTERING : 0) |
		     (i < 0 ? HICF_LEAVING : 0);
	if (comctl_notify_to(target(h, s), h, TBN_HOTITEMCHANGE, &nm.hdr))
		return;
	s->hot = i;
}

static void autosize(HWND h, struct toolbar *s)
{
	RECT pc;
	SIZE sz;
	HWND parent = comctl_parent(h);
	DWORD st = comctl_style(h);

	layout(h, s);
	if ((st & CCS_NORESIZE) || !parent)
		return;
	comctl_client_rect(parent, &pc);
	bounds(s, &sz);
	int cy = sz.cy ? sz.cy : s->cy;
	int y = (st & CCS_BOTTOM) == CCS_BOTTOM ? pc.bottom - cy : 0;
	comctl_move(h, 0, y, pc.right - pc.left, cy);
	layout(h, s);
}

/* TB_SETROWS: wrap the visible buttons into that many rows. */
static void set_rows(HWND h, struct toolbar *s, int want, RECT *out)
{
	int vis = 0, per, k = 0;

	for (int i = 0; i < s->n; i++) {
		s->b[i].fsState &= (BYTE)~TBSTATE_WRAP;
		if (!(s->b[i].fsState & TBSTATE_HIDDEN))
			vis++;
	}
	if (want < 1)
		want = 1;
	per = (vis + want - 1) / want;
	for (int i = 0; i < s->n && per; i++) {
		if (s->b[i].fsState & TBSTATE_HIDDEN)
			continue;
		if (++k % per == 0 && k < vis)
			s->b[i].fsState |= TBSTATE_WRAP;
	}
	layout(h, s);
	if (out) {
		SIZE sz;
		bounds(s, &sz);
		out->left = out->top = 0;
		out->right = sz.cx;
		out->bottom = sz.cy;
	}
}

static void click(HWND h, struct toolbar *s, int i)
{
	struct tb_button *b = &s->b[i];

	if (b->fsStyle & BTNS_CHECK)
		check(s, i, (b->fsStyle & BTNS_GROUP) ? TRUE :
			    !(b->fsState & TBSTATE_CHECKED));
	comctl_send(target(h, s), WM_COMMAND,
		    MAKEWPARAM(b->idCommand, 0), (LPARAM)h);
}

static LRESULT mouse(HWND h, struct toolbar *s, UINT msg, LPARAM lp)
{
	POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
	int i = hit(s, pt);

	if (msg == WM_MOUSEMOVE) {
		if (comctl_style(h) & TBSTYLE_FLAT || s->hot >= 0 || i >= 0)
			set_hot(h, s, i >= 0 && !(s->b[i].fsStyle & BTNS_SEP) ?
				      i : -1, HICF_MOUSE);
		return 0;
	}
	if (msg == WM_LBUTTONDOWN) {
		if (i < 0 || (s->b[i].fsStyle & BTNS_SEP) ||
		    !(s->b[i].fsState & TBSTATE_ENABLED))
			return 0;
		if (s->b[i].fsStyle & BTNS_DROPDOWN) {
			NMTOOLBARW nm;
			memset(&nm, 0, sizeof(nm));
			nm.iItem = s->b[i].idCommand;
			nm.rcButton = s->b[i].rc;
			comctl_notify_to(target(h, s), h, TBN_DROPDOWN,
					 &nm.hdr);
			return 0;
		}
		s->pressed = i;
		s->b[i].fsState |= TBSTATE_PRESSED;
		return 0;
	}
	/* WM_LBUTTONUP */
	int p = s->pressed;
	s->pressed = -1;
	if (p < 0 || p >= s->n)
		return 0;
	s->b[p].fsState &= (BYTE)~TBSTATE_PRESSED;
	if (i == p && (s->b[p].fsState & TBSTATE_ENABLED))
		click(h, s, p);
	return 0;
}

static void destroy(HWND h, struct toolbar *s)
{
	for (int i = 0; i < s->n; i++)
		clear(&s->b[i]);
	for (int i = 0; i < s->nstr; i++)
		free(s->str[i]);
	free(s->str);
	free(s->b);
	free(s->bm);
	free(s);
	comctl_set_state(h, NULL);
}

static LRESULT button_text_out(struct toolbar *s, int id, void *buf, int wide)
{
	int i = index_of(s, id);
	if (i < 0)
		return -1;
	const WCHAR *t = button_text(s, &s->b[i]);
	int len = t ? (int)wstrlen(t) : 0;
	if (buf) {
		if (wide) {
			memcpy(buf, t ? t : u"", (size_t)(len + 1) * sizeof(WCHAR));
		} else {
			char *a = comctl_w_to_a(t ? t : u"");
			if (!a)
				return -1;
			len = (int)strlen(a);
			memcpy(buf, a, (size_t)len + 1);
			free(a);
		}
	} else if (!wide && t) {
		char *a = comctl_w_to_a(t);
		len = a ? (int)strlen(a) : 0;
		free(a);
	}
	return len;
}

LRESULT STUB_WINAPI comctl_toolbar_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct toolbar *s = comctl_state(h);
	int wide = 1;

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->struct_size = sizeof(TBBUTTON);
		s->bm_cx = 16;
		s->bm_cy = 15;
		s->cx = 24;
		s->cy = 22;
		s->pad_cx = 7;
		s->pad_cy = 6;
		s->max_rows = 1;
		s->hot = s->pressed = -1;
		s->mark.iButton = -1;
		s->unicode = TRUE;
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case TB_BUTTONSTRUCTSIZE:
		if (wp >= offsetof(TBBUTTON, dwData) + sizeof(DWORD_PTR))
			s->struct_size = (UINT)wp;
		return 0;
	case TB_ADDBUTTONSA:
		wide = 0;
		/* fall through */
	case TB_ADDBUTTONSW:
		if (!lp && wp)
			return FALSE;
		for (UINT i = 0; i < (UINT)wp; i++) {
			TBBUTTON b;
			read_button(s, (const BYTE *)lp + (size_t)i *
					   s->struct_size, &b);
			if (!insert(h, s, -1, &b, wide))
				return FALSE;
		}
		return TRUE;
	case TB_INSERTBUTTONA:
		wide = 0;
		/* fall through */
	case TB_INSERTBUTTONW: {
		TBBUTTON b;
		if (!lp)
			return FALSE;
		read_button(s, (const BYTE *)lp, &b);
		return insert(h, s, (int)wp, &b, wide);
	}
	case TB_DELETEBUTTON:
		return delete(h, s, (int)wp);
	case TB_BUTTONCOUNT:
		return s->n;
	case TB_GETBUTTON: {
		int i = (int)wp;
		TBBUTTON *o = (TBBUTTON *)lp;
		if (i < 0 || i >= s->n || !o)
			return FALSE;
		memset(o, 0, sizeof(*o));
		o->iBitmap = s->b[i].iBitmap;
		o->idCommand = s->b[i].idCommand;
		o->fsState = s->b[i].fsState;
		o->fsStyle = s->b[i].fsStyle;
		o->dwData = s->b[i].dwData;
		o->iString = s->b[i].iString;
		return TRUE;
	}
	case TB_COMMANDTOINDEX:
		return index_of(s, (int)wp);
	case TB_MOVEBUTTON: {
		int from = (int)wp, to = (int)lp;
		if (from < 0 || from >= s->n || to < 0 || to >= s->n)
			return FALSE;
		struct tb_button t = s->b[from];
		if (from < to)
			memmove(&s->b[from], &s->b[from + 1],
				(size_t)(to - from) * sizeof(*s->b));
		else
			memmove(&s->b[to + 1], &s->b[to],
				(size_t)(from - to) * sizeof(*s->b));
		s->b[to] = t;
		/* The hot and pressed buttons stay the same buttons. */
		int *track[2] = { &s->hot, &s->pressed };
		for (int k = 0; k < 2; k++) {
			int v = *track[k];
			if (v == from)
				*track[k] = to;
			else if (from < to && v > from && v <= to)
				(*track[k])--;
			else if (from > to && v >= to && v < from)
				(*track[k])++;
		}
		layout(h, s);
		return TRUE;
	}
	case TB_SETCMDID:
		if ((int)wp < 0 || (int)wp >= s->n)
			return FALSE;
		s->b[wp].idCommand = (int)lp;
		return TRUE;
	case TB_ENABLEBUTTON:
		return set_flag(h, s, (int)wp, TBSTATE_ENABLED, LOWORD(lp));
	case TB_CHECKBUTTON:
		return set_flag(h, s, (int)wp, TBSTATE_CHECKED, LOWORD(lp));
	case TB_PRESSBUTTON:
		return set_flag(h, s, (int)wp, TBSTATE_PRESSED, LOWORD(lp));
	case TB_HIDEBUTTON:
		return set_flag(h, s, (int)wp, TBSTATE_HIDDEN, LOWORD(lp));
	case TB_INDETERMINATE:
		return set_flag(h, s, (int)wp, TBSTATE_INDETERMINATE,
				LOWORD(lp));
	case TB_MARKBUTTON:
		return set_flag(h, s, (int)wp, TBSTATE_MARKED, LOWORD(lp));
	case TB_ISBUTTONENABLED:
		return get_flag(s, (int)wp, TBSTATE_ENABLED);
	case TB_ISBUTTONCHECKED:
		return get_flag(s, (int)wp, TBSTATE_CHECKED);
	case TB_ISBUTTONPRESSED:
		return get_flag(s, (int)wp, TBSTATE_PRESSED);
	case TB_ISBUTTONHIDDEN:
		return get_flag(s, (int)wp, TBSTATE_HIDDEN);
	case TB_ISBUTTONINDETERMINATE:
		return get_flag(s, (int)wp, TBSTATE_INDETERMINATE);
	case TB_ISBUTTONHIGHLIGHTED:
		return get_flag(s, (int)wp, TBSTATE_MARKED);
	case TB_GETSTATE: {
		int i = index_of(s, (int)wp);
		return i < 0 ? -1 : s->b[i].fsState;
	}
	case TB_SETSTATE: {
		int i = index_of(s, (int)wp);
		if (i < 0)
			return FALSE;
		s->b[i].fsState = (BYTE)LOWORD(lp);
		layout(h, s);
		return TRUE;
	}
	case TB_ADDSTRINGA:
		wide = 0;
		/* fall through */
	case TB_ADDSTRINGW:
		return add_strings(s, wp, lp, wide);
	case TB_GETSTRINGA:
		wide = 0;
		/* fall through */
	case TB_GETSTRINGW: {
		int i = HIWORD(wp), max = LOWORD(wp);
		if (i < 0 || i >= s->nstr)
			return -1;
		if (lp && max > 0)
			comctl_copy_text_out(s->str[i], (void *)lp, max, wide);
		if (wide)
			return (LRESULT)wstrlen(s->str[i]);
		char *a = comctl_w_to_a(s->str[i]);
		int n = a ? (int)strlen(a) : -1;
		free(a);
		return n;
	}
	case TB_GETBUTTONTEXTA:
		wide = 0;
		/* fall through */
	case TB_GETBUTTONTEXTW:
		return button_text_out(s, (int)wp, (void *)lp, wide);
	case TB_ADDBITMAP:
		return add_bitmap(s, (int)wp, (const TBADDBITMAP *)lp);
	case TB_REPLACEBITMAP:
		return replace_bitmap(s, (const TBREPLACEBITMAP *)lp);
	case TB_LOADIMAGES: {
		TBADDBITMAP a = { (HINSTANCE)lp, wp };
		add_bitmap(s, 0, &a);
		return s->nimages;
	}
	case TB_GETBITMAP: {
		int i = index_of(s, (int)wp);
		return i < 0 ? 0 : s->b[i].iBitmap;
	}
	case TB_CHANGEBITMAP: {
		int i = index_of(s, (int)wp);
		if (i < 0)
			return FALSE;
		s->b[i].iBitmap = (int)lp;
		return TRUE;
	}
	case TB_GETBITMAPFLAGS:
		return 0;
	case TB_SETIMAGELIST:
	case TB_SETHOTIMAGELIST:
	case TB_SETDISABLEDIMAGELIST: {
		int k = msg == TB_SETIMAGELIST ? 0 :
			msg == TB_SETHOTIMAGELIST ? 1 : 2;
		HIMAGELIST old = s->himl[k];
		s->himl[k] = (HIMAGELIST)lp;
		return (LRESULT)old;
	}
	case TB_GETIMAGELIST:
		return (LRESULT)s->himl[0];
	case TB_GETHOTIMAGELIST:
		return (LRESULT)s->himl[1];
	case TB_GETDISABLEDIMAGELIST:
		return (LRESULT)s->himl[2];
	case TB_SETBITMAPSIZE:
		s->bm_cx = (short)LOWORD(lp) > 0 ? (short)LOWORD(lp) : 16;
		s->bm_cy = (short)HIWORD(lp) > 0 ? (short)HIWORD(lp) : 15;
		if (s->cx < s->bm_cx + s->pad_cx)
			s->cx = s->bm_cx + s->pad_cx;
		if (s->cy < s->bm_cy + s->pad_cy + 1)
			s->cy = s->bm_cy + s->pad_cy + 1;
		layout(h, s);
		return TRUE;
	case TB_SETBUTTONSIZE:
		if ((short)LOWORD(lp) > 0)
			s->cx = (short)LOWORD(lp);
		if ((short)HIWORD(lp) > 0)
			s->cy = (short)HIWORD(lp);
		layout(h, s);
		return TRUE;
	case TB_GETBUTTONSIZE:
		return MAKELONG(s->cx, s->cy);
	case TB_SETBUTTONWIDTH:
		s->min_cx = (short)LOWORD(lp);
		s->max_cx = (short)HIWORD(lp);
		layout(h, s);
		return TRUE;
	case TB_SETPADDING: {
		LRESULT old = MAKELONG(s->pad_cx, s->pad_cy);
		s->pad_cx = LOWORD(lp);
		s->pad_cy = HIWORD(lp);
		layout(h, s);
		return old;
	}
	case TB_GETPADDING:
		return MAKELONG(s->pad_cx, s->pad_cy);
	case TB_SETINDENT:
		s->indent = (int)wp;
		layout(h, s);
		return TRUE;
	case TB_SETMAXTEXTROWS:
		s->max_rows = (int)wp;
		return TRUE;
	case TB_GETTEXTROWS:
		return s->max_rows;
	case TB_SETDRAWTEXTFLAGS: {
		DWORD old = s->dt_flags;
		s->dt_mask = (DWORD)wp;
		s->dt_flags = (old & ~(DWORD)wp) | ((DWORD)lp & (DWORD)wp);
		return old;
	}
	case TB_SETSTYLE:
		comctl_set_style(h, (DWORD)lp);
		layout(h, s);
		return 0;
	case TB_GETSTYLE:
		return comctl_style(h);
	case TB_SETEXTENDEDSTYLE: {
		DWORD old = s->ex_style;
		s->ex_style = wp ? (old & ~(DWORD)wp) | ((DWORD)lp & (DWORD)wp)
				 : (DWORD)lp;
		layout(h, s);
		return old;
	}
	case TB_GETEXTENDEDSTYLE:
		return s->ex_style;
	case TB_GETITEMRECT: {
		int i = (int)wp;
		if (i < 0 || i >= s->n || !lp ||
		    (s->b[i].fsState & TBSTATE_HIDDEN))
			return FALSE;
		*(RECT *)lp = s->b[i].rc;
		return TRUE;
	}
	case TB_GETRECT: {
		int i = index_of(s, (int)wp);
		if (i < 0 || !lp || (s->b[i].fsState & TBSTATE_HIDDEN))
			return FALSE;
		*(RECT *)lp = s->b[i].rc;
		return TRUE;
	}
	case TB_GETMAXSIZE:
		if (!lp)
			return FALSE;
		bounds(s, (SIZE *)lp);
		return TRUE;
	case TB_AUTOSIZE:
		autosize(h, s);
		return 0;
	case TB_SETROWS:
		set_rows(h, s, LOWORD(wp), (RECT *)lp);
		return 0;
	case TB_GETROWS:
		return rows(s);
	case TB_HITTEST: {
		if (!lp)
			return -1;
		int i = hit(s, *(const POINT *)lp);
		if (i < 0)
			return -1;
		return (s->b[i].fsStyle & BTNS_SEP) ? -i : i;
	}
	case TB_GETHOTITEM:
		return s->hot;
	case TB_SETHOTITEM: {
		int old = s->hot, i = (int)wp;
		if (i < -1 || i >= s->n)
			i = -1;
		set_hot(h, s, i, HICF_OTHER);
		return old;
	}
	case TB_SETANCHORHIGHLIGHT: {
		BOOL old = s->anchor;
		s->anchor = (BOOL)wp;
		return old;
	}
	case TB_GETANCHORHIGHLIGHT:
		return s->anchor;
	case TB_SETINSERTMARK:
		if (lp)
			s->mark = *(const TBINSERTMARK *)lp;
		return 0;
	case TB_GETINSERTMARK:
		if (lp)
			*(TBINSERTMARK *)lp = s->mark;
		return 0;
	case TB_INSERTMARKHITTEST: {
		TBINSERTMARK *m = (TBINSERTMARK *)lp;
		if (!wp || !m)
			return FALSE;
		POINT pt = *(const POINT *)wp;
		int i = hit(s, pt);
		if (i < 0) {
			m->iButton = s->n - 1;
			m->dwFlags = TBIMHT_AFTER | TBIMHT_BACKGROUND;
			return FALSE;
		}
		m->iButton = i;
		m->dwFlags = pt.x * 2 >= s->b[i].rc.left + s->b[i].rc.right ?
			     TBIMHT_AFTER : 0;
		return TRUE;
	}
	case TB_SETINSERTMARKCOLOR: {
		COLORREF old = s->mark_colour;
		s->mark_colour = (COLORREF)lp;
		return old;
	}
	case TB_GETINSERTMARKCOLOR:
		return s->mark_colour;
	case TB_GETBUTTONINFOA:
		wide = 0;
		/* fall through */
	case TB_GETBUTTONINFOW:
		return get_info(s, (int)wp, (TBBUTTONINFOW *)lp, wide);
	case TB_SETBUTTONINFOA:
		wide = 0;
		/* fall through */
	case TB_SETBUTTONINFOW:
		return set_info(h, s, (int)wp, (const TBBUTTONINFOW *)lp, wide);
	case TB_SETPARENT: {
		HWND old = target(h, s);
		s->notify = (HWND)wp;
		return (LRESULT)old;
	}
	case TB_SETTOOLTIPS:
		s->tooltips = (HWND)wp;
		return 0;
	case TB_GETTOOLTIPS:
		return (LRESULT)s->tooltips;
	case TB_SAVERESTOREA:
	case TB_SAVERESTOREW:
	case TB_CUSTOMIZE:
		return 0;
	case TB_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case TB_GETUNICODEFORMAT:
		return s->unicode;
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
		return mouse(h, s, msg, lp);
	case WM_SIZE:
		layout(h, s);
		break;
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
