/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * user32, called through the export table the way a DLL's imports reach
 * it: classes, windows and messages, timers, strings, and the rest.
 */
#include <sched.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "test.h"

static struct module *u32, *gdi, *k32;

static void *get_from(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "%s has no %s\n", m->name, name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(u32, #name)
#define GDI(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(gdi, #name)
#define K32(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(k32, #name)

static WCHAR *w(const char *s)
{
	static WCHAR bufs[4][64];
	static int next;
	WCHAR *b = bufs[next++ % 4];
	size_t i = 0;

	for (; s[i] && i < 63; i++)
		b[i] = (BYTE)s[i];
	b[i] = 0;
	return b;
}

/* What the test window procedures saw. */
static char log_buf[1024];
static char created_name[64];
static WCHAR created_name_w[64];
static WNDPROC def_a, def_w;

static void note(const char *s)
{
	size_t n = strlen(log_buf);
	snprintf(log_buf + n, sizeof(log_buf) - n, "%s%s", n ? "," : "", s);
}

static LRESULT WINAPI proc_a(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	char buf[32];

	switch (msg) {
	case WM_NCCREATE:
		note("nccreate");
		snprintf(created_name, sizeof(created_name), "%s",
			 ((CREATESTRUCTA *)lp)->lpszName);
		break;
	case WM_CREATE:
		note("create");
		return 0;
	case WM_DESTROY:
		snprintf(buf, sizeof(buf), "destroy:%p", (void *)hwnd);
		note("destroy");
		return 0;
	case WM_NCDESTROY:
		note("ncdestroy");
		return 0;
	case WM_SHOWWINDOW:
		note(wp ? "show" : "hide");
		return 0;
	case WM_SIZE:
		snprintf(buf, sizeof(buf), "size:%dx%d", LOWORD(lp), HIWORD(lp));
		note(buf);
		return 0;
	case WM_MOVE:
		note("move");
		return 0;
	case WM_SETTEXT:
		/* An ANSI procedure gets ANSI text, whoever sent it. */
		snprintf(buf, sizeof(buf), "settext:%s", (const char *)lp);
		note(buf);
		break;
	case WM_USER:
		return wp * 10 + lp;
	}
	return def_a(hwnd, msg, wp, lp);
}

static LRESULT WINAPI proc_w(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		const WCHAR *n = ((CREATESTRUCTW *)lp)->lpszName;
		size_t i = 0;
		for (; n && n[i] && i < 63; i++)
			created_name_w[i] = n[i];
		created_name_w[i] = 0;
	}
	if (msg == WM_CREATE && ((CREATESTRUCTW *)lp)->lpCreateParams == (void *)1)
		return -1;	/* refuse */
	return def_w(hwnd, msg, wp, lp);
}

static void test_classes_and_windows(void)
{
	FN(ATOM, RegisterClassA, const WNDCLASSA *);
	FN(ATOM, RegisterClassExW, const WNDCLASSEXW *);
	FN(BOOL, UnregisterClassA, LPCSTR, HINSTANCE);
	FN(BOOL, GetClassInfoA, HINSTANCE, LPCSTR, WNDCLASSA *);
	FN(HWND, CreateWindowExA, DWORD, LPCSTR, LPCSTR, DWORD, int, int, int,
	   int, HWND, HMENU, HINSTANCE, LPVOID);
	FN(HWND, CreateWindowExW, DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int,
	   int, HWND, HMENU, HINSTANCE, LPVOID);
	FN(BOOL, DestroyWindow, HWND);
	FN(BOOL, IsWindow, HWND);
	FN(BOOL, IsWindowVisible, HWND);
	FN(BOOL, ShowWindow, HWND, int);
	FN(BOOL, MoveWindow, HWND, int, int, int, int, BOOL);
	FN(BOOL, GetWindowRect, HWND, LPRECT);
	FN(BOOL, GetClientRect, HWND, LPRECT);
	FN(BOOL, ClientToScreen, HWND, LPPOINT);
	FN(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	FN(BOOL, SetWindowTextW, HWND, LPCWSTR);
	FN(int, GetWindowTextA, HWND, LPSTR, int);
	FN(int, GetWindowTextW, HWND, LPWSTR, int);
	FN(int, GetWindowTextLengthA, HWND);
	FN(LONG, GetWindowLongA, HWND, int);
	FN(LONG, SetWindowLongA, HWND, int, LONG);
	LONG_PTR (WINAPI *GetWindowLongPtrA)(HWND, int) =
		(LONG_PTR (WINAPI *)(HWND, int))get_from(u32, WLP_NAME(Get, A));
	LONG_PTR (WINAPI *SetWindowLongPtrA)(HWND, int, LONG_PTR) =
		(LONG_PTR (WINAPI *)(HWND, int, LONG_PTR))
			get_from(u32, WLP_NAME(Set, A));
	FN(HWND, GetParent, HWND);
	FN(HWND, GetDlgItem, HWND, int);
	FN(int, GetClassNameA, HWND, LPSTR, int);
	FN(HWND, FindWindowA, LPCSTR, LPCSTR);
	FN(BOOL, SetPropA, HWND, LPCSTR, HANDLE);
	FN(HANDLE, GetPropA, HWND, LPCSTR);
	FN(HANDLE, RemovePropA, HWND, LPCSTR);
	FN(BOOL, IsChild, HWND, HWND);
	FN(HWND, GetDesktopWindow, void);
	FN(DWORD, GetWindowThreadProcessId, HWND, LPDWORD);
	K32(DWORD, GetLastError, void);
#if defined(__x86_64__)
	K32(void, SetLastError, DWORD);
#endif
	K32(DWORD, GetCurrentThreadId, void);

	def_a = (WNDPROC)get_from(u32, "DefWindowProcA");
	def_w = (WNDPROC)get_from(u32, "DefWindowProcW");
	HINSTANCE inst = (HINSTANCE)0x10000000;

	WNDCLASSA wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = proc_a;
	wc.hInstance = inst;
	wc.lpszClassName = "TestA";
	wc.cbWndExtra = 8;
	ATOM a = RegisterClassA(&wc);
	CHECK(a >= 0xc000);
	CHECK(!RegisterClassA(&wc));
	CHECK_EQ(GetLastError(), ERROR_CLASS_ALREADY_EXISTS);
	WNDCLASSA info;
	CHECK(GetClassInfoA(inst, "testa", &info));
	CHECK(info.lpfnWndProc == proc_a);
	CHECK_EQ(info.cbWndExtra, 8);
	CHECK_STR(info.lpszClassName, "TestA");
	CHECK(GetClassInfoA(NULL, "STATIC", &info));	/* a system class */

	WNDCLASSEXW wcw;
	memset(&wcw, 0, sizeof(wcw));
	wcw.cbSize = sizeof(wcw);
	wcw.lpfnWndProc = proc_w;
	wcw.lpszClassName = w("TestW");
	CHECK(RegisterClassExW(&wcw));

	/* Creation: messages in order, text from the CREATESTRUCT. */
	log_buf[0] = '\0';
	HWND h = CreateWindowExA(0, "TESTA", "caf\xe9", WS_OVERLAPPED | WS_VISIBLE,
				 10, 20, 100, 50, NULL, NULL, inst, NULL);
	CHECK(h != NULL);
	CHECK_STR(log_buf, "nccreate,create,show");
	CHECK_STR(created_name, "caf\xe9");
	CHECK(IsWindow(h) && IsWindowVisible(h));
	char buf[64];
	CHECK_EQ(GetWindowTextA(h, buf, sizeof(buf)), 4);
	CHECK_STR(buf, "caf\xe9");
	WCHAR wbuf[64];
	CHECK_EQ(GetWindowTextW(h, wbuf, 64), 4);
	CHECK_EQ(wbuf[3], 0xe9);
	CHECK_EQ(GetWindowTextA(h, buf, 3), 2);
	CHECK_STR(buf, "ca");
	CHECK_EQ(GetWindowTextLengthA(h), 4);
	CHECK_EQ(GetClassNameA(h, buf, sizeof(buf)), 5);
	CHECK_STR(buf, "TestA");
	CHECK(FindWindowA("testa", NULL) == h);
	CHECK(FindWindowA(NULL, "caf\xe9") == h);
	CHECK(FindWindowA("TestW", NULL) == NULL);
	DWORD pid = 0;
	CHECK_EQ(GetWindowThreadProcessId(h, &pid), GetCurrentThreadId());
	CHECK_EQ(pid, getpid());

	/* A wide sender, an ANSI procedure: the text is converted. */
	log_buf[0] = '\0';
	static const WCHAR euro[] = { 0x20ac, '!', 0 };
	CHECK(SetWindowTextW(h, euro));
	CHECK_STR(log_buf, "settext:\x80!");
	CHECK_EQ(SendMessageW(h, WM_USER, 4, 2), 42);

	/* Geometry. */
	RECT rc;
	CHECK(GetWindowRect(h, &rc));
	CHECK(rc.left == 10 && rc.top == 20 && rc.right == 110 && rc.bottom == 70);
	CHECK(GetClientRect(h, &rc));
	CHECK(rc.left == 0 && rc.right == 100 && rc.bottom == 50);
	log_buf[0] = '\0';
	CHECK(MoveWindow(h, 0, 0, 30, 40, TRUE));
	CHECK_STR(log_buf, "move,size:30x40");
	log_buf[0] = '\0';
	CHECK(ShowWindow(h, SW_HIDE));
	CHECK(!IsWindowVisible(h));
	CHECK_STR(log_buf, "hide");

	/* Window longs: the fields and the extra bytes. */
	CHECK_EQ(SetWindowLongA(h, GWLP_USERDATA, 77), 0);
	CHECK_EQ(GetWindowLongA(h, GWLP_USERDATA), 77);
	CHECK_EQ(SetWindowLongA(h, 4, 5), 0);
	CHECK_EQ(GetWindowLongA(h, 4), 5);
	CHECK_EQ(GetWindowLongA(h, 8), 0);
	CHECK_EQ(GetLastError(), ERROR_INVALID_INDEX);
	CHECK(GetWindowLongPtrA(h, GWLP_WNDPROC) == (LONG_PTR)proc_a);
	/* Read as Unicode, the ANSI procedure is a handle CallWindowProcW
	 * converts through; set back, it is the procedure again. */
	{
#if defined(__x86_64__)
#define PTR "Ptr"
#else
#define PTR ""		/* the Ptr forms are these, on Win32 */
#endif
		LONG_PTR (WINAPI *GetWLPW)(HWND, int) =
			get_from(u32, "GetWindowLong" PTR "W");
		LONG_PTR (WINAPI *SetWLPW)(HWND, int, LONG_PTR) =
			get_from(u32, "SetWindowLong" PTR "W");
		ULONG_PTR (WINAPI *GetCLPA)(HWND, int) =
			get_from(u32, "GetClassLong" PTR "A");
		ULONG_PTR (WINAPI *GetCLPW)(HWND, int) =
			get_from(u32, "GetClassLong" PTR "W");
		ULONG_PTR (WINAPI *SetCLPW)(HWND, int, LONG_PTR) =
			get_from(u32, "SetClassLong" PTR "W");
		LONG_PTR (WINAPI *GetWLPA)(HWND, int) =
			get_from(u32, "GetWindowLong" PTR "A");
		FN(LRESULT, CallWindowProcW, WNDPROC, HWND, UINT, WPARAM, LPARAM);
		FN(LRESULT, CallWindowProcA, WNDPROC, HWND, UINT, WPARAM, LPARAM);
		FN(BOOL, IsWindowUnicode, HWND);
		LONG_PTR wp = GetWLPW(h, GWLP_WNDPROC);
		CHECK(wp && wp != (LONG_PTR)proc_a);
		CHECK(GetWLPW(h, GWLP_WNDPROC) == wp);	/* stable */
		log_buf[0] = '\0';
		CHECK_EQ(CallWindowProcW((WNDPROC)wp, h, WM_SETTEXT, 0,
					 (LPARAM)w("hi")), TRUE);
		CHECK_STR(log_buf, "settext:hi");
		CHECK_EQ(CallWindowProcA((WNDPROC)wp, h, WM_USER, 4, 2), 42);
		CHECK(SetWLPW(h, GWLP_WNDPROC, wp) == wp);
		CHECK(GetWLPA(h, GWLP_WNDPROC) == (LONG_PTR)proc_a);
		CHECK(!IsWindowUnicode(h));
		/* The same for the class's procedure. */
		ULONG_PTR cp = GetCLPW(h, GCLP_WNDPROC);
		CHECK(cp && cp != (ULONG_PTR)proc_a);
		CHECK(GetCLPA(h, GCLP_WNDPROC) == (ULONG_PTR)proc_a);
		CHECK(SetCLPW(h, GCLP_WNDPROC, (LONG_PTR)cp) == cp);
		CHECK(GetCLPA(h, GCLP_WNDPROC) == (ULONG_PTR)proc_a);
		log_buf[0] = '\0';
		/* Past 256 procedures read that way, each still keeps its
		 * encoding (none of these is called). */
		int kept = 1;
		for (int i = 1; i <= 300; i++) {
			LONG_PTR fake = (LONG_PTR)proc_a + i;
			SetWindowLongPtrA(h, GWLP_WNDPROC, fake);
			LONG_PTR hp = GetWLPW(h, GWLP_WNDPROC);
			SetWLPW(h, GWLP_WNDPROC, hp);
			kept &= hp != fake && GetWLPA(h, GWLP_WNDPROC) == fake &&
				!IsWindowUnicode(h);
		}
		CHECK(kept);
		SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)proc_a);
	}
	/* The Ptr forms keep the whole pointer. */
	CHECK_EQ(SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)&log_buf), 77);
	CHECK(GetWindowLongPtrA(h, GWLP_USERDATA) == (LONG_PTR)&log_buf);
