/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's list view (SysListView32), headless. The control keeps its
 * items (text and image per subitem, state, lParam, indent, position),
 * its columns (mirrored into the header child LVM_GETHEADER gives), its
 * image lists, colours and styles, and answers Windows' messages from
 * them, with the LVN_ notifications: INSERTITEM, DELETEITEM,
 * DELETEALLITEMS, ITEMCHANGING (which can refuse a change) and
 * ITEMCHANGED, and GETDISPINFO for text, images, states and indents left
 * to the parent (LPSTR_TEXTCALLBACK, I_IMAGECALLBACK, the callback mask).
 * A LVS_OWNERDATA list view keeps only its count and states and asks the
 * parent for the rest.
 *
 * Nothing is drawn. Geometry follows a simple model, not Windows' pixels:
 * report rows ROW_HEIGHT high under the header, list columns as wide as
 * LVM_SETCOLUMNWIDTH says, icons on a grid of the item spacing (or where
 * LVM_SETITEMPOSITION put them). There is no label editing, dragging or
 * scrolling (the view never moves).
 */
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define ROW_HEIGHT    17
#define SMALL_ICON    16
#define ICON_CX       75	/* LVM_GETITEMSPACING's defaults */
#define ICON_CY       70
#define SMALL_CX      100
#define LIST_CX       100
#define DISPINFO_MAX  1024

struct lv_sub {
	WCHAR *text;
	BOOL callback;
	int image;
};

struct lv_item {
	struct lv_sub *sub;	/* by subitem number; 0 is the item's */
	int nsub;
	UINT state;
	LPARAM lParam;
	int indent;
	POINT pos;
	BOOL placed;		/* pos set by LVM_SETITEMPOSITION */
};

struct lv_col {
	int fmt, cx, sub, image;
	WCHAR *text;
};

struct listview {
	struct lv_item **item;
	int n, cap;
	int count;		/* LVS_OWNERDATA: the count */
	UINT *odstate;		/* LVS_OWNERDATA: the states */
	struct lv_col *col;
	int ncol;
	HWND header, tooltips;
	HIMAGELIST himl[3];
	COLORREF bk, text, textbk;
	UINT cbmask;
	DWORD exstyle;
	int mark, hot, list_cx;
	DWORD hover;
	SIZE spacing, small_spacing;
	BOOL tile, unicode;
	HFONT font;
	HWND edit;		/* a label being edited, and its item */
	int edit_item;
};

static const WCHAR header_class[] = u"SysHeader32";

static BOOL ownerdata(HWND h)
{
	return (comctl_style(h) & LVS_OWNERDATA) != 0;
}

static int count(HWND h, const struct listview *s)
{
	return ownerdata(h) ? s->count : s->n;
}

static UINT view_of(HWND h, const struct listview *s)
{
	return s->tile ? LV_VIEW_TILE : comctl_style(h) & LVS_TYPEMASK;
}

static WCHAR *text_in(const void *p, int wide)
{
	return wide ? comctl_w_dup(p) : comctl_a_to_w(p);
}

/* Case-insensitive, as lstrcmpi is for the Latin letters. */
static WCHAR fold(WCHAR c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		return c + 0x20;
	return c;
}

static int text_cmp(const WCHAR *a, const WCHAR *b, size_t n)
{
	if (!a)
		a = u"";
	if (!b)
		b = u"";
	for (size_t i = 0; i < n; i++) {
		WCHAR x = fold(a[i]), y = fold(b[i]);
		if (x != y)
			return x < y ? -1 : 1;
		if (!x)
			return 0;
	}
	return 0;
}

static size_t wlen(const WCHAR *s)
{
	size_t n = 0;

	while (s && s[n])
		n++;
	return n;
}

/* Subitem k of item it, made if need be; NULL if out of memory. */
static struct lv_sub *sub_of(struct lv_item *it, int k, BOOL make)
{
	if (k < 0)
		return NULL;
	if (k >= it->nsub) {
		if (!make)
			return NULL;
		struct lv_sub *n = realloc(it->sub, (k + 1) * sizeof(*n));
		if (!n)
			return NULL;
		memset(&n[it->nsub], 0, (k + 1 - it->nsub) * sizeof(*n));
		it->sub = n;
		it->nsub = k + 1;
	}
	return &it->sub[k];
}

static void set_text(struct lv_sub *sb, const void *p, int wide)
{
	free(sb->text);
	sb->text = NULL;
	sb->callback = p == (const void *)LPSTR_TEXTCALLBACKW;
	if (p && !sb->callback)
		sb->text = text_in(p, wide);
}

static void item_free(struct lv_item *it)
{
	for (int k = 0; k < it->nsub; k++)
		free(it->sub[k].text);
	free(it->sub);
	free(it);
}

/* ---- notifications ---- */

static LRESULT notify_lv(HWND h, UINT code, int i, int sub, UINT newst,
			 UINT oldst, UINT changed, LPARAM lParam)
{
	NMLISTVIEW nm;

	memset(&nm, 0, sizeof(nm));
	nm.iItem = i;
	nm.iSubItem = sub;
	nm.uNewState = newst;
	nm.uOldState = oldst;
	nm.uChanged = changed;
	nm.lParam = lParam;
	return comctl_notify(h, code, &nm.hdr);
}

/*
 * Asks the parent (LVN_GETDISPINFO) for what of item i's subitem sub is
 * left to it (need, LVIF_ flags), into out, a caller's LVITEM in the
 * charset wide says.
 */
static void dispinfo(HWND h, struct listview *s, int i, int sub, UINT need,
		     LVITEMW *out, int wide)
{
	BOOL uni = comctl_notify_unicode(h);
	NMLVDISPINFOW nm;
	WCHAR wbuf[DISPINFO_MAX];

	memset(&nm, 0, sizeof(nm));
	nm.item.mask = need;
	nm.item.iItem = i;
	nm.item.iSubItem = sub;
	nm.item.stateMask = s->cbmask;
	if (!ownerdata(h) && i >= 0 && i < s->n)
		nm.item.lParam = s->item[i]->lParam;
	if (need & LVIF_TEXT) {
		if (uni == wide && out->pszText && out->cchTextMax > 0) {
			nm.item.pszText = out->pszText;
			nm.item.cchTextMax = out->cchTextMax;
			out->pszText[0] = 0;
			if (!wide)
				((char *)out->pszText)[0] = 0;
		} else {
			wbuf[0] = 0;
			nm.item.pszText = wbuf;
			nm.item.cchTextMax = uni ? DISPINFO_MAX :
					     (int)sizeof(wbuf);
		}
	}
	comctl_notify(h, uni ? LVN_GETDISPINFOW : LVN_GETDISPINFOA, &nm.hdr);
	if ((need & LVIF_TEXT) && nm.item.pszText != out->pszText) {
		/* The parent's own string, or the one in wbuf: copy it over. */
		if (nm.item.pszText == LPSTR_TEXTCALLBACKW || !nm.item.pszText) {
			comctl_copy_text_out(u"", out->pszText,
					     out->cchTextMax, wide);
		} else {
			WCHAR *t = uni ? comctl_w_dup(nm.item.pszText) :
				   comctl_a_to_w((const char *)nm.item.pszText);
			comctl_copy_text_out(t, out->pszText, out->cchTextMax,
					     wide);
			free(t);
		}
	}
	if (need & LVIF_IMAGE)
		out->iImage = nm.item.iImage;
	if (need & LVIF_STATE)
		out->state = (out->state & ~s->cbmask) |
			     (nm.item.state & s->cbmask);
	if (need & LVIF_INDENT)
		out->iIndent = nm.item.iIndent;
	if ((need & LVIF_PARAM))
		out->lParam = nm.item.lParam;
}

