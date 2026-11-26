/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's tree view (SysTreeView32), headless. An HTREEITEM is the
 * address of the control's node for it, as on Windows. The control keeps
 * the tree (text, images, state, lParam, children count, integral
 * height), the caret, drop target and first visible item, its image
 * lists, colours, indent and item height, and answers Windows' messages
 * from them, with the TVN_ notifications: DELETEITEM, SELCHANGING (which
 * can refuse) and SELCHANGED, ITEMEXPANDING (which can refuse) and
 * ITEMEXPANDED (only until an item has been expanded once, as on
 * Windows), and GETDISPINFO for what is left to the parent.
 *
 * Nothing is drawn: visible items are rows ITEM_HEIGHT high from the top
 * of the client area, indented by the indent per level. There is no label
 * editing, dragging or scrolling.
 */
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define ITEM_HEIGHT   16
#define INDENT        19
#define DISPINFO_MAX  1024

struct tv_node {
	struct tv_node *parent, *first, *last, *next, *prev;
	WCHAR *text;
	BOOL callback;
	UINT state;
	int image, selimage, expimage, children, integral;
	BOOL children_set;
	LPARAM lParam;
};

struct treeview {
	struct tv_node root;
	struct tv_node *caret, *drop, *top;
	int count;
	HIMAGELIST himl[3];
	COLORREF bk, text, line, mark_color;
	int indent, height;
	UINT scroll_time;
	DWORD exstyle;
	HWND tooltips;
	HFONT font;
	BOOL unicode, deleting;
};

static WCHAR *text_in(const void *p, int wide)
{
	return wide ? comctl_w_dup(p) : comctl_a_to_w(p);
}

static WCHAR fold(WCHAR c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		return c + 0x20;
	return c;
}

static int text_cmp(const WCHAR *a, const WCHAR *b)
{
	if (!a)
		a = u"";
	if (!b)
		b = u"";
	for (;; a++, b++) {
		WCHAR x = fold(*a), y = fold(*b);
		if (x != y)
			return x < y ? -1 : 1;
		if (!x)
			return 0;
	}
}

/* The next node after n in a walk of the whole tree (pre-order). */
static struct tv_node *walk_next(const struct treeview *s, struct tv_node *n)
{
	if (n->first)
		return n->first;
	while (n && n != &s->root) {
		if (n->next)
			return n->next;
		n = n->parent;
	}
	return NULL;
}

/* The node a handle names, or NULL for one that isn't the control's. */
static struct tv_node *node(struct treeview *s, HTREEITEM h)
{
	if (!h)
		return NULL;
	if (h == TVI_ROOT)
		return &s->root;
	for (struct tv_node *n = s->root.first; n; n = walk_next(s, n))
		if ((HTREEITEM)n == h)
			return n;
	return NULL;
}

static HTREEITEM handle(const struct treeview *s, const struct tv_node *n)
{
	return n && n != &s->root ? (HTREEITEM)n : NULL;
}

static BOOL expanded(const struct tv_node *n)
{
	return (n->state & TVIS_EXPANDED) != 0;
}

/* Whether every parent of n is expanded. */
static BOOL visible(const struct treeview *s, const struct tv_node *n)
{
	for (n = n->parent; n && n != &s->root; n = n->parent)
		if (!expanded(n))
			return FALSE;
	return TRUE;
}

static struct tv_node *next_visible(const struct treeview *s,
				    struct tv_node *n)
{
	if (n->first && expanded(n))
		return n->first;
	while (n && n != &s->root) {
		if (n->next)
			return n->next;
		n = n->parent;
	}
	return NULL;
}

static struct tv_node *last_descendant(struct tv_node *n)
{
	while (n->last && expanded(n))
		n = n->last;
	return n;
}

static struct tv_node *prev_visible(const struct treeview *s,
				    struct tv_node *n)
{
	if (n->prev)
		return last_descendant(n->prev);
	return n->parent != &s->root ? n->parent : NULL;
}

