/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * user32: window classes, windows, message queues and timers - headless.
 *
 * Windows exist as objects with a class, a window procedure, a rectangle,
 * text, extra bytes and properties, and get the messages Windows would
 * send them as they are created, shown, moved and destroyed; nothing is
 * ever drawn, and no input arrives. Codecs that create a hidden window to
 * post themselves messages or run timers work.
 *
 * Each thread has a message queue, created on first use. PostMessage
 * queues to the window's thread; SendMessage calls the procedure directly
 * on the calling thread, even for another thread's window (Windows would
 * switch threads). Messages carrying text are converted between ANSI and
 * UTF-16 when the sender and the procedure disagree, as on Windows.
 *
 * Dialog boxes can't be shown: DialogBox* and CreateDialog* fail, saying
 * so on stderr.
 *
 * One lock guards everything here; it is never held while a window, timer
 * or enumeration procedure runs, so those may call back in freely.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/log.h"
#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/user.h"

#define IS_INTRESOURCE(p) (((ULONG_PTR)(p) >> 16) == 0)
#define DESKTOP_HANDLE ((HWND)0x00010010)
#define MESSAGE_ONLY 1

extern const struct builtin_dll builtin_kernel32;

struct wclass {
	struct wclass *next;
	ATOM atom;
	WCHAR *name;		/* NULL if registered by atom */
	char *name_a;
	HINSTANCE inst;
	UINT style;
	WNDPROC proc;
	int unicode;
	int cls_extra, wnd_extra;
	HICON icon, icon_sm;
	HCURSOR cursor;
	HBRUSH background;
	int nwindows;
};

struct prop {
	struct prop *next;
	WCHAR *name;		/* NULL for an atom */
	ATOM atom;
	HANDLE data;
};

struct window {
	HWND handle;
	struct wclass *cls;
	WNDPROC proc;
	int unicode;
	HWND parent, owner;
	int flags;
	DWORD style, exstyle;
	UINT_PTR id;
	HINSTANCE inst;
	RECT rect;		/* in the parent's client coordinates */
	WCHAR *text;
	LONG_PTR userdata;
	BYTE *extra;
	int nextra;
	DWORD tid;
	int destroying;
	struct prop *props;
};

struct msg_node {
	struct msg_node *next;
	MSG msg;
};

struct queue {
	struct queue *next;
	DWORD tid;
	struct msg_node *head, *tail;
	int quit, quit_code;
	DWORD last_time;
	DWORD last_pos;
};

struct timer {
	struct timer *next;
	HWND hwnd;
	UINT_PTR id;
	UINT elapse;
	TIMERPROC proc;
	DWORD tid;
	uint64_t due;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static struct wclass *classes;
static ATOM next_atom = 0xc000;
static struct window **wins;
static size_t nwins;
static ULONG_PTR next_handle = 0x00010020;
static struct queue *queues;
static struct timer *timers;
static UINT_PTR next_timer_id = 1;

static void init(void)
{
	pthread_condattr_t attr;

	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&cond, &attr);
	pthread_condattr_destroy(&attr);
}

static void enter(void)
{
	pthread_once(&once, init);
	pthread_mutex_lock(&lock);
}

static void leave(void)
{
	pthread_mutex_unlock(&lock);
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

DWORD user_ticks(void)
{
	return (DWORD)now_ms();
}

/* Strings. */

/* An ANSI argument as UTF-16 (malloc()ed); resource ids pass through. */
static const WCHAR *wide_arg(const char *s, WCHAR **to_free)
{
	*to_free = NULL;
	if (IS_INTRESOURCE(s))
		return (const WCHAR *)s;
	int n = cp_to_utf16(CP_ACP, s, -1, NULL, 0, 0);
	WCHAR *w = malloc((n > 0 ? n : 1) * sizeof(WCHAR));
	if (w) {
		cp_to_utf16(CP_ACP, s, -1, w, n, 0);
		*to_free = w;
	}
	return w;
}

static WCHAR *wdup(const WCHAR *s)
{
	size_t n = wstrlen(s) + 1;
	WCHAR *d = malloc(n * sizeof(WCHAR));

	if (d)
		memcpy(d, s, n * sizeof(WCHAR));
	return d;
}

/* Case-insensitive, ASCII only, as class names in practice are. */
static int wcasecmp_ascii(const WCHAR *a, const WCHAR *b)
{
	for (;; a++, b++) {
		WCHAR x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
		WCHAR y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
		if (x != y || !x)
			return x - y;
	}
}

/* Copies UTF-16 text out as the caller's charset; returns chars copied. */
static int copy_text(const WCHAR *text, void *buf, int size, int unicode)
{
	if (!buf || size <= 0)
		return 0;
	static const WCHAR empty[1];

	if (!text)
		text = empty;
	if (unicode) {
		int n = wstrlen(text);
		if (n > size - 1)
			n = size - 1;
		memcpy(buf, text, n * sizeof(WCHAR));
		((WCHAR *)buf)[n] = 0;
		return n;
	}
	int n = cp_from_utf16(CP_ACP, text, -1, NULL, 0, '?', NULL);
	char *a = malloc(n > 0 ? n : 1);
	if (!a) {
		*(char *)buf = '\0';
		return 0;
	}
	cp_from_utf16(CP_ACP, text, -1, a, n, '?', NULL);
	n = strlen(a);
	if (n > size - 1)
		n = size - 1;
	memcpy(buf, a, n);
	((char *)buf)[n] = '\0';
	free(a);
	return n;
}

/* Classes. */

static int class_matches(const struct wclass *c, const WCHAR *name)
{
	if (IS_INTRESOURCE(name))
		return c->atom == (ATOM)(ULONG_PTR)name;
	return c->name && !wcasecmp_ascii(c->name, name);
}

/* A class by name or atom: the instance's own first, then any. */
static struct wclass *class_find(const WCHAR *name, HINSTANCE inst)
{
	struct wclass *any = NULL;

	for (struct wclass *c = classes; c; c = c->next) {
		if (!class_matches(c, name))
			continue;
		if (c->inst == inst)
			return c;
		if (!any)
			any = c;
	}
	return any;
}

LRESULT WINAPI user_DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
LRESULT WINAPI user_DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
BOOL WINAPI user_DestroyWindow(HWND);

static ATOM register_class(const WNDCLASSEXW *wc, int unicode)
{
	struct wclass *c;
	ATOM atom = 0;

	if (!wc || !wc->lpszClassName) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	enter();
	for (c = classes; c; c = c->next)
		if (c->inst == wc->hInstance && class_matches(c, wc->lpszClassName))
			break;
	if (c) {
		win_set_error(ERROR_CLASS_ALREADY_EXISTS);
		goto out;
	}
	if (!(c = calloc(1, sizeof(*c))))
		goto nomem;
	if (IS_INTRESOURCE(wc->lpszClassName)) {
		c->atom = (ATOM)(ULONG_PTR)wc->lpszClassName;
	} else {
		c->atom = next_atom++;
		c->name = wdup(wc->lpszClassName);
		c->name_a = utf16_to_utf8_dup(c->name);
		char *a = c->name_a ? utf8_to_acp_dup(c->name_a) : NULL;
		free(c->name_a);
		c->name_a = a;
		if (!c->name || !c->name_a) {
			free(c->name);
			free(c->name_a);
			free(c);
			goto nomem;
		}
	}
	c->inst = wc->hInstance;
	c->style = wc->style;
	c->proc = wc->lpfnWndProc;
	c->unicode = unicode;
	c->cls_extra = wc->cbClsExtra;
	c->wnd_extra = wc->cbWndExtra > 0 ? wc->cbWndExtra : 0;
	c->icon = wc->hIcon;
	c->icon_sm = wc->cbSize >= sizeof(WNDCLASSEXW) ? wc->hIconSm : NULL;
	c->cursor = wc->hCursor;
	c->background = wc->hbrBackground;
	c->next = classes;
	classes = c;
	atom = c->atom;
	goto out;
nomem:
	win_set_error(ERROR_NOT_ENOUGH_MEMORY);
out:
	leave();
	return atom;
}

/* The system classes a codec might create a hidden window of. */
static void register_system_classes(void)
{
	static const char *const names[] = {
		"STATIC", "BUTTON", "EDIT", "#32770", "#32769", "Message",
	};
	static int done;

	if (__atomic_exchange_n(&done, 1, __ATOMIC_SEQ_CST))
		return;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		WCHAR w[16];
		WNDCLASSEXW wc;
		size_t j = 0;
		for (; names[i][j]; j++)
			w[j] = names[i][j];
		w[j] = 0;
		memset(&wc, 0, sizeof(wc));
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = user_DefWindowProcW;
		wc.lpszClassName = w;
		wc.cbWndExtra = !strcmp(names[i], "#32770") ? 30 : 0;
		register_class(&wc, 1);
	}
}