/* ---- states ---- */

static UINT state_get(HWND h, const struct listview *s, int i)
{
	if (ownerdata(h))
		return i >= 0 && i < s->count && s->odstate ? s->odstate[i] : 0;
	return i >= 0 && i < s->n ? s->item[i]->state : 0;
}

static void state_put(HWND h, struct listview *s, int i, UINT st)
{
	if (ownerdata(h)) {
		if (s->odstate && i >= 0 && i < s->count)
			s->odstate[i] = st;
	} else if (i >= 0 && i < s->n) {
		s->item[i]->state = st;
	}
}

static BOOL change_state(HWND h, struct listview *s, int i, UINT st,
			 UINT mask);

/* Item i's state becomes (old & ~mask) | (st & mask), if ITEMCHANGING
 * lets it; a single-selection list view has one selected item, and every
 * list view one focused one. */
static BOOL change_state(HWND h, struct listview *s, int i, UINT st,
			 UINT mask)
{
	UINT old = state_get(h, s, i);
	UINT new = (old & ~mask) | (st & mask);
	LPARAM lp = !ownerdata(h) ? s->item[i]->lParam : 0;

	if (new == old)
		return TRUE;
	if (notify_lv(h, LVN_ITEMCHANGING, i, 0, new, old, LVIF_STATE, lp))
		return FALSE;
	if ((new & ~old & LVIS_SELECTED) &&
	    (comctl_style(h) & LVS_SINGLESEL)) {
		for (int j = 0; j < count(h, s); j++)
			if (j != i && (state_get(h, s, j) & LVIS_SELECTED))
				change_state(h, s, j, 0, LVIS_SELECTED);
	}
	if (new & ~old & LVIS_FOCUSED) {
		for (int j = 0; j < count(h, s); j++)
			if (j != i && (state_get(h, s, j) & LVIS_FOCUSED))
				change_state(h, s, j, 0, LVIS_FOCUSED);
	}
	state_put(h, s, i, new);
	notify_lv(h, LVN_ITEMCHANGED, i, 0, new, old, LVIF_STATE, lp);
	return TRUE;
}

/* ---- columns ---- */

static void header_item(const struct lv_col *c, HDITEMW *hd)
{
	memset(hd, 0, sizeof(*hd));
	hd->mask = HDI_WIDTH | HDI_FORMAT | HDI_TEXT | HDI_LPARAM;
	hd->cxy = c->cx;
	hd->fmt = HDF_STRING | (c->fmt & LVCFMT_JUSTIFYMASK);
	hd->pszText = c->text ? c->text : (WCHAR *)u"";
	if (c->fmt & LVCFMT_IMAGE) {
		hd->mask |= HDI_IMAGE;
		hd->fmt |= HDF_IMAGE;
		hd->iImage = c->image;
	}
}

static void col_set(struct lv_col *c, const LVCOLUMNW *in, int wide)
{
	if (in->mask & LVCF_FMT)
		c->fmt = in->fmt;
	if (in->mask & LVCF_WIDTH)
		c->cx = in->cx;
	if (in->mask & LVCF_TEXT) {
		free(c->text);
		c->text = in->pszText ? text_in(in->pszText, wide) : NULL;
	}
	if (in->mask & LVCF_SUBITEM)
		c->sub = in->iSubItem;
	if (in->mask & LVCF_IMAGE)
		c->image = in->iImage;
}

static LRESULT col_insert(struct listview *s, int i, const LVCOLUMNW *in,
			  int wide)
{
	if (!in || i < 0)
		return -1;
	if (i > s->ncol)
		i = s->ncol;
	struct lv_col *n = realloc(s->col, (s->ncol + 1) * sizeof(*n));
	if (!n)
		return -1;
	s->col = n;
	memmove(&n[i + 1], &n[i], (s->ncol - i) * sizeof(*n));
	memset(&n[i], 0, sizeof(*n));
	n[i].sub = i;
	col_set(&n[i], in, wide);
	/* The first column is always left aligned. */
	if (i == 0)
		n[i].fmt &= ~LVCFMT_JUSTIFYMASK;
	s->ncol++;
	if (s->header) {
		HDITEMW hd;
		header_item(&n[i], &hd);
		if (in->mask & LVCF_ORDER) {
			hd.mask |= HDI_ORDER;
			hd.iOrder = in->iOrder;
		}
		comctl_send(s->header, HDM_INSERTITEMW, i, (LPARAM)&hd);
	}
	return i;
}

static void col_sync(struct listview *s, int i)
{
	if (s->header) {
		HDITEMW hd;
		header_item(&s->col[i], &hd);
		comctl_send(s->header, HDM_SETITEMW, i, (LPARAM)&hd);
	}
}

/* The text of item i's subitem sub as stored (callbacks aside). */
static const WCHAR *stored_text(const struct listview *s, int i, int sub)
{
	if (i < 0 || i >= s->n || sub >= s->item[i]->nsub ||
	    s->item[i]->sub[sub].callback)
		return NULL;
	return s->item[i]->sub[sub].text;
}

static int col_autosize(HWND h, struct listview *s, int c, BOOL use_header)
{
	int w = 0, sub = s->col[c].sub;

	for (int i = 0; i < s->n; i++) {
		int t = comctl_text_width(stored_text(s, i, sub));
		if (t > w)
			w = t;
	}
	w += 12;
	if (c == 0 && s->himl[LVSIL_SMALL])
		w += SMALL_ICON;
	if (use_header) {
		int t = comctl_text_width(s->col[c].text) + 12;
		if (t > w)
			w = t;
		if (c == s->ncol - 1) {
			/* The last column fills what's left. */
			RECT rc;
			int x = 0;
			comctl_client_rect(h, &rc);
			for (int k = 0; k < c; k++)
				x += s->col[k].cx;
			if (rc.right - x > w)
				w = rc.right - x;
		}
	}
	return w;
}

/* ---- items ---- */

static int sorted_place(HWND h, const struct listview *s, const WCHAR *text)
{
	DWORD style = comctl_style(h);
	int dir = style & LVS_SORTASCENDING ? 1 :
		  style & LVS_SORTDESCENDING ? -1 : 0;

	if (!dir || !text)
		return -1;
	int i = 0;
	while (i < s->n && dir * text_cmp(stored_text(s, i, 0), text,
					  (size_t)-1) <= 0)
		i++;
	return i;
}

