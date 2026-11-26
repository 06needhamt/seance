/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's SysLink control, headless: its window text is markup, plain
 * text with links written <a href="url" id="name">text</a> (attributes
 * in either quote, in any order, both optional, tag names in any case).
 * The control keeps each link's id, URL and state (LIS_ENABLED to begin
 * with, the first one focused) for LM_GETITEM, LM_SETITEM and
 * LM_HITTEST. Nothing is drawn: the text without its tags is one line,
 * LINE_CY high, each piece as wide as the default font makes it. A click
 * on a link, or Return on the focused one (unless LWS_IGNORERETURN),
 * sends NM_CLICK or NM_RETURN with an NMLINK; Tab moves the focus on.
 */
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define LINE_CY 16
#define IS_INTRES(p) (((UINT_PTR)(p) >> 16) == 0)

struct link {
	WCHAR id[MAX_LINKID_TEXT];
	WCHAR url[L_MAX_URL_LENGTH];
	UINT state;
	int start, len;		/* in the plain text */
};

struct syslink {
	WCHAR *plain;
	struct link *l;
	int n;
	HFONT font;
};

static int fold(WCHAR c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static BOOL starts(const WCHAR *p, const char *word)
{
	for (; *word; p++, word++)
		if (fold(*p) != *word)
			return FALSE;
	return TRUE;
}

static void copy_n(WCHAR *dst, size_t cap, const WCHAR *src, size_t n)
{
	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n * sizeof(WCHAR));
	dst[n] = 0;
}

/* One attribute of an <a ...> tag: name="value" or name='value'. */
static const WCHAR *attribute(const WCHAR *p, struct link *l)
{
	while (*p == ' ' || *p == '\t')
		p++;
	int which = starts(p, "href") ? 1 : starts(p, "id") ? 2 : 0;
	while (*p && *p != '=' && *p != '>' && *p != ' ')
		p++;
	if (*p != '=')
		return p;
	p++;
	WCHAR q = (*p == '"' || *p == '\'') ? *p++ : 0;
	const WCHAR *v = p;
	while (*p && (q ? *p != q : *p != ' ' && *p != '>'))
		p++;
	if (which == 1)
		copy_n(l->url, L_MAX_URL_LENGTH, v, (size_t)(p - v));
	else if (which == 2)
		copy_n(l->id, MAX_LINKID_TEXT, v, (size_t)(p - v));
	if (q && *p == q)
		p++;
	return p;
}

static void parse(struct syslink *s, const WCHAR *m)
{
	free(s->plain);
	free(s->l);
	s->plain = NULL;
	s->l = NULL;
	s->n = 0;
	size_t len = m ? wstrlen(m) : 0;
	if (!(s->plain = calloc(len + 1, sizeof(WCHAR))))
		return;
	int out = 0;
	struct link *open = NULL;
	for (const WCHAR *p = m; p && *p;) {
		if (!open && p[0] == '<' && fold(p[1]) == 'a' &&
		    (p[2] == '>' || p[2] == ' ' || p[2] == '\t')) {
			void *q = realloc(s->l, (size_t)(s->n + 1) * sizeof(*s->l));
			const WCHAR *e = p + 2;
			while (*e && *e != '>')
				e++;
			if (!q || !*e) {
				s->plain[out++] = *p++;
				continue;
			}
			s->l = q;
			open = &s->l[s->n++];
			memset(open, 0, sizeof(*open));
			open->state = LIS_ENABLED;
			open->start = out;
			p += 2;
			while (*p && *p != '>') {
				const WCHAR *next = attribute(p, open);
				p = next == p ? p + 1 : next;
			}
			if (*p == '>')
				p++;
			continue;
		}
		if (open && starts(p, "</a>")) {
			open->len = out - open->start;
			open = NULL;
			p += 4;
			continue;
		}
		s->plain[out++] = *p++;
	}
	if (open)
		open->len = out - open->start;
	s->plain[out] = 0;
	if (s->n)
		s->l[0].state |= LIS_FOCUSED;
}

static int text_cx(const WCHAR *t, int n)
{
	WCHAR buf[512];
	if (n <= 0)
		return 0;
	if (n > 511)
		n = 511;
	memcpy(buf, t, (size_t)n * sizeof(WCHAR));
	buf[n] = 0;
	return comctl_text_width(buf);
}