static void class_from_a(const WNDCLASSEXA *a, WNDCLASSEXW *w, WCHAR **f1,
			 WCHAR **f2)
{
	memcpy(w, a, sizeof(*w));
	w->lpszClassName = wide_arg(a->lpszClassName, f1);
	w->lpszMenuName = a->lpszMenuName ? wide_arg(a->lpszMenuName, f2) : NULL;
}

static void ex_from_plain(const WNDCLASSW *p, WNDCLASSEXW *e)
{
	memset(e, 0, sizeof(*e));
	e->cbSize = sizeof(*e);
	memcpy(&e->style, p, sizeof(*p));
}

WINFN(ATOM) user_RegisterClassExW(const WNDCLASSEXW *wc)
{
	register_system_classes();
	return register_class(wc, 1);
}

WINFN(ATOM) user_RegisterClassW(const WNDCLASSW *wc)
{
	WNDCLASSEXW e;

	if (!wc)
		return 0;
	ex_from_plain(wc, &e);
	return user_RegisterClassExW(&e);
}

WINFN(ATOM) user_RegisterClassExA(const WNDCLASSEXA *wc)
{
	WNDCLASSEXW w;
	WCHAR *f1, *f2 = NULL;

	if (!wc)
		return 0;
	register_system_classes();
	class_from_a(wc, &w, &f1, &f2);
	ATOM r = register_class(&w, 0);
	free(f1);
	free(f2);
	return r;
}

WINFN(ATOM) user_RegisterClassA(const WNDCLASSA *wc)
{
	WNDCLASSEXA e;

	if (!wc)
		return 0;
	memset(&e, 0, sizeof(e));
	e.cbSize = sizeof(e);
	memcpy(&e.style, wc, sizeof(*wc));
	return user_RegisterClassExA(&e);
}

static BOOL unregister_class(const WCHAR *name, HINSTANCE inst)
{
	BOOL ok = FALSE;

	enter();
	for (struct wclass **p = &classes; *p; p = &(*p)->next) {
		struct wclass *c = *p;
		if ((c->inst != inst && inst) || !class_matches(c, name))
			continue;
		if (c->nwindows) {
			win_set_error(ERROR_CLASS_HAS_WINDOWS);
		} else {
			*p = c->next;
			free(c->name);
			free(c->name_a);
			free(c);
			ok = TRUE;
		}
		break;
	}
	if (!ok && win_get_error() != ERROR_CLASS_HAS_WINDOWS)
		win_set_error(ERROR_CLASS_DOES_NOT_EXIST);
	leave();
	return ok;
}

WINFN(BOOL) user_UnregisterClassW(LPCWSTR name, HINSTANCE inst)
{
	return name ? unregister_class(name, inst) : FALSE;
}

WINFN(BOOL) user_UnregisterClassA(LPCSTR name, HINSTANCE inst)
{
	WCHAR *f = NULL;
	const WCHAR *w = name ? wide_arg(name, &f) : NULL;
	BOOL r = w ? unregister_class(w, inst) : FALSE;

	free(f);
	return r;
}

static BOOL class_info(HINSTANCE inst, const WCHAR *name, void *out, int ex,
		       int unicode)
{
	WNDCLASSEXW w;

	register_system_classes();
	enter();
	struct wclass *c = name ? class_find(name, inst) : NULL;
	if (c) {
		memset(&w, 0, sizeof(w));
		w.cbSize = sizeof(w);
		w.style = c->style;
		w.lpfnWndProc = c->proc;
		w.cbClsExtra = c->cls_extra;
		w.cbWndExtra = c->wnd_extra;
		w.hInstance = c->inst;
		w.hIcon = c->icon;
		w.hCursor = c->cursor;
		w.hbrBackground = c->background;
		w.lpszClassName = c->name ? c->name :
				  (LPCWSTR)(ULONG_PTR)c->atom;
		if (!unicode && c->name)
			w.lpszClassName = (LPCWSTR)c->name_a;
		w.hIconSm = c->icon_sm;
	} else {
		win_set_error(ERROR_CLASS_DOES_NOT_EXIST);
	}
	ATOM atom = c ? c->atom : 0;
	leave();
	if (!c || !out)
		return FALSE;
	/* The A and W structures share a layout. */
	if (ex)
		memcpy((BYTE *)out + sizeof(UINT), &w.style,
		       sizeof(w) - sizeof(UINT));
	else
		memcpy(out, &w.style, sizeof(WNDCLASSW));
	return ex ? atom : TRUE;
}

WINFN(BOOL) user_GetClassInfoW(HINSTANCE inst, LPCWSTR name, WNDCLASSW *wc)
{
	return class_info(inst, name, wc, 0, 1);
}

WINFN(BOOL) user_GetClassInfoExW(HINSTANCE inst, LPCWSTR name, WNDCLASSEXW *wc)
{
	return class_info(inst, name, wc, 1, 1);
}

WINFN(BOOL) user_GetClassInfoA(HINSTANCE inst, LPCSTR name, WNDCLASSA *wc)
{
	WCHAR *f;
	BOOL r = class_info(inst, wide_arg(name, &f), wc, 0, 0);
	free(f);
	return r;
}

WINFN(BOOL) user_GetClassInfoExA(HINSTANCE inst, LPCSTR name, WNDCLASSEXA *wc)
{
	WCHAR *f;
	BOOL r = class_info(inst, wide_arg(name, &f), wc, 1, 0);
	free(f);
	return r;
}

/* Windows. */

static struct window *find(HWND h)
{
	for (size_t i = 0; i < nwins; i++)
		if (wins[i]->handle == h)
			return wins[i];
	return NULL;
}

/* The window behind h, or NULL with ERROR_INVALID_WINDOW_HANDLE. */
static struct window *get(HWND h)
{
	struct window *w = find(h);

	if (!w)
		win_set_error(ERROR_INVALID_WINDOW_HANDLE);
	return w;
}

static void make_desktop(void)
{
	static const WCHAR name[] = { '#', '3', '2', '7', '6', '9', 0 };
	struct window *w;

	if (find(DESKTOP_HANDLE) || !(w = calloc(1, sizeof(*w))))
		return;
	struct window **grown = realloc(wins, (nwins + 1) * sizeof(*wins));
	if (!grown) {
		free(w);
		return;
	}
	wins = grown;
	wins[nwins++] = w;
	w->handle = DESKTOP_HANDLE;
	w->cls = class_find(name, NULL);
	if (w->cls)
		w->cls->nwindows++;
	w->proc = user_DefWindowProcW;
	w->unicode = 1;
	w->style = WS_POPUP | WS_VISIBLE;
	w->rect.right = USER_SCREEN_WIDTH;
	w->rect.bottom = USER_SCREEN_HEIGHT;
}

HWND user_desktop(void)
{
	register_system_classes();
	enter();
	make_desktop();
	leave();
	return DESKTOP_HANDLE;
}

int user_is_window(HWND hwnd)
{
	if (hwnd == DESKTOP_HANDLE)
		return user_desktop() != NULL;
	enter();
	int r = find(hwnd) != NULL;
	leave();
	return r;
}

int user_client_rect(HWND hwnd, RECT *rc)
{
	if (hwnd == DESKTOP_HANDLE)
		user_desktop();
	enter();
	struct window *w = get(hwnd);
	if (w) {
		rc->left = rc->top = 0;
		rc->right = w->rect.right - w->rect.left;
		rc->bottom = w->rect.bottom - w->rect.top;
	}
	leave();
	return w != NULL;
}

static LRESULT send(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int unicode);

/* A CREATESTRUCT's strings in the other charset. */
struct cs_conv {
	union {
		CREATESTRUCTA a;
		CREATESTRUCTW w;
	} cs;
	void *name, *cls;
};

static void convert_cs(const void *in, int to_unicode, struct cs_conv *out)
{
	memcpy(&out->cs, in, sizeof(CREATESTRUCTW));
	out->name = out->cls = NULL;
	if (to_unicode) {
		const CREATESTRUCTA *a = in;
		WCHAR *f;
		if (a->lpszName && !IS_INTRESOURCE(a->lpszName)) {
			out->cs.w.lpszName = wide_arg(a->lpszName, &f);
			out->name = f;
		}
		if (!IS_INTRESOURCE(a->lpszClass)) {
			out->cs.w.lpszClass = wide_arg(a->lpszClass, &f);
			out->cls = f;
		}
	} else {
		const CREATESTRUCTW *w = in;
		if (w->lpszName && !IS_INTRESOURCE(w->lpszName)) {
			char *u = utf16_to_utf8_dup(w->lpszName);
			out->name = u ? utf8_to_acp_dup(u) : NULL;
			out->cs.a.lpszName = out->name;
			free(u);
		}
		if (!IS_INTRESOURCE(w->lpszClass)) {
			char *u = utf16_to_utf8_dup(w->lpszClass);
			out->cls = u ? utf8_to_acp_dup(u) : NULL;
			out->cs.a.lpszClass = out->cls;
			free(u);
		}
	}
}