static LRESULT item_insert(HWND h, struct listview *s, const LVITEMW *in,
			   int wide)
{
	if (!in || in->iSubItem || ownerdata(h))
		return -1;
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 16;
		struct lv_item **n = realloc(s->item, cap * sizeof(*n));
		if (!n)
			return -1;
		s->item = n;
		s->cap = cap;
	}
	struct lv_item *it = calloc(1, sizeof(*it));
	if (!it || !sub_of(it, 0, TRUE)) {
		free(it);
		return -1;
	}
	if (in->mask & LVIF_TEXT)
		set_text(&it->sub[0], in->pszText, wide);
	it->sub[0].image = in->mask & LVIF_IMAGE ? in->iImage : 0;
	if (in->mask & LVIF_PARAM)
		it->lParam = in->lParam;
	if (in->mask & LVIF_INDENT)
		it->indent = in->iIndent;
	int i = in->iItem;
	int sorted = it->sub[0].callback ? -1 :
		     sorted_place(h, s, it->sub[0].text);
	if (sorted >= 0)
		i = sorted;
	else if (i < 0 || i > s->n)
		i = s->n;
	memmove(&s->item[i + 1], &s->item[i], (s->n - i) * sizeof(*s->item));
	s->item[i] = it;
	s->n++;
	if (s->mark >= i)
		s->mark++;
	if (s->hot >= i)
		s->hot++;
	notify_lv(h, LVN_INSERTITEM, i, 0, 0, 0, 0, it->lParam);
	if ((in->mask & LVIF_STATE) && (in->stateMask & in->state)) {
		UINT st = in->state & in->stateMask;
		it->state = 0;
		change_state(h, s, i, st, in->stateMask);
	}
	return i;
}

static BOOL item_delete(HWND h, struct listview *s, int i, BOOL tell)
{
	if (ownerdata(h)) {
		if (i < 0 || i >= s->count)
			return FALSE;
		if (tell)
			notify_lv(h, LVN_DELETEITEM, i, 0, 0, 0, 0, 0);
		if (s->odstate)
			memmove(&s->odstate[i], &s->odstate[i + 1],
				(s->count - i - 1) * sizeof(UINT));
		s->count--;
	} else {
		if (i < 0 || i >= s->n)
			return FALSE;
		if (tell)
			notify_lv(h, LVN_DELETEITEM, i, 0, 0, 0, 0,
				  s->item[i]->lParam);
		item_free(s->item[i]);
		memmove(&s->item[i], &s->item[i + 1],
			(s->n - i - 1) * sizeof(*s->item));
		s->n--;
	}
	if (s->mark == i)
		s->mark = -1;
	else if (s->mark > i)
		s->mark--;
	if (s->hot == i)
		s->hot = -1;
	else if (s->hot > i)
		s->hot--;
	return TRUE;
}

static BOOL delete_all(HWND h, struct listview *s)
{
	int n = count(h, s);

	if (!n)
		return TRUE;
	/* TRUE from the parent: no LVN_DELETEITEM for each. */
	BOOL each = !notify_lv(h, LVN_DELETEALLITEMS, -1, 0, 0, 0, 0, 0);
	while (count(h, s))
		item_delete(h, s, 0, each);
	return TRUE;
}

static BOOL item_get(HWND h, struct listview *s, LVITEMW *out, int wide)
{
	int i = out->iItem, k = out->iSubItem;
	UINT need = 0;

	if (i < 0 || i >= count(h, s) || k < 0)
		return FALSE;
	if (ownerdata(h)) {
		need = out->mask & (LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM |
				    LVIF_INDENT);
		if ((out->mask & LVIF_STATE) && !k) {
			out->state = state_get(h, s, i) & out->stateMask;
			if (s->cbmask & out->stateMask)
				need |= LVIF_STATE;
		}
		if (need)
			dispinfo(h, s, i, k, need, out, wide);
		return TRUE;
	}
	struct lv_item *it = s->item[i];
	struct lv_sub *sb = sub_of(it, k, FALSE);
	if (out->mask & LVIF_TEXT) {
		if (sb && sb->callback)
			need |= LVIF_TEXT;
		else
			comctl_copy_text_out(sb ? sb->text : NULL,
					     out->pszText, out->cchTextMax,
					     wide);
	}
	if (out->mask & LVIF_IMAGE) {
		int img = sb ? sb->image : 0;
		if (img == I_IMAGECALLBACK)
			need |= LVIF_IMAGE;
		out->iImage = img;
	}
	if (!k) {
		if (out->mask & LVIF_PARAM)
			out->lParam = it->lParam;
		if (out->mask & LVIF_STATE) {
			out->state = it->state & out->stateMask;
			if (s->cbmask & out->stateMask)
				need |= LVIF_STATE;
		}
		if (out->mask & LVIF_INDENT) {
			out->iIndent = it->indent;
			if (it->indent == I_INDENTCALLBACK)
				need |= LVIF_INDENT;
		}
	}
	if (need)
		dispinfo(h, s, i, k, need, out, wide);
	return TRUE;
}

static BOOL item_set(HWND h, struct listview *s, const LVITEMW *in, int wide)
{
	int i = in->iItem, k = in->iSubItem;

	if (i < 0 || i >= count(h, s) || k < 0)
		return FALSE;
	if (ownerdata(h)) {
		if ((in->mask & ~LVIF_STATE) || k)
			return FALSE;
		return change_state(h, s, i, in->state, in->stateMask);
	}
	struct lv_item *it = s->item[i];
	if (k) {
		if (in->mask & ~(LVIF_TEXT | LVIF_IMAGE))
			return FALSE;
		struct lv_sub *sb = sub_of(it, k, TRUE);
		if (!sb)
			return FALSE;
		if (in->mask & LVIF_TEXT)
			set_text(sb, in->pszText, wide);
		if (in->mask & LVIF_IMAGE)
			sb->image = in->iImage;
		return TRUE;
	}
	UINT changed = in->mask & (LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM);
	if (changed) {
		if (notify_lv(h, LVN_ITEMCHANGING, i, 0, it->state, it->state,
			      changed, it->lParam))
			return FALSE;
		if (in->mask & LVIF_TEXT)
			set_text(&it->sub[0], in->pszText, wide);
		if (in->mask & LVIF_IMAGE)
			it->sub[0].image = in->iImage;
		if (in->mask & LVIF_PARAM)
			it->lParam = in->lParam;
		notify_lv(h, LVN_ITEMCHANGED, i, 0, it->state, it->state,
			  changed, it->lParam);
	}
	if (in->mask & LVIF_INDENT)
		it->indent = in->iIndent;
	if (in->mask & LVIF_STATE)
		return change_state(h, s, i, in->state, in->stateMask);
	return TRUE;
}

static LRESULT next_item(HWND h, struct listview *s, int start, UINT flags)
{
	UINT want = flags & (LVNI_SELECTED | LVNI_FOCUSED | LVNI_CUT |
			     LVNI_DROPHILITED);
	int n = count(h, s);
	int step = flags & (LVNI_ABOVE | LVNI_TOLEFT) ? -1 : 1;

	if (start < -1 || start >= n)
		return -1;
	for (int i = start == -1 ? (step > 0 ? 0 : n - 1) : start + step;
	     i >= 0 && i < n; i += step)
		if ((state_get(h, s, i) & want) == want)
			return i;
	return -1;
}

