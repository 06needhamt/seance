/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * user32: dialog boxes and the standard controls, headless.
 *
 * - A dialog is built from its DLGTEMPLATE or DLGTEMPLATEEX, from memory
 *   or an RT_DIALOG resource, as on Windows: the "#32770" window (or the
 *   template's class) sized in dialog units, WM_SETFONT with the
 *   template's font, its controls in template order with their creation
 *   data, then WM_INITDIALOG, the focus on the first tab stop if that
 *   answers TRUE, and the window shown if the template says WS_VISIBLE.
 *   DefDlgProc calls the dialog procedure through DWLP_DLGPROC and
 *   answers with DWLP_MSGRESULT; unhandled, WM_CLOSE posts IDCANCEL and
 *   DM_GETDEFID/DM_SETDEFID and WM_NEXTDLGCTL work.
 * - A modeless dialog lives until it is destroyed; EndDialog hides it.
 *   A modal one (DialogBox*) disables its owner and runs the messages
 *   already queued. Nobody can press a button, so if the dialog hasn't
 *   ended then, it is answered as a user would most safely: its Cancel
 *   button is pressed (or, with $VFW_DIALOG=ok, its default button),
 *   which the dialog handles as it would a click. It says so on stderr.
 * - IsDialogMessage does the keyboard interface: Tab and Shift+Tab move
 *   between tab stops, Enter presses the default button (or the focused
 *   push button), Escape presses Cancel, with WM_GETDLGCODE asked first.
 * - The controls keep their state as on Windows: buttons their check
 *   state (BM_CLICK and the auto styles, radio groups included, notifying
 *   BN_CLICKED), edits their text, selection, limit and modified flag
 *   (typing with WM_CHAR, EM_REPLACESEL, EN_CHANGE and EN_UPDATE), list
 *   and combo boxes their items, item data and selection (sorted,
 *   multiple selection, owner-drawn data). Nothing is drawn.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/log.h"
#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/user.h"

#define IS_INTRESOURCE(p) (((ULONG_PTR)(p) >> 16) == 0)

extern const struct builtin_dll builtin_user32, builtin_kernel32,
	builtin_gdi32;

/* user32's own functions, through its export table. */
static struct {
	HWND (WINAPI *CreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
				       int, int, HWND, HMENU, HINSTANCE,
				       LPVOID);
	BOOL (WINAPI *DestroyWindow)(HWND);
	LRESULT (WINAPI *SendMessageW)(HWND, UINT, WPARAM, LPARAM);
	BOOL (WINAPI *PostMessageW)(HWND, UINT, WPARAM, LPARAM);
	LONG (WINAPI *GetWindowLongW)(HWND, int);
	LONG (WINAPI *SetWindowLongW)(HWND, int, LONG);
	LONG_PTR (WINAPI *GetWindowLongPtrW)(HWND, int);
	LONG_PTR (WINAPI *SetWindowLongPtrW)(HWND, int, LONG_PTR);
	LONG_PTR (WINAPI *SetWindowLongPtrA)(HWND, int, LONG_PTR);
	HWND (WINAPI *GetParent)(HWND);
	HWND (WINAPI *GetWindow)(HWND, UINT);
	HWND (WINAPI *GetDlgItem)(HWND, int);
	int (WINAPI *GetDlgCtrlID)(HWND);
	BOOL (WINAPI *IsWindow)(HWND);
	BOOL (WINAPI *IsChild)(HWND, HWND);
	BOOL (WINAPI *IsWindowVisible)(HWND);
	BOOL (WINAPI *IsWindowEnabled)(HWND);
	BOOL (WINAPI *EnableWindow)(HWND, BOOL);
	BOOL (WINAPI *ShowWindow)(HWND, int);
	HWND (WINAPI *SetFocus)(HWND);
	HWND (WINAPI *GetFocus)(void);
	BOOL (WINAPI *PeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);
	BOOL (WINAPI *TranslateMessage)(const MSG *);
	LRESULT (WINAPI *DispatchMessageW)(const MSG *);
	LRESULT (WINAPI *DispatchMessageA)(const MSG *);
	void (WINAPI *PostQuitMessage)(int);
	int (WINAPI *GetWindowTextW)(HWND, LPWSTR, int);
	int (WINAPI *GetWindowTextLengthW)(HWND);
	SHORT (WINAPI *GetKeyState)(int);
	HICON (WINAPI *LoadIconW)(HINSTANCE, LPCWSTR);
	HRSRC (WINAPI *FindResourceW)(HMODULE, LPCWSTR, LPCWSTR);
	HRSRC (WINAPI *FindResourceA)(HMODULE, LPCSTR, LPCSTR);
	HGLOBAL (WINAPI *LoadResource)(HMODULE, HRSRC);
	DWORD (WINAPI *SizeofResource)(HMODULE, HRSRC);
	HFONT (WINAPI *CreateFontW)(int, int, int, int, int, DWORD, DWORD,
				    DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
				    LPCWSTR);
	BOOL (WINAPI *DeleteObject)(HGDIOBJ);
} u;

static pthread_once_t u_once = PTHREAD_ONCE_INIT;

static void u_init(void)
{
#define U(n) *(void **)&u.n = builtin_export(&builtin_user32, #n)
#define K(n) *(void **)&u.n = builtin_export(&builtin_kernel32, #n)
#define G(n) *(void **)&u.n = builtin_export(&builtin_gdi32, #n)
	U(CreateWindowExW); U(DestroyWindow); U(SendMessageW);
	U(PostMessageW); U(GetWindowLongW); U(SetWindowLongW);
#if defined(__x86_64__)
	U(GetWindowLongPtrW); U(SetWindowLongPtrW); U(SetWindowLongPtrA);
#else
	*(void **)&u.GetWindowLongPtrW =
		builtin_export(&builtin_user32, "GetWindowLongW");
	*(void **)&u.SetWindowLongPtrW =
		builtin_export(&builtin_user32, "SetWindowLongW");
	*(void **)&u.SetWindowLongPtrA =
		builtin_export(&builtin_user32, "SetWindowLongA");
#endif
	U(GetParent); U(GetWindow); U(GetDlgItem); U(GetDlgCtrlID);
	U(IsWindow); U(IsChild); U(IsWindowVisible); U(IsWindowEnabled);
	U(EnableWindow); U(ShowWindow); U(SetFocus); U(GetFocus);
	U(PeekMessageW); U(TranslateMessage); U(DispatchMessageW);
	U(DispatchMessageA); U(PostQuitMessage); U(GetWindowTextW);
	U(GetWindowTextLengthW); U(GetKeyState); U(LoadIconW);
	K(FindResourceW); K(FindResourceA); K(LoadResource);
	K(SizeofResource);
	G(CreateFontW); G(DeleteObject);
#undef U
#undef K
#undef G
}

#define API() pthread_once(&u_once, u_init)

static LRESULT sendw(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	return u.SendMessageW(h, msg, wp, lp);
}

static DWORD style_of(HWND h)
{
	return (DWORD)u.GetWindowLongW(h, GWL_STYLE);
}

static void notify(HWND h, UINT code)
{
	HWND parent = u.GetParent(h);

	if (parent)
		sendw(parent, WM_COMMAND,
		      MAKEWPARAM(u.GetDlgCtrlID(h), code), (LPARAM)h);
}

/* A control's state, behind the pointer in its extra bytes. */
static void *state(HWND h, size_t size)
{
	void *p = (void *)u.GetWindowLongPtrW(h, 0);

	if (!p && size) {
		p = calloc(1, size);
		if (p)
			u.SetWindowLongPtrW(h, 0, (LONG_PTR)p);
	}
	return p;
}

static WCHAR *wdup_n(const WCHAR *s, size_t n)
{
	WCHAR *d = malloc((n + 1) * sizeof(WCHAR));

	if (d) {
		if (n)
			memcpy(d, s, n * sizeof(WCHAR));
		d[n] = 0;
	}
	return d;
}

static WCHAR wlower(WCHAR c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		return c + 32;
	return c;
}

static WCHAR wupper(WCHAR c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
		return c - 32;
	return c;
}

/* Case-insensitive: <0, 0, >0; with prefix, whether a starts with b. */
static int wcmpi(const WCHAR *a, const WCHAR *b, int prefix)
{
	for (;; a++, b++) {
		if (prefix && !*b)
			return 0;
		WCHAR x = wlower(*a), y = wlower(*b);
		if (x != y || !x)
			return (int)x - (int)y;
	}
}

/* A window's text, malloc'd (never NULL unless out of memory). */
static WCHAR *text_of(HWND h)
{
	int n = u.GetWindowTextLengthW(h);
	WCHAR *t = malloc((n + 1) * sizeof(WCHAR));

	if (t) {
		t[0] = 0;
		u.GetWindowTextW(h, t, n + 1);
	}
	return t;
}

/* The children of a window, in order (malloc'd; *n of them). */
static HWND *children(HWND parent, int *n)
{
	int cap = 16;
	HWND *list = malloc(cap * sizeof(*list));

	*n = 0;
	if (!list)
		return NULL;
	for (HWND c = u.GetWindow(parent, GW_CHILD); c;
	     c = u.GetWindow(c, GW_HWNDNEXT)) {
		if (*n == cap) {
			HWND *g = realloc(list, (cap *= 2) * sizeof(*list));
			if (!g)
				break;
			list = g;
		}
		list[(*n)++] = c;
	}
	return list;
}

/* Dialogs. */

struct dlg {
	struct dlg *next;
	HWND hwnd;
	DLGPROC proc;		/* if DWLP_DLGPROC doesn't fit the window */
	int unicode;
	int modal;
	int gone;		/* a modal one's window was destroyed */
	int ended;
	INT_PTR result;
	UINT defid;
	HFONT font;
};

static pthread_mutex_t dlg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dlg *dialogs;

static struct dlg *dlg_find(HWND h)
{
	pthread_mutex_lock(&dlg_lock);
	struct dlg *d = dialogs;
	while (d && d->hwnd != h)
		d = d->next;
	pthread_mutex_unlock(&dlg_lock);
	return d;
}

static void dlg_forget(HWND h)
{
	struct dlg *dead = NULL;

	pthread_mutex_lock(&dlg_lock);
	for (struct dlg **p = &dialogs; *p; p = &(*p)->next)
		if ((*p)->hwnd == h) {
			dead = *p;
			*p = dead->next;
			break;
		}
	pthread_mutex_unlock(&dlg_lock);
	if (!dead)
		return;
	if (dead->font)
		u.DeleteObject(dead->font);
	dead->font = NULL;
	/* DialogBox reads a modal one's result after its window is gone. */
	if (dead->modal)
		dead->gone = 1;
	else
		free(dead);
}

static DLGPROC dlg_proc_of(HWND h, struct dlg *d)
{
	DLGPROC p = (DLGPROC)u.GetWindowLongPtrW(h, DWLP_DLGPROC);
	return p ? p : d ? d->proc : NULL;
}

static LRESULT dlg_command(HWND dlg, int id)
{
	HWND ctl = u.GetDlgItem(dlg, id);
	return sendw(dlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
		     (LPARAM)ctl);
}

static UINT dlg_defid(HWND h)
{
	LRESULT r = sendw(h, DM_GETDEFID, 0, 0);
	return HIWORD(r) == DC_HASDEFID ? LOWORD(r) : IDOK;
}

/* A control's own WS_VISIBLE: its dialog may not be shown yet. */
static BOOL tab_stop(HWND c)
{
	DWORD st = style_of(c);
	return (st & WS_TABSTOP) && (st & WS_VISIBLE) && !(st & WS_DISABLED);
}

static HWND next_tab(HWND dlg, HWND ctl, BOOL previous)
{
	int n;
	HWND *list = children(dlg, &n), found = NULL;
	int at = -1;

	if (!list)
		return NULL;
	for (int i = 0; i < n; i++)
		if (list[i] == ctl)
			at = i;
	for (int k = 1; k <= n && !found; k++) {
		int i = at < 0 ? (previous ? n - k : k - 1) :
			(at + (previous ? n - k : k)) % n;
		if (tab_stop(list[i]))
			found = list[i];
	}
	free(list);
	return found ? found : ctl;
}

static void next_ctl(HWND dlg, HWND to)
{
	if (!to)
		return;
	u.SetFocus(to);
	if (sendw(to, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL)
		sendw(to, EM_SETSEL, 0, -1);
}

static void set_default(HWND dlg, struct dlg *d, UINT id)
{
	int n;
	HWND *list = children(dlg, &n);

	for (int i = 0; list && i < n; i++) {
		LRESULT code = sendw(list[i], WM_GETDLGCODE, 0, 0);
		if (!(code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)))
			continue;
		BOOL def = (UINT)u.GetDlgCtrlID(list[i]) == id;
		if (def != !!(code & DLGC_DEFPUSHBUTTON))
			sendw(list[i], BM_SETSTYLE,
			      def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
	}
	free(list);
	d->defid = id;
}

static LRESULT def_dlg(HWND h, UINT msg, WPARAM wp, LPARAM lp, int unicode)
{
	API();
	struct dlg *d = dlg_find(h);
	DLGPROC proc = dlg_proc_of(h, d);

	if (proc) {
		u.SetWindowLongPtrW(h, DWLP_MSGRESULT, 0);
		INT_PTR r = proc(h, msg, wp, lp);
		if (r || !u.IsWindow(h)) {
			switch (msg) {
			case WM_COMPAREITEM:
			case WM_VKEYTOITEM:
			case WM_CHARTOITEM:
			case WM_INITDIALOG:
			case WM_QUERYDRAGICON:
			case WM_CTLCOLORMSGBOX:
			case WM_CTLCOLOREDIT:
			case WM_CTLCOLORLISTBOX:
			case WM_CTLCOLORBTN:
			case WM_CTLCOLORDLG:
			case WM_CTLCOLORSCROLLBAR:
			case WM_CTLCOLORSTATIC:
				return r;
			default:
				return u.IsWindow(h) ?
				       u.GetWindowLongPtrW(h, DWLP_MSGRESULT) :
				       0;
			}
		}
	}
	switch (msg) {
	case WM_CLOSE:
		u.PostMessageW(h, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED),
			       (LPARAM)u.GetDlgItem(h, IDCANCEL));
		return 0;
	case DM_GETDEFID:
		return d && d->defid ? MAKELONG(d->defid, DC_HASDEFID) : 0;
	case DM_SETDEFID:
		if (d)
			set_default(h, d, (UINT)wp);
		return TRUE;
	case DM_REPOSITION:
		return 0;
	case WM_NEXTDLGCTL:
		next_ctl(h, LOWORD(lp) ? (HWND)wp :
			 next_tab(h, u.GetFocus(), (BOOL)wp));
		return 0;
	case WM_SETFOCUS: {
		HWND first = next_tab(h, NULL, FALSE);
		if (first)
			u.SetFocus(first);
		return 0;
	}
	case WM_NCDESTROY:
		dlg_forget(h);
		break;
	}
	return unicode ? user_DefWindowProcW(h, msg, wp, lp) :
			 user_DefWindowProcA(h, msg, wp, lp);
}

LRESULT STUB_WINAPI user_DefDlgProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	return def_dlg(h, msg, wp, lp, 1);
}

