/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32: property sheets and task dialogs, headless.
 *
 * - A property sheet page is a copy of the caller's PROPSHEETPAGE (all
 *   dwSize bytes of it, as codecs keep their own data after it), with its
 *   PSPCB_ADDREF and PSPCB_RELEASE callbacks and pcRefParent count kept
 *   as on Windows.
 * - PropertySheet builds the sheet as a dialog (user32's), sized for its
 *   largest page: a tab per page, OK, Cancel, Apply and Help, or a
 *   wizard's Back, Next and Finish. Each page is its own dialog, made
 *   when first shown (after PSPCB_CREATE) with the PROPSHEETPAGE as its
 *   WM_INITDIALOG lParam, and told PSN_SETACTIVE, PSN_KILLACTIVE,
 *   PSN_APPLY, PSN_RESET, PSN_QUERYCANCEL and the wizard notifications as
 *   on Windows; the PSM_ messages work, PSM_PRESSBUTTON included. A
 *   modeless sheet (PSH_MODELESS) is returned to run; a modal one runs
 *   as DialogBox does, so with nobody to press a button it is cancelled
 *   (or OK is pressed, with $VFW_DIALOG=ok). It answers 1 for OK, or
 *   ID_PSRESTARTWINDOWS or ID_PSREBOOTSYSTEM if a page asked, else 0.
 * - TaskDialog and TaskDialogIndirect print their text and answer with
 *   the default button, as MessageBox does; the default radio button and
 *   verification box are reported as they start. The callback isn't
 *   called: there is no window to send it messages about.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32/codepage.h"
#include "win32/comctl.h"
#include "win32/internal.h"
#include "win32/user.h"

#define PSP_MAGIC 0x50535048	/* "HPSP" */

struct _PSP {
	DWORD magic;
	int wide;
	/* The copy: at least a whole PROPSHEETPAGEW, zeroed past dwSize. */
	union {
		PROPSHEETPAGEA a;
		PROPSHEETPAGEW w;
		BYTE bytes[sizeof(PROPSHEETPAGEW)];
	} page;
};

static HPROPSHEETPAGE create_page(const void *src, int wide)
{
	const PROPSHEETPAGEW *p = src;

	if (!p || p->dwSize < VFW_PROPSHEETPAGE_V1_SIZE)
		return NULL;
	size_t size = p->dwSize > sizeof(PROPSHEETPAGEW) ? p->dwSize :
		      sizeof(PROPSHEETPAGEW);
	HPROPSHEETPAGE h = comctl_alloc(offsetof(struct _PSP, page) + size);
	if (!h)
		return NULL;
	h->magic = PSP_MAGIC;
	h->wide = wide;
	memcpy(&h->page, p, p->dwSize);

	PROPSHEETPAGEW *c = &h->page.w;
	if ((c->dwFlags & PSP_USEREFPARENT) && c->pcRefParent)
		(*c->pcRefParent)++;
	if ((c->dwFlags & PSP_USECALLBACK) && c->pfnCallback)
		c->pfnCallback(NULL, PSPCB_ADDREF, c);
	return h;
}

WINFN(HPROPSHEETPAGE) ps_CreatePropertySheetPageA(LPCPROPSHEETPAGEA p)
{
	return create_page(p, 0);
}

WINFN(HPROPSHEETPAGE) ps_CreatePropertySheetPageW(LPCPROPSHEETPAGEW p)
{
	return create_page(p, 1);
}

WINFN(BOOL) ps_DestroyPropertySheetPage(HPROPSHEETPAGE h)
{
	if (!h || h->magic != PSP_MAGIC)
		return FALSE;
	PROPSHEETPAGEW *c = &h->page.w;
	if ((c->dwFlags & PSP_USECALLBACK) && c->pfnCallback)
		c->pfnCallback(NULL, PSPCB_RELEASE, c);
	if ((c->dwFlags & PSP_USEREFPARENT) && c->pcRefParent)
		(*c->pcRefParent)--;
	h->magic = 0;
	comctl_free(h);
	return TRUE;
}

/* A string argument that may be a resource id, for printing. */
static char *res_text(const void *s, int wide)
{
	if (!s)
		return strdup("");
	if (((ULONG_PTR)s >> 16) == 0) {
		char buf[16];
		snprintf(buf, sizeof(buf), "#%u", (unsigned)(ULONG_PTR)s);
		return strdup(buf);
	}
	return wide ? utf16_to_utf8_dup(s) : acp_to_utf8(s);
}

/* The sheet. */

#define ID_TABCTRL   0x3020
#define ID_APPLY_NOW 0x3021
#define ID_WIZBACK   0x3023
#define ID_WIZNEXT   0x3024
#define ID_WIZFINISH 0x3025

extern const struct builtin_dll builtin_kernel32;

struct sheet {
	HWND hwnd, tab;
	int wide, wizard, modeless;
	DWORD flags;
	HWND parent;
	HINSTANCE inst;
	PFNPROPSHEETCALLBACK callback;
	WCHAR *caption;
	HPROPSHEETPAGE *page;
	HWND *page_hwnd;
	BYTE *changed;
	int n, cap;
	int cur;
	int start;
	INT_PTR result;		/* PropertySheet's answer */
	int ended;
	int restart;		/* ID_PSRESTARTWINDOWS or ID_PSREBOOTSYSTEM */
	int x, y;		/* where pages go, in pixels */
};

static struct {
	HWND (WINAPI *GetDlgItem)(HWND, int);
	LRESULT (WINAPI *SendMessageW)(HWND, UINT, WPARAM, LPARAM);
	BOOL (WINAPI *EnableWindow)(HWND, BOOL);
	BOOL (WINAPI *ShowWindow)(HWND, int);
	BOOL (WINAPI *DestroyWindow)(HWND);
	BOOL (WINAPI *SetWindowTextW)(HWND, LPCWSTR);
	BOOL (WINAPI *SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
	BOOL (WINAPI *EndDialog)(HWND, INT_PTR);
	BOOL (WINAPI *IsWindow)(HWND);
	BOOL (WINAPI *IsDialogMessageW)(HWND, LPMSG);
	HWND (WINAPI *CreateDialogIndirectParamW)(HINSTANCE, const void *,
						  HWND, DLGPROC, LPARAM);
	INT_PTR (WINAPI *DialogBoxIndirectParamW)(HINSTANCE, const void *,
						  HWND, DLGPROC, LPARAM);
	LONG_PTR (WINAPI *SetWindowLongPtrW)(HWND, int, LONG_PTR);
	LONG_PTR (WINAPI *GetWindowLongPtrW)(HWND, int);
	HRSRC (WINAPI *FindResourceW)(HMODULE, LPCWSTR, LPCWSTR);
	HRSRC (WINAPI *FindResourceA)(HMODULE, LPCSTR, LPCSTR);
	HGLOBAL (WINAPI *LoadResource)(HMODULE, HRSRC);
	DWORD (WINAPI *SizeofResource)(HMODULE, HRSRC);
	int (WINAPI *LoadStringW)(HINSTANCE, UINT, LPWSTR, int);
} su;

static pthread_once_t su_once = PTHREAD_ONCE_INIT;

static void su_init(void)
{
#define U(n) *(void **)&su.n = USER32(n)
#define K(n) *(void **)&su.n = builtin_export(&builtin_kernel32, #n)
	U(GetDlgItem); U(SendMessageW); U(EnableWindow); U(ShowWindow);
	U(DestroyWindow); U(SetWindowTextW); U(SetWindowPos); U(EndDialog);
	U(IsWindow); U(IsDialogMessageW); U(CreateDialogIndirectParamW);
	U(DialogBoxIndirectParamW); U(LoadStringW);
#if defined(__x86_64__)
	U(SetWindowLongPtrW); U(GetWindowLongPtrW);
#else
	*(void **)&su.SetWindowLongPtrW = USER32(SetWindowLongW);
	*(void **)&su.GetWindowLongPtrW = USER32(GetWindowLongW);
#endif
	K(FindResourceW); K(FindResourceA); K(LoadResource);
	K(SizeofResource);
#undef U
#undef K
}

/* A string argument (or resource id, from inst) as UTF-16, malloc'd. */
static WCHAR *res_wide(HINSTANCE inst, const void *s, int wide)
{
	if (!s)
		return NULL;
	if (((ULONG_PTR)s >> 16) == 0) {
		WCHAR buf[256];
		int n = su.LoadStringW(inst, (UINT)(ULONG_PTR)s, buf, 256);
		if (n <= 0)
			return NULL;
		WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
		if (w)
			memcpy(w, buf, (n + 1) * sizeof(WCHAR));
		return w;
	}
	if (wide) {
		const WCHAR *w = s;
		size_t n = 0;
		while (w[n])
			n++;
		WCHAR *d = malloc((n + 1) * sizeof(WCHAR));
		if (d)
			memcpy(d, w, (n + 1) * sizeof(WCHAR));
		return d;
	}
	char *u = acp_to_utf8(s);
	WCHAR *w = u ? utf8_to_utf16_dup(u) : NULL;
	free(u);
	return w;
}

static int wcscmp_ascii(const WCHAR *a, const WCHAR *b)
{
	for (;; a++, b++) {
		WCHAR x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
		WCHAR y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
		if (x != y || !x)
			return x - y;
	}
}

/* A page's dialog template and its size ((size_t)-1 if not known). */
static const void *page_template(HPROPSHEETPAGE pg, size_t *size)
{
	const PROPSHEETPAGEW *p = &pg->page.w;

	*size = (size_t)-1;
	if (p->dwFlags & PSP_DLGINDIRECT)
		return p->pResource;
	HRSRC r = pg->wide ?
		  su.FindResourceW(p->hInstance, p->pszTemplate,
				   (LPCWSTR)(ULONG_PTR)5) :
		  su.FindResourceA(p->hInstance, (LPCSTR)p->pszTemplate,
				   (LPCSTR)(ULONG_PTR)5);
	HGLOBAL g = r ? su.LoadResource(p->hInstance, r) : NULL;
	if (!g)
		return NULL;
	*size = su.SizeofResource(p->hInstance, r);
	return g;
}

/* What a page's tab says: its title, else its dialog's caption. */
static WCHAR *page_title(HPROPSHEETPAGE pg)
{
	const PROPSHEETPAGEW *p = &pg->page.w;
	size_t size;

	if ((p->dwFlags & PSP_USETITLE) && p->pszTitle)
		return res_wide(p->hInstance, p->pszTitle, pg->wide);
	const void *t = page_template(pg, &size);
	WCHAR *caption = NULL;
	if (t)
		user_dialog_template_info(t, size, NULL, NULL, &caption);
	return caption;
}

static LRESULT page_notify(struct sheet *s, int i, UINT code, LPARAM lp)
{
	PSHNOTIFY n;

	if (i < 0 || i >= s->n || !s->page_hwnd[i])
		return 0;
	memset(&n, 0, sizeof(n));
	n.hdr.hwndFrom = s->hwnd;
	n.hdr.idFrom = 0;
	n.hdr.code = code;
	n.lParam = lp;
	return su.SendMessageW(s->page_hwnd[i], WM_NOTIFY, 0, (LPARAM)&n);
}

/* Makes page i's dialog if it has none yet. */
static HWND page_window(struct sheet *s, int i)
{
	HPROPSHEETPAGE pg = s->page[i];
	PROPSHEETPAGEW *p = &pg->page.w;
	size_t size;

	if (s->page_hwnd[i])
		return s->page_hwnd[i];
	if ((p->dwFlags & PSP_USECALLBACK) && p->pfnCallback &&
	    !p->pfnCallback(NULL, PSPCB_CREATE, p))
		return NULL;
	const void *t = page_template(pg, &size);
	if (!t)
		return NULL;
	HWND h = user_dialog_child(p->hInstance, t, size, s->hwnd,
				   p->pfnDlgProc, (LPARAM)p, pg->wide);
	if (!h)
		return NULL;
	su.ShowWindow(h, SW_HIDE);
	su.SetWindowPos(h, NULL, s->x, s->y, 0, 0,
			SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	s->page_hwnd[i] = h;
	return h;
}

static void tab_add(struct sheet *s, int i)
{
	if (!s->tab)
		return;
	WCHAR *title = page_title(s->page[i]);
	TCITEMW item;
	memset(&item, 0, sizeof(item));
	item.mask = TCIF_TEXT;
	item.pszText = title ? title : (WCHAR *)u"";
	su.SendMessageW(s->tab, TCM_INSERTITEMW, i, (LPARAM)&item);
	free(title);
}

static void wiz_buttons(struct sheet *s, DWORD f)
{
	if (!s->wizard)
		return;
	su.EnableWindow(su.GetDlgItem(s->hwnd, ID_WIZBACK),
			(f & PSWIZB_BACK) != 0);
	su.EnableWindow(su.GetDlgItem(s->hwnd, ID_WIZNEXT),
			(f & PSWIZB_NEXT) != 0);
	su.ShowWindow(su.GetDlgItem(s->hwnd, ID_WIZNEXT),
		      f & (PSWIZB_FINISH | PSWIZB_DISABLEDFINISH) ? SW_HIDE :
		      SW_SHOW);
	su.ShowWindow(su.GetDlgItem(s->hwnd, ID_WIZFINISH),
		      f & (PSWIZB_FINISH | PSWIZB_DISABLEDFINISH) ? SW_SHOW :
		      SW_HIDE);
	su.EnableWindow(su.GetDlgItem(s->hwnd, ID_WIZFINISH),
			(f & PSWIZB_FINISH) != 0);
}

/* Shows page i, if the one shown lets go and i takes it: whether it did. */
static BOOL set_page(struct sheet *s, int i)
{
	if (i < 0 || i >= s->n)
		return FALSE;
	if (i == s->cur)
		return TRUE;
	if (s->cur >= 0 && page_notify(s, s->cur, PSN_KILLACTIVE, 0))
		return FALSE;
	if (!page_window(s, i))
		return FALSE;
	if (page_notify(s, i, PSN_SETACTIVE, 0) == -1)
		return FALSE;
	if (s->cur >= 0 && s->page_hwnd[s->cur])
		su.ShowWindow(s->page_hwnd[s->cur], SW_HIDE);
	s->cur = i;
	su.ShowWindow(s->page_hwnd[i], SW_SHOW);
	if (s->tab)
		su.SendMessageW(s->tab, TCM_SETCURSEL, i, 0);
	return TRUE;
}

static int any_changed(const struct sheet *s)
{
	for (int i = 0; i < s->n; i++)
		if (s->changed[i])
			return 1;
	return 0;
}

/* PSN_APPLY to the pages made: whether they all took it. */
static BOOL apply(struct sheet *s, BOOL closing)
{
	if (s->cur >= 0 && page_notify(s, s->cur, PSN_KILLACTIVE, 0))
		return FALSE;
	for (int i = 0; i < s->n; i++) {
		LRESULT r = page_notify(s, i, PSN_APPLY, closing);
		if (r == PSNRET_INVALID || r == PSNRET_INVALID_NOCHANGEPAGE) {
			if (r == PSNRET_INVALID)
				set_page(s, i);
			return FALSE;
		}
	}
	memset(s->changed, 0, s->n);
	su.EnableWindow(su.GetDlgItem(s->hwnd, ID_APPLY_NOW), FALSE);
	return TRUE;
}

static void end_sheet(struct sheet *s, INT_PTR r)
{
	s->result = r && s->restart ? s->restart : r;
	s->ended = 1;
	if (!s->modeless) {
		su.EndDialog(s->hwnd, s->result);
		return;
	}
	/* A modeless sheet's owner sees no current page, and destroys it. */
	su.ShowWindow(s->hwnd, SW_HIDE);
	for (int i = 0; i < s->n; i++)
		if (s->page_hwnd[i]) {
			su.DestroyWindow(s->page_hwnd[i]);
			s->page_hwnd[i] = NULL;
		}
	s->cur = -1;
}

static void cancel(struct sheet *s)
{
	if (s->cur >= 0 && page_notify(s, s->cur, PSN_QUERYCANCEL, 0))
		return;
	for (int i = 0; i < s->n; i++)
		page_notify(s, i, PSN_RESET, 0);
	end_sheet(s, 0);
}

/* The page a wizard's PSN_WIZBACK/NEXT answer means: -1 to stay. */
static int wiz_target(struct sheet *s, LRESULT r, int step)
{
	if (r == -1)
		return -1;
	if (r == 0)
		return s->cur + step;
	for (int i = 0; i < s->n; i++) {
		const PROPSHEETPAGEW *p = &s->page[i]->page.w;
		if (!(p->dwFlags & PSP_DLGINDIRECT) &&
		    (ULONG_PTR)p->pszTemplate == (ULONG_PTR)r)
			return i;
	}
	return -1;
}

static BOOL add_page(struct sheet *s, int at, HPROPSHEETPAGE pg)
{
	if (!pg || pg->magic != PSP_MAGIC || at < 0 || at > s->n)
		return FALSE;
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 4;
		HPROPSHEETPAGE *p = realloc(s->page, cap * sizeof(*p));
		if (p)
			s->page = p;
		HWND *w = realloc(s->page_hwnd, cap * sizeof(*w));
		if (w)
			s->page_hwnd = w;
		BYTE *c = realloc(s->changed, cap);
		if (c)
			s->changed = c;
		if (!p || !w || !c)
			return FALSE;
		s->cap = cap;
	}
	memmove(s->page + at + 1, s->page + at, (s->n - at) * sizeof(*s->page));
	memmove(s->page_hwnd + at + 1, s->page_hwnd + at,
		(s->n - at) * sizeof(*s->page_hwnd));
	memmove(s->changed + at + 1, s->changed + at, s->n - at);
	s->page[at] = pg;
	s->page_hwnd[at] = NULL;
	s->changed[at] = 0;
	s->n++;
	if (s->cur >= at)
		s->cur++;
	if (s->hwnd)
		tab_add(s, at);
	return TRUE;
}

static void remove_page(struct sheet *s, int i)
{
	if (i < 0 || i >= s->n)
		return;
	if (i == s->cur) {
		int to = i + 1 < s->n ? i + 1 : i - 1;
		if (to < 0 || !set_page(s, to)) {
			if (s->page_hwnd[i])
				su.ShowWindow(s->page_hwnd[i], SW_HIDE);
			s->cur = -1;
		}
	}
	if (s->page_hwnd[i])
		su.DestroyWindow(s->page_hwnd[i]);
	ps_DestroyPropertySheetPage(s->page[i]);
	memmove(s->page + i, s->page + i + 1, (s->n - i - 1) * sizeof(*s->page));
	memmove(s->page_hwnd + i, s->page_hwnd + i + 1,
		(s->n - i - 1) * sizeof(*s->page_hwnd));
	memmove(s->changed + i, s->changed + i + 1, s->n - i - 1);
	s->n--;
	if (s->cur > i)
		s->cur--;
	if (s->tab)
		su.SendMessageW(s->tab, TCM_DELETEITEM, i, 0);
}

static int index_of(const struct sheet *s, WPARAM wp, LPARAM lp)
{
	if (lp) {
		for (int i = 0; i < s->n; i++)
			if ((LPARAM)s->page[i] == lp)
				return i;
		return -1;
	}
	return (int)wp;
}

static void set_caption(struct sheet *s, DWORD style, const WCHAR *text)
{
	static const WCHAR props[] = u" Properties";
	size_t n = 0, k = sizeof(props) / sizeof(WCHAR) - 1;

	while (text && text[n])
		n++;
	WCHAR *c = malloc((n + k + 1) * sizeof(WCHAR));
	if (!c)
		return;
	if (n)
		memcpy(c, text, n * sizeof(WCHAR));
	if (style & PSH_PROPTITLE)
		memcpy(c + n, props, k * sizeof(WCHAR)), n += k;
	c[n] = 0;
	su.SetWindowTextW(s->hwnd, c);
	free(c);
}

static INT_PTR result(HWND h, LRESULT v)
{
	su.SetWindowLongPtrW(h, DWLP_MSGRESULT, v);
	return TRUE;
}

static INT_PTR STUB_WINAPI sheet_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct sheet *s = msg == WM_INITDIALOG ? (struct sheet *)lp :
			  (struct sheet *)su.GetWindowLongPtrW(h, DWLP_USER);
	int i;

	if (!s)
		return FALSE;
	switch (msg) {
	case WM_INITDIALOG: {
		su.SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)s);
		s->hwnd = h;
		s->tab = s->wizard ? NULL : su.GetDlgItem(h, ID_TABCTRL);
		set_caption(s, s->flags, s->caption);
		for (i = 0; i < s->n; i++)
			tab_add(s, i);
		if (s->flags & PSH_NOAPPLYNOW)
			su.ShowWindow(su.GetDlgItem(h, ID_APPLY_NOW), SW_HIDE);
		if (!(s->flags & PSH_HASHELP))
			su.ShowWindow(su.GetDlgItem(h, IDHELP), SW_HIDE);
		su.EnableWindow(su.GetDlgItem(h, ID_APPLY_NOW), FALSE);
		if (s->wizard)
			wiz_buttons(s, PSWIZB_BACK | PSWIZB_NEXT);
		if (s->callback)
			s->callback(h, PSCB_INITIALIZED, 0);
		if (!set_page(s, s->start))
			for (i = 0; i < s->n; i++)
				if (i != s->start && set_page(s, i))
					break;
		return TRUE;
	}
	case WM_NCDESTROY:
		for (i = 0; i < s->n; i++)
			ps_DestroyPropertySheetPage(s->page[i]);
		su.SetWindowLongPtrW(h, DWLP_USER, 0);
		if (s->modeless) {
			free(s->page);
			free(s->page_hwnd);
			free(s->changed);
			free(s->caption);
			free(s);
		} else {
			s->n = 0;	/* PropertySheet frees the rest */
		}
		return FALSE;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			if (apply(s, TRUE))
				end_sheet(s, 1);
			return TRUE;
		case IDCANCEL:
			cancel(s);
			return TRUE;
		case ID_APPLY_NOW:
			apply(s, FALSE);
			return TRUE;
		case IDHELP:
			page_notify(s, s->cur, PSN_HELP, 0);
			return TRUE;
		case ID_WIZBACK:
		case ID_WIZNEXT: {
			int back = LOWORD(wp) == ID_WIZBACK;
			int to = wiz_target(s, page_notify(s, s->cur,
				 back ? PSN_WIZBACK : PSN_WIZNEXT, 0),
				 back ? -1 : 1);
			if (to >= 0)
				set_page(s, to);
			return TRUE;
		}
		case ID_WIZFINISH:
			if (!page_notify(s, s->cur, PSN_WIZFINISH, 0))
				end_sheet(s, 1);
			return TRUE;
		}
		return FALSE;
	case WM_NOTIFY: {
		const NMHDR *n = (const NMHDR *)lp;
		if (n && n->hwndFrom == s->tab && n->code == TCN_SELCHANGE) {
			int to = (int)su.SendMessageW(s->tab, TCM_GETCURSEL, 0,
						      0);
			if (!set_page(s, to) && s->tab)
				su.SendMessageW(s->tab, TCM_SETCURSEL, s->cur,
						0);
			return TRUE;
		}
		return FALSE;
	}
	case PSM_SETCURSEL:
		return result(h, set_page(s, index_of(s, wp, lp)));
	case PSM_SETCURSELID:
		for (i = 0; i < s->n; i++)
			if ((LPARAM)s->page[i]->page.w.pszTemplate == lp)
				return result(h, set_page(s, i));
		return result(h, FALSE);
	case PSM_ADDPAGE:
		return result(h, add_page(s, s->n, (HPROPSHEETPAGE)lp));
	case PSM_INSERTPAGE:
		i = (wp >> 16) ? index_of(s, 0, (LPARAM)wp) + 1 : (int)wp;
		return result(h, add_page(s, i < 0 ? s->n :
				 i > s->n ? s->n : i, (HPROPSHEETPAGE)lp));
	case PSM_REMOVEPAGE:
		remove_page(s, index_of(s, wp, lp));
		return result(h, 0);
	case PSM_CHANGED:
	case PSM_UNCHANGED:
		for (i = 0; i < s->n; i++)
			if (s->page_hwnd[i] == (HWND)wp)
				s->changed[i] = msg == PSM_CHANGED;
		su.EnableWindow(su.GetDlgItem(h, ID_APPLY_NOW),
				any_changed(s));
		return result(h, 0);
	case PSM_APPLY:
		return result(h, apply(s, FALSE));
	case PSM_PRESSBUTTON: {
		static const int ids[] = { ID_WIZBACK, ID_WIZNEXT,
					   ID_WIZFINISH, IDOK, ID_APPLY_NOW,
					   IDCANCEL, IDHELP };
		if (wp < sizeof(ids) / sizeof(ids[0]))
			sheet_proc(h, WM_COMMAND, ids[wp], 0);
		return result(h, 0);
	}
	case PSM_QUERYSIBLINGS:
		for (i = 0; i < s->n; i++)
			if (s->page_hwnd[i]) {
				LRESULT r = su.SendMessageW(s->page_hwnd[i],
						PSM_QUERYSIBLINGS, wp, lp);
				if (r)
					return result(h, r);
			}
		return result(h, 0);
	case PSM_CANCELTOCLOSE:
		su.SetWindowTextW(su.GetDlgItem(h, IDOK), u"Close");
		su.EnableWindow(su.GetDlgItem(h, IDCANCEL), FALSE);
		return result(h, 0);
	case PSM_RESTARTWINDOWS:
		if (s->restart != ID_PSREBOOTSYSTEM)
			s->restart = ID_PSRESTARTWINDOWS;
		return result(h, 0);
	case PSM_REBOOTSYSTEM:
		s->restart = ID_PSREBOOTSYSTEM;
		return result(h, 0);
	case PSM_SETTITLEA:
	case PSM_SETTITLEW: {
		WCHAR *t = res_wide(s->inst, (const void *)lp,
				    msg == PSM_SETTITLEW);
		set_caption(s, (DWORD)wp, t);
		free(t);
		return result(h, 0);
	}
	case PSM_SETWIZBUTTONS:
		wiz_buttons(s, (DWORD)lp);
		return result(h, 0);
	case PSM_SETFINISHTEXTA:
	case PSM_SETFINISHTEXTW: {
		WCHAR *t = res_wide(s->inst, (const void *)lp,
				    msg == PSM_SETFINISHTEXTW);
		su.SetWindowTextW(su.GetDlgItem(h, ID_WIZFINISH), t ? t : u"");
		free(t);
		wiz_buttons(s, PSWIZB_BACK | PSWIZB_FINISH);
		return result(h, 0);
	}
	case PSM_GETTABCONTROL:
		return result(h, (LRESULT)s->tab);
	case PSM_GETCURRENTPAGEHWND:
		return result(h, s->cur >= 0 ? (LRESULT)s->page_hwnd[s->cur] :
				 0);
	case PSM_ISDIALOGMESSAGE:
		return result(h, su.IsDialogMessageW(h, (LPMSG)lp));
	case PSM_HWNDTOINDEX:
		for (i = 0; i < s->n; i++)
			if (s->page_hwnd[i] == (HWND)wp)
				return result(h, i);
		return result(h, -1);
	case PSM_INDEXTOHWND:
		return result(h, (int)wp >= 0 && (int)wp < s->n ?
				 (LRESULT)s->page_hwnd[wp] : 0);
	case PSM_PAGETOINDEX:
		return result(h, index_of(s, 0, lp));
	case PSM_INDEXTOPAGE:
		return result(h, (int)wp >= 0 && (int)wp < s->n ?
				 (LRESULT)s->page[wp] : 0);
	case PSM_IDTOINDEX:
		for (i = 0; i < s->n; i++)
			if ((LPARAM)s->page[i]->page.w.pszTemplate == lp)
				return result(h, i);
		return result(h, -1);
	case PSM_INDEXTOID:
		return result(h, (int)wp >= 0 && (int)wp < s->n ?
				 (LRESULT)s->page[wp]->page.w.pszTemplate : 0);
	case PSM_GETRESULT:
		return result(h, s->ended ? s->result : -1);
	case PSM_RECALCPAGESIZES:
	case PSM_SETHEADERTITLEA:
	case PSM_SETHEADERTITLEW:
	case PSM_SETHEADERSUBTITLEA:
	case PSM_SETHEADERSUBTITLEW:
		return result(h, TRUE);
	}
	return FALSE;
}