static LRESULT find_item(HWND h, struct listview *s, int start,
			 const LVFINDINFOW *f, int wide)
{
	int n = count(h, s);

	if (!f || ownerdata(h) || !n)
		return -1;
	WCHAR *want = (f->flags & (LVFI_STRING | LVFI_PARTIAL)) && f->psz ?
		      text_in(f->psz, wide) : NULL;
	size_t wl = wlen(want);
	int first = start < 0 || start >= n - 1 ? 0 : start + 1;
	int found = -1;
	for (int k = 0; k < n && found < 0; k++) {
		int i = first + k;
		if (i >= n) {
			if (!(f->flags & LVFI_WRAP))
				break;
			i -= n;
		}
		if (f->flags & LVFI_PARAM) {
			if (s->item[i]->lParam == f->lParam)
				found = i;
		} else if (want) {
			const WCHAR *t = stored_text(s, i, 0);
			if (f->flags & LVFI_PARTIAL ?
			    !text_cmp(t, want, wl) :
			    !text_cmp(t, want, (size_t)-1))
				found = i;
		}
	}
	free(want);
	return found;
}

struct sort_ctx {
	PFNLVCOMPARE fn;
	LPARAM param;
	BOOL by_index;
	struct lv_item **orig;
	int n;
};

static int orig_index(const struct sort_ctx *c, const struct lv_item *it)
{
	for (int i = 0; i < c->n; i++)
		if (c->orig[i] == it)
			return i;
	return -1;
}

static int sort_cmp(const struct sort_ctx *c, struct lv_item *a,
		    struct lv_item *b)
{
	if (c->by_index)
		return c->fn(orig_index(c, a), orig_index(c, b), c->param);
	return c->fn(a->lParam, b->lParam, c->param);
}

/* A stable merge sort through the caller's comparison. */
static void merge_sort(const struct sort_ctx *c, struct lv_item **a,
		       struct lv_item **tmp, int n)
{
	if (n < 2)
		return;
	int m = n / 2;
	merge_sort(c, a, tmp, m);
	merge_sort(c, a + m, tmp, n - m);
	int i = 0, j = m, k = 0;
	while (i < m && j < n)
		tmp[k++] = sort_cmp(c, a[j], a[i]) < 0 ? a[j++] : a[i++];
	while (i < m)
		tmp[k++] = a[i++];
	while (j < n)
		tmp[k++] = a[j++];
	memcpy(a, tmp, n * sizeof(*a));
}

static BOOL sort_items(HWND h, struct listview *s, PFNLVCOMPARE fn,
		       LPARAM param, BOOL by_index)
{
	if (!fn || ownerdata(h))
		return FALSE;
	struct lv_item **tmp = malloc((s->n + 1) * sizeof(*tmp));
	struct lv_item **orig = malloc((s->n + 1) * sizeof(*orig));
	if (!tmp || !orig) {
		free(tmp);
		free(orig);
		return FALSE;
	}
	memcpy(orig, s->item, s->n * sizeof(*orig));
	struct sort_ctx c = { fn, param, by_index, orig, s->n };
	merge_sort(&c, s->item, tmp, s->n);
	free(tmp);
	free(orig);
	return TRUE;
}

/* ---- geometry ---- */

static int header_height(HWND h, const struct listview *s)
{
	if (!s->header || view_of(h, s) != LV_VIEW_DETAILS ||
	    (comctl_style(h) & LVS_NOCOLUMNHEADER))
		return 0;
	RECT rc = { 0, 0, 0, 0 };
	WINDOWPOS wp;
	HDLAYOUT l = { &rc, &wp };
	return comctl_send(s->header, HDM_LAYOUT, 0, (LPARAM)&l) ? wp.cy : 0;
}

static int rows_per_column(HWND h)
{
	RECT rc;

	comctl_client_rect(h, &rc);
	int r = rc.bottom / ROW_HEIGHT;
	return r > 0 ? r : 1;
}

static SIZE cell(HWND h, const struct listview *s)
{
	SIZE z = { 0, ROW_HEIGHT };

	switch (view_of(h, s)) {
	case LV_VIEW_ICON:
	case LV_VIEW_TILE:
		return s->spacing;
	case LV_VIEW_SMALLICON:
		return s->small_spacing;
	case LV_VIEW_LIST:
		z.cx = s->list_cx;
		return z;
	}
	for (int c = 0; c < s->ncol; c++)
		z.cx += s->col[c].cx;
	return z;
}

static int per_row(HWND h, const struct listview *s)
{
	RECT rc;

	comctl_client_rect(h, &rc);
	SIZE z = cell(h, s);
	int n = z.cx > 0 ? rc.right / z.cx : 1;
	return n > 0 ? n : 1;
}

static POINT origin_of(HWND h, const struct listview *s, int i)
{
	POINT p = { 0, 0 };
	SIZE z = cell(h, s);

	switch (view_of(h, s)) {
	case LV_VIEW_DETAILS:
		p.y = header_height(h, s) + i * ROW_HEIGHT;
		break;
	case LV_VIEW_LIST: {
		int r = rows_per_column(h);
		p.x = (i / r) * z.cx;
		p.y = (i % r) * ROW_HEIGHT;
		break;
	}
	default:
		if (!ownerdata(h) && s->item[i]->placed)
			return s->item[i]->pos;
		p.x = (i % per_row(h, s)) * z.cx;
		p.y = (i / per_row(h, s)) * z.cy;
	}
	return p;
}

static BOOL item_rect(HWND h, struct listview *s, int i, int code, RECT *rc)
{
	if (i < 0 || i >= count(h, s))
		return FALSE;
	POINT p = origin_of(h, s, i);
	SIZE z = cell(h, s);
	UINT view = view_of(h, s);
	BOOL big = view == LV_VIEW_ICON || view == LV_VIEW_TILE;
	int icon = big ? 32 : s->himl[LVSIL_SMALL] ? SMALL_ICON : 0;
	int col0 = view == LV_VIEW_DETAILS ?
		   (s->ncol ? s->col[0].cx : 0) : z.cx;

	rc->left = p.x;
	rc->top = p.y;
	rc->right = p.x + z.cx;
	rc->bottom = p.y + z.cy;
	switch (code) {
	case LVIR_BOUNDS:
		return TRUE;
	case LVIR_ICON:
		if (big) {
			rc->left = p.x + (z.cx - icon) / 2;
			rc->right = rc->left + icon;
			rc->bottom = p.y + icon;
		} else {
			rc->right = p.x + icon;
		}
		return TRUE;
	case LVIR_LABEL:
	case LVIR_SELECTBOUNDS:
		if (big) {
			rc->top = p.y + (code == LVIR_LABEL ? icon : 0);
		} else {
			if (code == LVIR_LABEL)
				rc->left = p.x + icon;
			rc->right = p.x + col0;
		}
		return TRUE;
	}
	return FALSE;
}