LRESULT STUB_WINAPI user_DefDlgProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	return def_dlg(h, msg, wp, lp, 0);
}

/* Templates. */

struct item {
	DWORD style, exstyle;
	short x, y, cx, cy;
	DWORD id;
	const WCHAR *cls;
	const WCHAR *title;
	WORD title_ord;
	const BYTE *data;	/* its size word first, or NULL */
};

/* The bytes left before end, a number so that an in-memory template's
 * "to the end of memory" makes no pointer past an object. */
#define ROOM(p, end) ((uintptr_t)(p) >= (end) ? (size_t)0 : \
		      (size_t)((end) - (uintptr_t)(p)))

struct tmpl {
	DWORD style, exstyle;
	WORD n;
	short x, y, cx, cy;
	const WCHAR *cls;
	const WCHAR *title;
	WORD point;
	WORD weight;
	BYTE italic, charset;
	const WCHAR *font;
	int ex;
	const BYTE *items;
	uintptr_t end;		/* the last byte + 1, as a number */
};

/* A WORD-aligned sz_Or_Ord: its string (or NULL), or *ord. */
static const BYTE *sz_or_ord(const BYTE *p, uintptr_t end,
			     const WCHAR **s, WORD *ord)
{
	WORD w;

	*s = NULL;
	if (ord)
		*ord = 0;
	if (ROOM(p, end) < 2)
		return NULL;
	memcpy(&w, p, 2);
	if (w == 0)
		return p + 2;
	if (w == 0xffff) {
		if (ROOM(p, end) < 4)
			return NULL;
		if (ord)
			memcpy(ord, p + 2, 2);
		return p + 4;
	}
	*s = (const WCHAR *)p;
	for (; ROOM(p, end) >= 2; p += 2) {
		memcpy(&w, p, 2);
		if (!w)
			return p + 2;
	}
	return NULL;
}

static const BYTE *align4(const BYTE *p, const BYTE *base)
{
	return base + ((p - base + 3) & ~(size_t)3);
}