static int depth(const struct treeview *s, const struct tv_node *n)
{
	int d = 0;

	for (n = n->parent; n && n != &s->root; n = n->parent)
		d++;
	return d;
}

/* The row of visible item n from the first visible one, or -1. */
static int row_of(const struct treeview *s, const struct tv_node *n)
{
	int r = 0;

	if (!visible(s, n))
		return -1;
	for (struct tv_node *m = s->top; m; m = next_visible(s, m), r++)
		if (m == n)
			return r;
	return -1;
}

static int has_children(const struct tv_node *n)
{
	if (n->children_set)
		return n->children;
	return n->first != NULL;
}

/* ---- notifications ---- */

static void fill_item(const struct treeview *s, const struct tv_node *n,
		      TVITEMW *it)
{
	memset(it, 0, sizeof(*it));
	if (!n || n == &s->root)
		return;
	it->mask = TVIF_HANDLE | TVIF_STATE | TVIF_PARAM | TVIF_IMAGE |
		   TVIF_SELECTEDIMAGE;
	it->hItem = (HTREEITEM)n;
	it->state = n->state;
	it->stateMask = 0xffff;
	it->lParam = n->lParam;
	it->iImage = n->image;
	it->iSelectedImage = n->selimage;
}

static LRESULT notify_tv(HWND h, struct treeview *s, UINT code_a,
			 UINT code_w, UINT action, const struct tv_node *old,
			 const struct tv_node *new)
{
	NMTREEVIEWW nm;

	memset(&nm, 0, sizeof(nm));
	nm.action = action;
	fill_item(s, old, &nm.itemOld);
	fill_item(s, new, &nm.itemNew);
	return comctl_notify(h, comctl_notify_unicode(h) ? code_w : code_a,
			     &nm.hdr);
}

/* Asks the parent (TVN_GETDISPINFO) for what of n is left to it. */
static void dispinfo(HWND h, struct treeview *s, struct tv_node *n,
		     UINT need, TVITEMW *out, int wide)
{
	BOOL uni = comctl_notify_unicode(h);
	NMTVDISPINFOW nm;
	WCHAR wbuf[DISPINFO_MAX];

	memset(&nm, 0, sizeof(nm));
	nm.item.mask = need;
	nm.item.hItem = (HTREEITEM)n;
	nm.item.state = n->state;
	nm.item.lParam = n->lParam;
	if (need & TVIF_TEXT) {
		if (uni == wide && out->pszText && out->cchTextMax > 0) {
			nm.item.pszText = out->pszText;
			nm.item.cchTextMax = out->cchTextMax;
			if (wide)
				out->pszText[0] = 0;
			else
				((char *)out->pszText)[0] = 0;
		} else {
			wbuf[0] = 0;
			nm.item.pszText = wbuf;
			nm.item.cchTextMax = uni ? DISPINFO_MAX :
					     (int)sizeof(wbuf);
		}
	}
	comctl_notify(h, uni ? TVN_GETDISPINFOW : TVN_GETDISPINFOA, &nm.hdr);
	if ((need & TVIF_TEXT) && nm.item.pszText != out->pszText) {
		WCHAR *t = NULL;
		if (nm.item.pszText && nm.item.pszText != LPSTR_TEXTCALLBACKW)
			t = uni ? comctl_w_dup(nm.item.pszText) :
			    comctl_a_to_w((const char *)nm.item.pszText);
		comctl_copy_text_out(t, out->pszText, out->cchTextMax, wide);
		free(t);
	}
	if (need & TVIF_IMAGE)
		out->iImage = nm.item.iImage;
	if (need & TVIF_SELECTEDIMAGE)
		out->iSelectedImage = nm.item.iSelectedImage;
	if (need & TVIF_CHILDREN)
		out->cChildren = nm.item.cChildren;
}

/* ---- the tree ---- */