#if defined(__x86_64__)
	/* Win64: only they have the pointer fields. */
	SetLastError(0);
	CHECK_EQ(GetWindowLongA(h, GWLP_WNDPROC), 0);
	CHECK_EQ(GetLastError(), ERROR_INVALID_INDEX);
#endif
	CHECK(!(GetWindowLongA(h, GWL_STYLE) & WS_VISIBLE));

	/* Properties. */
	CHECK(SetPropA(h, "Key", (HANDLE)9));
	CHECK(GetPropA(h, "KEY") == (HANDLE)9);
	CHECK(RemovePropA(h, "key") == (HANDLE)9);
	CHECK(GetPropA(h, "key") == NULL);

	/* A child, which goes when its parent does. */
	HWND child = CreateWindowExA(0, "STATIC", "kid", WS_CHILD, 5, 5, 10, 10, h,
				     (HMENU)101, inst, NULL);
	CHECK(child != NULL);
	CHECK(GetParent(child) == h);
	CHECK(IsChild(h, child));
	CHECK(GetDlgItem(h, 101) == child);
	POINT pt = { 1, 1 };
	CHECK(ClientToScreen(child, &pt));
	CHECK(pt.x == 6 && pt.y == 6);
	/* The tree in z-order: children in creation order, first on top. */
	{
		FN(HWND, GetWindow, HWND, UINT);
		FN(HWND, GetTopWindow, HWND);
		HWND kid2 = CreateWindowExA(0, "STATIC", "kid2", WS_CHILD, 0, 0,
					    1, 1, h, (HMENU)102, inst, NULL);
		CHECK(kid2 != NULL);
		CHECK(GetWindow(h, 5) == child);		/* GW_CHILD */
		CHECK(GetTopWindow(h) == child);
		CHECK(GetWindow(child, 2) == kid2);	/* GW_HWNDNEXT */
		CHECK(GetWindow(kid2, 2) == NULL);
		CHECK(GetWindow(kid2, 3) == child);	/* GW_HWNDPREV */
		CHECK(GetWindow(kid2, 0) == child);	/* GW_HWNDFIRST */
		CHECK(GetWindow(child, 1) == kid2);	/* GW_HWNDLAST */
		CHECK(GetWindow(child, 5) == NULL);
		CHECK(GetWindow(child, 4) == NULL);	/* GW_OWNER */
		CHECK(GetWindow((HWND)0x1234, 5) == NULL);
		CHECK(DestroyWindow(kid2));
		CHECK(GetWindow(child, 2) == NULL);
	}
	CHECK(CreateWindowExA(0, "STATIC", "", WS_CHILD, 0, 0, 1, 1, NULL, NULL,
			      inst, NULL) == NULL);
	CHECK_EQ(GetLastError(), ERROR_TLW_WITH_WSCHILD);
	CHECK(CreateWindowExA(0, "NoSuchClass", "", 0, 0, 0, 1, 1, NULL, NULL,
			      inst, NULL) == NULL);
	CHECK_EQ(GetLastError(), ERROR_CANNOT_FIND_WND_CLASS);

	/* A class with windows can't go; a destroyed window is gone. */
	CHECK(!UnregisterClassA("TestA", inst));
	CHECK_EQ(GetLastError(), ERROR_CLASS_HAS_WINDOWS);
	log_buf[0] = '\0';
	CHECK(DestroyWindow(h));
	CHECK_STR(log_buf, "destroy,ncdestroy");
	CHECK(!IsWindow(h) && !IsWindow(child));
	CHECK(!DestroyWindow(h));
	CHECK_EQ(GetLastError(), ERROR_INVALID_WINDOW_HANDLE);
	CHECK(UnregisterClassA("TestA", inst));

	/* The wide class: an ANSI creator's name arrives wide; -1 refuses. */
	HWND hw = CreateWindowExA(0, "TestW", "wide\xe9", 0, 0, 0, 1, 1, NULL,
				  NULL, NULL, NULL);
	CHECK(hw != NULL);
	CHECK(created_name_w[0] == 'w' && created_name_w[4] == 0xe9);
	CHECK_EQ(GetWindowTextA(hw, buf, sizeof(buf)), 5);
	CHECK_STR(buf, "wide\xe9");
	DestroyWindow(hw);
	CHECK(CreateWindowExW(0, w("TestW"), w("x"), 0, 0, 0, 1, 1, NULL, NULL,
			      NULL, (LPVOID)1) == NULL);

	/* The desktop. */
	HWND desk = GetDesktopWindow();
	CHECK(IsWindow(desk));
	CHECK(GetWindowRect(desk, &rc));
	CHECK(rc.right == 1024 && rc.bottom == 768);
	CHECK(!DestroyWindow(desk));
}

static int timer_calls;
static UINT_PTR timer_id;

static void WINAPI timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
	timer_calls++;
	timer_id = id;
}

static DWORD WINAPI poster(LPVOID arg)
{
	LRESULT (WINAPI *post)(DWORD, UINT, WPARAM, LPARAM) =
		(LRESULT (WINAPI *)(DWORD, UINT, WPARAM, LPARAM))
			get_from(u32, "PostThreadMessageA");
	void (WINAPI *sleep)(DWORD) = (void (WINAPI *)(DWORD))get_from(k32, "Sleep");
	sleep(30);
	post((DWORD)(ULONG_PTR)arg, WM_APP + 1, 5, 6);
	return 0;
}