static int parse_tmpl(const void *data, size_t size, struct tmpl *t)
{
	const BYTE *p = data;
	uintptr_t end;
	WORD ver = 0, sig = 0;

	/* An in-memory template has no size: up to the end of memory. */
	if (size > UINTPTR_MAX - (uintptr_t)p)
		size = UINTPTR_MAX - (uintptr_t)p;
	end = (uintptr_t)p + size;
	WORD ord;

	memset(t, 0, sizeof(*t));
	if (!p || size < 18)
		return -1;
	memcpy(&ver, p, 2);
	memcpy(&sig, p + 2, 2);
	if (ver == 1 && sig == 0xffff) {
		if (size < 26)
			return -1;
		t->ex = 1;
		memcpy(&t->exstyle, p + 8, 4);
		memcpy(&t->style, p + 12, 4);
		memcpy(&t->n, p + 16, 2);
		memcpy(&t->x, p + 18, 2);
		memcpy(&t->y, p + 20, 2);
		memcpy(&t->cx, p + 22, 2);
		memcpy(&t->cy, p + 24, 2);
		p += 26;
	} else {
		memcpy(&t->style, p, 4);
		memcpy(&t->exstyle, p + 4, 4);
		memcpy(&t->n, p + 8, 2);
		memcpy(&t->x, p + 10, 2);
		memcpy(&t->y, p + 12, 2);
		memcpy(&t->cx, p + 14, 2);
		memcpy(&t->cy, p + 16, 2);
		p += 18;
	}
	const WCHAR *menu;
	if (!(p = sz_or_ord(p, end, &menu, NULL)) ||
	    !(p = sz_or_ord(p, end, &t->cls, &ord)) ||
	    !(p = sz_or_ord(p, end, &t->title, NULL)))
		return -1;
	if (!t->cls && ord)
		return -1;	/* a class atom: not here */
	if (t->style & DS_SETFONT) {
		if (ROOM(p, end) < 2)
			return -1;
		memcpy(&t->point, p, 2);
		p += 2;
		if (t->ex) {
			if (ROOM(p, end) < 4)
				return -1;
			memcpy(&t->weight, p, 2);
			t->italic = p[2];
			t->charset = p[3];
			p += 4;
		}
		if (!(p = sz_or_ord(p, end, &t->font, NULL)))
			return -1;
	}
	t->items = p;
	t->end = end;
	return 0;
}

static const WCHAR *ord_class(WORD ord)
{
	static const WCHAR button[] = u"BUTTON", edit[] = u"EDIT",
			   stat[] = u"STATIC", listbox[] = u"LISTBOX",
			   scroll[] = u"SCROLLBAR", combo[] = u"COMBOBOX";

	switch (ord) {
	case 0x80: return button;
	case 0x81: return edit;
	case 0x82: return stat;
	case 0x83: return listbox;
	case 0x84: return scroll;
	case 0x85: return combo;
	default: return NULL;
	}
}

/* The next item from *p (DWORD-aligned from base); NULL at a bad one. */
static const BYTE *parse_item(const struct tmpl *t, const BYTE *base,
			      const BYTE *p, struct item *it)
{
	uintptr_t end = t->end;
	WORD ord, extra;

	memset(it, 0, sizeof(*it));
	p = align4(p, base);
	if (t->ex) {
		if (ROOM(p, end) < 24)
			return NULL;
		memcpy(&it->exstyle, p + 4, 4);
		memcpy(&it->style, p + 8, 4);
		memcpy(&it->x, p + 12, 2);
		memcpy(&it->y, p + 14, 2);
		memcpy(&it->cx, p + 16, 2);
		memcpy(&it->cy, p + 18, 2);
		memcpy(&it->id, p + 20, 4);
		p += 24;
	} else {
		WORD id;
		if (ROOM(p, end) < 18)
			return NULL;
		memcpy(&it->style, p, 4);
		memcpy(&it->exstyle, p + 4, 4);
		memcpy(&it->x, p + 8, 2);
		memcpy(&it->y, p + 10, 2);
		memcpy(&it->cx, p + 12, 2);
		memcpy(&it->cy, p + 14, 2);
		memcpy(&id, p + 16, 2);
		it->id = id;
		p += 18;
	}
	if (!(p = sz_or_ord(p, end, &it->cls, &ord)))
		return NULL;
	if (!it->cls)
		it->cls = ord_class(ord);
	if (!(p = sz_or_ord(p, end, &it->title, &it->title_ord)) ||
	    ROOM(p, end) < 2)
		return NULL;
	memcpy(&extra, p, 2);
	if (extra) {
		if (ROOM(p, end) < (size_t)extra)
			return NULL;
		it->data = p;
		p += extra;
	} else {
		p += 2;
	}
	return p;
}

/* Dialog units to pixels, with base units 8x16 (GetDialogBaseUnits). */
#define DLU_X(v) ((int)(v) * 8 / 4)
#define DLU_Y(v) ((int)(v) * 16 / 8)