static BOOL subitem_rect(HWND h, struct listview *s, int i, int sub,
			 int code, RECT *rc)
{
	if (view_of(h, s) != LV_VIEW_DETAILS || sub < 0 || sub >= s->ncol)
		return sub == 0 && item_rect(h, s, i, code, rc);
	if (!item_rect(h, s, i, LVIR_BOUNDS, rc))
		return FALSE;
	if (sub == 0) {
		if (code != LVIR_BOUNDS)
			return item_rect(h, s, i, code, rc);
		return TRUE;
	}
	int x = rc->left;
	for (int c = 0; c < sub; c++)
		x += s->col[c].cx;
	rc->left = x;
	rc->right = x + s->col[sub].cx;
	if (code == LVIR_ICON)
		rc->right = rc->left + (s->himl[LVSIL_SMALL] &&
			    (s->exstyle & LVS_EX_SUBITEMIMAGES) ? SMALL_ICON : 0);
	return TRUE;
}

static int hit_test(HWND h, struct listview *s, LVHITTESTINFO *ht, BOOL sub)
{
	RECT c, rc;

	comctl_client_rect(h, &c);
	ht->flags = 0;
	ht->iItem = -1;
	if (sub)
		ht->iSubItem = 0;
	if (ht->pt.y < 0)
		ht->flags |= LVHT_ABOVE;
	else if (ht->pt.y >= c.bottom)
		ht->flags |= LVHT_BELOW;
	if (ht->pt.x < 0)
		ht->flags |= LVHT_TOLEFT;
	else if (ht->pt.x >= c.right)
		ht->flags |= LVHT_TORIGHT;
	if (ht->flags)
		return -1;
	for (int i = 0; i < count(h, s); i++) {
		if (!item_rect(h, s, i, LVIR_BOUNDS, &rc) ||
		    ht->pt.x < rc.left || ht->pt.x >= rc.right ||
		    ht->pt.y < rc.top || ht->pt.y >= rc.bottom)
			continue;
		RECT ic;
		item_rect(h, s, i, LVIR_ICON, &ic);
		ht->flags = ht->pt.x < ic.right && ht->pt.y < ic.bottom &&
			    ic.right > ic.left ? LVHT_ONITEMICON :
						 LVHT_ONITEMLABEL;
		ht->iItem = i;
		if (sub && view_of(h, s) == LV_VIEW_DETAILS) {
			int x = rc.left;
			for (int k = 0; k < s->ncol; k++) {
				if (ht->pt.x >= x && ht->pt.x < x + s->col[k].cx)
					ht->iSubItem = k;
				x += s->col[k].cx;
			}
		}
		return i;
	}
	ht->flags = LVHT_NOWHERE;
	return -1;
}

static void view_rect(HWND h, struct listview *s, RECT *out)
{
	RECT rc;

	memset(out, 0, sizeof(*out));
	for (int i = 0; i < count(h, s); i++) {
		item_rect(h, s, i, LVIR_BOUNDS, &rc);
		if (!i || rc.left < out->left)
			out->left = rc.left;
		if (!i || rc.top < out->top)
			out->top = rc.top;
		if (rc.right > out->right)
			out->right = rc.right;
		if (rc.bottom > out->bottom)
			out->bottom = rc.bottom;
	}
}

/* ---- the procedure ---- */

static void layout_header(HWND h, struct listview *s)
{
	if (!s->header)
		return;
	DWORD style = comctl_style(h);
	DWORD hs = comctl_style(s->header);
	BOOL hide = view_of(h, s) != LV_VIEW_DETAILS ||
		    (style & LVS_NOCOLUMNHEADER);
	comctl_set_style(s->header, hide ? hs | HDS_HIDDEN : hs & ~HDS_HIDDEN);
	RECT rc;
	comctl_client_rect(h, &rc);
	comctl_move(s->header, 0, 0, rc.right, header_height(h, s));
}

/* ---- label editing ---- */

#define EDIT_ID 0x4c564544	/* the edit's subclass */

static WCHAR *label_of(HWND h, struct listview *s, int i)
{
	WCHAR buf[DISPINFO_MAX];
	LVITEMW it;

	memset(&it, 0, sizeof(it));
	it.mask = LVIF_TEXT;
	it.iItem = i;
	it.pszText = buf;
	it.cchTextMax = DISPINFO_MAX;
	buf[0] = 0;
	if (!item_get(h, s, &it, 1))
		return NULL;
	return comctl_w_dup(it.pszText ? it.pszText : u"");
}

/* LVN_BEGIN/ENDLABELEDIT with item i's text (NULL: cancelled). */
static LRESULT label_notify(HWND h, struct listview *s, int i, BOOL end,
			    const WCHAR *text)
{
	BOOL uni = comctl_notify_unicode(h);
	NMLVDISPINFOW nm;
	char *a = NULL;

	memset(&nm, 0, sizeof(nm));
	nm.item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
	nm.item.iItem = i;
	nm.item.state = state_get(h, s, i);
	if (!ownerdata(h) && i >= 0 && i < s->n)
		nm.item.lParam = s->item[i]->lParam;
	if (text) {
		if (uni) {
			nm.item.pszText = (WCHAR *)text;
		} else {
			a = comctl_w_to_a(text);
			nm.item.pszText = (WCHAR *)a;
		}
		nm.item.cchTextMax = DISPINFO_MAX;
	}
	UINT code = end ? (uni ? LVN_ENDLABELEDITW : LVN_ENDLABELEDITA) :
		    (uni ? LVN_BEGINLABELEDITW : LVN_BEGINLABELEDITA);
	LRESULT r = comctl_notify(h, code, &nm.hdr);
	free(a);
	return r;
}

/*
 * Ends the edit: the parent is told the new text (or that it was
 * cancelled) and keeps it by answering TRUE. From the edit's own
 * messages it is only hidden, and destroyed once they are done.
 */
static void end_edit(HWND h, struct listview *s, BOOL save, BOOL from_edit)
{
	BOOL (WINAPI *destroy_w)(HWND) = USER32(DestroyWindow);
	HWND (WINAPI *get_focus)(void) = USER32(GetFocus);
	HWND (WINAPI *set_focus)(HWND) = USER32(SetFocus);
	int (WINAPI *get_len)(HWND) = USER32(GetWindowTextLengthW);
	int (WINAPI *get_text)(HWND, LPWSTR, int) = USER32(GetWindowTextW);
	HWND e = s->edit;
	int i = s->edit_item;

	if (!e)
		return;
	s->edit = NULL;
	WCHAR *text = NULL;
	if (save) {
		int n = get_len(e);
		text = malloc((n + 1) * sizeof(WCHAR));
		if (text) {
			text[0] = 0;
			get_text(e, text, n + 1);
		}
	}
	comctl_show(e, SW_HIDE);
	if (get_focus() == e)
		set_focus(h);
	if (label_notify(h, s, i, TRUE, text) && text && comctl_alive(h, s) &&
	    i >= 0 && i < count(h, s) && !ownerdata(h)) {
		LVITEMW q;
		memset(&q, 0, sizeof(q));
		q.mask = LVIF_TEXT;
		q.iItem = i;
		q.pszText = text;
		item_set(h, s, &q, 1);
	}
	free(text);
	if (from_edit)
		comctl_post(h, WIN_EDIT_DONE, 0, (LPARAM)e);
	else
		destroy_w(e);
}