static void test_messages(void)
{
	FN(BOOL, PostMessageA, HWND, UINT, WPARAM, LPARAM);
	FN(BOOL, PostThreadMessageA, DWORD, UINT, WPARAM, LPARAM);
	FN(BOOL, PeekMessageA, LPMSG, HWND, UINT, UINT, UINT);
	FN(BOOL, GetMessageA, LPMSG, HWND, UINT, UINT);
	FN(LRESULT, DispatchMessageA, const MSG *);
	FN(void, PostQuitMessage, int);
	FN(UINT_PTR, SetTimer, HWND, UINT_PTR, UINT, TIMERPROC);
	FN(BOOL, KillTimer, HWND, UINT_PTR);
	FN(DWORD, MsgWaitForMultipleObjects, DWORD, const HANDLE *, BOOL, DWORD,
	   DWORD);
	FN(HWND, CreateWindowExA, DWORD, LPCSTR, LPCSTR, DWORD, int, int, int,
	   int, HWND, HMENU, HINSTANCE, LPVOID);
	FN(BOOL, DestroyWindow, HWND);
	FN(UINT, RegisterWindowMessageA, LPCSTR);
	FN(UINT, RegisterWindowMessageW, LPCWSTR);
	K32(DWORD, GetCurrentThreadId, void);
	K32(HANDLE, CreateThread, LPSECURITY_ATTRIBUTES, SIZE_T,
	    LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
	K32(HANDLE, CreateEventA, LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
	K32(BOOL, SetEvent, HANDLE);
	K32(DWORD, WaitForSingleObject, HANDLE, DWORD);
	K32(BOOL, CloseHandle, HANDLE);

	MSG m;
	HWND h = CreateWindowExA(0, "STATIC", "", 0, 0, 0, 1, 1, HWND_MESSAGE,
				 NULL, NULL, NULL);
	CHECK(h != NULL);

	/* Posted messages come in order; filters pick by window and range. */
	CHECK(PostMessageA(h, WM_USER + 1, 1, 0));
	CHECK(PostMessageA(NULL, WM_USER + 2, 2, 0));
	CHECK(PostMessageA(h, WM_USER + 3, 3, 0));
	CHECK(PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE));
	CHECK_EQ(m.message, WM_USER + 1);
	CHECK(PeekMessageA(&m, NULL, WM_USER + 2, WM_USER + 3, PM_REMOVE));
	CHECK_EQ(m.message, WM_USER + 2);
	CHECK(m.hwnd == NULL);
	CHECK(!PeekMessageA(&m, (HWND)-1, 0, 0, PM_REMOVE));
	CHECK(PeekMessageA(&m, h, 0, 0, PM_REMOVE));
	CHECK_EQ(m.wParam, 1);
	CHECK(GetMessageA(&m, NULL, 0, 0));
	CHECK_EQ(m.wParam, 3);
	CHECK(!PeekMessageA(&m, NULL, 0, 0, PM_REMOVE));
	CHECK(!PostMessageA((HWND)0x1234, WM_USER, 0, 0));

	/* A message from another thread wakes GetMessage. */
	HANDLE t = CreateThread(NULL, 0, poster,
				(LPVOID)(ULONG_PTR)GetCurrentThreadId(), 0, NULL);
	CHECK(t != NULL);
	CHECK(GetMessageA(&m, NULL, 0, 0));
	CHECK(m.message == WM_APP + 1 && m.wParam == 5 && m.lParam == 6);
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);

	/* Timers: a window's arrives as WM_TIMER; a proc runs on dispatch. */
	CHECK_EQ(SetTimer(h, 3, 1, NULL), 3);
	CHECK(GetMessageA(&m, h, WM_TIMER, WM_TIMER));
	CHECK(m.hwnd == h && m.wParam == 3 && !m.lParam);
	CHECK(KillTimer(h, 3));
	CHECK(!KillTimer(h, 3));
	UINT_PTR id = SetTimer(NULL, 0, 1, timer_proc);
	CHECK(id != 0);
	CHECK(GetMessageA(&m, NULL, 0, 0));
	CHECK_EQ(m.message, WM_TIMER);
	DispatchMessageA(&m);
	CHECK_EQ(timer_calls, 1);
	CHECK_EQ(timer_id, id);
	CHECK(KillTimer(NULL, id));

	/* MsgWaitForMultipleObjects: a handle, a message, a timeout. */
	HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
	CHECK_EQ(MsgWaitForMultipleObjects(1, &ev, FALSE, 20, 0x4ff), WAIT_TIMEOUT);
	SetEvent(ev);
	CHECK_EQ(MsgWaitForMultipleObjects(1, &ev, FALSE, 1000, 0x4ff), WAIT_OBJECT_0);
	PostThreadMessageA(GetCurrentThreadId(), WM_USER, 0, 0);
	CHECK_EQ(MsgWaitForMultipleObjects(0, NULL, FALSE, 1000, 0x4ff), WAIT_OBJECT_0);
	CHECK(PeekMessageA(&m, NULL, 0, 0, PM_REMOVE));
	CHECK_EQ(MsgWaitForMultipleObjects(0, NULL, FALSE, 10, 0x4ff), WAIT_TIMEOUT);
	CloseHandle(ev);

	/* WM_QUIT: after the posted messages, whatever the filter. */
	PostMessageA(h, WM_USER, 0, 0);
	PostQuitMessage(3);
	CHECK(GetMessageA(&m, NULL, 0, 0));
	CHECK_EQ(m.message, WM_USER);
	CHECK(!GetMessageA(&m, h, WM_USER, WM_USER));
	CHECK(m.message == WM_QUIT && m.wParam == 3);
	CHECK(!PeekMessageA(&m, NULL, 0, 0, PM_REMOVE));

	/* Messages still queued for a window go with it. */
	PostMessageA(h, WM_USER, 0, 0);
	CHECK(DestroyWindow(h));
	CHECK(!PeekMessageA(&m, NULL, 0, 0, PM_REMOVE));

	UINT r = RegisterWindowMessageA("vfw-test-message");
	CHECK(r >= 0xc000);
	CHECK_EQ(RegisterWindowMessageW(w("VFW-TEST-MESSAGE")), r);
	CHECK(RegisterWindowMessageA("other") != r);
}

static void test_strings(void)
{
	int (CDECL *wsprintfA)(LPSTR, LPCSTR, ...) = get_from(u32, "wsprintfA");
	int (CDECL *wsprintfW)(LPWSTR, LPCWSTR, ...) = get_from(u32, "wsprintfW");
	FN(LPSTR, CharUpperA, LPSTR);
	FN(LPSTR, CharLowerA, LPSTR);
	FN(LPWSTR, CharUpperW, LPWSTR);
	FN(LPSTR, CharNextA, LPCSTR);
	FN(LPSTR, CharPrevA, LPCSTR, LPCSTR);
	FN(BOOL, IsCharAlphaA, CHAR);
	FN(BOOL, IsCharUpperW, WCHAR);
	FN(BOOL, CharToOemA, LPCSTR, LPSTR);
	FN(BOOL, OemToCharA, LPCSTR, LPSTR);

	char buf[1100];
	CHECK_EQ(wsprintfA(buf, "[%-4d][%4u][%x][%08X][%c][%.2s]", 7, 8u, 255u,
			   0xbeefu, 'q', "abc"), 33);
	CHECK_STR(buf, "[7   ][   8][ff][0000BEEF][q][ab]");
	CHECK_EQ(wsprintfA(buf, "%s %S %hs %ls", "a", w("b"), "c", w("d")), 7);
	CHECK_STR(buf, "a b c d");
	wsprintfA(buf, "%s|%i|%p|%y", NULL, -5, (void *)0x1234);
	/* %p: as many digits as a pointer has. */
	CHECK_STR(buf, sizeof(void *) == 8 ? "(null)|-5|0000000000001234|y" :
					     "(null)|-5|00001234|y");
	wsprintfA(buf, "%05d|%-05d|%#x", -42, 3, 0u);
	CHECK_STR(buf, "-0042|3    |0");
	/* Output stops at 1024 characters, NUL included. */
	char big[1500];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	CHECK_EQ(wsprintfA(buf, "%s", big), 1023);
	CHECK_EQ(strlen(buf), 1023);

	WCHAR wbuf[64];
	CHECK_EQ(wsprintfW(wbuf, w("%s-%S-%c"), w("wide"), "ansi", 'Z'), 11);
	CHECK(wbuf[0] == 'w' && wbuf[5] == 'a' && wbuf[10] == 'Z' && !wbuf[11]);

	strcpy(buf, "Mixed \xe9\xff");
	CHECK(CharUpperA(buf) == buf);
	CHECK_STR(buf, "MIXED \xc9\x9f");	/* U+0178 is 0x9f in 1252 */
	CHECK_STR(CharLowerA(buf), "mixed \xe9\xff");
	CHECK_EQ((ULONG_PTR)CharUpperA((LPSTR)(ULONG_PTR)'a'), 'A');
	WCHAR ws[] = { 'a', 0xe0, 0 };
	CharUpperW(ws);
	CHECK(ws[0] == 'A' && ws[1] == 0xc0);
	const char *s = "ab";
	CHECK(CharNextA(s) == s + 1);
	CHECK(CharNextA(s + 2) == s + 2);
	CHECK(CharPrevA(s, s) == s);
	CHECK(IsCharAlphaA('\xe9') && !IsCharAlphaA('%'));
	CHECK(IsCharUpperW(0xc0));
	char oem[8];
	CHECK(CharToOemA("\xe9", oem));
	CHECK_EQ((BYTE)oem[0], 0x82);	/* e-acute in code page 437 */
	CHECK(OemToCharA(oem, oem));
	CHECK_EQ((BYTE)oem[0], 0xe9);
}

#define WM_RENDERFORMAT 0x0305
#define CF_TEXT 1
#define CF_BITMAP 2

/* The clipboard: one thread at a time, delayed rendering, GDI data. */
static int renders, render_alls, destroys;
static UINT render_format;
#define CLIP_RENDERED 0xc322	/* rendered by WM_RENDERALLFORMATS */
#define CLIP_LEFT 0xc323	/* left unrendered */

static LRESULT WINAPI render_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_RENDERFORMAT) {
		HANDLE (WINAPI *set)(UINT, HANDLE) =
			(HANDLE (WINAPI *)(UINT, HANDLE))get_from(u32, "SetClipboardData");
		HGLOBAL (WINAPI *alloc)(UINT, SIZE_T) =
			(HGLOBAL (WINAPI *)(UINT, SIZE_T))get_from(k32, "GlobalAlloc");
		renders++;
		render_format = (UINT)wp;
		set((UINT)wp, alloc(GMEM_MOVEABLE, 16));
		return 0;
	}
	if (msg == 0x0306) {	/* WM_RENDERALLFORMATS */
		BOOL (WINAPI *open)(HWND) =
			(BOOL (WINAPI *)(HWND))get_from(u32, "OpenClipboard");
		BOOL (WINAPI *close)(void) =
			(BOOL (WINAPI *)(void))get_from(u32, "CloseClipboard");
		HANDLE (WINAPI *set)(UINT, HANDLE) =
			(HANDLE (WINAPI *)(UINT, HANDLE))get_from(u32, "SetClipboardData");
		HGLOBAL (WINAPI *alloc)(UINT, SIZE_T) =
			(HGLOBAL (WINAPI *)(UINT, SIZE_T))get_from(k32, "GlobalAlloc");
		render_alls++;
		if (open(hwnd)) {
			set(CLIP_RENDERED, alloc(GMEM_MOVEABLE, 4));
			close();
		}
		return 0;
	}
	if (msg == 0x0307)	/* WM_DESTROYCLIPBOARD */
		destroys++;
	return def_a(hwnd, msg, wp, lp);
}

static volatile BOOL other_opened = -1;