static HWND dialog_create(HINSTANCE inst, const void *data, size_t size,
			  HWND owner, DLGPROC proc, LPARAM param, int unicode,
			  int modal, int child)
{
	struct tmpl t;

	API();
	if (parse_tmpl(data, size, &t)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	DWORD style = t.style & ~WS_VISIBLE;
	if (modal)
		style &= ~WS_CHILD;
	if (child) {
		style &= ~(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
			   DS_MODALFRAME | WS_DISABLED);
		style |= WS_CHILD | DS_CONTROL;
		t.style |= WS_VISIBLE;
	}
	if ((style & WS_CHILD) && !owner) {
		win_set_error(ERROR_TLW_WITH_WSCHILD);
		return NULL;
	}
	int x = DLU_X(t.x), y = DLU_Y(t.y);
	int cx = DLU_X(t.cx), cy = DLU_Y(t.cy);
	if (!(style & WS_CHILD) && (style & DS_CENTER)) {
		x = (USER_SCREEN_WIDTH - cx) / 2;
		y = (USER_SCREEN_HEIGHT - cy) / 2;
	}
	struct dlg *d = calloc(1, sizeof(*d));
	if (!d) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	d->proc = proc;
	d->unicode = unicode;
	d->modal = modal;
	HWND h = u.CreateWindowExW(t.exstyle, t.cls ? t.cls : u"#32770",
				   t.title, style, x, y, cx, cy, owner, NULL,
				   inst, (void *)param);
	if (!h) {
		free(d);
		return NULL;
	}
	d->hwnd = h;
	pthread_mutex_lock(&dlg_lock);
	d->next = dialogs;
	dialogs = d;
	pthread_mutex_unlock(&dlg_lock);
	if (!t.cls && !unicode)
		u.SetWindowLongPtrA(h, GWLP_WNDPROC,
				    (LONG_PTR)user_DefDlgProcA);
	u.SetWindowLongPtrW(h, DWLP_DLGPROC, (LONG_PTR)proc);

	if (t.style & DS_SETFONT) {
		d->font = u.CreateFontW(-((int)t.point * 96 + 36) / 72, 0, 0, 0,
					t.weight ? t.weight : 400, t.italic,
					0, 0, t.charset, 0, 0, 0, 0,
					t.font ? t.font : u"MS Shell Dlg");
		sendw(h, WM_SETFONT, (WPARAM)d->font, FALSE);
	}

	/* The controls. */
	const BYTE *p = t.items;
	const BYTE *base = data;
	HWND focus = NULL;
	for (WORD i = 0; i < t.n && u.IsWindow(h); i++) {
		struct item it;
		if (!(p = parse_item(&t, base, p, &it))) {
			vfw_warn("dialog template: item %u is cut short", i);
			break;
		}
		if (!it.cls) {
			vfw_warn("dialog template: item %u has a class atom",
				 i);
			continue;
		}
		const WCHAR *title = it.title;
		WCHAR ord_title[3] = { 0xffff, it.title_ord, 0 };
		if (!title && it.title_ord)
			title = ord_title;
		HWND c = u.CreateWindowExW(it.exstyle | WS_EX_NOPARENTNOTIFY,
					   it.cls, title, it.style | WS_CHILD,
					   DLU_X(it.x), DLU_Y(it.y),
					   DLU_X(it.cx), DLU_Y(it.cy), h,
					   (HMENU)(ULONG_PTR)it.id, inst,
					   (void *)it.data);
		if (!c) {
			if (t.style & DS_NOFAILCREATE)
				continue;
			vfw_warn("dialog template: no control %u", i);
			u.DestroyWindow(h);
			goto gone;
		}
		if (d->font)
			sendw(c, WM_SETFONT, (WPARAM)d->font, FALSE);
		LRESULT code = sendw(c, WM_GETDLGCODE, 0, 0);
		if ((code & DLGC_DEFPUSHBUTTON) && !d->defid)
			d->defid = (UINT)it.id;
		if (!focus && tab_stop(c))
			focus = c;
	}
	if (!u.IsWindow(h))
		goto gone;
	if (!d->defid)
		d->defid = IDOK;

	LRESULT r = sendw(h, WM_INITDIALOG, (WPARAM)focus, param);
	if (!u.IsWindow(h))
		goto gone;
	if (r && focus)
		next_ctl(h, focus);
	if ((t.style & WS_VISIBLE) && !d->ended)
		u.ShowWindow(h, SW_SHOWNORMAL);
	return h;
gone:
	/* Destroyed while being made: a modal one's record is still ours. */
	if (modal)
		free(d);
	return NULL;
}

/* An RT_DIALOG resource: its data and size, or NULL. */
static const void *dialog_resource(HINSTANCE inst, const void *name,
				   int unicode, size_t *size)
{
	API();
	HRSRC r = unicode ? u.FindResourceW(inst, name, MAKEINTRESOURCEW(5)) :
		  u.FindResourceA(inst, name, (LPCSTR)MAKEINTRESOURCEW(5));
	HGLOBAL g = r ? u.LoadResource(inst, r) : NULL;
	if (!g) {
		win_set_error(ERROR_RESOURCE_NAME_NOT_FOUND);
		return NULL;
	}
	*size = u.SizeofResource(inst, r);
	return g;
}

static BOOL is_dialog_message(HWND dlg, LPMSG m, int unicode);

static int pump(HWND h, struct dlg *d, int *quit)
{
	MSG m;
	int n = 0;

	while (!d->ended && !d->gone &&
	       u.PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
		n++;
		if (m.message == WM_QUIT) {
			*quit = 1;
			u.PostQuitMessage((int)m.wParam);
			break;
		}
		if (!is_dialog_message(h, &m, d->unicode)) {
			u.TranslateMessage(&m);
			if (d->unicode)
				u.DispatchMessageW(&m);
			else
				u.DispatchMessageA(&m);
		}
	}
	return n;
}

static char *title_utf8(HWND h)
{
	WCHAR *t = text_of(h);
	char *s = t ? utf16_to_utf8_dup(t) : NULL;
	free(t);
	return s ? s : strdup("");
}

static INT_PTR dialog_box(HINSTANCE inst, const void *data, size_t size,
			  HWND owner, DLGPROC proc, LPARAM param, int unicode)
{
	API();
	BOOL reenable = owner && u.IsWindowEnabled(owner);
	if (reenable)
		u.EnableWindow(owner, FALSE);
	HWND h = dialog_create(inst, data, size, owner, proc, param, unicode,
			       1, 0);
	struct dlg *d = h ? dlg_find(h) : NULL;
	INT_PTR result = -1;
	if (d) {
		int quit = 0;
		pump(h, d, &quit);
		if (!d->ended && !d->gone && !quit) {
			const char *ans = getenv("VFW_DIALOG");
			int ok = ans && !strcasecmp(ans, "ok");
			int id = ok ? (int)dlg_defid(h) : IDCANCEL;
			char *title = title_utf8(h);
			fprintf(stderr, "DialogBox [%s]\n  (no GUI: pressing %s)\n",
				title, ok ? "the default button" : "Cancel");
			free(title);
			dlg_command(h, id);
			pump(h, d, &quit);
			if (!d->ended && !d->gone) {
				d->ended = 1;
				d->result = id;
			}
		}
		result = d->ended ? d->result : quit ? 0 : -1;
		if (!d->gone)
			u.DestroyWindow(h);
		free(d);
	}
	if (reenable)
		u.EnableWindow(owner, TRUE);
	return result;
}

WINFN(HWND) user_CreateDialogIndirectParamW(HINSTANCE inst, const void *tmpl,
					    HWND parent, DLGPROC proc,
					    LPARAM param)
{
	return dialog_create(inst, tmpl, (size_t)-1, parent, proc, param, 1, 0,
			     0);
}

WINFN(HWND) user_CreateDialogIndirectParamA(HINSTANCE inst, const void *tmpl,
					    HWND parent, DLGPROC proc,
					    LPARAM param)
{
	return dialog_create(inst, tmpl, (size_t)-1, parent, proc, param, 0, 0,
			     0);
}

WINFN(HWND) user_CreateDialogParamW(HINSTANCE inst, LPCWSTR name, HWND parent,
				    DLGPROC proc, LPARAM param)
{
	size_t size;
	const void *t = dialog_resource(inst, name, 1, &size);
	return t ? dialog_create(inst, t, size, parent, proc, param, 1, 0, 0) :
		   NULL;
}

WINFN(HWND) user_CreateDialogParamA(HINSTANCE inst, LPCSTR name, HWND parent,
				    DLGPROC proc, LPARAM param)
{
	size_t size;
	const void *t = dialog_resource(inst, name, 0, &size);
	return t ? dialog_create(inst, t, size, parent, proc, param, 0, 0, 0) :
		   NULL;
}

WINFN(INT_PTR) user_DialogBoxIndirectParamW(HINSTANCE inst, const void *tmpl,
					    HWND parent, DLGPROC proc,
					    LPARAM param)
{
	return dialog_box(inst, tmpl, (size_t)-1, parent, proc, param, 1);
}

WINFN(INT_PTR) user_DialogBoxIndirectParamA(HINSTANCE inst, const void *tmpl,
					    HWND parent, DLGPROC proc,
					    LPARAM param)
{
	return dialog_box(inst, tmpl, (size_t)-1, parent, proc, param, 0);
}

WINFN(INT_PTR) user_DialogBoxParamW(HINSTANCE inst, LPCWSTR name, HWND parent,
				    DLGPROC proc, LPARAM param)
{
	size_t size;
	const void *t = dialog_resource(inst, name, 1, &size);
	return t ? dialog_box(inst, t, size, parent, proc, param, 1) : -1;
}

WINFN(INT_PTR) user_DialogBoxParamA(HINSTANCE inst, LPCSTR name, HWND parent,
				    DLGPROC proc, LPARAM param)
{
	size_t size;
	const void *t = dialog_resource(inst, name, 0, &size);
	return t ? dialog_box(inst, t, size, parent, proc, param, 0) : -1;
}

WINFN(BOOL) user_EndDialog(HWND h, INT_PTR result)
{
	API();
	struct dlg *d = dlg_find(h);

	if (!d) {
		win_set_error(ERROR_INVALID_WINDOW_HANDLE);
		return FALSE;
	}
	d->ended = 1;
	d->result = result;
	u.ShowWindow(h, SW_HIDE);
	u.PostMessageW(h, WM_NULL, 0, 0);	/* wakes a modal loop */
	return TRUE;
}

/* The keyboard interface. */
static BOOL is_dialog_message(HWND dlg, LPMSG m, int unicode)
{
	API();
	if (!dlg || !m || !m->hwnd ||
	    (m->hwnd != dlg && !u.IsChild(dlg, m->hwnd)))
		return FALSE;
	if (m->message == WM_KEYDOWN || m->message == WM_CHAR) {
		LRESULT code = sendw(m->hwnd, WM_GETDLGCODE, m->wParam,
				     (LPARAM)m);
		if (code & (DLGC_WANTALLKEYS | DLGC_WANTMESSAGE))
			goto dispatch;
		if (m->message == WM_CHAR) {
			if (m->wParam == '\t' || m->wParam == '\r' ||
			    m->wParam == 0x1b)
				return TRUE;	/* their WM_KEYDOWN did it */
			goto dispatch;
		}
		switch (m->wParam) {
		case VK_TAB:
			if (code & DLGC_WANTTAB)
				goto dispatch;
			sendw(dlg, WM_NEXTDLGCTL, u.GetKeyState(VK_SHIFT) < 0,
			      0);
			return TRUE;
		case VK_RETURN: {
			int id = (int)dlg_defid(dlg);
			if (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))
				id = u.GetDlgCtrlID(m->hwnd);
			dlg_command(dlg, id);
			return TRUE;
		}
		case VK_ESCAPE:
			dlg_command(dlg, IDCANCEL);
			return TRUE;
		default:
			break;
		}
	}
dispatch:
	u.TranslateMessage(m);
	if (unicode)
		u.DispatchMessageW(m);
	else
		u.DispatchMessageA(m);
	return TRUE;
}

WINFN(BOOL) user_IsDialogMessageW(HWND dlg, LPMSG m)
{
	return is_dialog_message(dlg, m, 1);
}

WINFN(BOOL) user_IsDialogMessageA(HWND dlg, LPMSG m)
{
	return is_dialog_message(dlg, m, 0);
}

WINFN(HWND) user_GetNextDlgTabItem(HWND dlg, HWND ctl, BOOL previous)
{
	API();
	return u.IsWindow(dlg) ? next_tab(dlg, ctl, previous) : NULL;
}

/* The controls from ctl's group: from a WS_GROUP one to before the next. */
WINFN(HWND) user_GetNextDlgGroupItem(HWND dlg, HWND ctl, BOOL previous)
{
	int n, at = -1, first = 0, last;

	API();
	HWND *list = children(dlg, &n), found = ctl;
	if (!list)
		return NULL;
	for (int i = 0; i < n; i++)
		if (list[i] == ctl)
			at = i;
	if (at < 0) {
		found = n ? list[0] : NULL;
		free(list);
		return found;
	}
	for (int i = at; i >= 0; i--)
		if (style_of(list[i]) & WS_GROUP) {
			first = i;
			break;
		}
	last = n - 1;
	for (int i = at + 1; i < n; i++)
		if (style_of(list[i]) & WS_GROUP) {
			last = i - 1;
			break;
		}
	int size = last - first + 1;
	for (int k = 1; k < size; k++) {
		int i = first + ((at - first + (previous ? size - k : k)) %
				 size);
		DWORD st = style_of(list[i]);
		if ((st & WS_VISIBLE) && !(st & WS_DISABLED)) {
			found = list[i];
			break;
		}
	}
	free(list);
	return found;
}

WINFN(BOOL) user_CheckRadioButton(HWND dlg, int first, int last, int check)
{
	API();
	for (int id = first; id <= last; id++) {
		HWND c = u.GetDlgItem(dlg, id);
		if (c)
			sendw(c, BM_SETCHECK, id == check, 0);
	}
	return TRUE;
}

WINFN(LONG) user_GetDialogBaseUnits(void)
{
	return MAKELONG(8, 16);
}