/*
 * Calls a window procedure expecting proc_unicode messages with a message
 * from a unicode (or ANSI) sender, converting text where they differ.
 */
static LRESULT call_proc(WNDPROC proc, int proc_unicode, HWND hwnd, UINT msg,
			 WPARAM wp, LPARAM lp, int unicode)
{
	if (!proc)
		return 0;
	if (proc_unicode == unicode)
		return proc(hwnd, msg, wp, lp);

	LRESULT r;
	switch (msg) {
	case WM_SETTEXT: {
		if (!lp)
			return proc(hwnd, msg, wp, lp);
		void *s;
		if (proc_unicode) {
			WCHAR *f;
			wide_arg((const char *)lp, &f);
			s = f;
		} else {
			char *u = utf16_to_utf8_dup((const WCHAR *)lp);
			s = u ? utf8_to_acp_dup(u) : NULL;
			free(u);
		}
		r = proc(hwnd, msg, wp, (LPARAM)s);
		free(s);
		return r;
	}
	case WM_GETTEXT: {
		int size = (int)wp;
		WCHAR *w = malloc((size > 0 ? size : 1) * 2 * sizeof(WCHAR));
		if (!w || !lp)
			return free(w), 0;
		memset(w, 0, sizeof(WCHAR));
		r = proc(hwnd, msg, size * (proc_unicode ? 1 : 2), (LPARAM)w);
		WCHAR *text = w;
		WCHAR *f = NULL;
		if (size > 0 && proc_unicode) {
			w[size - 1] = 0;
		} else if (size > 0) {
			((char *)w)[size * 2 - 1] = '\0';
			wide_arg((const char *)w, &f);
			text = f;
		}
		r = copy_text(text, (void *)lp, size, unicode);
		free(f);
		free(w);
		return r;
	}
	case WM_NCCREATE:
	case WM_CREATE: {
		struct cs_conv c;
		if (!lp)
			return proc(hwnd, msg, wp, lp);
		convert_cs((const void *)lp, proc_unicode, &c);
		r = proc(hwnd, msg, wp, (LPARAM)&c.cs);
		free(c.name);
		free(c.cls);
		return r;
	}
	default:
		return proc(hwnd, msg, wp, lp);
	}
}

static LRESULT send(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int unicode)
{
	if (hwnd == DESKTOP_HANDLE)
		user_desktop();
	enter();
	struct window *w = get(hwnd);
	WNDPROC proc = w ? w->proc : NULL;
	int proc_unicode = w ? w->unicode : 1;
	leave();
	if (!w)
		return 0;
	return call_proc(proc, proc_unicode, hwnd, msg, wp, lp, unicode);
}

/* Handles of the windows matching pred, for iterating without the lock. */
static HWND *snapshot(int (*pred)(const struct window *, const void *),
		      const void *arg, size_t *n)
{
	HWND *out = malloc((nwins + 1) * sizeof(*out));

	*n = 0;
	for (size_t i = 0; out && i < nwins; i++)
		if (pred(wins[i], arg))
			out[(*n)++] = wins[i]->handle;
	return out;
}

static int is_top_level(const struct window *w, const void *arg)
{
	return !w->parent && w->handle != DESKTOP_HANDLE &&
	       !(w->flags & MESSAGE_ONLY);
}

static int is_top_level_of(const struct window *w, const void *arg)
{
	return is_top_level(w, NULL) && w->tid == *(const DWORD *)arg;
}

static int is_child_of(const struct window *w, const void *arg)
{
	return w->parent == *(const HWND *)arg;
}

static int is_owned_or_child(const struct window *w, const void *arg)
{
	HWND h = *(const HWND *)arg;
	return w->parent == h || w->owner == h;
}

static HWND create_window(DWORD exstyle, const WCHAR *cls_name,
			  const void *orig_class, const void *orig_title,
			  DWORD style, int x, int y, int cx, int cy,
			  HWND parent, HMENU menu, HINSTANCE inst,
			  LPVOID param, int unicode)
{
	struct window *w = NULL;
	HWND h = NULL;

	register_system_classes();
	enter();
	make_desktop();
	struct wclass *c = cls_name ? class_find(cls_name, inst) : NULL;
	int message_only = parent == HWND_MESSAGE;
	if (message_only)
		parent = NULL;
	if (!c) {
		win_set_error(ERROR_CANNOT_FIND_WND_CLASS);
	} else if ((style & WS_CHILD) && !parent) {
		win_set_error(ERROR_TLW_WITH_WSCHILD);
	} else if (parent && !find(parent)) {
		win_set_error(ERROR_INVALID_WINDOW_HANDLE);
	} else if (!(w = calloc(1, sizeof(*w))) ||
		   !(w->extra = calloc(1, c->wnd_extra ? c->wnd_extra : 1))) {
		if (w)
			free(w);
		w = NULL;
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	} else {
		struct window **grown = realloc(wins, (nwins + 1) * sizeof(*wins));
		if (!grown) {
			free(w->extra);
			free(w);
			w = NULL;
			win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		} else {
			wins = grown;
			wins[nwins++] = w;
			h = w->handle = (HWND)next_handle;
			next_handle += 2;
			w->cls = c;
			c->nwindows++;
			w->proc = c->proc;
			w->unicode = c->unicode;
			w->nextra = c->wnd_extra;
			w->style = style;
			w->exstyle = exstyle;
			w->inst = inst;
			w->tid = thread_current_id();
			w->flags = message_only ? MESSAGE_ONLY : 0;
			if (style & WS_CHILD) {
				w->parent = parent;
				w->id = (UINT_PTR)menu;
			} else {
				w->owner = parent;
			}
			if (x == CW_USEDEFAULT)
				x = y = 0;
			if (cx == CW_USEDEFAULT) {
				cx = USER_SCREEN_WIDTH * 3 / 4;
				cy = USER_SCREEN_HEIGHT * 3 / 4;
			}
			w->rect.left = x;
			w->rect.top = y;
			w->rect.right = x + (cx > 0 ? cx : 0);
			w->rect.bottom = y + (cy > 0 ? cy : 0);
		}
	}
	leave();
	if (!w)
		return NULL;

	/* The CREATESTRUCT in the caller's charset, with its own strings. */
	CREATESTRUCTW cs = {
		param, inst, menu, message_only ? HWND_MESSAGE : parent, cy, cx,
		y, x, (LONG)style, orig_title, orig_class, exstyle,
	};
	if (!send(h, WM_NCCREATE, 0, (LPARAM)&cs, unicode)) {
		TRACE("CreateWindowEx: WM_NCCREATE refused");
		user_DestroyWindow(h);
		return NULL;
	}
	if (send(h, WM_CREATE, 0, (LPARAM)&cs, unicode) == -1) {
		TRACE("CreateWindowEx: WM_CREATE refused");
		user_DestroyWindow(h);
		return NULL;
	}
	if (style & WS_VISIBLE)
		send(h, WM_SHOWWINDOW, TRUE, 0, unicode);
	TRACE("CreateWindowEx = %p", (void *)h);
	return h;
}

WINFN(HWND) user_CreateWindowExW(DWORD exstyle, LPCWSTR cls, LPCWSTR title,
				 DWORD style, int x, int y, int cx, int cy,
				 HWND parent, HMENU menu, HINSTANCE inst,
				 LPVOID param)
{
	return create_window(exstyle, cls, cls, title, style, x, y, cx, cy,
			     parent, menu, inst, param, 1);
}

WINFN(HWND) user_CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR title,
				 DWORD style, int x, int y, int cx, int cy,
				 HWND parent, HMENU menu, HINSTANCE inst,
				 LPVOID param)
{
	WCHAR *f = NULL;
	const WCHAR *w = cls ? wide_arg(cls, &f) : NULL;
	HWND h = create_window(exstyle, w, cls, title, style, x, y, cx, cy,
			       parent, menu, inst, param, 0);
	free(f);
	return h;
}

static void free_window(struct window *w)
{
	for (struct prop *p = w->props, *next; p; p = next) {
		next = p->next;
		free(p->name);
		free(p);
	}
	free(w->text);
	free(w->extra);
	free(w);
}