static DWORD WINAPI open_elsewhere(void *arg)
{
	BOOL (WINAPI *open)(HWND) =
		(BOOL (WINAPI *)(HWND))get_from(u32, "OpenClipboard");
	BOOL (WINAPI *close)(void) =
		(BOOL (WINAPI *)(void))get_from(u32, "CloseClipboard");
	other_opened = open(NULL);
	if (other_opened)
		close();
	return 0;
}

static void test_clipboard(void)
{
	FN(ATOM, RegisterClassA, const WNDCLASSA *);
	FN(HWND, CreateWindowExA, DWORD, LPCSTR, LPCSTR, DWORD, int, int, int,
	   int, HWND, HMENU, HINSTANCE, LPVOID);
	FN(BOOL, DestroyWindow, HWND);
	FN(BOOL, OpenClipboard, HWND);
	FN(BOOL, CloseClipboard, void);
	FN(BOOL, EmptyClipboard, void);
	FN(HANDLE, SetClipboardData, UINT, HANDLE);
	FN(HANDLE, GetClipboardData, UINT);
	FN(BOOL, IsClipboardFormatAvailable, UINT);
	FN(HWND, GetClipboardOwner, void);
	GDI(HBITMAP, CreateBitmap, int, int, UINT, UINT, const void *);
	GDI(DWORD, GetObjectType, HGDIOBJ);
	K32(HGLOBAL, GlobalAlloc, UINT, SIZE_T);
	K32(SIZE_T, GlobalSize, HGLOBAL);
	K32(HGLOBAL, GlobalFree, HGLOBAL);
	K32(HANDLE, CreateThread, LPSECURITY_ATTRIBUTES, SIZE_T,
	    LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
	K32(DWORD, WaitForSingleObject, HANDLE, DWORD);
	K32(BOOL, CloseHandle, HANDLE);
	const UINT delayed = 0xc321;

	def_a = (WNDPROC)get_from(u32, "DefWindowProcA");
	WNDCLASSA wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = render_proc;
	wc.lpszClassName = "ClipOwner";
	CHECK(RegisterClassA(&wc));
	HWND owner = CreateWindowExA(0, "ClipOwner", "", 0, 0, 0, 1, 1, NULL,
				     NULL, NULL, NULL);
	CHECK(owner != NULL);

	CHECK(OpenClipboard(owner));
	CHECK(EmptyClipboard());
	CHECK(GetClipboardOwner() == owner);
	HBITMAP bm = CreateBitmap(1, 1, 1, 32, NULL);
	CHECK(bm && GetObjectType(bm) == OBJ_BITMAP);
	CHECK(SetClipboardData(CF_BITMAP, bm) == bm);
	CHECK(SetClipboardData(CF_TEXT, GlobalAlloc(GMEM_MOVEABLE, 8)) != NULL);
	/* Delayed rendering: NULL back, as there is no handle yet. */
	CHECK(SetClipboardData(delayed, NULL) == NULL);
	CHECK(IsClipboardFormatAvailable(delayed));

	/* Another thread can't open it while this one has it. */
	HANDLE t = CreateThread(NULL, 0, open_elsewhere, NULL, 0, NULL);
	CHECK(t && WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
	CloseHandle(t);
	CHECK_EQ(other_opened, FALSE);
	CHECK(CloseClipboard());
	CHECK(!CloseClipboard());
	t = CreateThread(NULL, 0, open_elsewhere, NULL, 0, NULL);
	CHECK(t && WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
	CloseHandle(t);
	CHECK_EQ(other_opened, TRUE);

	/* Asking for the delayed format has the owner render it, once. */
	CHECK(OpenClipboard(NULL));
	renders = 0;
	HANDLE got = GetClipboardData(delayed);
	CHECK(got != NULL && renders == 1 && render_format == delayed);
	CHECK(GetClipboardData(delayed) == got && renders == 1);
	/* A private format's handle stays the application's. */
	HGLOBAL mine = GlobalAlloc(GMEM_MOVEABLE, 8);
	CHECK(SetClipboardData(0x0200, mine) == mine);
	/* Emptying deletes the bitmap, as a GDI object, and tells the owner. */
	destroys = 0;
	CHECK(EmptyClipboard());
	CHECK_EQ(destroys, 1);
	CHECK_EQ(GetObjectType(bm), 0);
	CHECK(!IsClipboardFormatAvailable(delayed));
	CHECK(GlobalSize(mine) == 8);
	CHECK(GlobalFree(mine) == NULL);
	CHECK(CloseClipboard());

	/* An owner destroyed renders what it can; the rest goes. */
	CHECK(OpenClipboard(owner));
	CHECK(EmptyClipboard());
	CHECK(SetClipboardData(CLIP_RENDERED, NULL) == NULL);
	CHECK(SetClipboardData(CLIP_LEFT, NULL) == NULL);
	CHECK(CloseClipboard());
	render_alls = renders = 0;
	DestroyWindow(owner);
	CHECK_EQ(render_alls, 1);
	CHECK(GetClipboardOwner() == NULL);
	CHECK(IsClipboardFormatAvailable(CLIP_RENDERED));
	CHECK(!IsClipboardFormatAvailable(CLIP_LEFT));
	CHECK(OpenClipboard(NULL));
	CHECK(GetClipboardData(CLIP_RENDERED) != NULL && renders == 0);
	CHECK(EmptyClipboard());
	CHECK(CloseClipboard());
}

static void test_misc(void)
{
	FN(int, MessageBoxA, HWND, LPCSTR, LPCSTR, UINT);
	FN(BOOL, TrackPopupMenu, HMENU, UINT, int, int, int, HWND, const RECT *);
	FN(HWND, GetShellWindow, void);
	FN(int, GetSystemMetrics, int);
	FN(DWORD, GetSysColor, int);
	FN(HBRUSH, GetSysColorBrush, int);
	FN(BOOL, SetRect, LPRECT, int, int, int, int);
	FN(BOOL, IntersectRect, LPRECT, const RECT *, const RECT *);
	FN(BOOL, UnionRect, LPRECT, const RECT *, const RECT *);
	FN(BOOL, PtInRect, const RECT *, POINT);
	FN(BOOL, IsRectEmpty, const RECT *);
	FN(BOOL, InflateRect, LPRECT, int, int);
	FN(HDC, GetDC, HWND);
	FN(int, ReleaseDC, HWND, HDC);
	FN(SHORT, GetKeyState, int);
	FN(int, GetKeyboardType, int);
	FN(BOOL, EnumDisplaySettingsA, LPCSTR, DWORD, DEVMODEA *);
	FN(BOOL, EnumDisplaySettingsW, LPCWSTR, DWORD, DEVMODEW *);
	FN(BOOL, EnumDisplaySettingsExA, LPCSTR, DWORD, DEVMODEA *, DWORD);
	FN(int, GetKeyboardLayoutList, int, HANDLE *);
	FN(BOOL, GetCursorPos, LPPOINT);
	FN(BOOL, SetCursorPos, int, int);
	FN(HWINSTA, GetProcessWindowStation, void);
	FN(BOOL, GetUserObjectInformationA, HANDLE, int, PVOID, DWORD, LPDWORD);
	FN(HMONITOR, MonitorFromWindow, HWND, DWORD);
	FN(BOOL, GetMonitorInfoA, HMONITOR, LPMONITORINFO);
	FN(BOOL, SystemParametersInfoA, UINT, UINT, PVOID, UINT);
	FN(HICON, LoadIconA, HINSTANCE, LPCSTR);
	FN(int, LoadStringA, HINSTANCE, UINT, LPSTR, int);
	FN(BOOL, AdjustWindowRectEx, LPRECT, DWORD, BOOL, DWORD);
	FN(int, DrawTextA, HDC, LPCSTR, int, LPRECT, UINT);
	FN(BOOL, GetUpdateRect, HWND, LPRECT, BOOL);
	FN(BOOL, MapDialogRect, HWND, LPRECT);
	FN(int, SetScrollPos, HWND, int, int, BOOL);
	GDI(int, GetDeviceCaps, HDC, int);
	GDI(DWORD, GetObjectType, HGDIOBJ);
	GDI(int, GetObjectA, HGDIOBJ, int, LPVOID);

	/* Menus can be built and thrown away, though nothing shows them. */
	{
		FN(HMENU, CreatePopupMenu, void);
		FN(HMENU, CreateMenu, void);
		FN(BOOL, InsertMenuItemW, HMENU, UINT, BOOL, const void *);
		FN(BOOL, DestroyMenu, HMENU);
		BYTE info[48] = { 48 };
		HMENU a = CreatePopupMenu(), b = CreateMenu();

		CHECK(a != NULL && b != NULL && a != b);
		CHECK(InsertMenuItemW(a, 0, TRUE, info));
		CHECK(!InsertMenuItemW(NULL, 0, TRUE, info));
		CHECK(!InsertMenuItemW(a, 0, TRUE, NULL));
		CHECK(DestroyMenu(a) && DestroyMenu(b));
	}

	/* No menus are shown and there is no shell window. */
	CHECK(!TrackPopupMenu(NULL, 0, 0, 0, 0, NULL, NULL));
	CHECK(GetShellWindow() == NULL);

	/* The default button answers. */
	CHECK_EQ(MessageBoxA(NULL, "text", "caption", MB_OK), IDOK);
	CHECK_EQ(MessageBoxA(NULL, "text", NULL, MB_YESNO | MB_DEFBUTTON2), IDNO);
	CHECK_EQ(MessageBoxA(NULL, "text", NULL, MB_YESNOCANCEL | MB_DEFBUTTON3),
		 IDCANCEL);
	CHECK_EQ(MessageBoxA(NULL, "text", NULL, MB_RETRYCANCEL | MB_ICONHAND),
		 IDRETRY);

	CHECK_EQ(GetSystemMetrics(SM_CXSCREEN), 1024);
	CHECK_EQ(GetSystemMetrics(SM_CYSCREEN), 768);
	CHECK_EQ(GetSystemMetrics(SM_CMONITORS), 1);
	CHECK_EQ(GetSystemMetrics(SM_REMOTESESSION), 0);
	CHECK_EQ(GetSysColor(COLOR_WINDOW), RGB(255, 255, 255));
	HBRUSH br = GetSysColorBrush(COLOR_BTNFACE);
	CHECK_EQ(GetObjectType(br), OBJ_BRUSH);
	CHECK(GetSysColorBrush(COLOR_BTNFACE) == br);
	LOGBRUSH lb;
	GetObjectA(br, sizeof(lb), &lb);
	CHECK_EQ(lb.lbColor, GetSysColor(COLOR_BTNFACE));

	RECT a, b, r;
	SetRect(&a, 0, 0, 10, 10);
	SetRect(&b, 5, 5, 20, 20);
	CHECK(IntersectRect(&r, &a, &b));
	CHECK(r.left == 5 && r.right == 10);
	CHECK(UnionRect(&r, &a, &b));
	CHECK(r.left == 0 && r.bottom == 20);
	SetRect(&b, 10, 0, 20, 10);
	CHECK(!IntersectRect(&r, &a, &b));
	CHECK(IsRectEmpty(&r));
	POINT in = { 9, 9 }, out = { 10, 5 };
	CHECK(PtInRect(&a, in) && !PtInRect(&a, out));
	InflateRect(&a, 1, 2);
	CHECK(a.left == -1 && a.bottom == 12);

	HDC dc = GetDC(NULL);
	CHECK(dc != NULL);
	CHECK_EQ(GetDeviceCaps(dc, BITSPIXEL), 32);
	CHECK_EQ(ReleaseDC(NULL, dc), 1);
	CHECK_EQ(ReleaseDC(NULL, dc), 0);
	CHECK(GetDC((HWND)0x1234) == NULL);

	CHECK_EQ(GetKeyState(0x10), 0);
	/* What Delphi's start-up code asks: not a Japanese keyboard. */
	CHECK_EQ(GetKeyboardType(0), 4);
	CHECK_EQ(GetKeyboardType(1), 0);
	CHECK_EQ(GetKeyboardType(2), 12);
	HANDLE layouts[4] = { 0 };
	CHECK_EQ(GetKeyboardLayoutList(0, NULL), 1);
	CHECK_EQ(GetKeyboardLayoutList(4, layouts), 1);
	CHECK(layouts[0] == (HANDLE)0x04090409 && !layouts[1]);
	POINT pt;
	CHECK(GetCursorPos(&pt) && pt.x == 512);
	/* The cursor stays where it is put, inside the screen. */
	CHECK(SetCursorPos(10, 20) && GetCursorPos(&pt));
	CHECK(pt.x == 10 && pt.y == 20);
	CHECK(SetCursorPos(-5, 100000) && GetCursorPos(&pt));
	CHECK(pt.x == 0 && pt.y == 767);
	SetCursorPos(512, 384);

	/* One display mode, the screen's (frapsvid.dll asks). */
	DEVMODEA dm;
	memset(&dm, 0xcc, sizeof(dm));
	dm.dmSize = sizeof(dm);
	dm.dmDriverExtra = 0;
	CHECK(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm));
	CHECK_EQ(dm.dmSize, sizeof(DEVMODEA));
	CHECK_EQ(dm.dmPelsWidth, 1024);
	CHECK_EQ(dm.dmPelsHeight, 768);
	CHECK_EQ(dm.dmBitsPerPel, 32);
	CHECK_EQ(dm.dmDisplayFrequency, 60);
	CHECK(dm.dmFields & DM_PELSWIDTH);
	CHECK_EQ(dm.dmPosition.x, 0);
	CHECK(!strcmp((const char *)dm.dmDeviceName, "DISPLAY"));
	CHECK(EnumDisplaySettingsA("\\\\.\\display1", 0, &dm));
	CHECK(EnumDisplaySettingsExA(NULL, ENUM_REGISTRY_SETTINGS, &dm, 0));
	CHECK(!EnumDisplaySettingsA(NULL, 1, &dm));	/* no more modes */
	CHECK(!EnumDisplaySettingsA("\\\\.\\DISPLAY2", 0, &dm));
	DEVMODEW dw;
	memset(&dw, 0, sizeof(dw));
	dw.dmSize = sizeof(dw);
	CHECK(EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dw));
	CHECK(dw.dmPelsWidth == 1024 && dw.dmBitsPerPel == 32 &&
	      dw.dmDeviceName[0] == 'D' && dw.dmSize == sizeof(DEVMODEW));
	/* A smaller structure (an older DEVMODE) is written only so far. */
	BYTE small[sizeof(DEVMODEA) + 4];
	memset(small, 0xcc, sizeof(small));
	((DEVMODEA *)small)->dmSize = offsetof(DEVMODEA, dmICMMethod);
	CHECK(EnumDisplaySettingsA(NULL, 0, (DEVMODEA *)small));
	CHECK(((DEVMODEA *)small)->dmPelsHeight == 768 &&
	      small[offsetof(DEVMODEA, dmICMMethod)] == 0xcc);

	/* What the MSVC CRT asks before it shows a message box. */
	USEROBJECTFLAGS f;
	DWORD n;
	CHECK(GetUserObjectInformationA(GetProcessWindowStation(), UOI_FLAGS, &f,
					sizeof(f), &n));
	CHECK(f.dwFlags & WSF_VISIBLE);
	char name[16];
	CHECK(!GetUserObjectInformationA(GetProcessWindowStation(), UOI_NAME, name,
					 2, &n));
	CHECK_EQ(n, 8);

	MONITORINFO mi;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	CHECK(GetMonitorInfoA(MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY), &mi));
	CHECK(mi.rcMonitor.right == 1024 && (mi.dwFlags & MONITORINFOF_PRIMARY));
	CHECK(SystemParametersInfoA(SPI_GETWORKAREA, 0, &r, 0));
	CHECK_EQ(r.bottom, 768);

	CHECK(LoadIconA(NULL, (LPCSTR)32512) != NULL);	/* IDI_APPLICATION */

	RECT legacy = { 0, 0, 40, 20 };
	CHECK(AdjustWindowRectEx(&legacy, 0x00c00000, TRUE, 0));
	CHECK(legacy.left == -4 && legacy.top == -23 &&
	      legacy.right == 44 && legacy.bottom == 24);
	legacy = (RECT){ 0 };
	/* No DC: measured in the system font (16 high, 16 * 8 / 13 wide). */
	CHECK_EQ(DrawTextA(NULL, "one\ntwo", -1, &legacy, 0x400), 32);
	CHECK(legacy.right == 30 && legacy.bottom == 32);
	legacy = (RECT){ 1, 2, 3, 4 };
	CHECK(!GetUpdateRect(NULL, &legacy, FALSE));
	CHECK(legacy.left == 0 && legacy.bottom == 0);
	legacy = (RECT){ 1, 2, 3, 4 };
	CHECK(MapDialogRect(NULL, &legacy));
	CHECK(legacy.left == 2 && legacy.top == 4 &&
	      legacy.right == 6 && legacy.bottom == 8);
	CHECK_EQ(SetScrollPos(NULL, 0, 37, FALSE), 0);	/* no window */

	/* pe_basic's own strings, through LoadString. */
	const char *d = getenv("VFW_FIXTURES");
	if (d) {
		struct module *m = mod_load(fixture("pe_basic.dll"), NULL);
		CHECK(m != NULL);
		if (m) {
			char s[64];
			CHECK_EQ(LoadStringA((HINSTANCE)m->base, 2, s, sizeof(s)), 19);
			CHECK_STR(s, "pe_basic string two");
			CHECK_EQ(LoadStringA((HINSTANCE)m->base, 3, s, sizeof(s)), 0);
			CHECK(LoadIconA((HINSTANCE)m->base, "nosuch") == NULL);
			mod_free(m);
		}
	}
}