WINFN(BOOL) user_MapDialogRect(HWND dlg, LPRECT rc)
{
	if (!rc)
		return FALSE;
	rc->left = DLU_X(rc->left);
	rc->right = DLU_X(rc->right);
	rc->top = DLU_Y(rc->top);
	rc->bottom = DLU_Y(rc->bottom);
	return TRUE;
}

/* Static controls: text, or an icon or bitmap. */

struct stat {
	HANDLE image;
};

LRESULT STUB_WINAPI user_static_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	API();
	struct stat *s = state(h, msg == WM_NCCREATE ? sizeof(*s) : 0);

	switch (msg) {
	case WM_NCCREATE: {
		if (!s)
			return FALSE;
		const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
		/* A template's ordinal title: an icon or bitmap resource. */
		if (cs && cs->lpszName && !IS_INTRESOURCE(cs->lpszName) &&
		    cs->lpszName[0] == 0xffff) {
			CREATESTRUCTW c = *cs;
			c.lpszName = NULL;
			DWORD type = cs->style & SS_TYPEMASK;
			if (type == SS_ICON)
				s->image = u.LoadIconW(cs->hInstance,
					MAKEINTRESOURCEW(cs->lpszName[1]));
			return user_DefWindowProcW(h, msg, wp, (LPARAM)&c);
		}
		break;
	}
	case WM_NCDESTROY:
		free(s);
		u.SetWindowLongPtrW(h, 0, 0);
		break;
	case STM_SETICON:
	case STM_SETIMAGE: {
		if (!s)
			return 0;
		HANDLE old = s->image;
		s->image = (HANDLE)(msg == STM_SETICON ? wp : (WPARAM)lp);
		return (LRESULT)old;
	}
	case STM_GETICON:
	case STM_GETIMAGE:
		return s ? (LRESULT)s->image : 0;
	case WM_GETDLGCODE:
		return DLGC_STATIC;
	case WM_LBUTTONUP:
		if (style_of(h) & SS_NOTIFY)
			notify(h, STN_CLICKED);
		return 0;
	}
	return user_DefWindowProcW(h, msg, wp, lp);
}

/* Buttons. */

struct button {
	UINT check;
	UINT state;		/* BST_PUSHED, BST_FOCUS */
	HANDLE image;
};

static DWORD button_type(HWND h)
{
	return style_of(h) & BS_TYPEMASK;
}

static int checkable(DWORD type)
{
	return type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
	       type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON ||
	       type == BS_3STATE || type == BS_AUTO3STATE;
}

/* Unchecks the other automatic radio buttons in h's group. */
static void radio_group(HWND h)
{
	HWND parent = u.GetParent(h);
	int n, at = -1, first = 0;
	HWND *list = parent ? children(parent, &n) : NULL;

	if (!list)
		return;
	for (int i = 0; i < n; i++)
		if (list[i] == h)
			at = i;
	for (int i = at; i >= 0; i--)
		if (style_of(list[i]) & WS_GROUP) {
			first = i;
			break;
		}
	for (int i = first; at >= 0 && i < n; i++) {
		if (i > first && (style_of(list[i]) & WS_GROUP))
			break;
		if (list[i] != h && button_type(list[i]) == BS_AUTORADIOBUTTON &&
		    sendw(list[i], WM_GETDLGCODE, 0, 0) & DLGC_RADIOBUTTON)
			sendw(list[i], BM_SETCHECK, BST_UNCHECKED, 0);
	}
	free(list);
}

static void button_click(HWND h, struct button *b)
{
	DWORD type = button_type(h);

	if (!u.IsWindowEnabled(h))
		return;
	switch (type) {
	case BS_AUTOCHECKBOX:
		b->check = b->check ? BST_UNCHECKED : BST_CHECKED;
		break;
	case BS_AUTO3STATE:
		b->check = (b->check + 1) % 3;
		break;
	case BS_AUTORADIOBUTTON:
		b->check = BST_CHECKED;
		u.SetWindowLongW(h, GWL_STYLE, (LONG)(style_of(h) | WS_TABSTOP));
		radio_group(h);
		break;
	default:
		break;
	}
	notify(h, BN_CLICKED);
}

LRESULT STUB_WINAPI user_button_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	API();
	struct button *b = state(h, msg == WM_NCCREATE ? sizeof(*b) : 0);
	DWORD type;

	switch (msg) {
	case WM_NCCREATE:
		if (!b)
			return FALSE;
		break;
	case WM_NCDESTROY:
		free(b);
		u.SetWindowLongPtrW(h, 0, 0);
		break;
	case BM_GETCHECK:
		return b && checkable(button_type(h)) ? b->check : 0;
	case BM_SETCHECK:
		type = button_type(h);
		if (b && checkable(type)) {
			UINT v = (UINT)wp;
			if (type != BS_3STATE && type != BS_AUTO3STATE)
				v = v ? BST_CHECKED : BST_UNCHECKED;
			else if (v > BST_INDETERMINATE)
				v = BST_INDETERMINATE;
			b->check = v;
		}
		return 0;
	case BM_GETSTATE:
		return b ? (LRESULT)(b->state |
				     (checkable(button_type(h)) ? b->check : 0)) :
			   0;
	case BM_SETSTATE:
		if (b)
			b->state = wp ? b->state | BST_PUSHED :
				   b->state & ~BST_PUSHED;
		return 0;
	case BM_SETSTYLE:
		u.SetWindowLongW(h, GWL_STYLE,
				 (LONG)((style_of(h) & ~BS_TYPEMASK) |
					(wp & BS_TYPEMASK)));
		return 0;
	case BM_CLICK:
		if (b)
			button_click(h, b);
		return 0;
	case BM_SETIMAGE: {
		if (!b)
			return 0;
		HANDLE old = b->image;
		b->image = (HANDLE)lp;
		return (LRESULT)old;
	}
	case BM_GETIMAGE:
		return b ? (LRESULT)b->image : 0;
	case WM_LBUTTONDOWN:
		if (b)
			b->state |= BST_PUSHED;
		return 0;
	case WM_LBUTTONUP:
		if (b && (b->state & BST_PUSHED)) {
			b->state &= ~BST_PUSHED;
			button_click(h, b);
		}
		return 0;
	case WM_KEYUP:
		if (b && wp == ' ')
			button_click(h, b);
		return 0;
	case WM_SETFOCUS:
		if (b)
			b->state |= BST_FOCUS;
		return 0;
	case WM_KILLFOCUS:
		if (b)
			b->state &= ~BST_FOCUS;
		return 0;
	case WM_GETDLGCODE:
		switch (button_type(h)) {
		case BS_DEFPUSHBUTTON:
			return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
		case BS_PUSHBUTTON:
		case BS_PUSHBOX:
		case BS_OWNERDRAW:
			return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
		case BS_RADIOBUTTON:
		case BS_AUTORADIOBUTTON:
			return DLGC_BUTTON | DLGC_RADIOBUTTON;
		case BS_GROUPBOX:
			return DLGC_STATIC;
		default:
			return DLGC_BUTTON;
		}
	}
	return user_DefWindowProcW(h, msg, wp, lp);
}

/* Edit controls. The text is the window's; this is the rest. */

struct edit {
	DWORD start, end;	/* the selection; the caret is at end */
	UINT limit;
	BOOL modified;
	WCHAR password;
};

static void edit_changed(HWND h)
{
	notify(h, EN_UPDATE);
	notify(h, EN_CHANGE);
}

/* Replaces the selection with s[0..n), within the limit. */
static BOOL edit_replace(HWND h, struct edit *e, const WCHAR *s, size_t n,
			 BOOL user)
{
	WCHAR *t = text_of(h);
	if (!t)
		return FALSE;
	size_t len = wstrlen(t);
	DWORD a = e->start < e->end ? e->start : e->end;
	DWORD b = e->start < e->end ? e->end : e->start;
	if (a > len)
		a = (DWORD)len;
	if (b > len)
		b = (DWORD)len;
	size_t keep = len - (b - a);
	BOOL cut = FALSE;
	if (keep + n > e->limit) {
		n = keep < e->limit ? e->limit - keep : 0;
		cut = TRUE;
	}
	WCHAR *out = malloc((keep + n + 1) * sizeof(WCHAR));
	if (!out) {
		free(t);
		return FALSE;
	}
	DWORD style = style_of(h);
	memcpy(out, t, a * sizeof(WCHAR));
	for (size_t i = 0; i < n; i++)
		out[a + i] = style & ES_UPPERCASE ? wupper(s[i]) :
			     style & ES_LOWERCASE ? wlower(s[i]) : s[i];
	memcpy(out + a + n, t + b, (len - b) * sizeof(WCHAR));
	out[keep + n] = 0;
	free(t);
	user_DefWindowProcW(h, WM_SETTEXT, 0, (LPARAM)out);
	free(out);
	e->start = e->end = a + (DWORD)n;
	e->modified = TRUE;
	if (cut)
		notify(h, EN_MAXTEXT);
	if (n || a != b || user)
		edit_changed(h);
	return TRUE;
}

static DWORD edit_len(HWND h)
{
	return (DWORD)u.GetWindowTextLengthW(h);
}