static void set_text(struct tv_node *n, const void *p, int wide)
{
	free(n->text);
	n->text = NULL;
	n->callback = p == (const void *)LPSTR_TEXTCALLBACKW;
	if (p && !n->callback)
		n->text = text_in(p, wide);
}

/* What a TVITEM(EX) says, into n. */
static void set_fields(struct tv_node *n, const TVITEMEXW *in, int wide)
{
	UINT m = in->mask;

	if (m & TVIF_TEXT)
		set_text(n, in->pszText, wide);
	if (m & TVIF_IMAGE)
		n->image = in->iImage;
	if (m & TVIF_SELECTEDIMAGE)
		n->selimage = in->iSelectedImage;
	if (m & TVIF_PARAM)
		n->lParam = in->lParam;
	if (m & TVIF_STATE)
		n->state = (n->state & ~in->stateMask) |
			   (in->state & in->stateMask);
	if (m & TVIF_CHILDREN) {
		n->children = in->cChildren;
		n->children_set = TRUE;
	}
	if (m & TVIF_INTEGRAL)
		n->integral = in->iIntegral;
	if (m & TVIF_EXPANDEDIMAGE)
		n->expimage = in->iExpandedImage;
}

static void link_after(struct tv_node *parent, struct tv_node *after,
		       struct tv_node *n)
{
	n->parent = parent;
	n->prev = after;
	n->next = after ? after->next : parent->first;
	if (n->next)
		n->next->prev = n;
	else
		parent->last = n;
	if (after)
		after->next = n;
	else
		parent->first = n;
}

static void unlink_node(struct tv_node *n)
{
	struct tv_node *p = n->parent;

	if (n->prev)
		n->prev->next = n->next;
	else
		p->first = n->next;
	if (n->next)
		n->next->prev = n->prev;
	else
		p->last = n->prev;
	n->next = n->prev = NULL;
}

static HTREEITEM insert(HWND h, struct treeview *s,
			const TVINSERTSTRUCTW *in, int wide)
{
	if (!in)
		return NULL;
	struct tv_node *parent = in->hParent && in->hParent != TVI_ROOT ?
				 node(s, in->hParent) : &s->root;
	if (!parent)
		return NULL;
	struct tv_node *n = calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	TVITEMEXW item;
	memset(&item, 0, sizeof(item));
	/* (a plain TVITEM has no iIntegral and the rest after it) */
	memcpy(&item, &in->item, sizeof(in->item));
	if (in->itemex.mask & (TVIF_INTEGRAL | TVIF_EXPANDEDIMAGE)) {
		item.iIntegral = in->itemex.iIntegral;
		item.iExpandedImage = in->itemex.iExpandedImage;
	}
	n->integral = 1;
	set_fields(n, &item, wide);
	if (!(item.mask & TVIF_INTEGRAL) || n->integral < 1)
		n->integral = 1;
	struct tv_node *after;
	HTREEITEM where = in->hInsertAfter;
	if (where == TVI_FIRST) {
		after = NULL;
	} else if (where == TVI_SORT && !n->callback) {
		after = NULL;
		for (struct tv_node *c = parent->first; c; c = c->next) {
			if (!c->callback && text_cmp(c->text, n->text) > 0)
				break;
			after = c;
		}
	} else {
		after = parent->last;
		struct tv_node *a = where != TVI_LAST && where != TVI_SORT ?
				    node(s, where) : NULL;
		if (a && a->parent == parent)
			after = a;
	}
	link_after(parent, after, n);
	s->count++;
	/* (nothing scrolls: the first visible item stays the first) */
	if (!s->top || (parent == &s->root && n->next == s->top))
		s->top = n;
	return (HTREEITEM)n;
}

static void delete_node(HWND h, struct treeview *s, struct tv_node *n);

static void delete_children(HWND h, struct treeview *s, struct tv_node *n)
{
	while (n->first)
		delete_node(h, s, n->first);
}