/* The sheet's own template: the pages' area, a tab, the buttons. */
struct wbuf {
	WORD *w;
	size_t n, cap;
};

static void wb_word(struct wbuf *b, WORD v)
{
	if (b->n == b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 256;
		WORD *g = realloc(b->w, cap * sizeof(WORD));
		if (!g)
			return;
		b->w = g;
		b->cap = cap;
	}
	if (b->n < b->cap)
		b->w[b->n++] = v;
}

static void wb_dword(struct wbuf *b, DWORD v)
{
	wb_word(b, LOWORD(v));
	wb_word(b, HIWORD(v));
}

static void wb_str(struct wbuf *b, const WCHAR *s)
{
	do
		wb_word(b, *s);
	while (*s++);
}

static void wb_item(struct wbuf *b, DWORD style, int x, int y, int cx,
		    int cy, WORD id, const WCHAR *cls, WORD ord,
		    const WCHAR *title)
{
	if (b->n & 1)
		wb_word(b, 0);
	wb_dword(b, style | WS_CHILD | WS_VISIBLE);
	wb_dword(b, 0);
	wb_word(b, (WORD)x);
	wb_word(b, (WORD)y);
	wb_word(b, (WORD)cx);
	wb_word(b, (WORD)cy);
	wb_word(b, id);
	if (cls) {
		wb_str(b, cls);
	} else {
		wb_word(b, 0xffff);
		wb_word(b, ord);
	}
	wb_str(b, title);
	wb_word(b, 0);
}