LRESULT STUB_WINAPI user_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	API();
	struct edit *e = state(h, msg == WM_NCCREATE ? sizeof(*e) : 0);
	DWORD style = 0;

	if (msg != WM_NCCREATE && msg != WM_NCDESTROY && !e)
		return user_DefWindowProcW(h, msg, wp, lp);
	switch (msg) {
	case WM_NCCREATE:
		if (!e)
			return FALSE;
		e->limit = 30000;
		if (((const CREATESTRUCTW *)lp)->style & ES_PASSWORD)
			e->password = '*';
		break;
	case WM_NCDESTROY:
		free(e);
		u.SetWindowLongPtrW(h, 0, 0);
		break;
	case WM_SETTEXT: {
		LRESULT r = user_DefWindowProcW(h, msg, wp, lp);
		e->start = e->end = 0;
		e->modified = FALSE;
		edit_changed(h);
		return r;
	}
	case EM_GETSEL:
		if (wp)
			*(DWORD *)wp = e->start;
		if (lp)
			*(DWORD *)lp = e->end;
		return MAKELONG(e->start > 0xffff ? 0xffff : e->start,
				e->end > 0xffff ? 0xffff : e->end);
	case EM_SETSEL: {
		DWORD len = edit_len(h);
		LONG a = (LONG)wp, b = (LONG)lp;
		if (a == -1) {
			e->start = e->end;
			return 0;
		}
		if (b < 0 || (DWORD)b > len)
			b = (LONG)len;
		if (a < 0 || (DWORD)a > len)
			a = (LONG)len;
		e->start = (DWORD)a;
		e->end = (DWORD)b;
		return 0;
	}
	case EM_REPLACESEL: {
		const WCHAR *s = (const WCHAR *)lp;
		if (s)
			edit_replace(h, e, s, wstrlen(s), FALSE);
		return 0;
	}
	case EM_LIMITTEXT:
		e->limit = wp ? (UINT)wp : 0x7ffffffe;
		return 0;
	case EM_GETLIMITTEXT:
		return e->limit;
	case EM_GETMODIFY:
		return e->modified;
	case EM_SETMODIFY:
		e->modified = (BOOL)wp;
		return 0;
	case EM_SETREADONLY:
		style = style_of(h);
		u.SetWindowLongW(h, GWL_STYLE, (LONG)(wp ? style | ES_READONLY :
						      style & ~ES_READONLY));
		return TRUE;
	case EM_SETPASSWORDCHAR:
		e->password = (WCHAR)wp;
		return 0;
	case EM_GETPASSWORDCHAR:
		return e->password;
	case EM_CANUNDO:
	case EM_UNDO:
		return FALSE;
	case EM_EMPTYUNDOBUFFER:
		return 0;
	case EM_GETLINECOUNT:
	case EM_LINELENGTH: {
		WCHAR *t = text_of(h);
		LRESULT n = 1;
		if (!t)
			return msg == EM_GETLINECOUNT;
		if (!(style_of(h) & ES_MULTILINE)) {
			n = msg == EM_GETLINECOUNT ? 1 : (LRESULT)wstrlen(t);
		} else if (msg == EM_GETLINECOUNT) {
			for (WCHAR *p = t; *p; p++)
				n += *p == '\n';
		} else {
			size_t len = wstrlen(t);
			size_t at = (LONG)wp < 0 ? e->end : (size_t)wp;
			if (at > len)
				at = len;
			size_t a = at, b = at;
			while (a && t[a - 1] != '\n')
				a--;
			while (b < len && t[b] != '\r' && t[b] != '\n')
				b++;
			n = (LRESULT)(b - a);
		}
		free(t);
		return n;
	}
	case WM_CHAR: {
		WCHAR c = (WCHAR)wp;
		style = style_of(h);
		if (style & ES_READONLY)
			return 0;
		if (c == 8) {	/* backspace */
			if (e->start == e->end && e->end)
				e->start = e->end - 1;
			edit_replace(h, e, NULL, 0, TRUE);
			return 0;
		}
		if (c == '\r' && (style & ES_MULTILINE)) {
			edit_replace(h, e, u"\r\n", 2, TRUE);
			return 0;
		}
		if (c < 0x20)
			return 0;
		if ((style & ES_NUMBER) && (c < '0' || c > '9'))
			return 0;
		edit_replace(h, e, &c, 1, TRUE);
		return 0;
	}
	case WM_KEYDOWN: {
		DWORD len = edit_len(h);
		switch (wp) {
		case VK_DELETE:
			if (style_of(h) & ES_READONLY)
				return 0;
			if (e->start == e->end && e->end < len)
				e->end++;
			edit_replace(h, e, NULL, 0, TRUE);
			break;
		case VK_LEFT:
			e->start = e->end = e->end ? e->end - 1 : 0;
			break;
		case VK_RIGHT:
			e->start = e->end = e->end < len ? e->end + 1 : len;
			break;
		case VK_HOME:
			e->start = e->end = 0;
			break;
		case VK_END:
			e->start = e->end = len;
			break;
		}
		return 0;
	}
	case WM_SETFOCUS:
		notify(h, EN_SETFOCUS);
		return 0;
	case WM_KILLFOCUS:
		notify(h, EN_KILLFOCUS);
		return 0;
	case WM_GETDLGCODE: {
		LRESULT code = DLGC_WANTCHARS | DLGC_HASSETSEL |
			       DLGC_WANTARROWS;
		const MSG *m = (const MSG *)lp;
		if ((style_of(h) & ES_MULTILINE)) {
			code |= DLGC_WANTALLKEYS;
			if (m && m->message == WM_KEYDOWN &&
			    (m->wParam == VK_ESCAPE || m->wParam == VK_TAB ||
			     (m->wParam == VK_RETURN &&
			      !(style_of(h) & ES_WANTRETURN))))
				code &= ~DLGC_WANTALLKEYS;
		}
		return code;
	}
	}
	return user_DefWindowProcW(h, msg, wp, lp);
}

/* List boxes, and the list of a combo box. */

struct lbitem {
	WCHAR *text;		/* NULL for owner-drawn data */
	ULONG_PTR data;
	BOOL sel;
};

struct lbox {
	struct lbitem *it;
	int n, cap;
	int cur;		/* the selection (single) or caret (multiple) */
	int top, anchor;
	int height;
	BOOL strings;		/* has strings, or data in their place */
	BOOL sorted;
	BOOL multi;
	BOOL nosel;
};

static void lb_init(struct lbox *l, BOOL strings, BOOL sorted, BOOL multi,
		    BOOL nosel)
{
	l->cur = -1;
	l->height = 16;
	l->strings = strings;
	l->sorted = sorted && strings;
	l->multi = multi;
	l->nosel = nosel;
}

static void lb_free(struct lbox *l)
{
	for (int i = 0; i < l->n; i++)
		free(l->it[i].text);
	free(l->it);
	l->it = NULL;
	l->n = l->cap = 0;
	l->cur = -1;
	l->top = l->anchor = 0;
}

/* Adds at index (-1: the end, or its sorted place); its index or LB_ERR*. */
static LRESULT lb_insert(struct lbox *l, int index, LPARAM lp, BOOL add)
{
	if (index < -1 || index > l->n)
		return LB_ERR;
	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 8;
		struct lbitem *g = realloc(l->it, cap * sizeof(*g));
		if (!g)
			return LB_ERRSPACE;
		l->it = g;
		l->cap = cap;
	}
	struct lbitem item = { NULL, 0, FALSE };
	if (l->strings) {
		const WCHAR *s = lp ? (const WCHAR *)lp : u"";
		if (!(item.text = wdup_n(s, wstrlen(s))))
			return LB_ERRSPACE;
	} else {
		item.data = (ULONG_PTR)lp;
	}
	if (index < 0) {
		index = l->n;
		if (add && l->sorted)
			for (index = 0; index < l->n &&
			     wcmpi(l->it[index].text, item.text, 0) <= 0;
			     index++)
				;
	}
	memmove(l->it + index + 1, l->it + index,
		(l->n - index) * sizeof(*l->it));
	l->it[index] = item;
	l->n++;
	if (l->cur >= index)
		l->cur++;
	return index;
}

static LRESULT lb_delete(struct lbox *l, int index)
{
	if (index < 0 || index >= l->n)
		return LB_ERR;
	free(l->it[index].text);
	memmove(l->it + index, l->it + index + 1,
		(l->n - index - 1) * sizeof(*l->it));
	l->n--;
	if (l->cur == index)
		l->cur = l->multi && l->n ? (index < l->n ? index : l->n - 1) :
			 -1;
	else if (l->cur > index)
		l->cur--;
	if (l->top >= l->n)
		l->top = l->n ? l->n - 1 : 0;
	return l->n;
}

static LRESULT lb_find(const struct lbox *l, int start, LPARAM lp, BOOL exact)
{
	if (!l->n)
		return LB_ERR;
	if (start < -1 || start >= l->n)
		start = -1;
	for (int k = 1; k <= l->n; k++) {
		int i = (start + k) % l->n;
		const struct lbitem *it = &l->it[i];
		if (!l->strings) {
			if (it->data == (ULONG_PTR)lp)
				return i;
		} else if (lp && !wcmpi(it->text, (const WCHAR *)lp, !exact)) {
			return i;
		}
	}
	return LB_ERR;
}