static void test_fixture(void)
{
	const char *d = getenv("VFW_FIXTURES");
	char path[512];

	snprintf(path, sizeof(path), "%s/pe_gui.dll", d ? d : ".");
	if (!d || access(path, R_OK)) {
		printf("test_user32: pe_gui.dll not built, skipping the fixture\n");
		return;
	}
	struct module *m = mod_load(path, NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	static const char *const tests[] = { "gui_strings", "gui_window", "gui_bitmap" };
	for (size_t i = 0; i < 3; i++) {
		int (CDECL *fn)(void) = mod_get_proc(m, tests[i]);
		CHECK(fn != NULL);
		if (!fn)
			continue;
		int r = fn();
		if (r)
			fprintf(stderr, "pe_gui %s: check %d failed\n", tests[i], r);
		CHECK_EQ(r, 0);
	}
	mod_free(m);
}

/* Messages sent to a window of another thread run on that thread. */
static DWORD ran_on;
static BOOL in_send_seen;
static HWND owned;
static HANDLE owned_ready, owned_go;

static LRESULT WINAPI owned_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	K32(DWORD, GetCurrentThreadId, void);
	FN(BOOL, InSendMessage, void);
	FN(BOOL, ReplyMessage, LRESULT);
	FN(void, PostQuitMessage, int);
	FN(LRESULT, DefWindowProcA, HWND, UINT, WPARAM, LPARAM);

	switch (msg) {
	case WM_USER + 1:
		ran_on = GetCurrentThreadId();
		in_send_seen = InSendMessage();
		return 42;
	case WM_USER + 2:
		ReplyMessage(7);	/* the sender has its answer from here */
		return 9;
	case WM_USER + 3: {
		K32(void, ExitThread, DWORD);
		ExitThread(11);		/* its sender gets 0 */
		return 0;
	}
	case WM_USER + 4: {
		/* Sends back to wp, a window of the thread sending this. */
		FN(LRESULT, SendMessageTimeoutA, HWND, UINT, WPARAM, LPARAM,
		   UINT, UINT, PDWORD_PTR);
		DWORD_PTR r;
		return SendMessageTimeoutA((HWND)wp, WM_USER + 1, 0, 0, 0, 200,
					   &r) ? 1 : 2;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND make_owned(void)
{
	FN(HWND, CreateWindowExA, DWORD, LPCSTR, LPCSTR, DWORD, int, int, int,
	   int, HWND, HMENU, HINSTANCE, LPVOID);
	return CreateWindowExA(0, "vfw-owned", "", 0, 0, 0, 1, 1, NULL, NULL,
			       NULL, NULL);
}

/* Makes a window and runs its message loop until WM_QUIT, as VTZ32 does. */
static DWORD WINAPI pump(LPVOID arg)
{
	FN(BOOL, GetMessageA, LPMSG, HWND, UINT, UINT);
	FN(LRESULT, DispatchMessageA, const MSG *);
	K32(BOOL, SetEvent, HANDLE);
	MSG m;

	owned = make_owned();
	SetEvent(owned_ready);
	while (GetMessageA(&m, NULL, 0, 0) > 0)
		DispatchMessageA(&m);
	return 5;
}

/* Makes a window, then waits without looking at its messages. */
static DWORD WINAPI busy(LPVOID arg)
{
	FN(BOOL, PeekMessageA, LPMSG, HWND, UINT, UINT, UINT);
	K32(BOOL, SetEvent, HANDLE);
	K32(DWORD, WaitForSingleObject, HANDLE, DWORD);
	MSG m;

	*(HWND *)arg = make_owned();
	SetEvent(owned_ready);
	WaitForSingleObject(owned_go, INFINITE);
	PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE);	/* runs what was given up on */
	return 0;
}

static void test_sent_messages(void)
{
	FN(ATOM, RegisterClassA, const WNDCLASSA *);
	FN(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	FN(LRESULT, SendMessageTimeoutA, HWND, UINT, WPARAM, LPARAM, UINT, UINT,
	   PDWORD_PTR);
	FN(BOOL, SendNotifyMessageA, HWND, UINT, WPARAM, LPARAM);
	FN(BOOL, InSendMessage, void);
	FN(BOOL, ReplyMessage, LRESULT);
	FN(BOOL, IsWindow, HWND);
	FN(BOOL, DestroyWindow, HWND);
	FN(BOOL, PeekMessageA, LPMSG, HWND, UINT, UINT, UINT);
	K32(DWORD, GetCurrentThreadId, void);
	K32(HANDLE, CreateThread, LPSECURITY_ATTRIBUTES, SIZE_T,
	    LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
	K32(HANDLE, CreateEventA, LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
	K32(BOOL, SetEvent, HANDLE);
	K32(DWORD, WaitForSingleObject, HANDLE, DWORD);
	K32(BOOL, GetExitCodeThread, HANDLE, LPDWORD);
	K32(BOOL, CloseHandle, HANDLE);
	K32(DWORD, GetLastError, void);

	WNDCLASSA wc = { .lpfnWndProc = owned_proc, .lpszClassName = "vfw-owned" };
	CHECK(RegisterClassA(&wc) != 0);
	CHECK(!InSendMessage());
	CHECK(!ReplyMessage(1));
	owned_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
	owned_go = CreateEventA(NULL, TRUE, FALSE, NULL);

	DWORD tid = 0;
	HANDLE t = CreateThread(NULL, 0, pump, NULL, 0, &tid);
	CHECK(t != NULL);
	CHECK_EQ(WaitForSingleObject(owned_ready, 5000), WAIT_OBJECT_0);
	CHECK(owned != NULL);
	CHECK_EQ(SendMessageA(owned, WM_USER + 1, 0, 0), 42);
	CHECK_EQ(ran_on, tid);
	CHECK(in_send_seen);
	CHECK_EQ(SendMessageA(owned, WM_USER + 2, 0, 0), 7);
	DWORD_PTR res = 0;
	CHECK(SendMessageTimeoutA(owned, WM_USER + 1, 0, 0, 0, 5000, &res));
	CHECK_EQ(res, 42);
	ran_on = 0;
	CHECK(SendNotifyMessageA(owned, WM_USER + 1, 0, 0));
	/* WM_CLOSE: its DestroyWindow, and the WM_QUIT WM_DESTROY posts, are
	 * the owner's, so its loop ends. */
	SendMessageA(owned, WM_CLOSE, 0, 0);
	CHECK_EQ(WaitForSingleObject(t, 5000), WAIT_OBJECT_0);
	DWORD code = 0;
	CHECK(GetExitCodeThread(t, &code) && code == 5);
	CHECK_EQ(ran_on, tid);	/* the notification ran there too */
	CHECK(!IsWindow(owned));
	CloseHandle(t);

	/* An owner that isn't looking: the timeout, then the message runs
	 * when it does look. */
	HWND h2 = NULL;
	t = CreateThread(NULL, 0, busy, &h2, 0, &tid);
	CHECK_EQ(WaitForSingleObject(owned_ready, 5000), WAIT_OBJECT_0);
	res = 1;
	/* A timeout of 0 is a wait that ends at once, not a notification. */
	CHECK(!SendMessageTimeoutA(h2, WM_USER + 1, 0, 0, 0, 0, &res));
	CHECK_EQ(GetLastError(), 1460);	/* ERROR_TIMEOUT */
	CHECK(!SendMessageTimeoutA(h2, WM_USER + 1, 0, 0, 0, 30, &res));
	CHECK_EQ(GetLastError(), 1460);	/* ERROR_TIMEOUT */
	ran_on = 0;
	SetEvent(owned_go);
	CHECK_EQ(WaitForSingleObject(t, 5000), WAIT_OBJECT_0);
	CHECK_EQ(ran_on, tid);
	CloseHandle(t);
	/* Its thread has ended: the message is the caller's to run. */
	CHECK_EQ(SendMessageA(h2, WM_USER + 1, 0, 0), 42);
	CHECK_EQ(ran_on, GetCurrentThreadId());
	DestroyWindow(h2);
	/* Its WM_DESTROY, run here, posted this thread a WM_QUIT. */
	MSG m;
	CHECK(PeekMessageA(&m, NULL, 0, 0, PM_REMOVE) && m.message == WM_QUIT);
	CHECK(!PeekMessageA(&m, NULL, 0, 0, PM_REMOVE));
	CloseHandle(owned_ready);
	CloseHandle(owned_go);
}

/* Blocks in GetMessage on a queue with nothing coming. */
static DWORD WINAPI idle_loop(LPVOID arg)
{
	FN(BOOL, GetMessageA, LPMSG, HWND, UINT, UINT);
	FN(BOOL, PeekMessageA, LPMSG, HWND, UINT, UINT, UINT);
	K32(BOOL, SetEvent, HANDLE);
	MSG m;

	PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE);	/* makes its queue */
	SetEvent(arg);
	GetMessageA(&m, NULL, 0, 0);
	return 1;
}

/* Busy in host code until told, then into GetMessage: a request made
 * while it was busy, with no wake to follow, is seen before it sleeps. */
static volatile int late_go;
static DWORD WINAPI late_loop(LPVOID arg)
{
	FN(BOOL, GetMessageA, LPMSG, HWND, UINT, UINT);
	FN(BOOL, PeekMessageA, LPMSG, HWND, UINT, UINT, UINT);
	K32(BOOL, SetEvent, HANDLE);
	MSG m;

	PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE);
	SetEvent(arg);
	while (!__atomic_load_n(&late_go, __ATOMIC_ACQUIRE))
		sched_yield();
	GetMessageA(&m, NULL, 0, 0);
	return 1;
}

/* Sends to a window whose owner never looks, and waits. */
static HWND never_looks;
static DWORD WINAPI blocked_sender(LPVOID arg)
{
	FN(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	K32(BOOL, SetEvent, HANDLE);

	SetEvent(arg);
	SendMessageA(never_looks, WM_USER + 1, 0, 0);
	return 1;
}

/* Threads ended in user32's waits, or while handling a sent message. */
static void test_send_endings(void)
{
	FN(LRESULT, SendMessageTimeoutA, HWND, UINT, WPARAM, LPARAM, UINT, UINT,
	   PDWORD_PTR);
	FN(BOOL, SendNotifyMessageA, HWND, UINT, WPARAM, LPARAM);
	FN(BOOL, DestroyWindow, HWND);
	FN(BOOL, PeekMessageA, LPMSG, HWND, UINT, UINT, UINT);
	K32(HANDLE, CreateThread, LPSECURITY_ATTRIBUTES, SIZE_T,
	    LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
	K32(HANDLE, CreateEventA, LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
	K32(BOOL, SetEvent, HANDLE);
	K32(DWORD, WaitForSingleObject, HANDLE, DWORD);
	K32(BOOL, GetExitCodeThread, HANDLE, LPDWORD);
	K32(BOOL, TerminateThread, HANDLE, DWORD);
	K32(BOOL, CloseHandle, HANDLE);
	K32(DWORD, GetLastError, void);
	DWORD code = 0;
	DWORD_PTR res = 0;
	MSG m;

	HANDLE ready = CreateEventA(NULL, FALSE, FALSE, NULL);
	owned_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
	owned_go = CreateEventA(NULL, TRUE, FALSE, NULL);

	/* Asleep in GetMessage, it is woken to end. */
	HANDLE t = CreateThread(NULL, 0, idle_loop, ready, 0, NULL);
	CHECK_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
	CHECK(TerminateThread(t, 3));
	CHECK_EQ(WaitForSingleObject(t, 5000), WAIT_OBJECT_0);
	CHECK(GetExitCodeThread(t, &code) && code == 3);
	CloseHandle(t);

	/* Asked while busy; TerminateThread gives up waiting, then the
	 * thread goes to sleep in GetMessage and must end there. */
	t = CreateThread(NULL, 0, late_loop, ready, 0, NULL);
	CHECK_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
	CHECK(TerminateThread(t, 6));
	__atomic_store_n(&late_go, 1, __ATOMIC_RELEASE);
	CHECK_EQ(WaitForSingleObject(t, 5000), WAIT_OBJECT_0);
	CHECK(GetExitCodeThread(t, &code) && code == 6);
	CloseHandle(t);

	/* Waiting on a reply, likewise; the message still runs later. */
	t = CreateThread(NULL, 0, busy, &never_looks, 0, NULL);
	CHECK_EQ(WaitForSingleObject(owned_ready, 5000), WAIT_OBJECT_0);
	HANDLE s = CreateThread(NULL, 0, blocked_sender, ready, 0, NULL);
	CHECK_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
	CHECK(TerminateThread(s, 4));
	CHECK_EQ(WaitForSingleObject(s, 5000), WAIT_OBJECT_0);
	CHECK(GetExitCodeThread(s, &code) && code == 4);
	CloseHandle(s);
	ran_on = 0;
	SetEvent(owned_go);
	CHECK_EQ(WaitForSingleObject(t, 5000), WAIT_OBJECT_0);
	CHECK(ran_on != 0);
	CloseHandle(t);
	DestroyWindow(never_looks);
	while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE))
		;	/* its WM_DESTROY's WM_QUIT */

	/* A procedure handling a sent message ends its thread: the sender
	 * has 0, not a wait that never ends. */
	t = CreateThread(NULL, 0, pump, NULL, 0, NULL);
	CHECK_EQ(WaitForSingleObject(owned_ready, 5000), WAIT_OBJECT_0);
	res = 1;
	CHECK(SendMessageTimeoutA(owned, WM_USER + 3, 0, 0, 0, 5000, &res));
	CHECK_EQ(res, 0);
	CHECK_EQ(WaitForSingleObject(t, 5000), WAIT_OBJECT_0);
	CHECK(GetExitCodeThread(t, &code) && code == 11);
	CloseHandle(t);
	DestroyWindow(owned);
	while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE))
		;

	/* SMTO_BLOCK: what is sent to the waiting sender waits too. */
	t = CreateThread(NULL, 0, pump, NULL, 0, NULL);
	CHECK_EQ(WaitForSingleObject(owned_ready, 5000), WAIT_OBJECT_0);
	HWND mine = make_owned();
	CHECK(SendMessageTimeoutA(owned, WM_USER + 4, (WPARAM)mine, 0,
				  SMTO_NORMAL, 5000, &res));
	CHECK_EQ(res, 1);
	CHECK(SendMessageTimeoutA(owned, WM_USER + 4, (WPARAM)mine, 0,
				  SMTO_BLOCK, 5000, &res));
	CHECK_EQ(res, 2);
	PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE);	/* runs what timed out */

	/* Pointers can't be sent without waiting. */
	CHECK(!SendNotifyMessageA(owned, WM_SETTEXT, 0, (LPARAM)"x"));
	CHECK_EQ(GetLastError(), 1159);	/* ERROR_MESSAGE_SYNC_ONLY */

	SendMessageTimeoutA(owned, WM_CLOSE, 0, 0, 0, 5000, &res);
	CHECK_EQ(WaitForSingleObject(t, 5000), WAIT_OBJECT_0);
	CloseHandle(t);
	DestroyWindow(mine);
	while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE))
		;
	CloseHandle(ready);
	CloseHandle(owned_ready);
	CloseHandle(owned_go);
}