__attribute__((force_align_arg_pointer))
BOOL WINAPI user_DestroyWindow(HWND hwnd)
{
	size_t n = 0;
	HWND *kids = NULL;

	enter();
	struct window *w = get(hwnd);
	int ok = w && hwnd != DESKTOP_HANDLE;
	int again = w && w->destroying;
	if (ok && !again) {
		w->destroying = 1;
		kids = snapshot(is_owned_or_child, &hwnd, &n);
	}
	leave();
	if (!ok || again)
		return ok;

	send(hwnd, WM_DESTROY, 0, 0, 1);
	for (size_t i = 0; i < n; i++)
		user_DestroyWindow(kids[i]);
	free(kids);
	send(hwnd, WM_NCDESTROY, 0, 0, 1);

	enter();
	for (struct timer **t = &timers; *t;) {
		if ((*t)->hwnd == hwnd) {
			struct timer *dead = *t;
			*t = dead->next;
			free(dead);
		} else {
			t = &(*t)->next;
		}
	}
	for (struct queue *q = queues; q; q = q->next) {
		struct msg_node **p = &q->head, *prev = NULL;
		while (*p) {
			if ((*p)->msg.hwnd == hwnd) {
				struct msg_node *dead = *p;
				*p = dead->next;
				free(dead);
			} else {
				prev = *p;
				p = &(*p)->next;
			}
		}
		q->tail = prev;
	}
	for (size_t i = 0; i < nwins; i++) {
		if (wins[i] == w) {
			memmove(&wins[i], &wins[i + 1], (nwins - i - 1) * sizeof(*wins));
			nwins--;
			break;
		}
	}
	w->cls->nwindows--;
	free_window(w);
	leave();
	TRACE("DestroyWindow(%p)", (void *)hwnd);
	return TRUE;
}

/* DefWindowProc: text, WM_CLOSE, and nothing else to do headless. */
static LRESULT def_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int unicode);

static LRESULT def_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int unicode)
{
	switch (msg) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
		if (cs && cs->lpszName && !IS_INTRESOURCE(cs->lpszName))
			def_proc(hwnd, WM_SETTEXT, 0, (LPARAM)cs->lpszName, unicode);
		return TRUE;
	}
	case WM_SETTEXT: {
		WCHAR *f = NULL, *text;
		if (!lp)
			text = NULL;
		else if (unicode)
			text = wdup((const WCHAR *)lp);
		else
			text = (wide_arg((const char *)lp, &f), f);
		enter();
		struct window *w = get(hwnd);
		if (w) {
			free(w->text);
			w->text = text;
			text = NULL;
		}
		leave();
		free(text);
		return w != NULL;
	}
	case WM_GETTEXT: {
		enter();
		struct window *w = get(hwnd);
		WCHAR *text = w && w->text ? wdup(w->text) : NULL;
		leave();
		int n = copy_text(text, (void *)lp, (int)wp, unicode);
		free(text);
		return n;
	}
	case WM_GETTEXTLENGTH: {
		enter();
		struct window *w = get(hwnd);
		LRESULT n = w && w->text ? wstrlen(w->text) : 0;
		leave();
		return n;
	}
	case WM_CLOSE:
		user_DestroyWindow(hwnd);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	default:
		return 0;
	}
}

__attribute__((force_align_arg_pointer))
LRESULT WINAPI user_DefWindowProcW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	return def_proc(hwnd, msg, wp, lp, 1);
}

__attribute__((force_align_arg_pointer))
LRESULT WINAPI user_DefWindowProcA(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	return def_proc(hwnd, msg, wp, lp, 0);
}

WINFN(LRESULT) user_CallWindowProcA(WNDPROC proc, HWND hwnd, UINT msg,
				    WPARAM wp, LPARAM lp)
{
	return proc ? proc(hwnd, msg, wp, lp) : 0;
}

WINFN(LRESULT) user_CallWindowProcW(WNDPROC proc, HWND hwnd, UINT msg,
				    WPARAM wp, LPARAM lp)
{
	return proc ? proc(hwnd, msg, wp, lp) : 0;
}

WINFN(BOOL) user_IsWindow(HWND hwnd)
{
	return hwnd && user_is_window(hwnd);
}

WINFN(BOOL) user_IsWindowUnicode(HWND hwnd)
{
	enter();
	struct window *w = find(hwnd);
	BOOL r = w && w->unicode;
	leave();
	return r;
}

/* Visible only if its ancestors are too. */
WINFN(BOOL) user_IsWindowVisible(HWND hwnd)
{
	BOOL r = FALSE;

	enter();
	for (struct window *w = find(hwnd); w; w = w->parent ? find(w->parent) : NULL) {
		r = (w->style & WS_VISIBLE) != 0;
		if (!r)
			break;
	}
	leave();
	return r;
}

WINFN(BOOL) user_IsWindowEnabled(HWND hwnd)
{
	enter();
	struct window *w = find(hwnd);
	BOOL r = w && !(w->style & WS_DISABLED);
	leave();
	return r;
}

WINFN(BOOL) user_IsChild(HWND parent, HWND hwnd)
{
	BOOL r = FALSE;

	enter();
	for (struct window *w = find(hwnd); w && w->parent && !r;
	     w = find(w->parent))
		r = w->parent == parent;
	leave();
	return r;
}

WINFN(BOOL) user_IsIconic(HWND hwnd) { return FALSE; }
WINFN(BOOL) user_IsZoomed(HWND hwnd) { return FALSE; }

WINFN(BOOL) user_EnableWindow(HWND hwnd, BOOL enable)
{
	enter();
	struct window *w = get(hwnd);
	BOOL was_disabled = w && (w->style & WS_DISABLED);
	int changed = w && (!enable) != was_disabled;
	if (w && enable)
		w->style &= ~WS_DISABLED;
	else if (w)
		w->style |= WS_DISABLED;
	leave();
	if (changed)
		send(hwnd, WM_ENABLE, enable, 0, 1);
	return was_disabled;
}

WINFN(BOOL) user_ShowWindow(HWND hwnd, int cmd)
{
	enter();
	struct window *w = get(hwnd);
	BOOL was = w && (w->style & WS_VISIBLE);
	BOOL show = cmd != SW_HIDE;
	if (w && show)
		w->style |= WS_VISIBLE;
	else if (w)
		w->style &= ~WS_VISIBLE;
	leave();
	if (w && show != was)
		send(hwnd, WM_SHOWWINDOW, show, 0, 1);
	return was;
}

WINFN(BOOL) user_UpdateWindow(HWND hwnd)
{
	return user_is_window(hwnd);
}

WINFN(BOOL) user_InvalidateRect(HWND hwnd, const RECT *rc, BOOL erase)
{
	return !hwnd || user_is_window(hwnd);
}

WINFN(BOOL) user_ValidateRect(HWND hwnd, const RECT *rc)
{
	return !hwnd || user_is_window(hwnd);
}

/* A child's parent, or a top-level window's owner. */
WINFN(HWND) user_GetParent(HWND hwnd)
{
	enter();
	struct window *w = get(hwnd);
	HWND p = w ? (w->parent ? w->parent : w->owner) : NULL;
	leave();
	return p;
}

WINFN(HWND) user_SetParent(HWND hwnd, HWND parent)
{
	HWND old = NULL;

	enter();
	struct window *w = get(hwnd);
	if (w && (!parent || find(parent))) {
		old = w->parent ? w->parent : DESKTOP_HANDLE;
		w->parent = parent == DESKTOP_HANDLE ? NULL : parent;
	}
	leave();
	return old;
}

WINFN(DWORD) user_GetWindowThreadProcessId(HWND hwnd, LPDWORD pid)
{
	if (hwnd == DESKTOP_HANDLE)
		user_desktop();
	enter();
	struct window *w = get(hwnd);
	DWORD tid = w ? w->tid : 0;
	leave();
	if (w && pid)
		*pid = getpid();
	return tid;
}

/* The screen position of a window's client origin. */
static POINT origin(struct window *w)
{
	POINT p = { 0, 0 };

	for (; w; w = w->parent ? find(w->parent) : NULL) {
		p.x += w->rect.left;
		p.y += w->rect.top;
	}
	return p;
}

WINFN(BOOL) user_GetWindowRect(HWND hwnd, LPRECT rc)
{
	if (hwnd == DESKTOP_HANDLE)
		user_desktop();
	enter();
	struct window *w = get(hwnd);
	if (w && rc) {
		POINT o = origin(w);
		rc->left = o.x;
		rc->top = o.y;
		rc->right = o.x + w->rect.right - w->rect.left;
		rc->bottom = o.y + w->rect.bottom - w->rect.top;
	}
	leave();
	return w && rc;
}

WINFN(BOOL) user_GetClientRect(HWND hwnd, LPRECT rc)
{
	return rc && user_client_rect(hwnd, rc);
}

static BOOL map_point(HWND hwnd, LPPOINT pt, int sign)
{
	if (hwnd == DESKTOP_HANDLE)
		user_desktop();
	enter();
	struct window *w = get(hwnd);
	if (w && pt) {
		POINT o = origin(w);
		pt->x += sign * o.x;
		pt->y += sign * o.y;
	}
	leave();
	return w && pt;
}

WINFN(BOOL) user_ClientToScreen(HWND hwnd, LPPOINT pt)
{
	return map_point(hwnd, pt, 1);
}

WINFN(BOOL) user_ScreenToClient(HWND hwnd, LPPOINT pt)
{
	return map_point(hwnd, pt, -1);
}