static LRESULT CALLBACK edit_sub(HWND e, UINT msg, WPARAM wp, LPARAM lp,
				 UINT_PTR id, DWORD_PTR ref)
{
	HWND lv = (HWND)ref;
	struct listview *s = comctl_state(lv);

	switch (msg) {
	case WM_GETDLGCODE:
		return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS |
		       DLGC_HASSETSEL;
	case WM_KEYDOWN:
		if ((wp == VK_RETURN || wp == VK_ESCAPE) && s && s->edit == e) {
			end_edit(lv, s, wp == VK_RETURN, TRUE);
			return 0;
		}
		break;
	case WM_CHAR:
		if (wp == '\r' || wp == 0x1b)
			return 0;
		break;
	case WM_KILLFOCUS: {
		LRESULT r = comctl_def_subclass(e, msg, wp, lp);
		if (s && s->edit == e)
			end_edit(lv, s, TRUE, TRUE);
		return r;
	}
	}
	return comctl_def_subclass(e, msg, wp, lp);
}

static HWND begin_edit(HWND h, struct listview *s, int i)
{
	HWND (WINAPI *set_focus)(HWND) = USER32(SetFocus);

	if (s->edit)
		end_edit(h, s, TRUE, FALSE);
	if (i < 0 || i >= count(h, s) || !comctl_alive(h, s))
		return NULL;
	WCHAR *text = label_of(h, s, i);
	if (!text)
		return NULL;
	if (label_notify(h, s, i, FALSE, text) || !comctl_alive(h, s)) {
		free(text);
		return NULL;
	}
	RECT rc;
	if (!item_rect(h, s, i, LVIR_LABEL, &rc))
		memset(&rc, 0, sizeof(rc));
	HWND e = comctl_create(u"EDIT", WS_CHILD | WS_VISIBLE | WS_BORDER |
			       ES_AUTOHSCROLL, rc.left, rc.top,
			       rc.right - rc.left, rc.bottom - rc.top, h, 1);
	if (!e) {
		free(text);
		return NULL;
	}
	if (s->font)
		comctl_send(e, WM_SETFONT, (WPARAM)s->font, FALSE);
	comctl_send(e, WM_SETTEXT, 0, (LPARAM)text);
	free(text);
	comctl_send(e, EM_SETSEL, 0, -1);
	comctl_set_subclass(e, edit_sub, EDIT_ID, (DWORD_PTR)h);
	s->edit = e;
	s->edit_item = i;
	set_focus(e);
	return s->edit;
}

static void destroy(struct listview *s)
{
	for (int i = 0; i < s->n; i++)
		item_free(s->item[i]);
	free(s->item);
	free(s->odstate);
	for (int c = 0; c < s->ncol; c++)
		free(s->col[c].text);
	free(s->col);
	free(s);
}