/* Dialogs: a template built in memory, and what its procedure saw. */
struct tbuf {
	BYTE b[2048];
	size_t n;
};

static void t_put(struct tbuf *t, const void *p, size_t n)
{
	memcpy(t->b + t->n, p, n);
	t->n += n;
}

static void t_word(struct tbuf *t, WORD v)
{
	t_put(t, &v, 2);
}

static void t_dword(struct tbuf *t, DWORD v)
{
	t_put(t, &v, 4);
}

static void t_str(struct tbuf *t, const char *s)
{
	do
		t_word(t, (BYTE)*s);
	while (*s++);
}

static void t_align(struct tbuf *t)
{
	while (t->n & 3)
		t->b[t->n++] = 0;
}

static void t_item(struct tbuf *t, int ex, DWORD style, short x, short y,
		   short cx, short cy, WORD id, WORD cls, const char *title)
{
	t_align(t);
	if (ex) {
		t_dword(t, 0);		/* help id */
		t_dword(t, 0);		/* exstyle */
		t_dword(t, style);
	} else {
		t_dword(t, style);
		t_dword(t, 0);
	}
	t_word(t, (WORD)x);
	t_word(t, (WORD)y);
	t_word(t, (WORD)cx);
	t_word(t, (WORD)cy);
	if (ex)
		t_dword(t, id);
	else
		t_word(t, id);
	t_word(t, 0xffff);
	t_word(t, cls);
	t_str(t, title);
	t_word(t, 0);			/* no creation data */
}