/* The text of an item into buf (if not NULL): its length, or LB_ERR. */
static LRESULT lb_text(const struct lbox *l, int i, WCHAR *buf)
{
	if (i < 0 || i >= l->n)
		return LB_ERR;
	if (!l->strings) {
		if (buf)
			memcpy(buf, &l->it[i].data, sizeof(ULONG_PTR));
		return sizeof(ULONG_PTR);
	}
	size_t n = wstrlen(l->it[i].text);
	if (buf)
		memcpy(buf, l->it[i].text, (n + 1) * sizeof(WCHAR));
	return (LRESULT)n;
}

static void lb_snapshot(const struct lbox *l, int i, struct win_lbtext *q)
{
	q->done = 1;
	q->text = NULL;
	if (i < 0 || i >= l->n || !l->strings) {
		q->result = i < 0 || i >= l->n ? LB_ERR : 0;
		return;
	}
	q->text = wdup_n(l->it[i].text, wstrlen(l->it[i].text));
	q->result = q->text ? (LRESULT)wstrlen(q->text) : LB_ERRSPACE;
}

static void lb_notify(HWND h, UINT code)
{
	if (style_of(h) & LBS_NOTIFY)
		notify(h, code);
}

/* The list box messages; *handled is 0 for others. */
static LRESULT lb_message(HWND h, struct lbox *l, UINT msg, WPARAM wp,
			  LPARAM lp, int *handled)
{
	*handled = 1;
	switch (msg) {
	case LB_ADDSTRING:
		return lb_insert(l, -1, lp, TRUE);
	case LB_INSERTSTRING:
		return lb_insert(l, (int)wp, lp, FALSE);
	case LB_DELETESTRING:
		return lb_delete(l, (int)wp);
	case LB_RESETCONTENT:
		lb_free(l);
		return 0;
	case LB_GETCOUNT:
		return l->n;
	case LB_GETTEXT:
		return lb_text(l, (int)wp, (WCHAR *)lp);
	case LB_GETTEXTLEN:
		return lb_text(l, (int)wp, NULL);
	case WIN_LBTEXT_SNAPSHOT:
		lb_snapshot(l, (int)wp, (struct win_lbtext *)lp);
		return 0;
	case LB_SETCURSEL:
		if (l->multi || l->nosel || (int)wp >= l->n || (int)wp < -1)
			return LB_ERR;
		l->cur = (int)wp;
		return l->cur < 0 ? LB_ERR : l->cur;
	case LB_GETCURSEL:
		return l->cur;
	case LB_SETSEL: {
		int i = (int)lp;
		if (!l->multi || i < -1 || i >= l->n)
			return LB_ERR;
		for (int k = 0; k < l->n; k++)
			if (i < 0 || k == i)
				l->it[k].sel = (BOOL)wp;
		if (i >= 0) {
			l->cur = i;
			if (wp)
				l->anchor = i;
		}
		return 0;
	}
	case LB_GETSEL: {
		int i = (int)wp;
		if (i < 0 || i >= l->n)
			return LB_ERR;
		return l->multi ? l->it[i].sel : i == l->cur;
	}
	case LB_GETSELCOUNT: {
		if (!l->multi)
			return LB_ERR;
		int n = 0;
		for (int k = 0; k < l->n; k++)
			n += l->it[k].sel;
		return n;
	}
	case LB_GETSELITEMS: {
		if (!l->multi)
			return LB_ERR;
		int n = 0, *out = (int *)lp;
		for (int k = 0; k < l->n && n < (int)wp; k++)
			if (l->it[k].sel && out)
				out[n++] = k;
		return n;
	}
	case LB_SELITEMRANGE: {
		if (!l->multi)
			return LB_ERR;
		int a = LOWORD(lp), b = HIWORD(lp);
		if (a > b) {
			int t = a;
			a = b;
			b = t;
		}
		for (int k = a; k <= b && k < l->n; k++)
			l->it[k].sel = (BOOL)wp;
		return 0;
	}
	case LB_SELECTSTRING: {
		LRESULT i = lb_find(l, (int)wp, lp, FALSE);
		if (i == LB_ERR)
			return LB_ERR;
		if (l->multi)
			return LB_ERR;
		l->cur = (int)i;
		return i;
	}
	case LB_FINDSTRING:
		return lb_find(l, (int)wp, lp, FALSE);
	case LB_FINDSTRINGEXACT:
		return lb_find(l, (int)wp, lp, TRUE);
	case LB_GETITEMDATA:
		if ((int)wp < 0 || (int)wp >= l->n)
			return LB_ERR;
		return (LRESULT)l->it[wp].data;
	case LB_SETITEMDATA:
		if ((int)wp < 0 || (int)wp >= l->n)
			return LB_ERR;
		l->it[wp].data = (ULONG_PTR)lp;
		return TRUE;
	case LB_GETTOPINDEX:
		return l->top;
	case LB_SETTOPINDEX:
		if ((int)wp < 0 || ((int)wp >= l->n && l->n))
			return LB_ERR;
		l->top = (int)wp;
		return 0;
	case LB_GETANCHORINDEX:
		return l->anchor;
	case LB_SETANCHORINDEX:
		if ((int)wp < 0 || (int)wp >= l->n)
			return LB_ERR;
		l->anchor = (int)wp;
		return 0;
	case LB_GETCARETINDEX:
		return l->cur < 0 ? 0 : l->cur;
	case LB_SETCARETINDEX:
		if ((int)wp < 0 || (int)wp >= l->n)
			return LB_ERR;
		if (l->multi)
			l->cur = (int)wp;
		return 0;
	case LB_GETITEMHEIGHT:
		return l->height;
	case LB_SETITEMHEIGHT:
		if (LOWORD(lp) == 0 || LOWORD(lp) > 255)
			return LB_ERR;
		l->height = LOWORD(lp);
		return 0;
	case LB_INITSTORAGE: {
		int want = l->n + (int)wp;
		if (want > l->cap) {
			struct lbitem *g = realloc(l->it, want * sizeof(*g));
			if (!g)
				return LB_ERRSPACE;
			l->it = g;
			l->cap = want;
		}
		return l->cap;
	}
	case LB_SETTABSTOPS:
		return TRUE;
	case LB_SETHORIZONTALEXTENT:
	case LB_SETCOLUMNWIDTH:
		return 0;
	default:
		*handled = 0;
		return 0;
	}
}

/* Moves the selection by keyboard: whether it changed. */
static BOOL lb_key(struct lbox *l, WPARAM key)
{
	int to = l->cur;

	if (!l->n || l->nosel)
		return FALSE;
	switch (key) {
	case VK_UP: to = to > 0 ? to - 1 : 0; break;
	case VK_DOWN: to = to < l->n - 1 ? to + 1 : l->n - 1; break;
	case VK_HOME: to = 0; break;
	case VK_END: to = l->n - 1; break;
	default: return FALSE;
	}
	if (to == l->cur && (!l->multi || l->it[to].sel))
		return FALSE;
	if (l->multi) {
		for (int k = 0; k < l->n; k++)
			l->it[k].sel = k == to;
		l->anchor = to;
	}
	l->cur = to;
	return TRUE;
}

LRESULT STUB_WINAPI user_listbox_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	API();
	struct lbox *l = state(h, msg == WM_NCCREATE ? sizeof(*l) : 0);
	int handled;

	if (msg == WM_NCCREATE) {
		if (!l)
			return FALSE;
		DWORD st = ((const CREATESTRUCTW *)lp)->style;
		BOOL owner = (st & (LBS_OWNERDRAWFIXED |
				    LBS_OWNERDRAWVARIABLE)) != 0;
		lb_init(l, !owner || (st & LBS_HASSTRINGS), (st & LBS_SORT) != 0,
			(st & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0,
			(st & LBS_NOSEL) != 0);
		return user_DefWindowProcW(h, msg, wp, lp);
	}
	if (!l)
		return user_DefWindowProcW(h, msg, wp, lp);
	if (msg == WM_NCDESTROY) {
		lb_free(l);
		free(l);
		u.SetWindowLongPtrW(h, 0, 0);
		return user_DefWindowProcW(h, msg, wp, lp);
	}
	LRESULT r = lb_message(h, l, msg, wp, lp, &handled);
	if (handled)
		return r;
	switch (msg) {
	case WM_KEYDOWN:
		if (lb_key(l, wp))
			lb_notify(h, LBN_SELCHANGE);
		return 0;
	case WM_SETFOCUS:
		lb_notify(h, LBN_SETFOCUS);
		return 0;
	case WM_KILLFOCUS:
		lb_notify(h, LBN_KILLFOCUS);
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	}
	return user_DefWindowProcW(h, msg, wp, lp);
}

/* Combo boxes: a list, and the text of the selection or of the edit. */

struct combo {
	struct lbox list;
	BOOL dropped;
	BOOL extended_ui;
	int dropped_width;
	UINT limit;
	DWORD sel;		/* the edit's selection, MAKELONG(start, end) */
};