static WORD *sheet_template(const struct sheet *s, int *items_out)
{
	struct wbuf b = { NULL, 0, 0 };
	int cx = 212, cy = 188;	/* PROP_MED_CXDLG, PROP_MED_CYDLG */

	for (int i = 0; i < s->n; i++) {
		size_t size;
		int w, h;
		const void *t = page_template(s->page[i], &size);
		if (t && !user_dialog_template_info(t, size, &w, &h, NULL)) {
			if (w > cx)
				cx = w;
			if (h > cy)
				cy = h;
		}
	}
	int px = s->wizard ? 0 : 11, py = s->wizard ? 0 : 25;
	int wcx = cx + 2 * px, by = py + cy + (s->wizard ? 7 : 11);
	int wcy = by + 14 + 7;
	int items = 5;

	wb_dword(&b, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
		 DS_SETFONT | DS_CENTER | DS_3DLOOK | WS_VISIBLE);
	wb_dword(&b, 0);
	wb_word(&b, (WORD)items);
	wb_word(&b, 0);
	wb_word(&b, 0);
	wb_word(&b, (WORD)wcx);
	wb_word(&b, (WORD)wcy);
	wb_word(&b, 0);
	wb_word(&b, 0);
	wb_word(&b, 0);
	wb_word(&b, 8);
	wb_str(&b, u"MS Shell Dlg");
	int x = wcx - 7 - 50;
	if (s->wizard) {
		wb_item(&b, WS_TABSTOP, x, by, 50, 14, IDHELP, NULL, 0x80,
			u"Help");
		x -= 57;
		wb_item(&b, WS_TABSTOP, x, by, 50, 14, IDCANCEL, NULL, 0x80,
			u"Cancel");
		x -= 57;
		wb_item(&b, WS_TABSTOP | BS_DEFPUSHBUTTON, x, by, 50, 14,
			ID_WIZNEXT, NULL, 0x80, u"&Next >");
		wb_item(&b, WS_TABSTOP, x, by, 50, 14, ID_WIZFINISH, NULL,
			0x80, u"Finish");
		x -= 50;
		wb_item(&b, WS_TABSTOP, x, by, 50, 14, ID_WIZBACK, NULL, 0x80,
			u"< &Back");
	} else {
		wb_item(&b, WS_TABSTOP | WS_GROUP, 7, 7, wcx - 14, cy + 25,
			ID_TABCTRL, u"SysTabControl32", 0, u"");
		wb_item(&b, WS_TABSTOP, x, by, 50, 14, IDHELP, NULL, 0x80,
			u"Help");
		x -= 54;
		wb_item(&b, WS_TABSTOP, x, by, 50, 14, ID_APPLY_NOW, NULL, 0x80,
			u"&Apply");
		x -= 54;
		wb_item(&b, WS_TABSTOP, x, by, 50, 14, IDCANCEL, NULL, 0x80,
			u"Cancel");
		x -= 54;
		wb_item(&b, WS_TABSTOP | BS_DEFPUSHBUTTON, x, by, 50, 14, IDOK,
			NULL, 0x80, u"OK");
	}
	*items_out = items;
	return b.n ? b.w : (free(b.w), NULL);
}