/* Moves and/or sizes a window, sending WM_MOVE and WM_SIZE on change. */
static BOOL set_pos(HWND hwnd, int x, int y, int cx, int cy, UINT flags)
{
	enter();
	struct window *w = get(hwnd);
	int moved = 0, sized = 0, shown = -1;
	if (w) {
		if (!(flags & SWP_NOMOVE) &&
		    (x != w->rect.left || y != w->rect.top)) {
			w->rect.right += x - w->rect.left;
			w->rect.bottom += y - w->rect.top;
			w->rect.left = x;
			w->rect.top = y;
			moved = 1;
		}
		if (!(flags & SWP_NOSIZE) &&
		    (cx != w->rect.right - w->rect.left ||
		     cy != w->rect.bottom - w->rect.top)) {
			w->rect.right = w->rect.left + (cx > 0 ? cx : 0);
			w->rect.bottom = w->rect.top + (cy > 0 ? cy : 0);
			sized = 1;
		}
		if ((flags & SWP_SHOWWINDOW) && !(w->style & WS_VISIBLE)) {
			w->style |= WS_VISIBLE;
			shown = 1;
		} else if ((flags & SWP_HIDEWINDOW) && (w->style & WS_VISIBLE)) {
			w->style &= ~WS_VISIBLE;
			shown = 0;
		}
	}
	RECT r = w ? w->rect : (RECT){ 0, 0, 0, 0 };
	leave();
	if (shown >= 0)
		send(hwnd, WM_SHOWWINDOW, shown, 0, 1);
	if (moved)
		send(hwnd, WM_MOVE, 0, MAKELONG(r.left, r.top), 1);
	if (sized)
		send(hwnd, WM_SIZE, 0, MAKELONG(r.right - r.left, r.bottom - r.top), 1);
	return w != NULL;
}

WINFN(BOOL) user_MoveWindow(HWND hwnd, int x, int y, int cx, int cy, BOOL repaint)
{
	return set_pos(hwnd, x, y, cx, cy, 0);
}

WINFN(BOOL) user_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx,
			      int cy, UINT flags)
{
	return set_pos(hwnd, x, y, cx, cy, flags);
}

static LONG window_long(HWND hwnd, int index, int set, LONG value, int unicode)
{
	LONG old = 0;

	enter();
	struct window *w = get(hwnd);
	if (!w)
		goto out;
	LONG *field = NULL;
	switch (index) {
	case GWL_WNDPROC:
		old = (LONG)(ULONG_PTR)w->proc;
		if (set) {
			w->proc = (WNDPROC)(ULONG_PTR)value;
			w->unicode = unicode;
		}
		goto out;
	case GWL_HINSTANCE:
		field = (LONG *)&w->inst;
		break;
	case GWL_HWNDPARENT:
		field = (LONG *)(w->parent ? &w->parent : &w->owner);
		break;
	case GWL_ID:
		field = (LONG *)&w->id;
		break;
	case GWL_STYLE:
		field = (LONG *)&w->style;
		break;
	case GWL_EXSTYLE:
		field = (LONG *)&w->exstyle;
		break;
	case GWL_USERDATA:
		field = (LONG *)&w->userdata;
		break;
	default:
		if (index >= 0 && index + 4 <= w->nextra) {
			memcpy(&old, w->extra + index, 4);
			if (set)
				memcpy(w->extra + index, &value, 4);
		} else {
			win_set_error(ERROR_INVALID_INDEX);
		}
		goto out;
	}
	old = *field;
	if (set)
		*field = value;
out:
	leave();
	return old;
}

WINFN(LONG) user_GetWindowLongA(HWND h, int i) { return window_long(h, i, 0, 0, 0); }
WINFN(LONG) user_GetWindowLongW(HWND h, int i) { return window_long(h, i, 0, 0, 1); }
WINFN(LONG) user_SetWindowLongA(HWND h, int i, LONG v) { return window_long(h, i, 1, v, 0); }
WINFN(LONG) user_SetWindowLongW(HWND h, int i, LONG v) { return window_long(h, i, 1, v, 1); }

WINFN(BOOL) user_SetWindowTextA(HWND hwnd, LPCSTR text)
{
	return send(hwnd, WM_SETTEXT, 0, (LPARAM)text, 0);
}

WINFN(BOOL) user_SetWindowTextW(HWND hwnd, LPCWSTR text)
{
	return send(hwnd, WM_SETTEXT, 0, (LPARAM)text, 1);
}

WINFN(int) user_GetWindowTextA(HWND hwnd, LPSTR buf, int size)
{
	if (buf && size > 0)
		*buf = '\0';
	return send(hwnd, WM_GETTEXT, size, (LPARAM)buf, 0);
}

WINFN(int) user_GetWindowTextW(HWND hwnd, LPWSTR buf, int size)
{
	if (buf && size > 0)
		*buf = 0;
	return send(hwnd, WM_GETTEXT, size, (LPARAM)buf, 1);
}

WINFN(int) user_GetWindowTextLengthA(HWND hwnd)
{
	return send(hwnd, WM_GETTEXTLENGTH, 0, 0, 0);
}

WINFN(int) user_GetWindowTextLengthW(HWND hwnd)
{
	return send(hwnd, WM_GETTEXTLENGTH, 0, 0, 1);
}

static int class_name(HWND hwnd, void *buf, int size, int unicode)
{
	WCHAR name[16];

	if (hwnd == DESKTOP_HANDLE)
		user_desktop();
	enter();
	struct window *w = get(hwnd);
	WCHAR *copy = NULL;
	if (w && w->cls->name) {
		copy = wdup(w->cls->name);
	} else if (w) {
		/* An atom-only class reads as "#<atom>". */
		char tmp[16];
		int n = snprintf(tmp, sizeof(tmp), "#%u", w->cls->atom);
		for (int i = 0; i <= n; i++)
			name[i] = tmp[i];
		copy = wdup(name);
	}
	leave();
	int r = w ? copy_text(copy, buf, size, unicode) : 0;
	free(copy);
	return r;
}

WINFN(int) user_GetClassNameA(HWND hwnd, LPSTR buf, int size)
{
	return class_name(hwnd, buf, size, 0);
}

WINFN(int) user_GetClassNameW(HWND hwnd, LPWSTR buf, int size)
{
	return class_name(hwnd, buf, size, 1);
}

/* Enumeration and searching. */

typedef BOOL (CALLBACK *WNDENUMPROC)(HWND, LPARAM);

static BOOL enumerate(HWND *list, size_t n, WNDENUMPROC proc, LPARAM lp)
{
	for (size_t i = 0; list && i < n; i++)
		if (user_is_window(list[i]) && !proc(list[i], lp))
			break;
	free(list);
	return list != NULL;
}

WINFN(BOOL) user_EnumWindows(WNDENUMPROC proc, LPARAM lp)
{
	size_t n;

	enter();
	HWND *list = snapshot(is_top_level, NULL, &n);
	leave();
	return enumerate(list, n, proc, lp);
}

WINFN(BOOL) user_EnumThreadWindows(DWORD tid, WNDENUMPROC proc, LPARAM lp)
{
	size_t n;

	enter();
	HWND *list = snapshot(is_top_level_of, &tid, &n);
	leave();
	return enumerate(list, n, proc, lp) && n;
}

/* Every descendant, depth first. */
WINFN(BOOL) user_EnumChildWindows(HWND parent, WNDENUMPROC proc, LPARAM lp)
{
	size_t n;

	enter();
	HWND *list = snapshot(is_child_of, &parent, &n);
	leave();
	for (size_t i = 0; list && i < n; i++) {
		if (!user_is_window(list[i]))
			continue;
		if (!proc(list[i], lp)) {
			free(list);
			return FALSE;
		}
		user_EnumChildWindows(list[i], proc, lp);
	}
	free(list);
	return TRUE;
}

static HWND find_window(const WCHAR *cls, const WCHAR *title)
{
	HWND h = NULL;

	enter();
	for (size_t i = 0; i < nwins && !h; i++) {
		struct window *w = wins[i];
		if (!is_top_level(w, NULL))
			continue;
		if (cls && !class_matches(w->cls, cls))
			continue;
		if (title && (!w->text || wcasecmp_ascii(w->text, title)))
			continue;
		h = w->handle;
	}
	leave();
	return h;
}

WINFN(HWND) user_FindWindowW(LPCWSTR cls, LPCWSTR title)
{
	return find_window(cls, title);
}

WINFN(HWND) user_FindWindowA(LPCSTR cls, LPCSTR title)
{
	WCHAR *f1 = NULL, *f2 = NULL;
	const WCHAR *c = cls ? wide_arg(cls, &f1) : NULL;
	const WCHAR *t = title ? wide_arg(title, &f2) : NULL;
	HWND h = find_window(c, t);

	free(f1);
	free(f2);
	return h;
}

/* Focus and activation: there is no user, so nothing is ever active. */