static void delete_node(HWND h, struct treeview *s, struct tv_node *n)
{
	delete_children(h, s, n);
	notify_tv(h, s, TVN_DELETEITEMA, TVN_DELETEITEMW, 0, n, NULL);
	if (s->top == n)
		s->top = next_visible(s, n) ? next_visible(s, n) :
			 prev_visible(s, n);
	if (s->top == n)
		s->top = NULL;
	unlink_node(n);
	if (s->caret == n)
		s->caret = NULL;
	if (s->drop == n)
		s->drop = NULL;
	free(n->text);
	free(n);
	s->count--;
}

static BOOL get_item(HWND h, struct treeview *s, TVITEMW *out, int wide)
{
	struct tv_node *n = out->hItem ? node(s, out->hItem) : NULL;
	UINT need = 0;

	if (!n || n == &s->root)
		return FALSE;
	if (out->mask & TVIF_TEXT) {
		if (n->callback)
			need |= TVIF_TEXT;
		else
			comctl_copy_text_out(n->text, out->pszText,
					     out->cchTextMax, wide);
	}
	if (out->mask & TVIF_IMAGE) {
		out->iImage = n->image;
		if (n->image == I_IMAGECALLBACK)
			need |= TVIF_IMAGE;
	}
	if (out->mask & TVIF_SELECTEDIMAGE) {
		out->iSelectedImage = n->selimage;
		if (n->selimage == I_IMAGECALLBACK)
			need |= TVIF_SELECTEDIMAGE;
	}
	if (out->mask & TVIF_PARAM)
		out->lParam = n->lParam;
	if (out->mask & TVIF_STATE)
		out->state = n->state & out->stateMask;
	if (out->mask & TVIF_CHILDREN) {
		out->cChildren = has_children(n);
		if (n->children_set && n->children == I_CHILDRENCALLBACK)
			need |= TVIF_CHILDREN;
	}
	if (out->mask & TVIF_INTEGRAL)
		((TVITEMEXW *)out)->iIntegral = n->integral;
	if (out->mask & TVIF_EXPANDEDIMAGE)
		((TVITEMEXW *)out)->iExpandedImage = n->expimage;
	if (need)
		dispinfo(h, s, n, need, out, wide);
	return TRUE;
}

static BOOL set_item(struct treeview *s, const TVITEMW *in, int wide)
{
	struct tv_node *n = node(s, in->hItem);

	if (!n || n == &s->root)
		return FALSE;
	TVITEMEXW item;
	memset(&item, 0, sizeof(item));
	memcpy(&item, in, sizeof(*in));
	if (in->mask & (TVIF_INTEGRAL | TVIF_EXPANDEDIMAGE)) {
		item.iIntegral = ((const TVITEMEXW *)in)->iIntegral;
		item.iExpandedImage = ((const TVITEMEXW *)in)->iExpandedImage;
	}
	set_fields(n, &item, wide);
	if (n->integral < 1)
		n->integral = 1;
	return TRUE;
}

/* Makes n the caret (TVGN_CARET), if SELCHANGING lets it. */
static BOOL select_caret(HWND h, struct treeview *s, struct tv_node *n,
			 UINT how);