static DWORD combo_type(HWND h)
{
	return style_of(h) & 3;
}

static void combo_show(HWND h, struct combo *c)
{
	WCHAR *t = NULL;

	if (c->list.cur >= 0 && c->list.strings)
		t = c->list.it[c->list.cur].text;
	user_DefWindowProcW(h, WM_SETTEXT, 0, (LPARAM)(t ? t : u""));
}

/* Translates a combo message to its list box one, or 0. */
static UINT combo_to_list(UINT msg)
{
	switch (msg) {
	case CB_ADDSTRING: return LB_ADDSTRING;
	case CB_INSERTSTRING: return LB_INSERTSTRING;
	case CB_DELETESTRING: return LB_DELETESTRING;
	case CB_RESETCONTENT: return LB_RESETCONTENT;
	case CB_GETCOUNT: return LB_GETCOUNT;
	case CB_GETLBTEXT: return LB_GETTEXT;
	case CB_GETLBTEXTLEN: return LB_GETTEXTLEN;
	case CB_FINDSTRING: return LB_FINDSTRING;
	case CB_FINDSTRINGEXACT: return LB_FINDSTRINGEXACT;
	case CB_GETITEMDATA: return LB_GETITEMDATA;
	case CB_SETITEMDATA: return LB_SETITEMDATA;
	case CB_GETTOPINDEX: return LB_GETTOPINDEX;
	case CB_SETTOPINDEX: return LB_SETTOPINDEX;
	case CB_INITSTORAGE: return LB_INITSTORAGE;
	case WIN_LBTEXT_SNAPSHOT: return WIN_LBTEXT_SNAPSHOT;
	default: return 0;
	}
}

LRESULT STUB_WINAPI user_combobox_proc(HWND h, UINT msg, WPARAM wp,
				       LPARAM lp)
{
	API();
	struct combo *c = state(h, msg == WM_NCCREATE ? sizeof(*c) : 0);
	int handled;

	if (msg == WM_NCCREATE) {
		if (!c)
			return FALSE;
		DWORD st = ((const CREATESTRUCTW *)lp)->style;
		BOOL owner = (st & (CBS_OWNERDRAWFIXED |
				    CBS_OWNERDRAWVARIABLE)) != 0;
		lb_init(&c->list, !owner || (st & CBS_HASSTRINGS),
			(st & CBS_SORT) != 0, FALSE, FALSE);
		c->limit = 30000;
		return user_DefWindowProcW(h, msg, wp, lp);
	}
	if (!c)
		return user_DefWindowProcW(h, msg, wp, lp);
	if (msg == WM_NCDESTROY) {
		lb_free(&c->list);
		free(c);
		u.SetWindowLongPtrW(h, 0, 0);
		return user_DefWindowProcW(h, msg, wp, lp);
	}
	UINT lbmsg = combo_to_list(msg);
	if (lbmsg) {
		int cur = c->list.cur;
		LRESULT r = lb_message(h, &c->list, lbmsg, wp, lp, &handled);
		if (cur != c->list.cur && cur >= 0 && c->list.cur < 0)
			combo_show(h, c);	/* the selection was deleted */
		return r;
	}
	switch (msg) {
	case CB_SETCURSEL:
		if ((int)wp < -1 || (int)wp >= c->list.n)
			wp = (WPARAM)-1;
		c->list.cur = (int)wp;
		combo_show(h, c);
		return c->list.cur < 0 ? CB_ERR : c->list.cur;
	case CB_GETCURSEL:
		return c->list.cur;
	case CB_SELECTSTRING: {
		LRESULT i = lb_find(&c->list, (int)wp, lp, FALSE);
		if (i == LB_ERR)
			return CB_ERR;
		c->list.cur = (int)i;
		combo_show(h, c);
		return i;
	}
	case CB_SHOWDROPDOWN:
		if (combo_type(h) != CBS_SIMPLE && c->dropped != !!wp) {
			c->dropped = !!wp;
			notify(h, wp ? CBN_DROPDOWN : CBN_CLOSEUP);
		}
		return TRUE;
	case CB_GETDROPPEDSTATE:
		return c->dropped;
	case CB_SETEXTENDEDUI:
		c->extended_ui = (BOOL)wp;
		return CB_OKAY;
	case CB_GETEXTENDEDUI:
		return c->extended_ui;
	case CB_SETDROPPEDWIDTH:
		c->dropped_width = (int)wp;
		return wp;
	case CB_GETDROPPEDWIDTH:
		return c->dropped_width;
	case CB_GETITEMHEIGHT:
		return c->list.height;
	case CB_SETITEMHEIGHT:
		if ((int)lp <= 0 || lp > 255)
			return CB_ERR;
		c->list.height = (int)lp;
		return 0;
	case CB_LIMITTEXT:
		if (combo_type(h) == CBS_DROPDOWNLIST)
			return FALSE;
		c->limit = wp ? (UINT)wp : 0x7ffffffe;
		return TRUE;
	case CB_GETEDITSEL:
		if (wp)
			*(DWORD *)wp = LOWORD(c->sel);
		if (lp)
			*(DWORD *)lp = HIWORD(c->sel);
		return c->sel;
	case CB_SETEDITSEL:
		if (combo_type(h) == CBS_DROPDOWNLIST)
			return CB_ERR;
		c->sel = (DWORD)lp;
		return TRUE;
	case WM_SETTEXT: {
		LRESULT i = lp ? lb_find(&c->list, -1, lp, TRUE) : LB_ERR;
		if (combo_type(h) == CBS_DROPDOWNLIST) {
			if (i == LB_ERR)
				return CB_ERR;
			c->list.cur = (int)i;
			combo_show(h, c);
			return TRUE;
		}
		c->list.cur = i == LB_ERR ? -1 : (int)i;
		return user_DefWindowProcW(h, msg, wp, lp);
	}
	case WM_CHAR: {
		if (combo_type(h) == CBS_DROPDOWNLIST)
			return 0;
		WCHAR *t = text_of(h);
		size_t n = t ? wstrlen(t) : 0;
		WCHAR ch = (WCHAR)wp;
		WCHAR *out = t ? malloc((n + 2) * sizeof(WCHAR)) : NULL;
		if (out) {
			memcpy(out, t, n * sizeof(WCHAR));
			if (ch == 8 && n)
				n--;
			else if (ch >= 0x20 && n < c->limit)
				out[n++] = ch;
			out[n] = 0;
			user_DefWindowProcW(h, WM_SETTEXT, 0, (LPARAM)out);
			c->list.cur = -1;
			notify(h, CBN_EDITUPDATE);
			notify(h, CBN_EDITCHANGE);
		}
		free(out);
		free(t);
		return 0;
	}
	case WM_KEYDOWN:
		if (wp == VK_F4 || ((wp == VK_UP || wp == VK_DOWN) &&
				    (u.GetKeyState(VK_MENU) < 0))) {
			user_combobox_proc(h, CB_SHOWDROPDOWN, !c->dropped, 0);
			return 0;
		}
		if (lb_key(&c->list, wp)) {
			combo_show(h, c);
			notify(h, CBN_SELCHANGE);
		}
		return 0;
	case WM_SETFOCUS:
		notify(h, CBN_SETFOCUS);
		return 0;
	case WM_KILLFOCUS:
		if (c->dropped)
			user_combobox_proc(h, CB_SHOWDROPDOWN, FALSE, 0);
		notify(h, CBN_KILLFOCUS);
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	}
	return user_DefWindowProcW(h, msg, wp, lp);
}

/* For comctl32's property sheets (user.h). */
HWND user_dialog_child(HINSTANCE inst, const void *tmpl, size_t size,
		       HWND parent, DLGPROC proc, LPARAM param, int unicode)
{
	return dialog_create(inst, tmpl, size, parent, proc, param, unicode, 0,
			     1);
}

int user_dialog_template_info(const void *tmpl, size_t size, int *cx,
			      int *cy, WCHAR **caption)
{
	struct tmpl t;

	if (caption)
		*caption = NULL;
	if (parse_tmpl(tmpl, size, &t))
		return -1;
	if (cx)
		*cx = t.cx;
	if (cy)
		*cy = t.cy;
	if (caption && t.title)
		*caption = wdup_n(t.title, wstrlen(t.title));
	return 0;
}

#define E(name) { #name, 0, (void *)user_##name }

const struct builtin_export u32_dialog_exports[] = {
	E(CheckRadioButton),
	E(CreateDialogIndirectParamA),
	E(CreateDialogIndirectParamW),
	E(CreateDialogParamA),
	E(CreateDialogParamW),
	{ "DefDlgProcA", 0, (void *)user_DefDlgProcA },
	{ "DefDlgProcW", 0, (void *)user_DefDlgProcW },
	E(DialogBoxIndirectParamA),
	E(DialogBoxIndirectParamW),
	E(DialogBoxParamA),
	E(DialogBoxParamW),
	E(EndDialog),
	E(GetDialogBaseUnits),
	E(GetNextDlgGroupItem),
	E(GetNextDlgTabItem),
	E(IsDialogMessageA),
	E(IsDialogMessageW),
	E(MapDialogRect),
	BUILTIN_END
};