WINFN(HWND) user_GetDesktopWindow(void) { return user_desktop(); }
WINFN(HWND) user_GetActiveWindow(void) { return NULL; }
WINFN(HWND) user_GetForegroundWindow(void) { return NULL; }
WINFN(HWND) user_GetFocus(void) { return NULL; }
WINFN(HWND) user_GetCapture(void) { return NULL; }
WINFN(HWND) user_SetFocus(HWND hwnd) { return NULL; }
WINFN(HWND) user_SetActiveWindow(HWND hwnd) { return NULL; }
WINFN(HWND) user_SetCapture(HWND hwnd) { return NULL; }
WINFN(BOOL) user_ReleaseCapture(void) { return TRUE; }
WINFN(BOOL) user_SetForegroundWindow(HWND hwnd) { return user_is_window(hwnd); }
WINFN(HWND) user_GetLastActivePopup(HWND hwnd) { return hwnd; }

/* Properties. */

static BOOL prop(HWND hwnd, const WCHAR *name, HANDLE *data, int op)
{
	BOOL found = FALSE;

	enter();
	struct window *w = get(hwnd);
	struct prop **p = w ? &w->props : NULL;
	for (; p && *p; p = &(*p)->next) {
		if (IS_INTRESOURCE(name) ? (*p)->atom == (ATOM)(ULONG_PTR)name :
		    ((*p)->name && !wcasecmp_ascii((*p)->name, name)))
			break;
	}
	if (p && *p) {
		found = TRUE;
		if (op == 0) {		/* get */
			*data = (*p)->data;
		} else if (op == 1) {	/* set */
			(*p)->data = *data;
		} else {		/* remove */
			struct prop *dead = *p;
			*data = dead->data;
			*p = dead->next;
			free(dead->name);
			free(dead);
		}
	} else if (p && op == 1) {
		struct prop *n = calloc(1, sizeof(*n));
		if (n && (IS_INTRESOURCE(name) || (n->name = wdup(name)))) {
			n->atom = IS_INTRESOURCE(name) ? (ATOM)(ULONG_PTR)name : 0;
			n->data = *data;
			n->next = w->props;
			w->props = n;
			found = TRUE;
		} else {
			free(n);
		}
	}
	leave();
	return found;
}

static HANDLE prop_w(HWND hwnd, const WCHAR *name, HANDLE data, int op)
{
	return name && prop(hwnd, name, &data, op) ? data : NULL;
}

static HANDLE prop_a(HWND hwnd, const char *name, HANDLE data, int op)
{
	WCHAR *f;
	const WCHAR *w = name ? wide_arg(name, &f) : NULL;
	HANDLE r = prop_w(hwnd, w, data, op);

	if (name)
		free(f);
	return r;
}

WINFN(BOOL) user_SetPropA(HWND h, LPCSTR n, HANDLE d) { return prop_a(h, n, d, 1) != NULL || !d; }
WINFN(BOOL) user_SetPropW(HWND h, LPCWSTR n, HANDLE d) { return prop_w(h, n, d, 1) != NULL || !d; }
WINFN(HANDLE) user_GetPropA(HWND h, LPCSTR n) { return prop_a(h, n, NULL, 0); }
WINFN(HANDLE) user_GetPropW(HWND h, LPCWSTR n) { return prop_w(h, n, NULL, 0); }
WINFN(HANDLE) user_RemovePropA(HWND h, LPCSTR n) { return prop_a(h, n, NULL, 2); }
WINFN(HANDLE) user_RemovePropW(HWND h, LPCWSTR n) { return prop_w(h, n, NULL, 2); }

/* Message queues. */

static struct queue *queue_for(DWORD tid)
{
	struct queue *q;

	for (q = queues; q; q = q->next)
		if (q->tid == tid)
			return q;
	if ((q = calloc(1, sizeof(*q)))) {
		q->tid = tid;
		q->next = queues;
		queues = q;
	}
	return q;
}

static BOOL post(DWORD tid, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	struct queue *q = queue_for(tid);
	struct msg_node *n = q ? calloc(1, sizeof(*n)) : NULL;

	if (!n) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	n->msg.hwnd = hwnd;
	n->msg.message = msg;
	n->msg.wParam = wp;
	n->msg.lParam = lp;
	n->msg.time = user_ticks();
	n->msg.pt.x = USER_SCREEN_WIDTH / 2;
	n->msg.pt.y = USER_SCREEN_HEIGHT / 2;
	if (q->tail)
		q->tail->next = n;
	else
		q->head = n;
	q->tail = n;
	pthread_cond_broadcast(&cond);
	return TRUE;
}

static BOOL post_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	BOOL ok = TRUE;

	enter();
	if (!hwnd) {
		ok = post(thread_current_id(), NULL, msg, wp, lp);
	} else if (hwnd == HWND_BROADCAST) {
		for (size_t i = 0; i < nwins; i++)
			if (is_top_level(wins[i], NULL))
				post(wins[i]->tid, wins[i]->handle, msg, wp, lp);
	} else {
		struct window *w = get(hwnd);
		ok = w && post(w->tid, hwnd, msg, wp, lp);
	}
	leave();
	return ok;
}

WINFN(BOOL) user_PostMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	return post_message(h, m, wp, lp);
}

WINFN(BOOL) user_PostMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	return post_message(h, m, wp, lp);
}

WINFN(BOOL) user_PostThreadMessageA(DWORD tid, UINT m, WPARAM wp, LPARAM lp)
{
	enter();
	BOOL ok = post(tid, NULL, m, wp, lp);
	leave();
	return ok;
}

WINFN(BOOL) user_PostThreadMessageW(DWORD tid, UINT m, WPARAM wp, LPARAM lp)
{
	return user_PostThreadMessageA(tid, m, wp, lp);
}

WINFN(void) user_PostQuitMessage(int code)
{
	enter();
	struct queue *q = queue_for(thread_current_id());
	if (q) {
		q->quit = 1;
		q->quit_code = code;
		pthread_cond_broadcast(&cond);
	}
	leave();
}

static int in_filter(const MSG *m, HWND hwnd, UINT min, UINT max)
{
	if (hwnd == (HWND)-1 ? m->hwnd != NULL : (hwnd && m->hwnd != hwnd))
		return 0;
	return (!min && !max) || (m->message >= min && m->message <= max);
}

/*
 * The next message for q's thread matching the filter, in Windows' order:
 * posted messages, then WM_QUIT, then due timers. Called with the lock.
 */
static int take(struct queue *q, HWND hwnd, UINT min, UINT max, int remove,
		MSG *out)
{
	uint64_t now = now_ms();

	for (struct msg_node **p = &q->head, *prev = NULL; *p;
	     prev = *p, p = &(*p)->next) {
		if (!in_filter(&(*p)->msg, hwnd, min, max))
			continue;
		*out = (*p)->msg;
		if (remove) {
			struct msg_node *n = *p;
			*p = n->next;
			if (q->tail == n)
				q->tail = prev;
			free(n);
		}
		return 1;
	}
	MSG m;
	memset(&m, 0, sizeof(m));
	m.time = (DWORD)now;
	m.pt.x = USER_SCREEN_WIDTH / 2;
	m.pt.y = USER_SCREEN_HEIGHT / 2;
	/* WM_QUIT comes whatever the filter, as on Windows. */
	if (q->quit) {
		m.message = WM_QUIT;
		m.wParam = q->quit_code;
		if (remove)
			q->quit = 0;
		*out = m;
		return 1;
	}
	for (struct timer *t = timers; t; t = t->next) {
		if (t->tid != q->tid || t->due > now)
			continue;
		m.hwnd = t->hwnd;
		m.message = WM_TIMER;
		m.wParam = t->id;
		m.lParam = (LPARAM)t->proc;
		if (!in_filter(&m, hwnd, min, max))
			continue;
		if (remove)
			t->due = now + t->elapse;
		*out = m;
		return 1;
	}
	return 0;
}

/* The time until q's thread's next timer is due, or -1 if it has none. */
static long next_timer(DWORD tid)
{
	uint64_t now = now_ms();
	long best = -1;

	for (struct timer *t = timers; t; t = t->next) {
		if (t->tid != tid)
			continue;
		long d = t->due > now ? (long)(t->due - now) : 0;
		if (best < 0 || d < best)
			best = d;
	}
	return best;
}

/* Sleeps on the queue condition for at most ms (-1: until woken). */
static void wait_ms(long ms)
{
	if (ms < 0) {
		pthread_cond_wait(&cond, &lock);
		return;
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait(&cond, &lock, &ts);
}

static BOOL get_message(MSG *msg, HWND hwnd, UINT min, UINT max)
{
	MSG m;

	enter();
	struct queue *q = queue_for(thread_current_id());
	while (q && !take(q, hwnd, min, max, 1, &m))
		wait_ms(next_timer(q->tid));
	if (q) {
		q->last_time = m.time;
		q->last_pos = MAKELONG(m.pt.x, m.pt.y);
	}
	leave();
	if (!q)
		return -1;
	if (msg)
		*msg = m;
	return m.message != WM_QUIT;
}

WINFN(BOOL) user_GetMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max)
{
	return get_message(msg, hwnd, min, max);
}