static int hit(const struct syslink *s, POINT pt)
{
	if (pt.y < 0 || pt.y >= LINE_CY || !s->plain)
		return -1;
	for (int i = 0; i < s->n; i++) {
		int l = text_cx(s->plain, s->l[i].start);
		int r = text_cx(s->plain, s->l[i].start + s->l[i].len);
		if (pt.x >= l && pt.x < r)
			return i;
	}
	return -1;
}

static void fill(const struct link *l, int i, LITEM *it)
{
	it->mask = LIF_ITEMINDEX | LIF_STATE | LIF_ITEMID | LIF_URL;
	it->iLink = i;
	it->state = l->state;
	it->stateMask = ~0u;
	memcpy(it->szID, l->id, sizeof(it->szID));
	memcpy(it->szUrl, l->url, sizeof(it->szUrl));
}

static int find(const struct syslink *s, const LITEM *it)
{
	if (it->mask & LIF_ITEMINDEX)
		return it->iLink >= 0 && it->iLink < s->n ? it->iLink : -1;
	if (it->mask & LIF_ITEMID)
		for (int i = 0; i < s->n; i++)
			if (!memcmp(s->l[i].id, it->szID,
				    (wstrlen(it->szID) + 1) * sizeof(WCHAR)))
				return i;
	return -1;
}

static void notify_link(HWND h, const struct syslink *s, int i, UINT code)
{
	NMLINK *nm = calloc(1, sizeof(*nm));
	if (!nm)
		return;
	fill(&s->l[i], i, &nm->item);
	comctl_notify(h, code, &nm->hdr);
	free(nm);
}

static int focused(const struct syslink *s)
{
	for (int i = 0; i < s->n; i++)
		if (s->l[i].state & LIS_FOCUSED)
			return i;
	return -1;
}

static void focus(struct syslink *s, int i)
{
	for (int j = 0; j < s->n; j++)
		s->l[j].state &= ~(UINT)LIS_FOCUSED;
	if (i >= 0 && i < s->n)
		s->l[i].state |= LIS_FOCUSED;
}

LRESULT STUB_WINAPI comctl_link_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct syslink *s = comctl_state(h);

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		comctl_set_state(h, s);
		const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
		parse(s, cs && !IS_INTRES(cs->lpszName) ? cs->lpszName :
								NULL);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case WM_SETTEXT:
		parse(s, (const WCHAR *)lp);
		break;		/* DefWindowProc keeps the markup */
	case LM_GETITEM: {
		LITEM *it = (LITEM *)lp;
		int i = it ? find(s, it) : -1;
		if (i < 0)
			return FALSE;
		UINT mask = it->stateMask;
		fill(&s->l[i], i, it);
		it->state = s->l[i].state & mask;
		it->stateMask = mask;
		return TRUE;
	}
	case LM_SETITEM: {
		const LITEM *it = (const LITEM *)lp;
		int i = it ? find(s, it) : -1;
		if (i < 0)
			return FALSE;
		struct link *l = &s->l[i];
		if (it->mask & LIF_STATE) {
			if (it->stateMask & it->state & LIS_FOCUSED)
				focus(s, i);
			l->state = (l->state & ~it->stateMask) |
				   (it->state & it->stateMask);
		}
		if ((it->mask & LIF_ITEMID) && (it->mask & LIF_ITEMINDEX))
			copy_n(l->id, MAX_LINKID_TEXT, it->szID,
			       wstrlen(it->szID));
		if (it->mask & LIF_URL)
			copy_n(l->url, L_MAX_URL_LENGTH, it->szUrl,
			       wstrlen(it->szUrl));
		return TRUE;
	}
	case LM_HITTEST: {
		LHITTESTINFO *hi = (LHITTESTINFO *)lp;
		int i = hi ? hit(s, hi->pt) : -1;
		if (i < 0)
			return FALSE;
		fill(&s->l[i], i, &hi->item);
		return TRUE;
	}
	case LM_GETIDEALHEIGHT:
		return LINE_CY;
	case WM_LBUTTONDOWN: {
		POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
		int i = hit(s, pt);
		if (i >= 0 && (s->l[i].state & LIS_ENABLED)) {
			focus(s, i);
			notify_link(h, s, i, NM_CLICK);
		}
		return 0;
	}
	case WM_KEYDOWN: {
		int i = focused(s);
		if (wp == VK_RETURN && i >= 0 &&
		    !(comctl_style(h) & LWS_IGNORERETURN) &&
		    (s->l[i].state & LIS_ENABLED))
			notify_link(h, s, i, NM_RETURN);
		else if (wp == VK_TAB && s->n)
			focus(s, (i + 1) % s->n);
		return 0;
	}
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		free(s->plain);
		free(s->l);
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}