/* The test dialog: an edit, a check box, two radios, a list, a combo, OK
 * and Cancel. */
static void t_dialog(struct tbuf *t, int ex, DWORD style)
{
	t->n = 0;
	style |= DS_SETFONT;
	if (ex) {
		t_word(t, 1);
		t_word(t, 0xffff);
		t_dword(t, 0);
		t_dword(t, 0);
		t_dword(t, style);
	} else {
		t_dword(t, style);
		t_dword(t, 0);
	}
	t_word(t, 8);
	t_word(t, 10);
	t_word(t, 20);
	t_word(t, 200);
	t_word(t, 100);
	t_word(t, 0);			/* no menu */
	t_word(t, 0);			/* the dialog class */
	t_str(t, "Settings");
	t_word(t, 8);
	if (ex) {
		t_word(t, 400);
		t_word(t, 0);		/* italic, charset */
	}
	t_str(t, "MS Shell Dlg");
	const DWORD vis = WS_CHILD | WS_VISIBLE;
	t_item(t, ex, vis | WS_TABSTOP | ES_AUTOHSCROLL, 5, 5, 100, 12, 100,
	       0x81, "abc");
	t_item(t, ex, vis | WS_TABSTOP | BS_AUTOCHECKBOX, 5, 20, 100, 10, 101,
	       0x80, "Check");
	t_item(t, ex, vis | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 5, 32,
	       50, 10, 102, 0x80, "One");
	t_item(t, ex, vis | BS_AUTORADIOBUTTON, 60, 32, 50, 10, 103, 0x80,
	       "Two");
	t_item(t, ex, vis | WS_TABSTOP | WS_GROUP | LBS_SORT | LBS_NOTIFY, 5,
	       45, 60, 40, 104, 0x83, "");
	t_item(t, ex, vis | WS_TABSTOP | CBS_DROPDOWNLIST, 70, 45, 60, 40, 105,
	       0x85, "");
	t_item(t, ex, vis | WS_TABSTOP | BS_DEFPUSHBUTTON, 90, 80, 50, 14, IDOK,
	       0x80, "OK");
	t_item(t, ex, vis | WS_TABSTOP, 145, 80, 50, 14, IDCANCEL, 0x80,
	       "Cancel");
	/* The item count. */
	WORD n = 8;
	memcpy(t->b + (ex ? 16 : 8), &n, 2);
}

static char dlog[512];
static LPARAM dlg_param;
static int dlg_end_in_init, dlg_ignore_cancel;
static HWND dlg_owner;
static BOOL owner_enabled_in_init;
static BOOL (WINAPI *p_EndDialog)(HWND, INT_PTR);
static BOOL (WINAPI *p_IsWindowEnabled)(HWND);

static void dnote(const char *fmt, int a, int b)
{
	size_t n = strlen(dlog);
	snprintf(dlog + n, sizeof(dlog) - n, n ? ",%s" : "%s", fmt);
	n = strlen(dlog);
	char tmp[64];
	snprintf(tmp, sizeof(tmp), dlog + n - strlen(fmt), a, b);
	dlog[n - strlen(fmt)] = 0;
	n = strlen(dlog);
	snprintf(dlog + n, sizeof(dlog) - n, "%s", tmp);
}

static INT_PTR WINAPI dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_INITDIALOG:
		dlg_param = lp;
		dnote("init", 0, 0);
		if (dlg_owner)
			owner_enabled_in_init = p_IsWindowEnabled(dlg_owner);
		if (dlg_end_in_init)
			p_EndDialog(h, 5);
		return TRUE;
	case WM_COMMAND:
		dnote("cmd%d/%d", LOWORD(wp), HIWORD(wp));
		if (LOWORD(wp) == IDOK)
			p_EndDialog(h, 42);
		else if (LOWORD(wp) == IDCANCEL && !dlg_ignore_cancel)
			p_EndDialog(h, 7);
		return TRUE;
	}
	return FALSE;
}