static void free_sheet(struct sheet *s)
{
	for (int i = 0; i < s->n; i++)
		ps_DestroyPropertySheetPage(s->page[i]);
	free(s->page);
	free(s->page_hwnd);
	free(s->changed);
	free(s->caption);
	free(s);
}

static INT_PTR property_sheet(const PROPSHEETHEADERW *h, int wide)
{
	pthread_once(&su_once, su_init);
	if (!h || h->dwSize < VFW_PROPSHEETHEADER_V1_SIZE)
		return -1;
	struct sheet *s = calloc(1, sizeof(*s));
	if (!s)
		return -1;
	s->wide = wide;
	s->flags = h->dwFlags;
	s->wizard = (h->dwFlags & (PSH_WIZARD | PSH_WIZARD97)) != 0;
	s->modeless = (h->dwFlags & PSH_MODELESS) != 0;
	s->parent = h->hwndParent;
	s->inst = h->hInstance;
	s->cur = -1;
	s->callback = h->dwFlags & PSH_USECALLBACK ? h->pfnCallback : NULL;
	s->caption = res_wide(h->hInstance, h->pszCaption, wide);

	/* The pages, created from the array or given as handles. */
	if (h->dwFlags & PSH_PROPSHEETPAGE) {
		const BYTE *p = (const BYTE *)h->ppsp;
		for (UINT i = 0; p && i < h->nPages; i++) {
			DWORD size = ((const PROPSHEETPAGEW *)p)->dwSize;
			HPROPSHEETPAGE pg = create_page(p, wide);
			if (!pg || !add_page(s, s->n, pg)) {
				if (pg)
					ps_DestroyPropertySheetPage(pg);
				break;
			}
			if (size < VFW_PROPSHEETPAGE_V1_SIZE)
				break;
			p += size;
		}
	} else {
		for (UINT i = 0; h->phpage && i < h->nPages; i++)
			if (!add_page(s, s->n, h->phpage[i]) && h->phpage[i])
				ps_DestroyPropertySheetPage(h->phpage[i]);
	}
	s->start = 0;
	if (h->dwFlags & PSH_USEPSTARTPAGE) {
		WCHAR *want = res_wide(h->hInstance, h->pStartPage, wide);
		for (int i = 0; want && i < s->n; i++) {
			WCHAR *t = page_title(s->page[i]);
			int same = t && !wcscmp_ascii(t, want);
			free(t);
			if (same) {
				s->start = i;
				break;
			}
		}
		free(want);
	} else if (h->nStartPage < (UINT)s->n) {
		s->start = (int)h->nStartPage;
	}

	int items;
	WORD *tmpl = sheet_template(s, &items);
	if (!tmpl) {
		free_sheet(s);
		return -1;
	}
	if (s->callback)
		s->callback(NULL, PSCB_PRECREATE, (LPARAM)tmpl);
	/* Where the pages go: inside the tab, in pixels (8x16 base). */
	s->x = s->wizard ? 0 : 11 * 2;
	s->y = s->wizard ? 0 : 25 * 2;
	if (s->modeless) {
		HWND w = su.CreateDialogIndirectParamW(s->inst, tmpl, s->parent,
						       (DLGPROC)sheet_proc,
						       (LPARAM)s);
		free(tmpl);
		if (!w)
			free_sheet(s);
		return (INT_PTR)w;	/* freed with the window */
	}
	INT_PTR r = su.DialogBoxIndirectParamW(s->inst, tmpl, s->parent,
					       (DLGPROC)sheet_proc, (LPARAM)s);
	free(tmpl);
	if (r >= 0 && s->ended)
		r = s->result;
	free_sheet(s);
	return r;
}

