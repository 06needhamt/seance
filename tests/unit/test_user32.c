/*
 * user32, called through the export table the way a DLL's imports reach
 * it: classes, windows and messages, timers, strings, and the rest.
 */
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
	CHECK_EQ(SetWindowLongA(h, GWL_USERDATA, 77), 0);
	CHECK_EQ(GetWindowLongA(h, GWL_USERDATA), 77);
	CHECK_EQ(SetWindowLongA(h, 4, 5), 0);
	CHECK_EQ(GetWindowLongA(h, 4), 5);
	CHECK_EQ(GetWindowLongA(h, 8), 0);
	CHECK_EQ(GetLastError(), ERROR_INVALID_INDEX);
	CHECK(GetWindowLongA(h, GWL_WNDPROC) == (LONG)(ULONG_PTR)proc_a);
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
	int (*wsprintfA)(LPSTR, LPCSTR, ...) = get_from(u32, "wsprintfA");
	int (*wsprintfW)(LPWSTR, LPCWSTR, ...) = get_from(u32, "wsprintfW");
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
	CHECK_STR(buf, "(null)|-5|00001234|y");
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

static void test_misc(void)
{
	FN(int, MessageBoxA, HWND, LPCSTR, LPCSTR, UINT);
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
	FN(BOOL, GetCursorPos, LPPOINT);
	FN(HWINSTA, GetProcessWindowStation, void);
	FN(BOOL, GetUserObjectInformationA, HANDLE, int, PVOID, DWORD, LPDWORD);
	FN(HMONITOR, MonitorFromWindow, HWND, DWORD);
	FN(BOOL, GetMonitorInfoA, HMONITOR, LPMONITORINFO);
	FN(BOOL, SystemParametersInfoA, UINT, UINT, PVOID, UINT);
	FN(HICON, LoadIconA, HINSTANCE, LPCSTR);
	FN(int, LoadStringA, HINSTANCE, UINT, LPSTR, int);
	GDI(int, GetDeviceCaps, HDC, int);
	GDI(DWORD, GetObjectType, HGDIOBJ);
	GDI(int, GetObjectA, HGDIOBJ, int, LPVOID);

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
	POINT pt;
	CHECK(GetCursorPos(&pt) && pt.x == 512);

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
		int (*fn)(void) = mod_get_proc(m, tests[i]);
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
	test_strings();
	test_misc();
	test_fixture();

	mod_free(u32);
	mod_free(gdi);
	mod_free(k32);
	return test_summary("test_user32");
}
