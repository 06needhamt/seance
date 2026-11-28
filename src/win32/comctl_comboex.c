/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's ComboBoxEx (ComboBoxEx32), headless: its items (text or
 * LPSTR_TEXTCALLBACK answered by CBEN_GETDISPINFO, images, overlay,
 * indent, lParam), the selection, the image list and the extended
 * styles, with Windows' CBEM_ messages and the combo box messages a
 * caller sends it (CB_GETCOUNT, CB_SETCURSEL, CB_GETLBTEXT, ...).
 * It answers them itself rather than through a user32 combo box child:
 * CBEM_GETCOMBOCONTROL is the control, and there is no edit control.
 * It sends CBEN_INSERTITEM and CBEN_DELETEITEM, and CBN_SELCHANGE,
 * CBN_DROPDOWN and CBN_CLOSEUP as WM_COMMAND.
 */
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

struct cbe_item {
	WCHAR *text;
	BOOL callback;
	int image, selimage, overlay, indent;
	LPARAM lParam;
	unsigned uid;		/* the item's identity while indexes move */
};

struct comboex {
	struct cbe_item *it;
	int n, cap, sel;
	HIMAGELIST himl;
	DWORD ex_style;
	BOOL dropped, unicode;	/* unicode: the parent takes CBEN_*W */
	unsigned next_uid;
	HFONT font;
};

static int find_uid(const struct comboex *s, unsigned uid)
{
	for (int i = 0; i < s->n; i++)
		if (s->it[i].uid == uid)
			return i;
	return -1;
}

static UINT control_id(HWND h)
{
	return (UINT)((LONG (WINAPI *)(HWND, int))USER32(GetWindowLongW))(
		h, GWL_ID);
}

static void command(HWND h, UINT code)
{
	HWND parent = comctl_parent(h);
	if (parent)
		comctl_send(parent, WM_COMMAND,
			    MAKEWPARAM(control_id(h), code), (LPARAM)h);
}

static void to_item(const struct cbe_item *t, int i, COMBOBOXEXITEMW *out)
{
	memset(out, 0, sizeof(*out));
	out->mask = CBEIF_TEXT | CBEIF_IMAGE | CBEIF_SELECTEDIMAGE |
		    CBEIF_OVERLAY | CBEIF_INDENT | CBEIF_LPARAM;
	out->iItem = i;
	out->pszText = t->text;
	out->iImage = t->image;
	out->iSelectedImage = t->selimage;
	out->iOverlay = t->overlay;
	out->iIndent = t->indent;
	out->lParam = t->lParam;
}

static void notify_item(HWND h, UINT code, const struct cbe_item *t, int i)
{
	NMCOMBOBOXEXW nm;
	memset(&nm, 0, sizeof(nm));
	to_item(t, i, &nm.ceItem);
	comctl_notify(h, code, &nm.hdr);
}

/* Item i's text, asking the parent when it is a callback, with
 * CBEN_GETDISPINFOA or W as the parent's notification format is (the
 * result is the caller's to free). The handler runs synchronously and may
 * change the items or destroy the control: callers check comctl_alive and
 * find their item again afterwards. */