static BOOL expand(HWND h, struct treeview *s, struct tv_node *n, UINT code)
{
	if (!n || n == &s->root)
		return FALSE;
	UINT action = code & 3;
	BOOL open = expanded(n);
	if (action == TVE_TOGGLE)
		action = open ? TVE_COLLAPSE : TVE_EXPAND;
	if (action == TVE_EXPAND) {
		if (!has_children(n))
			return FALSE;
		if (open)
			return TRUE;
		BOOL tell = !(n->state & TVIS_EXPANDEDONCE);
		if (tell && notify_tv(h, s, TVN_ITEMEXPANDINGA,
				      TVN_ITEMEXPANDINGW, TVE_EXPAND, NULL, n))
			return FALSE;
		n->state |= TVIS_EXPANDED | TVIS_EXPANDEDONCE;
		if (tell)
			notify_tv(h, s, TVN_ITEMEXPANDEDA, TVN_ITEMEXPANDEDW,
				  TVE_EXPAND, NULL, n);
		return TRUE;
	}
	if (action != TVE_COLLAPSE)
		return FALSE;
	if (!open && !(code & TVE_COLLAPSERESET))
		return !n->first ? FALSE : TRUE;
	BOOL tell = !(n->state & TVIS_EXPANDEDONCE) ||
		    (code & TVE_COLLAPSERESET);
	if (open && tell && notify_tv(h, s, TVN_ITEMEXPANDINGA,
				      TVN_ITEMEXPANDINGW, TVE_COLLAPSE, NULL,
				      n))
		return FALSE;
	n->state &= ~TVIS_EXPANDED;
	/* A caret under n moves to n. */
	for (struct tv_node *c = s->caret; c; c = c->parent)
		if (c->parent == n) {
			select_caret(h, s, n, TVC_UNKNOWN);
			break;
		}
	if (s->top && !visible(s, s->top))
		s->top = n;
	if (open && tell)
		notify_tv(h, s, TVN_ITEMEXPANDEDA, TVN_ITEMEXPANDEDW,
			  TVE_COLLAPSE, NULL, n);
	if (code & TVE_COLLAPSERESET) {
		n->state &= ~TVIS_EXPANDEDONCE;
		delete_children(h, s, n);
	}
	return TRUE;
}

static void show(HWND h, struct treeview *s, struct tv_node *n)
{
	for (struct tv_node *p = n->parent; p && p != &s->root; p = p->parent)
		if (!expanded(p))
			expand(h, s, p, TVE_EXPAND);
}

static BOOL select_caret(HWND h, struct treeview *s, struct tv_node *n,
			 UINT how)
{
	struct tv_node *old = s->caret;

	if (n == old)
		return TRUE;
	if (notify_tv(h, s, TVN_SELCHANGINGA, TVN_SELCHANGINGW, how, old, n))
		return FALSE;
	if (old)
		old->state &= ~TVIS_SELECTED;
	s->caret = n;
	if (n) {
		n->state |= TVIS_SELECTED;
		show(h, s, n);
	}
	notify_tv(h, s, TVN_SELCHANGEDA, TVN_SELCHANGEDW, how, old, n);
	return TRUE;
}

static HTREEITEM next_item(struct treeview *s, UINT code, HTREEITEM from)
{
	struct tv_node *n = from && code != TVGN_ROOT && code != TVGN_CARET &&
			    code != TVGN_DROPHILITE &&
			    code != TVGN_FIRSTVISIBLE &&
			    code != TVGN_LASTVISIBLE ? node(s, from) : NULL;

	switch (code) {
	case TVGN_ROOT:
		return handle(s, s->root.first);
	case TVGN_CARET:
		return handle(s, s->caret);
	case TVGN_DROPHILITE:
		return handle(s, s->drop);
	case TVGN_FIRSTVISIBLE:
		return handle(s, s->top);
	case TVGN_LASTVISIBLE:
		return s->root.last ?
		       handle(s, last_descendant(s->root.last)) : NULL;
	case TVGN_CHILD:
		if (!from || from == TVI_ROOT)
			return handle(s, s->root.first);
		return n ? handle(s, n->first) : NULL;
	}
	if (!n || n == &s->root)
		return NULL;
	switch (code) {
	case TVGN_NEXT:
		return handle(s, n->next);
	case TVGN_PREVIOUS:
		return handle(s, n->prev);
	case TVGN_PARENT:
		return handle(s, n->parent);
	case TVGN_NEXTVISIBLE:
		return visible(s, n) ? handle(s, next_visible(s, n)) : NULL;
	case TVGN_PREVIOUSVISIBLE:
		return visible(s, n) ? handle(s, prev_visible(s, n)) : NULL;
	}
	return NULL;
}