WINFN(BOOL) user_GetMessageW(LPMSG msg, HWND hwnd, UINT min, UINT max)
{
	return get_message(msg, hwnd, min, max);
}

static BOOL peek_message(MSG *msg, HWND hwnd, UINT min, UINT max, UINT flags)
{
	MSG m;

	enter();
	struct queue *q = queue_for(thread_current_id());
	int got = q && take(q, hwnd, min, max, flags & PM_REMOVE, &m);
	if (got) {
		q->last_time = m.time;
		q->last_pos = MAKELONG(m.pt.x, m.pt.y);
	}
	leave();
	if (got && msg)
		*msg = m;
	return got;
}

WINFN(BOOL) user_PeekMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT flags)
{
	return peek_message(msg, hwnd, min, max, flags);
}

WINFN(BOOL) user_PeekMessageW(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT flags)
{
	return peek_message(msg, hwnd, min, max, flags);
}

WINFN(BOOL) user_WaitMessage(void)
{
	MSG m;

	enter();
	struct queue *q = queue_for(thread_current_id());
	while (q && !take(q, NULL, 0, 0, 0, &m))
		wait_ms(next_timer(q->tid));
	leave();
	return q != NULL;
}

static LRESULT dispatch(const MSG *msg, int unicode)
{
	if (!msg)
		return 0;
	if (msg->message == WM_TIMER && msg->lParam) {
		((TIMERPROC)msg->lParam)(msg->hwnd, WM_TIMER, msg->wParam, user_ticks());
		return 0;
	}
	if (!msg->hwnd)
		return 0;
	return send(msg->hwnd, msg->message, msg->wParam, msg->lParam, unicode);
}

WINFN(LRESULT) user_DispatchMessageA(const MSG *msg) { return dispatch(msg, 0); }
WINFN(LRESULT) user_DispatchMessageW(const MSG *msg) { return dispatch(msg, 1); }
WINFN(BOOL) user_TranslateMessage(const MSG *msg) { return FALSE; }
WINFN(BOOL) user_InSendMessage(void) { return FALSE; }
WINFN(BOOL) user_ReplyMessage(LRESULT r) { return FALSE; }

WINFN(LONG) user_GetMessageTime(void)
{
	enter();
	struct queue *q = queue_for(thread_current_id());
	LONG t = q ? (LONG)q->last_time : 0;
	leave();
	return t;
}

WINFN(DWORD) user_GetMessagePos(void)
{
	enter();
	struct queue *q = queue_for(thread_current_id());
	DWORD p = q ? q->last_pos : 0;
	leave();
	return p;
}

static LRESULT send_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
			    int unicode)
{
	if (hwnd == HWND_BROADCAST) {
		size_t n;
		enter();
		HWND *list = snapshot(is_top_level, NULL, &n);
		leave();
		for (size_t i = 0; list && i < n; i++)
			send(list[i], msg, wp, lp, unicode);
		free(list);
		return 0;
	}
	return send(hwnd, msg, wp, lp, unicode);
}

WINFN(LRESULT) user_SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	return send_message(h, m, wp, lp, 0);
}

WINFN(LRESULT) user_SendMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	return send_message(h, m, wp, lp, 1);
}

WINFN(LRESULT) user_SendMessageTimeoutA(HWND h, UINT m, WPARAM wp, LPARAM lp,
					UINT flags, UINT timeout, PDWORD_PTR result)
{
	if (h != HWND_BROADCAST && !user_is_window(h))
		return 0;
	LRESULT r = send_message(h, m, wp, lp, 0);
	if (result)
		*result = r;
	return 1;
}

WINFN(LRESULT) user_SendMessageTimeoutW(HWND h, UINT m, WPARAM wp, LPARAM lp,
					UINT flags, UINT timeout, PDWORD_PTR result)
{
	if (h != HWND_BROADCAST && !user_is_window(h))
		return 0;
	LRESULT r = send_message(h, m, wp, lp, 1);
	if (result)
		*result = r;
	return 1;
}

WINFN(BOOL) user_SendNotifyMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	send_message(h, m, wp, lp, 0);
	return h == HWND_BROADCAST || user_is_window(h);
}

WINFN(BOOL) user_SendNotifyMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	send_message(h, m, wp, lp, 1);
	return h == HWND_BROADCAST || user_is_window(h);
}

/*
 * Waits for the handles or for a message in the queue; the handles are
 * polled, since kernel32's waits know nothing of message queues.
 */
WINFN(DWORD) user_MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE *handles,
					      DWORD timeout, DWORD mask,
					      DWORD flags)
{
	DWORD (WINAPI *wait)(DWORD, const HANDLE *, BOOL, DWORD) =
		builtin_export(&builtin_kernel32, "WaitForMultipleObjects");
	uint64_t end = now_ms() + timeout;
	MSG m;

	for (;;) {
		enter();
		struct queue *q = queue_for(thread_current_id());
		int ready = mask && q && take(q, NULL, 0, 0, 0, &m);
		long slice = 10;
		if (!ready && !n && q) {
			/* Nothing else to watch: sleep on the queue. */
			long t = next_timer(q->tid);
			long left = timeout == INFINITE ? -1 :
				    (long)(end > now_ms() ? end - now_ms() : 0);
			slice = t < 0 ? left : (left < 0 || t < left ? t : left);
			if (slice)
				wait_ms(slice);
			ready = mask && take(q, NULL, 0, 0, 0, &m);
		}
		leave();
		if (ready)
			return WAIT_OBJECT_0 + n;
		if (n) {
			uint64_t now = now_ms();
			if (timeout != INFINITE)
				slice = now >= end ? 0 : (end - now < 10 ? (long)(end - now) : 10);
			DWORD r = wait(n, handles, flags & 1, slice);
			if (r != WAIT_TIMEOUT)
				return r;
		}
		if (timeout != INFINITE && now_ms() >= end)
			return WAIT_TIMEOUT;
	}
}

WINFN(DWORD) user_MsgWaitForMultipleObjects(DWORD n, const HANDLE *handles,
					    BOOL all, DWORD timeout, DWORD mask)
{
	return user_MsgWaitForMultipleObjectsEx(n, handles, timeout, mask,
						all ? 1 : 0);
}

/* Timers. */

WINFN(UINT_PTR) user_SetTimer(HWND hwnd, UINT_PTR id, UINT elapse, TIMERPROC proc)
{
	UINT_PTR r = 0;

	enter();
	if (hwnd && !get(hwnd))
		goto out;
	struct timer *t = NULL;
	if (hwnd)
		for (t = timers; t && !(t->hwnd == hwnd && t->id == id); t = t->next)
			;
	if (!t) {
		if (!(t = calloc(1, sizeof(*t)))) {
			win_set_error(ERROR_NOT_ENOUGH_MEMORY);
			goto out;
		}
		t->hwnd = hwnd;
		t->id = hwnd ? id : next_timer_id++;
		t->next = timers;
		timers = t;
	}
	t->elapse = elapse ? elapse : 1;
	t->proc = proc;
	t->tid = hwnd ? find(hwnd)->tid : thread_current_id();
	t->due = now_ms() + t->elapse;
	r = hwnd ? 1 : t->id;
	if (hwnd && id)
		r = id;
	pthread_cond_broadcast(&cond);
out:
	leave();
	return r;
}

WINFN(BOOL) user_KillTimer(HWND hwnd, UINT_PTR id)
{
	BOOL ok = FALSE;

	enter();
	for (struct timer **t = &timers; *t; t = &(*t)->next) {
		if ((*t)->hwnd == hwnd && (*t)->id == id) {
			struct timer *dead = *t;
			*t = dead->next;
			free(dead);
			ok = TRUE;
			break;
		}
	}
	leave();
	return ok;
}

/* Dialogs: there is no GUI to show them on. */

static void no_dialog(const char *fn)
{
	vfw_warn("%s: a DLL tried to open a dialog box, but there is no GUI; "
		 "it fails", fn);
	win_set_error(ERROR_CALL_NOT_IMPLEMENTED);
}

WINFN(INT_PTR) user_DialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent,
				    DLGPROC proc, LPARAM param)
{
	no_dialog("DialogBoxParamA");
	return -1;
}

WINFN(INT_PTR) user_DialogBoxParamW(HINSTANCE inst, LPCWSTR tmpl, HWND parent,
				    DLGPROC proc, LPARAM param)
{
	no_dialog("DialogBoxParamW");
	return -1;
}

WINFN(INT_PTR) user_DialogBoxIndirectParamA(HINSTANCE inst, const void *tmpl,
					    HWND parent, DLGPROC proc,
					    LPARAM param)
{
	no_dialog("DialogBoxIndirectParamA");
	return -1;
}

WINFN(INT_PTR) user_DialogBoxIndirectParamW(HINSTANCE inst, const void *tmpl,
					    HWND parent, DLGPROC proc,
					    LPARAM param)
{
	no_dialog("DialogBoxIndirectParamW");
	return -1;
}