static WCHAR *text_of(HWND h, struct comboex *s, int i)
{
	const struct cbe_item *t = &s->it[i];
	if (!t->callback)
		return comctl_w_dup(t->text ? t->text : u"");
	unsigned uid = t->uid;
	WCHAR *got;
	UINT mask;
	if (s->unicode) {
		WCHAR buf[260];
		NMCOMBOBOXEXW nm;
		memset(&nm, 0, sizeof(nm));
		buf[0] = 0;
		nm.ceItem.mask = CBEIF_TEXT;
		nm.ceItem.iItem = i;
		nm.ceItem.pszText = buf;
		nm.ceItem.cchTextMax = 260;
		nm.ceItem.lParam = t->lParam;
		comctl_notify(h, CBEN_GETDISPINFOW, &nm.hdr);
		buf[259] = 0;
		got = comctl_w_dup(nm.ceItem.pszText ? nm.ceItem.pszText : u"");
		mask = nm.ceItem.mask;
	} else {
		char buf[260];
		NMCOMBOBOXEXA nm;
		memset(&nm, 0, sizeof(nm));
		buf[0] = 0;
		nm.ceItem.mask = CBEIF_TEXT;
		nm.ceItem.iItem = i;
		nm.ceItem.pszText = buf;
		nm.ceItem.cchTextMax = 260;
		nm.ceItem.lParam = t->lParam;
		comctl_notify(h, CBEN_GETDISPINFOA, &nm.hdr);
		buf[259] = 0;
		got = comctl_a_to_w(nm.ceItem.pszText ? nm.ceItem.pszText : "");
		mask = nm.ceItem.mask;
	}
	if ((mask & CBEIF_DI_SETITEM) && comctl_alive(h, s) &&
	    (i = find_uid(s, uid)) >= 0) {
		free(s->it[i].text);
		s->it[i].text = comctl_w_dup(got ? got : u"");
		s->it[i].callback = FALSE;
	}
	return got;
}

static void set_fields(struct cbe_item *t, const COMBOBOXEXITEMW *in,
		       int wide)
{
	UINT m = in->mask;

	if (m & CBEIF_TEXT) {
		free(t->text);
		t->text = NULL;
		t->callback = in->pszText == LPSTR_TEXTCALLBACKW;
		if (in->pszText && !t->callback)
			t->text = wide ? comctl_w_dup(in->pszText) :
					 comctl_a_to_w((const char *)in->pszText);
	}
	if (m & CBEIF_IMAGE)
		t->image = in->iImage;
	if (m & CBEIF_SELECTEDIMAGE)
		t->selimage = in->iSelectedImage;
	if (m & CBEIF_OVERLAY)
		t->overlay = in->iOverlay;
	if (m & CBEIF_INDENT)
		t->indent = in->iIndent;
	if (m & CBEIF_LPARAM)
		t->lParam = in->lParam;
}

static LRESULT insert(HWND h, struct comboex *s, const COMBOBOXEXITEMW *in,
		      int wide)
{
	if (!in)
		return -1;
	int at = (int)in->iItem;
	if (at < 0 || at > s->n)
		at = s->n;
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 8;
		void *p = realloc(s->it, (size_t)cap * sizeof(*s->it));
		if (!p)
			return -1;
		s->it = p;
		s->cap = cap;
	}
	memmove(&s->it[at + 1], &s->it[at], (size_t)(s->n - at) * sizeof(*s->it));
	struct cbe_item *t = &s->it[at];
	memset(t, 0, sizeof(*t));
	set_fields(t, in, wide);
	t->uid = ++s->next_uid;
	s->n++;
	if (s->sel >= at)
		s->sel++;
	notify_item(h, CBEN_INSERTITEM, t, at);
	return at;
}

static LRESULT delete(HWND h, struct comboex *s, int i)
{
	if (i < 0 || i >= s->n)
		return CB_ERR;
	/* The item leaves the list before CBEN_DELETEITEM, so a handler
	 * that edits the list (even CB_RESETCONTENT) cannot free it twice;
	 * the notification describes the copy, freed afterwards. */
	struct cbe_item gone = s->it[i];
	memmove(&s->it[i], &s->it[i + 1], (size_t)(s->n - i - 1) * sizeof(*s->it));
	s->n--;
	if (s->sel == i)
		s->sel = -1;
	else if (s->sel > i)
		s->sel--;
	LRESULT left = s->n;
	notify_item(h, CBEN_DELETEITEM, &gone, i);
	free(gone.text);
	return comctl_alive(h, s) ? s->n : left;
}