WINFN(INT_PTR) ps_PropertySheetA(LPCPROPSHEETHEADERA h)
{
	return property_sheet((const PROPSHEETHEADERW *)h, 0);
}

WINFN(INT_PTR) ps_PropertySheetW(LPCPROPSHEETHEADERW h)
{
	return property_sheet(h, 1);
}

/* Task dialogs. */

static const struct {
	int flag, id;
	const char *name;
} common[] = {
	{ TDCBF_OK_BUTTON, IDOK, "OK" },
	{ TDCBF_YES_BUTTON, IDYES, "Yes" },
	{ TDCBF_NO_BUTTON, IDNO, "No" },
	{ TDCBF_RETRY_BUTTON, IDRETRY, "Retry" },
	{ TDCBF_CANCEL_BUTTON, IDCANCEL, "Cancel" },
	{ TDCBF_CLOSE_BUTTON, IDCLOSE, "Close" },
};

/* The buttons in the order shown: custom ones, then the common ones. */
static int button_at(const TASKDIALOGCONFIG *c, int k, char **name)
{
	int n = 0;

	for (UINT i = 0; c->pButtons && i < c->cButtons; i++, n++)
		if (n == k) {
			*name = res_text(c->pButtons[i].pszButtonText, 1);
			return c->pButtons[i].nButtonID;
		}
	int flags = c->dwCommonButtons;
	if (!flags && !(c->pButtons && c->cButtons))
		flags = TDCBF_OK_BUTTON;
	for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++)
		if ((flags & common[i].flag) && n++ == k) {
			*name = strdup(common[i].name);
			return common[i].id;
		}
	return 0;
}