LRESULT STUB_WINAPI comctl_listview_proc(HWND h, UINT msg, WPARAM wp,
					 LPARAM lp)
{
	struct listview *s = comctl_state(h);
	int wide = 1;
	RECT *rc;

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->bk = 0x00ffffff;
		s->text = 0;
		s->textbk = CLR_DEFAULT;
		s->mark = s->hot = -1;
		s->list_cx = LIST_CX;
		s->hover = 0xffffffff;		/* HOVER_DEFAULT */
		s->spacing.cx = ICON_CX;
		s->spacing.cy = ICON_CY;
		s->small_spacing.cx = SMALL_CX;
		s->small_spacing.cy = ROW_HEIGHT;
		s->unicode = TRUE;
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case WM_CREATE: {
		DWORD style = comctl_style(h);
		DWORD hs = WS_CHILD | WS_VISIBLE | HDS_HORZ;
		if (!(style & LVS_NOSORTHEADER))
			hs |= HDS_BUTTONS;
		s->header = comctl_create(header_class, hs, 0, 0, 0, 0, h, 0);
		layout_header(h, s);
		break;
	}
	case WM_SIZE:
	case WM_STYLECHANGED:
		layout_header(h, s);
		break;
	case LVM_GETBKCOLOR:
		return s->bk;
	case LVM_SETBKCOLOR:
		s->bk = (COLORREF)lp;
		return TRUE;
	case LVM_GETTEXTCOLOR:
		return s->text;
	case LVM_SETTEXTCOLOR:
		s->text = (COLORREF)lp;
		return TRUE;
	case LVM_GETTEXTBKCOLOR:
		return s->textbk;
	case LVM_SETTEXTBKCOLOR:
		s->textbk = (COLORREF)lp;
		return TRUE;
	case LVM_GETIMAGELIST:
		return wp < 3 ? (LRESULT)s->himl[wp] : 0;
	case LVM_SETIMAGELIST: {
		if (wp >= 3)
			return 0;
		HIMAGELIST old = s->himl[wp];
		s->himl[wp] = (HIMAGELIST)lp;
		return (LRESULT)old;
	}
	case LVM_GETITEMCOUNT:
		return count(h, s);
	case LVM_SETITEMCOUNT:
		if (ownerdata(h)) {
			int n = (int)wp;
			UINT *st = realloc(s->odstate,
					   (n ? n : 1) * sizeof(UINT));
			if (n < 0 || !st)
				return FALSE;
			if (n > s->count)
				memset(&st[s->count], 0,
				       (n - s->count) * sizeof(UINT));
			s->odstate = st;
			s->count = n;
			if (s->mark >= n)
				s->mark = -1;
			return TRUE;
		}
		if ((int)wp > s->cap) {
			struct lv_item **n = realloc(s->item,
						     wp * sizeof(*n));
			if (!n)
				return FALSE;
			s->item = n;
			s->cap = (int)wp;
		}
		return TRUE;
	case LVM_INSERTITEMA:
		wide = 0;
		/* fall through */
	case LVM_INSERTITEMW:
		return item_insert(h, s, (const LVITEMW *)lp, wide);
	case LVM_DELETEITEM:
		return item_delete(h, s, (int)wp, TRUE);
	case LVM_DELETEALLITEMS:
		return delete_all(h, s);
	case LVM_GETITEMA:
		wide = 0;
		/* fall through */
	case LVM_GETITEMW:
		return lp ? item_get(h, s, (LVITEMW *)lp, wide) : FALSE;
	case LVM_SETITEMA:
		wide = 0;
		/* fall through */
	case LVM_SETITEMW:
		return lp ? item_set(h, s, (const LVITEMW *)lp, wide) : FALSE;
	case LVM_GETITEMTEXTA:
		wide = 0;
		/* fall through */
	case LVM_GETITEMTEXTW: {
		LVITEMW *it = (LVITEMW *)lp;
		if (!it || !it->pszText || it->cchTextMax <= 0)
			return 0;
		LVITEMW q;
		memset(&q, 0, sizeof(q));
		q.mask = LVIF_TEXT;
		q.iItem = (int)wp;
		q.iSubItem = it->iSubItem;
		q.pszText = it->pszText;
		q.cchTextMax = it->cchTextMax;
		if (!item_get(h, s, &q, wide))
			return 0;
		return wide ? (LRESULT)wlen(it->pszText) :
			      (LRESULT)strlen((const char *)it->pszText);
	}
	case LVM_SETITEMTEXTA:
		wide = 0;
		/* fall through */
	case LVM_SETITEMTEXTW: {
		const LVITEMW *it = (const LVITEMW *)lp;
		if (!it)
			return FALSE;
		LVITEMW q;
		memset(&q, 0, sizeof(q));
		q.mask = LVIF_TEXT;
		q.iItem = (int)wp;
		q.iSubItem = it->iSubItem;
		q.pszText = it->pszText;
		return item_set(h, s, &q, wide);
	}
	case LVM_GETITEMSTATE:
		return state_get(h, s, (int)wp) & (UINT)lp;
	case LVM_SETITEMSTATE: {
		const LVITEMW *it = (const LVITEMW *)lp;
		if (!it)
			return FALSE;
		if ((int)wp == -1) {
			BOOL ok = TRUE;
			for (int i = 0; i < count(h, s); i++)
				ok &= change_state(h, s, i, it->state,
						   it->stateMask);
			return ok;
		}
		if ((int)wp < 0 || (int)wp >= count(h, s))
			return FALSE;
		return change_state(h, s, (int)wp, it->state, it->stateMask);
	}
	case LVM_GETCALLBACKMASK:
		return s->cbmask;
	case LVM_SETCALLBACKMASK:
		s->cbmask = (UINT)wp;
		return TRUE;
	case LVM_GETNEXTITEM:
		return next_item(h, s, (int)wp, (UINT)lp);
	case LVM_FINDITEMA:
		wide = 0;
		/* fall through */
	case LVM_FINDITEMW:
		return find_item(h, s, (int)wp, (const LVFINDINFOW *)lp, wide);
	case LVM_GETSELECTEDCOUNT: {
		int n = 0;
		for (int i = 0; i < count(h, s); i++)
			n += (state_get(h, s, i) & LVIS_SELECTED) != 0;
		return n;
	}
	case LVM_GETSELECTIONMARK:
		return s->mark;
	case LVM_SETSELECTIONMARK: {
		int old = s->mark;
		s->mark = (int)lp >= -1 && (int)lp < count(h, s) ? (int)lp : -1;
		return old;
	}
	case LVM_GETHOTITEM:
		return s->hot;
	case LVM_SETHOTITEM: {
		int old = s->hot;
		s->hot = (int)wp;
		return old;
	}
	case LVM_GETHOVERTIME:
		return s->hover;
	case LVM_SETHOVERTIME: {
		DWORD old = s->hover;
		s->hover = (DWORD)lp;
		return old;
	}
	case LVM_GETTOOLTIPS:
		return (LRESULT)s->tooltips;
	case LVM_SETTOOLTIPS: {
		HWND old = s->tooltips;
		s->tooltips = (HWND)wp;
		return (LRESULT)old;
	}
	case LVM_GETEXTENDEDLISTVIEWSTYLE:
		return s->exstyle;
	case LVM_SETEXTENDEDLISTVIEWSTYLE: {
		DWORD old = s->exstyle, mask = wp ? (DWORD)wp : 0xffffffff;
		s->exstyle = (old & ~mask) | ((DWORD)lp & mask);
		return old;
	}
	case LVM_SORTITEMS:
		return sort_items(h, s, (PFNLVCOMPARE)lp, (LPARAM)wp, FALSE);
	case LVM_SORTITEMSEX:
		return sort_items(h, s, (PFNLVCOMPARE)lp, (LPARAM)wp, TRUE);
	case LVM_INSERTCOLUMNA:
		wide = 0;
		/* fall through */
	case LVM_INSERTCOLUMNW:
		return col_insert(s, (int)wp, (const LVCOLUMNW *)lp, wide);
	case LVM_DELETECOLUMN: {
		int c = (int)wp;
		if (c < 0 || c >= s->ncol)
			return FALSE;
		free(s->col[c].text);
		memmove(&s->col[c], &s->col[c + 1],
			(s->ncol - c - 1) * sizeof(*s->col));
		s->ncol--;
		if (s->header)
			comctl_send(s->header, HDM_DELETEITEM, c, 0);
		return TRUE;
	}
	case LVM_GETCOLUMNA:
		wide = 0;
		/* fall through */
	case LVM_GETCOLUMNW: {
		LVCOLUMNW *out = (LVCOLUMNW *)lp;
		int c = (int)wp;
		if (!out || c < 0 || c >= s->ncol)
			return FALSE;
		if (out->mask & LVCF_FMT)
			out->fmt = s->col[c].fmt;
		if (out->mask & LVCF_WIDTH)
			out->cx = s->col[c].cx;
		if (out->mask & LVCF_TEXT)
			comctl_copy_text_out(s->col[c].text, out->pszText,
					     out->cchTextMax, wide);
		if (out->mask & LVCF_SUBITEM)
			out->iSubItem = s->col[c].sub;
		if (out->mask & LVCF_IMAGE)
			out->iImage = s->col[c].image;
		if ((out->mask & LVCF_ORDER) && s->header) {
			HDITEMW hd;
			memset(&hd, 0, sizeof(hd));
			hd.mask = HDI_ORDER;
			comctl_send(s->header, HDM_GETITEMW, c, (LPARAM)&hd);
			out->iOrder = hd.iOrder;
		}
		return TRUE;
	}
	case LVM_SETCOLUMNA:
		wide = 0;
		/* fall through */
	case LVM_SETCOLUMNW: {
		const LVCOLUMNW *in = (const LVCOLUMNW *)lp;
		int c = (int)wp;
		if (!in || c < 0 || c >= s->ncol)
			return FALSE;
		col_set(&s->col[c], in, wide);
		col_sync(s, c);
		if ((in->mask & LVCF_ORDER) && s->header) {
			HDITEMW hd;
			memset(&hd, 0, sizeof(hd));
			hd.mask = HDI_ORDER;
			hd.iOrder = in->iOrder;
			comctl_send(s->header, HDM_SETITEMW, c, (LPARAM)&hd);
		}
		return TRUE;
	}
	case LVM_GETCOLUMNWIDTH:
		if (view_of(h, s) == LV_VIEW_LIST)
			return s->list_cx;
		return (int)wp >= 0 && (int)wp < s->ncol ? s->col[wp].cx : 0;
	case LVM_SETCOLUMNWIDTH: {
		int c = (int)wp, cx = (short)LOWORD(lp);
		if (view_of(h, s) == LV_VIEW_LIST) {
			if (c || cx < 0)
				return FALSE;
			s->list_cx = cx;
			return TRUE;
		}
		if (c < 0 || c >= s->ncol)
			return FALSE;
		if (cx == LVSCW_AUTOSIZE || cx == LVSCW_AUTOSIZE_USEHEADER)
			cx = col_autosize(h, s, c,
					  cx == LVSCW_AUTOSIZE_USEHEADER);
		else if (cx < 0)
			return FALSE;
		s->col[c].cx = cx;
		col_sync(s, c);
		return TRUE;
	}
	case LVM_GETCOLUMNORDERARRAY:
		if (!lp || (int)wp != s->ncol)
			return FALSE;
		if (!s->header) {
			for (int c = 0; c < s->ncol; c++)
				((int *)lp)[c] = c;
			return TRUE;
		}
		return comctl_send(s->header, HDM_GETORDERARRAY, wp, lp);
	case LVM_SETCOLUMNORDERARRAY:
		return s->header &&
		       comctl_send(s->header, HDM_SETORDERARRAY, wp, lp);
	case LVM_GETHEADER:
		return (LRESULT)s->header;
	case LVM_GETITEMRECT:
		return (rc = (RECT *)lp) ?
		       item_rect(h, s, (int)wp, rc->left, rc) : FALSE;
	case LVM_GETSUBITEMRECT:
		return (rc = (RECT *)lp) ?
		       subitem_rect(h, s, (int)wp, rc->top, rc->left, rc) :
		       FALSE;
	case LVM_GETITEMPOSITION: {
		POINT *pt = (POINT *)lp;
		if (!pt || (int)wp < 0 || (int)wp >= count(h, s))
			return FALSE;
		*pt = origin_of(h, s, (int)wp);
		return TRUE;
	}
	case LVM_SETITEMPOSITION:
	case LVM_SETITEMPOSITION32: {
		int i = (int)wp;
		UINT view = view_of(h, s);
		if (i < 0 || i >= s->n || ownerdata(h) ||
		    (view != LV_VIEW_ICON && view != LV_VIEW_SMALLICON))
			return FALSE;
		POINT p;
		if (msg == LVM_SETITEMPOSITION) {
			p.x = (short)LOWORD(lp);
			p.y = (short)HIWORD(lp);
		} else if (lp) {
			p = *(const POINT *)lp;
		} else {
			return FALSE;
		}
		s->item[i]->pos = p;
		s->item[i]->placed = TRUE;
		return TRUE;
	}
	case LVM_GETORIGIN: {
		POINT *pt = (POINT *)lp;
		UINT view = view_of(h, s);
		if (!pt || view == LV_VIEW_DETAILS || view == LV_VIEW_LIST)
			return FALSE;
		pt->x = pt->y = 0;
		return TRUE;
	}
	case LVM_GETVIEWRECT:
		if (!lp || view_of(h, s) == LV_VIEW_DETAILS)
			return FALSE;
		view_rect(h, s, (RECT *)lp);
		return TRUE;
	case LVM_APPROXIMATEVIEWRECT: {
		int n = (int)wp == -1 ? count(h, s) : (int)wp;
		SIZE z = cell(h, s);
		UINT view = view_of(h, s);
		int w, ht;
		if (view == LV_VIEW_DETAILS) {
			w = z.cx;
			ht = header_height(h, s) + n * ROW_HEIGHT;
		} else if (view == LV_VIEW_LIST) {
			int r = rows_per_column(h);
			w = ((n + r - 1) / r) * z.cx;
			ht = (n < r ? n : r) * ROW_HEIGHT;
		} else {
			int pr = per_row(h, s);
			w = (n < pr ? n : pr) * z.cx;
			ht = ((n + pr - 1) / pr) * z.cy;
		}
		return MAKELONG(w, ht);
	}
	case LVM_GETITEMSPACING: {
		SIZE z = wp ? s->small_spacing : s->spacing;
		return MAKELONG(z.cx, z.cy);
	}
	case LVM_SETICONSPACING: {
		SIZE old = s->spacing;
		int cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
		s->spacing.cx = cx == -1 ? ICON_CX : cx;
		s->spacing.cy = cy == -1 ? ICON_CY : cy;
		return MAKELONG(old.cx, old.cy);
	}
	case LVM_GETCOUNTPERPAGE: {
		RECT c;
		comctl_client_rect(h, &c);
		UINT view = view_of(h, s);
		if (view == LV_VIEW_DETAILS) {
			int r = (c.bottom - header_height(h, s)) / ROW_HEIGHT;
			return r > 0 ? r : 0;
		}
		if (view == LV_VIEW_LIST) {
			int cols = s->list_cx ? c.right / s->list_cx : 0;
			return (cols > 0 ? cols : 1) * rows_per_column(h);
		}
		return count(h, s);
	}
	case LVM_GETTOPINDEX:
		return 0;
	case LVM_HITTEST:
		return lp ? hit_test(h, s, (LVHITTESTINFO *)lp, FALSE) : -1;
	case LVM_SUBITEMHITTEST:
		return lp ? hit_test(h, s, (LVHITTESTINFO *)lp, TRUE) : -1;
	case LVM_GETSTRINGWIDTHA:
	case LVM_GETSTRINGWIDTHW: {
		if (!lp)
			return 0;
		WCHAR *t = text_in((const void *)lp,
				   msg == LVM_GETSTRINGWIDTHW);
		int w = comctl_text_width(t);
		free(t);
		return w;
	}
	case LVM_ENSUREVISIBLE:
		return (int)wp >= 0 && (int)wp < count(h, s);
	case LVM_SCROLL:
	case LVM_REDRAWITEMS:
	case LVM_UPDATE:
		return TRUE;
	case LVM_ARRANGE:
		if (!ownerdata(h))
			for (int i = 0; i < s->n; i++)
				s->item[i]->placed = FALSE;
		return TRUE;
	case LVM_EDITLABELA:
	case LVM_EDITLABELW:
		if ((int)wp == -1) {
			end_edit(h, s, FALSE, FALSE);
			return 0;
		}
		return (LRESULT)begin_edit(h, s, (int)wp);
	case LVM_GETEDITCONTROL:
		return (LRESULT)s->edit;
	case LVM_CANCELEDITLABEL:
		end_edit(h, s, FALSE, FALSE);
		return 0;
	case WIN_EDIT_DONE: {
		BOOL (WINAPI *destroy_w)(HWND) = USER32(DestroyWindow);
		if ((HWND)lp && (HWND)lp != s->edit)
			destroy_w((HWND)lp);
		return 0;
	}
	case LVM_CREATEDRAGIMAGE:
		return 0;
	case LVM_GETISEARCHSTRINGA:
	case LVM_GETISEARCHSTRINGW:
		return 0;
	case LVM_GETVIEW:
		return view_of(h, s);
	case LVM_SETVIEW: {
		if (wp > LV_VIEW_TILE)
			return -1;
		s->tile = wp == LV_VIEW_TILE;
		if (!s->tile)
			comctl_set_style(h, (comctl_style(h) & ~LVS_TYPEMASK) |
					    (DWORD)wp);
		layout_header(h, s);
		return 1;
	}
	case LVM_SETUNICODEFORMAT: {
		BOOL old = s->unicode;
		s->unicode = (BOOL)wp;
		return old;
	}
	case LVM_GETUNICODEFORMAT:
		return s->unicode;
	case WM_SETFONT:
		s->font = (HFONT)wp;
		if (s->header)
			comctl_send(s->header, WM_SETFONT, wp, lp);
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		delete_all(h, s);
		destroy(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}