static BOOL get_fields(HWND h, struct comboex *s, COMBOBOXEXITEMW *out,
		       int wide)
{
	if (!out)
		return FALSE;
	int i = (int)out->iItem;
	if (i == -1)
		i = s->sel;	/* the edit box: what is selected */
	if (i < 0 || i >= s->n)
		return FALSE;
	UINT m = out->mask;
	if ((m & CBEIF_TEXT) && out->pszText) {
		unsigned uid = s->it[i].uid;
		WCHAR *text = text_of(h, s, i);
		comctl_copy_text_out(text ? text : u"", out->pszText,
				     out->cchTextMax, wide);
		free(text);
		if (!comctl_alive(h, s) || (i = find_uid(s, uid)) < 0)
			return FALSE;
	}
	const struct cbe_item *t = &s->it[i];
	if (m & CBEIF_IMAGE)
		out->iImage = t->image;
	if (m & CBEIF_SELECTEDIMAGE)
		out->iSelectedImage = t->selimage;
	if (m & CBEIF_OVERLAY)
		out->iOverlay = t->overlay;
	if (m & CBEIF_INDENT)
		out->iIndent = t->indent;
	if (m & CBEIF_LPARAM)
		out->lParam = t->lParam;
	return TRUE;
}

static int fold(WCHAR c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static BOOL same(const WCHAR *a, const WCHAR *b, BOOL prefix, BOOL cs)
{
	for (; *b; a++, b++)
		if (!*a || (cs ? *a != *b : fold(*a) != fold(*b)))
			return FALSE;
	return prefix || !*a;
}

static LRESULT find(HWND h, struct comboex *s, int start, const WCHAR *what,
		    BOOL prefix)
{
	BOOL cs = (s->ex_style & CBES_EX_CASESENSITIVE) != 0;
	if (!what || !s->n)
		return CB_ERR;
	int begin = start < 0 || start >= s->n ? 0 : start + 1;
	for (int k = 0; k < s->n; k++) {
		int i = (begin + k) % s->n;
		unsigned uid = s->it[i].uid;
		WCHAR *t = text_of(h, s, i);
		BOOL hit = t && same(t, what, prefix, cs);
		free(t);
		if (!comctl_alive(h, s))
			return CB_ERR;
		if (hit)
			return find_uid(s, uid) >= 0 ? find_uid(s, uid) : CB_ERR;
		if (!s->n)
			return CB_ERR;
	}
	return CB_ERR;
}

LRESULT STUB_WINAPI comctl_comboex_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct comboex *s = comctl_state(h);
	int wide = 1;

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->sel = -1;
		s->unicode = TRUE;
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case WM_CREATE: {
		/* The parent's WM_NOTIFYFORMAT handler may destroy the
		 * control: then the creation fails. */
		BOOL unicode = comctl_notify_unicode(h);
		if (!comctl_alive(h, s))
			return -1;
		s->unicode = unicode;
		break;
	}
	case WM_NOTIFYFORMAT:
		if (lp == NF_REQUERY) {
			BOOL unicode = comctl_notify_unicode(h);
			if (comctl_alive(h, s))
				s->unicode = unicode;
			return unicode ? NFR_UNICODE : NFR_ANSI;
		}
		break;
	case CBEM_INSERTITEMA:
		wide = 0;
		/* fall through */
	case CBEM_INSERTITEMW:
		return insert(h, s, (const COMBOBOXEXITEMW *)lp, wide);
	case CBEM_DELETEITEM:	/* CB_DELETESTRING */
		return delete(h, s, (int)wp);
	case CBEM_GETITEMA:
		wide = 0;
		/* fall through */
	case CBEM_GETITEMW:
		return get_fields(h, s, (COMBOBOXEXITEMW *)lp, wide);
	case CBEM_SETITEMA:
		wide = 0;
		/* fall through */
	case CBEM_SETITEMW: {
		const COMBOBOXEXITEMW *in = (const COMBOBOXEXITEMW *)lp;
		if (!in)
			return FALSE;
		int i = in->iItem == -1 ? s->sel : (int)in->iItem;
		if (i < 0 || i >= s->n)
			return FALSE;
		set_fields(&s->it[i], in, wide);
		return TRUE;
	}
	case CBEM_SETIMAGELIST: {
		HIMAGELIST old = s->himl;
		s->himl = (HIMAGELIST)lp;
		return (LRESULT)old;
	}
	case CBEM_GETIMAGELIST:
		return (LRESULT)s->himl;
	case CBEM_GETCOMBOCONTROL:
		return (LRESULT)h;
	case CBEM_GETEDITCONTROL:
		return 0;
	case CBEM_SETEXSTYLE:
	case CBEM_SETEXTENDEDSTYLE: {
		DWORD old = s->ex_style;
		s->ex_style = wp ? (old & ~(DWORD)wp) | ((DWORD)lp & (DWORD)wp)
				 : (DWORD)lp;
		return old;
	}
	case CBEM_GETEXTENDEDSTYLE:
		return s->ex_style;
	case CBEM_HASEDITCHANGED:
		return FALSE;
	case CBEM_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case CBEM_GETUNICODEFORMAT:
		return s->unicode;
	case CB_GETCOUNT:
		return s->n;
	case CB_GETCURSEL:
		return s->sel;
	case CB_SETCURSEL:
		if ((int)wp < 0 || (int)wp >= s->n) {
			s->sel = -1;
			return CB_ERR;
		}
		s->sel = (int)wp;
		return s->sel;
	case CB_GETLBTEXT:
	case CB_GETLBTEXTLEN: {
		int i = (int)wp;
		if (i < 0 || i >= s->n)
			return CB_ERR;
		WCHAR *t = text_of(h, s, i);
		int len = t ? (int)wstrlen(t) : 0;
		if (msg == CB_GETLBTEXT && lp)
			memcpy((void *)lp, t ? t : u"",
			       (size_t)(len + 1) * sizeof(WCHAR));
		free(t);
		return len;
	}
	case WIN_LBTEXT_SNAPSHOT: {
		struct win_lbtext *q = (struct win_lbtext *)lp;
		int i = (int)wp;
		if (!q)
			return 0;
		q->done = 1;
		q->text = NULL;
		if (i < 0 || i >= s->n) {
			q->result = CB_ERR;
			return 0;
		}
		q->text = text_of(h, s, i);
		q->result = q->text ? (LRESULT)wstrlen(q->text) : 0;
		return 0;
	}
	case CB_GETITEMDATA:
		if ((int)wp < 0 || (int)wp >= s->n)
			return CB_ERR;
		return s->it[wp].lParam;
	case CB_SETITEMDATA:
		if ((int)wp < 0 || (int)wp >= s->n)
			return CB_ERR;
		s->it[wp].lParam = lp;
		return TRUE;
	case CB_RESETCONTENT:
		/* A CBEN_DELETEITEM handler may destroy the control. */
		while (comctl_alive(h, s) && s->n)
			delete(h, s, s->n - 1);
		return 0;
	case CB_FINDSTRING:
	case CB_FINDSTRINGEXACT:
		return find(h, s, (int)wp, (const WCHAR *)lp,
			    msg == CB_FINDSTRING);
	case CB_ADDSTRING:
	case CB_INSERTSTRING:
		return CB_ERR;	/* ComboBoxEx takes CBEM_INSERTITEM only */
	case CB_SHOWDROPDOWN:
		if ((BOOL)wp != s->dropped) {
			s->dropped = (BOOL)wp;
			command(h, s->dropped ? CBN_DROPDOWN : CBN_CLOSEUP);
		}
		return TRUE;
	case CB_GETDROPPEDSTATE:
		return s->dropped;
	case WM_KEYDOWN:
		/* Up and down arrows move the selection, as a user would. */
		if ((wp == 0x26 && s->sel > 0) ||
		    (wp == 0x28 && s->sel < s->n - 1)) {
			s->sel += wp == 0x26 ? -1 : 1;
			command(h, CBN_SELCHANGE);
		}
		return 0;
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		for (int i = 0; i < s->n; i++)
			free(s->it[i].text);
		free(s->it);
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}