static void test_dialogs(void)
{
	FN(HWND, CreateDialogIndirectParamW, HINSTANCE, const void *, HWND,
	   DLGPROC, LPARAM);
	FN(HWND, CreateDialogIndirectParamA, HINSTANCE, const void *, HWND,
	   DLGPROC, LPARAM);
	FN(INT_PTR, DialogBoxIndirectParamW, HINSTANCE, const void *, HWND,
	   DLGPROC, LPARAM);
	FN(BOOL, EndDialog, HWND, INT_PTR);
	FN(HWND, GetDlgItem, HWND, int);
	FN(BOOL, IsWindowVisible, HWND);
	FN(BOOL, IsWindowEnabled, HWND);
	FN(BOOL, IsWindowUnicode, HWND);
	FN(BOOL, DestroyWindow, HWND);
	FN(HWND, GetFocus, void);
	FN(HWND, SetFocus, HWND);
	FN(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	FN(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	FN(BOOL, SetDlgItemTextA, HWND, int, LPCSTR);
	FN(UINT, GetDlgItemTextA, HWND, int, LPSTR, int);
	FN(int, GetClassNameA, HWND, LPSTR, int);
	FN(int, GetWindowTextA, HWND, LPSTR, int);
	FN(UINT, IsDlgButtonChecked, HWND, int);
	FN(BOOL, CheckRadioButton, HWND, int, int, int);
	FN(BOOL, IsDialogMessageW, HWND, LPMSG);
	FN(HWND, GetNextDlgTabItem, HWND, HWND, BOOL);
	FN(HWND, GetNextDlgGroupItem, HWND, HWND, BOOL);
	FN(BOOL, PeekMessageW, LPMSG, HWND, UINT, UINT, UINT);
	FN(LRESULT, DispatchMessageW, const MSG *);
	FN(LONG, GetDialogBaseUnits, void);
	FN(BOOL, GetClientRect, HWND, LPRECT);
	FN(HWND, CreateWindowExA, DWORD, LPCSTR, LPCSTR, DWORD, int, int, int,
	   int, HWND, HMENU, HINSTANCE, LPVOID);
	p_EndDialog = EndDialog;
	p_IsWindowEnabled = IsWindowEnabled;
	struct tbuf t;
	char buf[64];

	CHECK_EQ(GetDialogBaseUnits(), MAKELONG(8, 16));
	for (int ex = 0; ex < 2; ex++) {
		t_dialog(&t, ex, WS_POPUP | WS_CAPTION | WS_VISIBLE);
		dlog[0] = 0;
		HWND h = CreateDialogIndirectParamW(NULL, t.b, NULL, dlg_proc,
						    1234);
		CHECK(h != NULL);
		if (!h)
			continue;
		/* The edit gets the focus: EN_SETFOCUS. */
		CHECK(!strcmp(dlog, "init,cmd100/256") && dlg_param == 1234);
		CHECK(IsWindowVisible(h) && IsWindowUnicode(h));
		CHECK(GetClassNameA(h, buf, sizeof(buf)) &&
		      !strcmp(buf, "#32770"));
		CHECK(GetWindowTextA(h, buf, sizeof(buf)) &&
		      !strcmp(buf, "Settings"));
		RECT rc;
		CHECK(GetClientRect(h, &rc) && rc.right == 400 &&
		      rc.bottom == 200);
		HWND edit = GetDlgItem(h, 100), check = GetDlgItem(h, 101);
		HWND list = GetDlgItem(h, 104), combo = GetDlgItem(h, 105);
		CHECK(edit && check && list && combo && GetDlgItem(h, IDOK));
		CHECK(GetClassNameA(edit, buf, sizeof(buf)) &&
		      !strcmp(buf, "EDIT"));
		CHECK(GetDlgItemTextA(h, 100, buf, sizeof(buf)) &&
		      !strcmp(buf, "abc"));
		/* The first tab stop has the focus, its text selected. */
		CHECK(GetFocus() == edit);
		DWORD a = 9, b = 9;
		SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
		CHECK(a == 0 && b == 3);
		CHECK_EQ(SendMessageW(h, DM_GETDEFID, 0, 0),
			 MAKELONG(IDOK, DC_HASDEFID));

		/* The edit: typed over its selection, with EN_CHANGE. */
		dlog[0] = 0;
		SendMessageW(edit, WM_CHAR, 'x', 0);
		SendMessageW(edit, WM_CHAR, 'y', 0);
		SendMessageW(edit, WM_CHAR, 8, 0);
		CHECK(GetDlgItemTextA(h, 100, buf, sizeof(buf)) &&
		      !strcmp(buf, "x"));
		CHECK(strstr(dlog, "cmd100/768") != NULL);	/* EN_CHANGE */
		CHECK_EQ(SendMessageW(edit, EM_GETMODIFY, 0, 0), TRUE);
		SendMessageW(edit, EM_LIMITTEXT, 3, 0);
		SendMessageA(edit, EM_REPLACESEL, 0, (LPARAM)"12345");
		CHECK(GetDlgItemTextA(h, 100, buf, sizeof(buf)) &&
		      !strcmp(buf, "x12"));
		CHECK(SetDlgItemTextA(h, 100, "hello"));
		CHECK_EQ(SendMessageW(edit, EM_GETMODIFY, 0, 0), FALSE);

		/* Buttons: clicks toggle and notify; radio groups. */
		dlog[0] = 0;
		CHECK_EQ(IsDlgButtonChecked(h, 101), BST_UNCHECKED);
		SendMessageW(check, BM_CLICK, 0, 0);
		CHECK_EQ(IsDlgButtonChecked(h, 101), BST_CHECKED);
		CHECK(!strcmp(dlog, "cmd101/0"));
		CHECK(CheckRadioButton(h, 102, 103, 102));
		SendMessageW(GetDlgItem(h, 103), BM_CLICK, 0, 0);
		CHECK(IsDlgButtonChecked(h, 103) == BST_CHECKED &&
		      IsDlgButtonChecked(h, 102) == BST_UNCHECKED);
		CHECK(GetNextDlgGroupItem(h, GetDlgItem(h, 102), FALSE) ==
		      GetDlgItem(h, 103));
		CHECK(GetNextDlgGroupItem(h, GetDlgItem(h, 103), FALSE) ==
		      GetDlgItem(h, 102));

		/* A sorted list box, from ANSI and Unicode senders. */
		SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)"pear");
		SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)"apple");
		SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)w("fig"));
		CHECK_EQ(SendMessageW(list, LB_GETCOUNT, 0, 0), 3);
		CHECK_EQ(SendMessageA(list, LB_GETTEXTLEN, 1, 0), 3);
		CHECK(SendMessageA(list, LB_GETTEXT, 2, (LPARAM)buf) == 4 &&
		      !strcmp(buf, "pear"));
		CHECK_EQ(SendMessageA(list, LB_FINDSTRING, (WPARAM)-1,
				      (LPARAM)"F"), 1);
		CHECK_EQ(SendMessageA(list, LB_FINDSTRINGEXACT, (WPARAM)-1,
				      (LPARAM)"pea"), LB_ERR);
		CHECK_EQ(SendMessageW(list, LB_SETCURSEL, 0, 0), 0);
		SendMessageW(list, LB_SETITEMDATA, 0, 77);
		CHECK_EQ(SendMessageW(list, LB_GETITEMDATA, 0, 0), 77);
		dlog[0] = 0;
		SendMessageW(list, WM_KEYDOWN, VK_DOWN, 0);
		CHECK_EQ(SendMessageW(list, LB_GETCURSEL, 0, 0), 1);
		CHECK(!strcmp(dlog, "cmd104/1"));	/* LBN_SELCHANGE */
		CHECK_EQ(SendMessageW(list, LB_DELETESTRING, 0, 0), 2);
		CHECK_EQ(SendMessageW(list, LB_GETCURSEL, 0, 0), 0);

		/* A drop-down list: its text is the selection's. */
		SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"low");
		SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"high");
		CHECK_EQ(SendMessageW(combo, CB_SETCURSEL, 1, 0), 1);
		CHECK(GetWindowTextA(combo, buf, sizeof(buf)) &&
		      !strcmp(buf, "high"));
		CHECK(SendMessageA(combo, CB_GETLBTEXT, 0, (LPARAM)buf) == 3 &&
		      !strcmp(buf, "low"));
		CHECK_EQ(SendMessageA(combo, WM_SETTEXT, 0, (LPARAM)"none"),
			 CB_ERR);
		CHECK_EQ(SendMessageA(combo, WM_SETTEXT, 0, (LPARAM)"LOW"), TRUE);
		CHECK_EQ(SendMessageW(combo, CB_GETCURSEL, 0, 0), 0);
		dlog[0] = 0;
		SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
		CHECK(SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) &&
		      !strcmp(dlog, "cmd105/7"));

		/* The keyboard: Tab moves on, Escape and Enter press Cancel
		 * and the default button. */
		SetFocus(edit);
		MSG m = { edit, WM_KEYDOWN, VK_TAB, 0, 0, { 0, 0 } };
		CHECK(IsDialogMessageW(h, &m));
		CHECK(GetFocus() == check);
		CHECK(GetNextDlgTabItem(h, check, TRUE) == edit);
		CHECK(GetNextDlgTabItem(h, GetDlgItem(h, IDCANCEL), FALSE) ==
		      edit);
		MSG out = { h, WM_KEYDOWN, VK_TAB, 0, 0, { 0, 0 } };
		CHECK(!IsDialogMessageW(NULL, &out));
		dlog[0] = 0;
		dlg_ignore_cancel = 1;
		m.message = WM_KEYDOWN;
		m.wParam = VK_ESCAPE;
		CHECK(IsDialogMessageW(h, &m));
		CHECK(!strcmp(dlog, "cmd2/0"));
		/* WM_CLOSE posts IDCANCEL. */
		dlog[0] = 0;
		SendMessageW(h, WM_CLOSE, 0, 0);
		CHECK(IsWindowVisible(h) && !dlog[0]);
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&m);
		CHECK(!strcmp(dlog, "cmd2/0"));
		dlg_ignore_cancel = 0;
		dlog[0] = 0;
		m.hwnd = edit;
		m.message = WM_KEYDOWN;
		m.wParam = VK_RETURN;
		CHECK(IsDialogMessageW(h, &m));
		CHECK(!strcmp(dlog, "cmd1/0"));
		/* EndDialog only hides a modeless one. */
		CHECK(!IsWindowVisible(h));
		CHECK(DestroyWindow(h));
		CHECK(!EndDialog(h, 0));
	}

	/* An ANSI dialog gets ANSI messages. */
	t_dialog(&t, 0, WS_POPUP);
	HWND ha = CreateDialogIndirectParamA(NULL, t.b, NULL, dlg_proc, 0);
	CHECK(ha && !IsWindowUnicode(ha) && !IsWindowVisible(ha));
	DestroyWindow(ha);

	/* Modal: the owner is disabled while it runs, and nobody presses a
	 * button, so Cancel is. */
	dlg_owner = CreateWindowExA(0, "STATIC", "owner", 0, 0, 0, 10, 10,
				    NULL, NULL, NULL, NULL);
	dlog[0] = 0;
	t_dialog(&t, 1, WS_POPUP | WS_VISIBLE);
	CHECK_EQ(DialogBoxIndirectParamW(NULL, t.b, dlg_owner, dlg_proc, 9),
		 7);
	CHECK(!strcmp(dlog, "init,cmd100/256,cmd2/0"));
	CHECK(!owner_enabled_in_init && IsWindowEnabled(dlg_owner));
	/* One that ends itself as it starts. */
	dlg_end_in_init = 1;
	dlog[0] = 0;
	CHECK_EQ(DialogBoxIndirectParamW(NULL, t.b, dlg_owner, dlg_proc, 9),
		 5);
	CHECK(!strcmp(dlog, "init,cmd100/256"));
	dlg_end_in_init = 0;
	/* $VFW_DIALOG=ok presses the default button. */
	setenv("VFW_DIALOG", "ok", 1);
	dlog[0] = 0;
	CHECK_EQ(DialogBoxIndirectParamW(NULL, t.b, dlg_owner, dlg_proc, 9),
		 42);
	CHECK(!strcmp(dlog, "init,cmd100/256,cmd1/0"));
	unsetenv("VFW_DIALOG");
	/* One that won't end is ended with the button pressed. */
	dlg_ignore_cancel = 1;
	CHECK_EQ(DialogBoxIndirectParamW(NULL, t.b, dlg_owner, dlg_proc, 9),
		 IDCANCEL);
	dlg_ignore_cancel = 0;
	DestroyWindow(dlg_owner);
	dlg_owner = NULL;
}

int main(void)
{
	CHECK_EQ(lowmem_reserve(), 0);
	CHECK_EQ(runtime_enter(), 0);
	u32 = mod_load("user32.dll", NULL);
	gdi = mod_load("gdi32.dll", NULL);
	k32 = mod_load("kernel32.dll", NULL);
	CHECK(u32 && gdi && k32);
	if (!u32 || !gdi || !k32)
		return test_summary("test_user32");
	CHECK(u32->builtin && !u32->builtin->fallback);

	test_classes_and_windows();
	test_messages();
	test_sent_messages();
	test_send_endings();
	test_strings();
	test_misc();
	test_clipboard();
	test_fixture();
	test_dialogs();

	mod_free(u32);
	mod_free(gdi);
	mod_free(k32);
	return test_summary("test_user32");
}