WINFN(HRESULT) ps_TaskDialogIndirect(const TASKDIALOGCONFIG *c, int *button,
				     int *radio, BOOL *verified)
{
	if (!c || c->cbSize != sizeof(*c))
		return E_INVALIDARG;

	/* The default button, or the first. */
	char *name = NULL;
	int answer = button_at(c, 0, &name);
	for (int k = 0;; k++) {
		char *n = NULL;
		int id = button_at(c, k, &n);
		if (!id) {
			free(n);
			break;
		}
		if (id == c->nDefaultButton) {
			free(name);
			name = n;
			answer = id;
			break;
		}
		free(n);
	}

	int chosen = 0;
	if (c->pRadioButtons && c->cRadioButtons &&
	    !(c->dwFlags & TDF_NO_DEFAULT_RADIO_BUTTON)) {
		chosen = c->pRadioButtons[0].nButtonID;
		for (UINT i = 0; i < c->cRadioButtons; i++)
			if (c->pRadioButtons[i].nButtonID == c->nDefaultRadioButton)
				chosen = c->nDefaultRadioButton;
	}

	char *title = res_text(c->pszWindowTitle, 1);
	char *main = res_text(c->pszMainInstruction, 1);
	char *content = res_text(c->pszContent, 1);
	fprintf(stderr, "TaskDialog [%s]: %s%s%s\n  (no GUI: answering %s)\n",
		title, main, *main && *content ? "\n  " : "", content,
		name ? name : "?");
	free(title);
	free(main);
	free(content);
	free(name);

	if (button)
		*button = answer;
	if (radio)
		*radio = chosen;
	if (verified)
		*verified = !!(c->dwFlags & TDF_VERIFICATION_FLAG_CHECKED);
	return S_OK;
}