WINFN(HWND) user_CreateDialogParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent,
				    DLGPROC proc, LPARAM param)
{
	no_dialog("CreateDialogParamA");
	return NULL;
}

WINFN(HWND) user_CreateDialogParamW(HINSTANCE inst, LPCWSTR tmpl, HWND parent,
				    DLGPROC proc, LPARAM param)
{
	no_dialog("CreateDialogParamW");
	return NULL;
}

WINFN(HWND) user_CreateDialogIndirectParamA(HINSTANCE inst, const void *tmpl,
					    HWND parent, DLGPROC proc,
					    LPARAM param)
{
	no_dialog("CreateDialogIndirectParamA");
	return NULL;
}

WINFN(HWND) user_CreateDialogIndirectParamW(HINSTANCE inst, const void *tmpl,
					    HWND parent, DLGPROC proc,
					    LPARAM param)
{
	no_dialog("CreateDialogIndirectParamW");
	return NULL;
}

WINFN(BOOL) user_EndDialog(HWND hwnd, INT_PTR result)
{
	win_set_error(ERROR_INVALID_WINDOW_HANDLE);
	return FALSE;
}

/* Dialog items: the child windows with a given id. */

WINFN(HWND) user_GetDlgItem(HWND hwnd, int id)
{
	HWND h = NULL;

	enter();
	for (size_t i = 0; i < nwins && !h; i++)
		if (wins[i]->parent == hwnd && wins[i]->id == (UINT_PTR)id)
			h = wins[i]->handle;
	if (!h)
		win_set_error(ERROR_CONTROL_ID_NOT_FOUND);
	leave();
	return h;
}

WINFN(int) user_GetDlgCtrlID(HWND hwnd)
{
	enter();
	struct window *w = get(hwnd);
	int id = w && w->parent ? (int)w->id : 0;
	leave();
	return id;
}

WINFN(LRESULT) user_SendDlgItemMessageA(HWND hwnd, int id, UINT m, WPARAM wp,
					LPARAM lp)
{
	HWND item = user_GetDlgItem(hwnd, id);
	return item ? send(item, m, wp, lp, 0) : 0;
}

WINFN(LRESULT) user_SendDlgItemMessageW(HWND hwnd, int id, UINT m, WPARAM wp,
					LPARAM lp)
{
	HWND item = user_GetDlgItem(hwnd, id);
	return item ? send(item, m, wp, lp, 1) : 0;
}

WINFN(BOOL) user_SetDlgItemTextA(HWND hwnd, int id, LPCSTR text)
{
	HWND item = user_GetDlgItem(hwnd, id);
	return item && send(item, WM_SETTEXT, 0, (LPARAM)text, 0);
}

WINFN(BOOL) user_SetDlgItemTextW(HWND hwnd, int id, LPCWSTR text)
{
	HWND item = user_GetDlgItem(hwnd, id);
	return item && send(item, WM_SETTEXT, 0, (LPARAM)text, 1);
}

WINFN(UINT) user_GetDlgItemTextA(HWND hwnd, int id, LPSTR buf, int size)
{
	HWND item = user_GetDlgItem(hwnd, id);
	if (buf && size > 0)
		*buf = '\0';
	return item ? send(item, WM_GETTEXT, size, (LPARAM)buf, 0) : 0;
}

WINFN(UINT) user_GetDlgItemTextW(HWND hwnd, int id, LPWSTR buf, int size)
{
	HWND item = user_GetDlgItem(hwnd, id);
	if (buf && size > 0)
		*buf = 0;
	return item ? send(item, WM_GETTEXT, size, (LPARAM)buf, 1) : 0;
}

WINFN(BOOL) user_SetDlgItemInt(HWND hwnd, int id, UINT value, BOOL is_signed)
{
	char buf[16];

	snprintf(buf, sizeof(buf), is_signed ? "%d" : "%u", value);
	return user_SetDlgItemTextA(hwnd, id, buf);
}

WINFN(UINT) user_GetDlgItemInt(HWND hwnd, int id, BOOL *ok, BOOL is_signed)
{
	char buf[32], *end;

	if (ok)
		*ok = FALSE;
	if (!user_GetDlgItemTextA(hwnd, id, buf, sizeof(buf)))
		return 0;
	long v = strtol(buf, &end, 10);
	if (end == buf || *end || (!is_signed && v < 0))
		return 0;
	if (ok)
		*ok = TRUE;
	return (UINT)v;
}

WINFN(BOOL) user_CheckDlgButton(HWND hwnd, int id, UINT check)
{
	HWND item = user_GetDlgItem(hwnd, id);
	if (item)
		send(item, BM_SETCHECK, check, 0, 1);
	return item != NULL;
}

WINFN(UINT) user_IsDlgButtonChecked(HWND hwnd, int id)
{
	HWND item = user_GetDlgItem(hwnd, id);
	return item ? send(item, BM_GETCHECK, 0, 0, 1) : 0;
}

#define E(name) { #name, 0, (void *)user_##name }

const struct builtin_export u32_window_exports[] = {
	E(CallWindowProcA),
	E(CallWindowProcW),
	E(CheckDlgButton),
	E(ClientToScreen),
	E(CreateDialogIndirectParamA),
	E(CreateDialogIndirectParamW),
	E(CreateDialogParamA),
	E(CreateDialogParamW),
	E(CreateWindowExA),
	E(CreateWindowExW),
	E(DefWindowProcA),
	E(DefWindowProcW),
	E(DestroyWindow),
	E(DialogBoxIndirectParamA),
	E(DialogBoxIndirectParamW),
	E(DialogBoxParamA),
	E(DialogBoxParamW),
	E(DispatchMessageA),
	E(DispatchMessageW),
	E(EnableWindow),
	E(EndDialog),
	E(EnumChildWindows),
	E(EnumThreadWindows),
	E(EnumWindows),
	E(FindWindowA),
	E(FindWindowW),
	E(GetActiveWindow),
	E(GetCapture),
	E(GetClassInfoA),
	E(GetClassInfoExA),
	E(GetClassInfoExW),
	E(GetClassInfoW),
	E(GetClassNameA),
	E(GetClassNameW),
	E(GetClientRect),
	E(GetDesktopWindow),
	E(GetDlgCtrlID),
	E(GetDlgItem),
	E(GetDlgItemInt),
	E(GetDlgItemTextA),
	E(GetDlgItemTextW),
	E(GetFocus),
	E(GetForegroundWindow),
	E(GetLastActivePopup),
	E(GetMessageA),
	E(GetMessagePos),
	E(GetMessageTime),
	E(GetMessageW),
	E(GetParent),
	E(GetPropA),
	E(GetPropW),
	E(GetWindowLongA),
	E(GetWindowLongW),
	E(GetWindowRect),
	E(GetWindowTextA),
	E(GetWindowTextLengthA),
	E(GetWindowTextLengthW),
	E(GetWindowTextW),
	E(GetWindowThreadProcessId),
	E(InSendMessage),
	E(InvalidateRect),
	E(IsChild),
	E(IsDlgButtonChecked),
	E(IsIconic),
	E(IsWindow),
	E(IsWindowEnabled),
	E(IsWindowUnicode),
	E(IsWindowVisible),
	E(IsZoomed),
	E(KillTimer),
	E(MoveWindow),
	E(MsgWaitForMultipleObjects),
	E(MsgWaitForMultipleObjectsEx),
	E(PeekMessageA),
	E(PeekMessageW),
	E(PostMessageA),
	E(PostMessageW),
	E(PostQuitMessage),
	E(PostThreadMessageA),
	E(PostThreadMessageW),
	E(RegisterClassA),
	E(RegisterClassExA),
	E(RegisterClassExW),
	E(RegisterClassW),
	E(ReleaseCapture),
	E(RemovePropA),
	E(RemovePropW),
	E(ReplyMessage),
	E(ScreenToClient),
	E(SendDlgItemMessageA),
	E(SendDlgItemMessageW),
	E(SendMessageA),
	E(SendMessageTimeoutA),
	E(SendMessageTimeoutW),
	E(SendMessageW),
	E(SendNotifyMessageA),
	E(SendNotifyMessageW),
	E(SetActiveWindow),
	E(SetCapture),
	E(SetDlgItemInt),
	E(SetDlgItemTextA),
	E(SetDlgItemTextW),
	E(SetFocus),
	E(SetForegroundWindow),
	E(SetParent),
	E(SetPropA),
	E(SetPropW),
	E(SetTimer),
	E(SetWindowLongA),
	E(SetWindowLongW),
	E(SetWindowPos),
	E(SetWindowTextA),
	E(SetWindowTextW),
	E(ShowWindow),
	E(TranslateMessage),
	E(UnregisterClassA),
	E(UnregisterClassW),
	E(UpdateWindow),
	E(ValidateRect),
	E(WaitMessage),
	BUILTIN_END
};
