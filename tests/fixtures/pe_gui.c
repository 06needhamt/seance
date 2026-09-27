/*
 * Exercises user32 and gdi32 from the Win32 side, with the 4-byte stack
 * alignment old MSVC code has: strings and wsprintf, a hidden window with
 * messages, a timer and WM_QUIT, and a bitmap resource. Each export
 * returns 0, or the number of the first check that failed.
 */
#include "fixture.h"

static HINSTANCE self;
static int created, destroyed, user_msgs;
static char seen_title[32];

static int same(const char *a, const char *b)
{
	return !lstrcmpA(a, b);
}

EXPORT int gui_strings(void)
{
	char buf[128];
	WCHAR w[32];
	const WCHAR *ro;

	/* 1-3: LoadString */
	if (LoadStringA(self, 1, buf, sizeof(buf)) != 5 || !same(buf, "hello"))
		return 1;
	if (LoadStringA(self, 17, buf, 6) != 5 || !same(buf, "block"))
		return 2;
	if (LoadStringW(self, 18, w, 32) != 6 || w[5] != 0x20ac ||
	    LoadStringW(self, 18, (LPWSTR)&ro, 0) != 6 || ro[0] != 'e')
		return 3;
	if (LoadStringA(self, 99, buf, sizeof(buf)) || buf[0])
		return 4;

	/* 5-6: wsprintf, varargs from a misaligned stack */
	int n = wsprintfA(buf, "%d|%5s|%-3c|%04x|%S|%lu|%#X|%%", -12, "ab", 'z',
			  0xab, L"wide", 7ul, 255);
	if (!same(buf, "-12|   ab|z  |00ab|wide|7|0XFF|%") || n != lstrlenA(buf))
		return 5;
	wsprintfW(w, L"%s=%d", L"x", 3);
	if (w[0] != 'x' || w[1] != '=' || w[2] != '3' || w[3])
		return 6;

	/* 7: Char* */
	lstrcpyA(buf, "abc\xe9");
	CharUpperA(buf);
	if (!same(buf, "ABC\xc9") || !IsCharAlphaA('q') || IsCharAlphaA('1'))
		return 7;
	return 0;
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE: {
		const CREATESTRUCTA *cs = (const CREATESTRUCTA *)lp;
		lstrcpynA(seen_title, cs->lpszName, sizeof(seen_title));
		created++;
		return 0;
	}
	case WM_DESTROY:
		destroyed++;
		PostQuitMessage(42);
		return 0;
	case WM_USER:
		user_msgs++;
		return wp + lp;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}

EXPORT int gui_window(void)
{
	WNDCLASSA wc = { 0 };
	char buf[32];
	MSG msg;

	wc.lpfnWndProc = proc;
	wc.hInstance = self;
	wc.lpszClassName = "PeGuiClass";
	wc.cbWndExtra = 8;
	if (!RegisterClassA(&wc))
		return 1;

	HWND hwnd = CreateWindowExA(0, "pegUICLASS", "the title", WS_POPUP, 0, 0,
				    64, 32, NULL, NULL, self, NULL);
	if (!hwnd || created != 1 || !same(seen_title, "the title"))
		return 2;
	if (IsWindowVisible(hwnd) || GetWindowTextA(hwnd, buf, sizeof(buf)) != 9)
		return 3;

	/* 4-5: sent and posted messages, window longs */
	if (SendMessageA(hwnd, WM_USER, 2, 3) != 5)
		return 4;
	SetWindowLongA(hwnd, GWL_USERDATA, 1234);
	SetWindowLongA(hwnd, 4, 99);
	if (GetWindowLongA(hwnd, GWL_USERDATA) != 1234 || GetWindowLongA(hwnd, 4) != 99)
		return 5;
	PostMessageA(hwnd, WM_USER, 10, 0);
	if (!PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) || msg.hwnd != hwnd ||
	    DispatchMessageA(&msg) != 10 || user_msgs != 2)
		return 6;
	if (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
		return 7;

	/* 8: a timer arrives through GetMessage */
	if (SetTimer(hwnd, 7, 5, NULL) != 7)
		return 8;
	if (!GetMessageA(&msg, NULL, 0, 0) || msg.message != WM_TIMER ||
	    msg.wParam != 7 || !KillTimer(hwnd, 7))
		return 8;

	/* 9: destroying posts the quit message, which ends the loop */
	if (!DestroyWindow(hwnd) || destroyed != 1 || IsWindow(hwnd))
		return 9;
	int loops = 0;
	while (GetMessageA(&msg, NULL, 0, 0))
		loops++;
	if (loops || msg.message != WM_QUIT || msg.wParam != 42)
		return 10;
	if (!UnregisterClassA("PeGuiClass", self))
		return 11;
	return 0;
}

EXPORT int gui_bitmap(void)
{
	BITMAP bm;

	HBITMAP h = LoadBitmapA(self, MAKEINTRESOURCEA(10));
	if (!h || GetObjectA(h, sizeof(bm), &bm) != sizeof(bm) ||
	    bm.bmWidth != 2 || bm.bmHeight != 2 || bm.bmBitsPixel != 32)
		return 1;
	HDC dc = CreateCompatibleDC(NULL);
	HGDIOBJ old = SelectObject(dc, h);
	/* The DIB's top row is its second: bytes 07 08 09 are B, G, R. */
	if (GetPixel(dc, 0, 0) != RGB(9, 8, 7) || GetPixel(dc, 1, 1) != RGB(6, 5, 4))
		return 2;
	RECT rc = { 0, 0, 1, 1 };
	if (!FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1)) ||
	    GetPixel(dc, 0, 0) != RGB(255, 255, 255))
		return 3;
	SelectObject(dc, old);
	DeleteDC(dc);
	if (!DeleteObject(h))
		return 4;

	HDC screen = GetDC(NULL);
	if (!screen || GetDeviceCaps(screen, BITSPIXEL) != 32 ||
	    !ReleaseDC(NULL, screen))
		return 5;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		self = inst;
	return TRUE;
}