WINFN(HRESULT) ps_TaskDialog(HWND owner, HINSTANCE inst, LPCWSTR title,
			     LPCWSTR main, LPCWSTR content, int buttons,
			     LPCWSTR icon, int *button)
{
	TASKDIALOGCONFIG c;

	memset(&c, 0, sizeof(c));
	c.cbSize = sizeof(c);
	c.hwndParent = owner;
	c.hInstance = inst;
	c.dwCommonButtons = buttons;
	c.pszWindowTitle = title;
	c.pszMainIcon = icon;
	c.pszMainInstruction = main;
	c.pszContent = content;
	return ps_TaskDialogIndirect(&c, button, NULL, NULL);
}

#define E(name, ord) { #name, ord, (void *)ps_##name }

const struct builtin_export comctl_sheet_exports[] = {
	{ "CreatePropertySheetPage", 0, (void *)ps_CreatePropertySheetPageA },
	E(CreatePropertySheetPageA, 0),
	E(CreatePropertySheetPageW, 0),
	E(DestroyPropertySheetPage, 0),
	{ "PropertySheet", 0, (void *)ps_PropertySheetA },
	E(PropertySheetA, 0),
	E(PropertySheetW, 0),
	E(TaskDialog, 344),
	E(TaskDialogIndirect, 345),
	BUILTIN_END
};