struct sort_ctx {
	PFNTVCOMPARE fn;
	LPARAM param;
};

static int sort_cmp(const struct sort_ctx *c, struct tv_node *a,
		    struct tv_node *b)
{
	if (c->fn)
		return c->fn(a->lParam, b->lParam, c->param);
	return text_cmp(a->text, b->text);
}

/* A stable insertion sort of n's children, then theirs if recurse. */
static BOOL sort_children(struct treeview *s, struct tv_node *n,
			  const struct sort_ctx *c, BOOL recurse)
{
	if (!n)
		return FALSE;
	struct tv_node *list = n->first;
	n->first = n->last = NULL;
	while (list) {
		struct tv_node *x = list, *after = n->last;
		list = list->next;
		while (after && sort_cmp(c, after, x) > 0)
			after = after->prev;
		link_after(n, after, x);
	}
	if (recurse)
		for (struct tv_node *k = n->first; k; k = k->next)
			sort_children(s, k, c, TRUE);
	return TRUE;
}

static int visible_count(HWND h, const struct treeview *s)
{
	RECT rc;

	comctl_client_rect(h, &rc);
	return rc.bottom / s->height;
}

static BOOL item_rect(HWND h, struct treeview *s, struct tv_node *n,
		      BOOL text_only, RECT *rc)
{
	if (!n || n == &s->root)
		return FALSE;
	int r = row_of(s, n);
	if (r < 0)
		return FALSE;
	RECT c;
	comctl_client_rect(h, &c);
	int y = 0;
	for (struct tv_node *m = s->top; m && m != n; m = next_visible(s, m))
		y += s->height * m->integral;
	rc->top = y;
	rc->bottom = y + s->height * n->integral;
	if (!text_only) {
		rc->left = 0;
		rc->right = c.right;
		return TRUE;
	}
	DWORD style = comctl_style(h);
	int levels = depth(s, n) +
		     ((style & TVS_LINESATROOT) &&
		      (style & (TVS_HASLINES | TVS_HASBUTTONS)) ? 1 : 0);
	int x = levels * s->indent;
	if (s->himl[TVSIL_STATE])
		x += 16;
	if (s->himl[TVSIL_NORMAL])
		x += 16 + 3;
	rc->left = x;
	rc->right = x + comctl_text_width(n->text) + 4;
	return TRUE;
}

static HTREEITEM hit_test(HWND h, struct treeview *s, TVHITTESTINFO *ht)
{
	RECT c, rc;

	comctl_client_rect(h, &c);
	ht->flags = 0;
	ht->hItem = NULL;
	if (ht->pt.y < 0)
		ht->flags |= TVHT_ABOVE;
	else if (ht->pt.y >= c.bottom)
		ht->flags |= TVHT_BELOW;
	if (ht->pt.x < 0)
		ht->flags |= TVHT_TOLEFT;
	else if (ht->pt.x >= c.right)
		ht->flags |= TVHT_TORIGHT;
	if (ht->flags)
		return NULL;
	for (struct tv_node *m = s->top; m; m = next_visible(s, m)) {
		item_rect(h, s, m, FALSE, &rc);
		if (ht->pt.y < rc.top || ht->pt.y >= rc.bottom)
			continue;
		RECT t;
		item_rect(h, s, m, TRUE, &t);
		int icon = s->himl[TVSIL_NORMAL] ? 16 + 3 : 0;
		int state = s->himl[TVSIL_STATE] ? 16 : 0;
		int base = t.left - icon - state;
		if (ht->pt.x >= t.right)
			ht->flags = TVHT_ONITEMRIGHT;
		else if (ht->pt.x >= t.left)
			ht->flags = TVHT_ONITEMLABEL;
		else if (ht->pt.x >= base + state)
			ht->flags = TVHT_ONITEMICON;
		else if (ht->pt.x >= base)
			ht->flags = TVHT_ONITEMSTATEICON;
		else if (ht->pt.x >= base - s->indent && has_children(m) &&
			 (comctl_style(h) & TVS_HASBUTTONS))
			ht->flags = TVHT_ONITEMBUTTON;
		else
			ht->flags = TVHT_ONITEMINDENT;
		ht->hItem = (HTREEITEM)m;
		return ht->hItem;
	}
	ht->flags = TVHT_NOWHERE;
	return NULL;
}

LRESULT STUB_WINAPI comctl_treeview_proc(HWND h, UINT msg, WPARAM wp,
					 LPARAM lp)
{
	struct treeview *s = comctl_state(h);
	int wide = 1;
	struct tv_node *n;

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->root.state = TVIS_EXPANDED;
		s->bk = s->text = CLR_NONE;
		s->line = CLR_DEFAULT;
		s->indent = INDENT;
		s->height = ITEM_HEIGHT;
		s->unicode = TRUE;
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case TVM_INSERTITEMA:
		wide = 0;
		/* fall through */
	case TVM_INSERTITEMW:
		return (LRESULT)insert(h, s, (const TVINSERTSTRUCTW *)lp, wide);
	case TVM_DELETEITEM:
		if (!lp || (HTREEITEM)lp == TVI_ROOT) {
			delete_children(h, s, &s->root);
			return TRUE;
		}
		if (!(n = node(s, (HTREEITEM)lp)))
			return FALSE;
		delete_node(h, s, n);
		return TRUE;
	case TVM_GETITEMA:
		wide = 0;
		/* fall through */
	case TVM_GETITEMW:
		return lp ? get_item(h, s, (TVITEMW *)lp, wide) : FALSE;
	case TVM_SETITEMA:
		wide = 0;
		/* fall through */
	case TVM_SETITEMW:
		return lp ? set_item(s, (const TVITEMW *)lp, wide) : FALSE;
	case TVM_GETNEXTITEM:
		return (LRESULT)next_item(s, (UINT)wp, (HTREEITEM)lp);
	case TVM_GETCOUNT:
		return s->count;
	case TVM_EXPAND:
		return expand(h, s, node(s, (HTREEITEM)lp), (UINT)wp);
	case TVM_SELECTITEM:
		n = lp ? node(s, (HTREEITEM)lp) : NULL;
		if (lp && (!n || n == &s->root))
			return FALSE;
		switch (wp & 0xff) {
		case TVGN_CARET:
			return select_caret(h, s, n, TVC_UNKNOWN);
		case TVGN_DROPHILITE:
			if (s->drop)
				s->drop->state &= ~TVIS_DROPHILITED;
			s->drop = n;
			if (n)
				n->state |= TVIS_DROPHILITED;
			return TRUE;
		case TVGN_FIRSTVISIBLE:
			if (!n)
				return FALSE;
			show(h, s, n);
			s->top = n;
			return TRUE;
		}
		return FALSE;
	case TVM_ENSUREVISIBLE:
		if (!(n = node(s, (HTREEITEM)lp)) || n == &s->root)
			return FALSE;
		show(h, s, n);
		return TRUE;
	case TVM_GETITEMSTATE:
		n = node(s, (HTREEITEM)wp);
		return n && n != &s->root ? n->state & (UINT)lp : 0;
	case TVM_GETINDENT:
		return s->indent;
	case TVM_SETINDENT:
		s->indent = (int)wp < 0 ? 0 : (int)wp;
		return 0;
	case TVM_GETIMAGELIST:
		return wp < 3 ? (LRESULT)s->himl[wp] : 0;
	case TVM_SETIMAGELIST: {
		if (wp != TVSIL_NORMAL && wp != TVSIL_STATE)
			return 0;
		HIMAGELIST old = s->himl[wp];
		s->himl[wp] = (HIMAGELIST)lp;
		return (LRESULT)old;
	}
	case TVM_GETITEMHEIGHT:
		return s->height;
	case TVM_SETITEMHEIGHT: {
		int old = s->height, v = (short)LOWORD(wp);
		if (v == -1)
			v = ITEM_HEIGHT;
		if (v < 1)
			v = 1;
		if (!(comctl_style(h) & TVS_NONEVENHEIGHT) && v > 1)
			v &= ~1;
		s->height = v;
		return old;
	}
	case TVM_GETVISIBLECOUNT:
		return visible_count(h, s);
	case TVM_GETITEMRECT: {
		RECT *rc = (RECT *)lp;
		if (!rc)
			return FALSE;
		HTREEITEM item;
		memcpy(&item, rc, sizeof(item));
		return item_rect(h, s, node(s, item), (BOOL)wp, rc);
	}
	case TVM_HITTEST:
		return lp ? (LRESULT)hit_test(h, s, (TVHITTESTINFO *)lp) : 0;
	case TVM_SORTCHILDREN: {
		struct sort_ctx c = { NULL, 0 };
		n = !lp || (HTREEITEM)lp == TVI_ROOT ? &s->root :
		    node(s, (HTREEITEM)lp);
		return sort_children(s, n, &c, (BOOL)wp);
	}
	case TVM_SORTCHILDRENCB: {
		const TVSORTCB *cb = (const TVSORTCB *)lp;
		if (!cb || !cb->lpfnCompare)
			return FALSE;
		struct sort_ctx c = { cb->lpfnCompare, cb->lParam };
		n = !cb->hParent || cb->hParent == TVI_ROOT ? &s->root :
		    node(s, cb->hParent);
		return sort_children(s, n, &c, (BOOL)wp);
	}
	case TVM_GETBKCOLOR:
		return s->bk;
	case TVM_SETBKCOLOR: {
		COLORREF old = s->bk;
		s->bk = (COLORREF)lp;
		return old;
	}
	case TVM_GETTEXTCOLOR:
		return s->text;
	case TVM_SETTEXTCOLOR: {
		COLORREF old = s->text;
		s->text = (COLORREF)lp;
		return old;
	}
	case TVM_GETLINECOLOR:
		return s->line;
	case TVM_SETLINECOLOR: {
		COLORREF old = s->line;
		s->line = (COLORREF)lp;
		return old;
	}
	case TVM_GETINSERTMARKCOLOR:
		return s->mark_color;
	case TVM_SETINSERTMARKCOLOR: {
		COLORREF old = s->mark_color;
		s->mark_color = (COLORREF)lp;
		return old;
	}
	case TVM_GETSCROLLTIME:
		return s->scroll_time;
	case TVM_SETSCROLLTIME: {
		UINT old = s->scroll_time;
		s->scroll_time = (UINT)wp;
		return old;
	}
	case TVM_GETEXTENDEDSTYLE:
		return s->exstyle;
	case TVM_SETEXTENDEDSTYLE:
		s->exstyle = (s->exstyle & ~(DWORD)wp) | ((DWORD)lp & (DWORD)wp);
		return S_OK;
	case TVM_GETTOOLTIPS:
		return (LRESULT)s->tooltips;
	case TVM_SETTOOLTIPS: {
		HWND old = s->tooltips;
		s->tooltips = (HWND)wp;
		return (LRESULT)old;
	}
	case TVM_SETINSERTMARK:
		return TRUE;
	case TVM_ENDEDITLABELNOW:
		return FALSE;
	case TVM_EDITLABELA:
	case TVM_EDITLABELW:
	case TVM_GETEDITCONTROL:
	case TVM_CREATEDRAGIMAGE:
	case TVM_GETISEARCHSTRINGA:
	case TVM_GETISEARCHSTRINGW:
		return 0;
	case TVM_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case TVM_GETUNICODEFORMAT:
		return s->unicode;
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		delete_children(h, s, &s->root);
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}
