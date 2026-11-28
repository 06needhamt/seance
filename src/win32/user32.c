/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * user32.dll: message boxes, system metrics and colours, DCs and
 * painting, rectangles, input state, icons, cursors and bitmaps, window
 * stations and monitors - and the export table list. Windows and messages
 * are in u32_window.c, strings in u32_string.c.
 *
 * A message box can't be shown: its text goes to stderr and it answers
 * with its default button, as if someone pressed Enter. There is no
 * keyboard or mouse: no key is ever down and the cursor rests mid-screen.
 * Icons and cursors are handles with nothing behind them.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/error.h"
#include "loader/modules.h"
#include "loader/pe.h"
#include "win32/codepage.h"
#include "win32/gdi.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"
#include "win32/user.h"

#define IS_INTRESOURCE(p) (((ULONG_PTR)(p) >> 16) == 0)

extern const struct builtin_dll builtin_gdi32, builtin_user32;

/* Message boxes. */

static int message_box(const char *text, const char *caption, UINT type)
{
	static const int buttons[7][3] = {
		{ IDOK }, { IDOK, IDCANCEL }, { IDABORT, IDRETRY, IDIGNORE },
		{ IDYES, IDNO, IDCANCEL }, { IDYES, IDNO }, { IDRETRY, IDCANCEL },
		{ IDCANCEL, IDTRYAGAIN, IDCONTINUE },
	};
	static const char *const names[] = {
		NULL, "OK", "Cancel", "Abort", "Retry", "Ignore", "Yes", "No",
		NULL, NULL, "Try Again", "Continue",
	};
	UINT kind = type & MB_TYPEMASK, def = (type & MB_DEFMASK) >> 8;

	if (kind > 6)
		kind = 0;
	if (def > 2 || !buttons[kind][def])
		def = 0;
	int answer = buttons[kind][def];
	fprintf(stderr, "MessageBox [%s]: %s\n  (no GUI: answering %s)\n",
		caption ? caption : "Error", text ? text : "", names[answer]);
	return answer;
}

WINFN(int) user_MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type)
{
	char *t = acp_to_utf8(text), *c = acp_to_utf8(caption);
	int r = message_box(t, c, type);
	free(t);
	free(c);
	return r;
}

WINFN(int) user_MessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type)
{
	char *t = utf16_to_utf8_dup(text), *c = utf16_to_utf8_dup(caption);
	int r = message_box(t, c, type);
	free(t);
	free(c);
	return r;
}

WINFN(int) user_MessageBoxExA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type,
			      WORD lang)
{
	return user_MessageBoxA(hwnd, text, caption, type);
}

WINFN(int) user_MessageBoxExW(HWND hwnd, LPCWSTR text, LPCWSTR caption,
			      UINT type, WORD lang)
{
	return user_MessageBoxW(hwnd, text, caption, type);
}

WINFN(BOOL) user_MessageBeep(UINT type)
{
	return TRUE;
}

/* System metrics and colours: Windows XP's, classic theme, 1024x768. */

WINFN(int) user_GetSystemMetrics(int index)
{
	switch (index) {
	case SM_CXSCREEN:
	case SM_CXFULLSCREEN:
	case SM_CXVIRTUALSCREEN: return USER_SCREEN_WIDTH;
	case SM_CYSCREEN:
	case SM_CYVIRTUALSCREEN: return USER_SCREEN_HEIGHT;
	case SM_CYFULLSCREEN:    return USER_SCREEN_HEIGHT - 19;
	case SM_CXVSCROLL:
	case SM_CYHSCROLL:
	case SM_CYVSCROLL:
	case SM_CXHSCROLL:
	case SM_CYVTHUMB:
	case SM_CXHTHUMB:        return 16;
	case SM_CYCAPTION:
	case SM_CYMENU:          return 19;
	case SM_CXBORDER:
	case SM_CYBORDER:        return 1;
	case SM_CXDLGFRAME:
	case SM_CYDLGFRAME:      return 3;
	case SM_CXFRAME:
	case SM_CYFRAME:
	case SM_CXDOUBLECLK:
	case SM_CYDOUBLECLK:     return 4;
	case SM_CXICON:
	case SM_CYICON:
	case SM_CXCURSOR:
	case SM_CYCURSOR:        return 32;
	case SM_CXSMICON:
	case SM_CYSMICON:        return 16;
	case SM_CXSIZE:
	case SM_CYSIZE:          return 18;
	case SM_CXMIN:
	case SM_CXMINTRACK:      return 112;
	case SM_CYMIN:
	case SM_CYMINTRACK:      return 27;
	case SM_CXMAXIMIZED:     return USER_SCREEN_WIDTH + 8;
	case SM_CYMAXIMIZED:     return USER_SCREEN_HEIGHT + 8;
	case SM_MOUSEPRESENT:
	case SM_NETWORK:
	case SM_CMONITORS:
	case SM_SAMEDISPLAYFORMAT: return 1;
	case SM_CMOUSEBUTTONS:   return 3;
	default:                 return 0;
	}
}

static const COLORREF sys_colors[31] = {
	RGB(212, 208, 200), RGB(58, 110, 165), RGB(10, 36, 106),
	RGB(128, 128, 128), RGB(212, 208, 200), RGB(255, 255, 255),
	RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), RGB(255, 255, 255),
	RGB(212, 208, 200), RGB(212, 208, 200), RGB(128, 128, 128),
	RGB(10, 36, 106), RGB(255, 255, 255), RGB(212, 208, 200),
	RGB(128, 128, 128), RGB(128, 128, 128), RGB(0, 0, 0),
	RGB(212, 208, 200), RGB(255, 255, 255), RGB(64, 64, 64),
	RGB(212, 208, 200), RGB(0, 0, 0), RGB(255, 255, 225),
	RGB(181, 181, 181), RGB(0, 0, 128), RGB(166, 202, 240),
	RGB(192, 192, 192), RGB(10, 36, 106), RGB(212, 208, 200),
};

WINFN(DWORD) user_GetSysColor(int index)
{
	return index >= 0 && index < 31 ? sys_colors[index] : 0;
}

WINFN(HBRUSH) user_GetSysColorBrush(int index)
{
	return index >= 0 && index < 31 ? gdi_sys_color_brush(index, sys_colors[index])
					: NULL;
}

WINFN(BOOL) user_SystemParametersInfoA(UINT action, UINT param, PVOID pv,
				       UINT ini)
{
	if (action == SPI_GETWORKAREA && pv) {
		RECT *rc = pv;
		rc->left = rc->top = 0;
		rc->right = USER_SCREEN_WIDTH;
		rc->bottom = USER_SCREEN_HEIGHT;
		return TRUE;
	}
	TRACE("SystemParametersInfo(%#x) not supported", action);
	win_set_error(ERROR_INVALID_PARAMETER);
	return FALSE;
}

WINFN(BOOL) user_SystemParametersInfoW(UINT action, UINT param, PVOID pv,
				       UINT ini)
{
	return user_SystemParametersInfoA(action, param, pv, ini);
}

/* DCs and painting. */

WINFN(HDC) user_GetDC(HWND hwnd)
{
	if (hwnd && !user_is_window(hwnd)) {
		win_set_error(ERROR_INVALID_WINDOW_HANDLE);
		return NULL;
	}
	return gdi_display_dc(hwnd ? hwnd : user_desktop());
}

WINFN(HDC) user_GetWindowDC(HWND hwnd)
{
	return user_GetDC(hwnd);
}

WINFN(HDC) user_GetDCEx(HWND hwnd, HRGN clip, DWORD flags)
{
	return user_GetDC(hwnd);
}

WINFN(int) user_ReleaseDC(HWND hwnd, HDC hdc)
{
	return gdi_release_dc(hwnd, hdc);
}

WINFN(HDC) user_BeginPaint(HWND hwnd, LPPAINTSTRUCT ps)
{
	RECT rc;

	if (!ps || !user_client_rect(hwnd, &rc))
		return NULL;
	memset(ps, 0, sizeof(*ps));
	ps->hdc = user_GetDC(hwnd);
	ps->rcPaint = rc;
	return ps->hdc;
}

WINFN(BOOL) user_EndPaint(HWND hwnd, const PAINTSTRUCT *ps)
{
	if (ps)
		gdi_release_dc(hwnd, ps->hdc);
	return TRUE;
}

/* A brush of COLOR_x + 1 means that system colour. */
WINFN(int) user_FillRect(HDC hdc, const RECT *rc, HBRUSH brush)
{
	if ((ULONG_PTR)brush && (ULONG_PTR)brush <= 31)
		brush = user_GetSysColorBrush((int)(ULONG_PTR)brush - 1);
	return gdi_fill_rect(hdc, rc, brush);
}

/* Rectangles. */

WINFN(BOOL) user_SetRect(LPRECT rc, int l, int t, int r, int b)
{
	if (!rc)
		return FALSE;
	rc->left = l;
	rc->top = t;
	rc->right = r;
	rc->bottom = b;
	return TRUE;
}

WINFN(BOOL) user_SetRectEmpty(LPRECT rc)
{
	return user_SetRect(rc, 0, 0, 0, 0);
}

WINFN(BOOL) user_CopyRect(LPRECT dst, const RECT *src)
{
	if (!dst || !src)
		return FALSE;
	*dst = *src;
	return TRUE;
}

WINFN(BOOL) user_IsRectEmpty(const RECT *rc)
{
	return !rc || rc->right <= rc->left || rc->bottom <= rc->top;
}

WINFN(BOOL) user_EqualRect(const RECT *a, const RECT *b)
{
	return a && b && !memcmp(a, b, sizeof(*a));
}

WINFN(BOOL) user_InflateRect(LPRECT rc, int dx, int dy)
{
	if (!rc)
		return FALSE;
	rc->left -= dx;
	rc->right += dx;
	rc->top -= dy;
	rc->bottom += dy;
	return TRUE;
}

WINFN(BOOL) user_OffsetRect(LPRECT rc, int dx, int dy)
{
	if (!rc)
		return FALSE;
	rc->left += dx;
	rc->right += dx;
	rc->top += dy;
	rc->bottom += dy;
	return TRUE;
}

WINFN(BOOL) user_IntersectRect(LPRECT dst, const RECT *a, const RECT *b)
{
	if (!dst || !a || !b)
		return FALSE;
	RECT r = {
		a->left > b->left ? a->left : b->left,
		a->top > b->top ? a->top : b->top,
		a->right < b->right ? a->right : b->right,
		a->bottom < b->bottom ? a->bottom : b->bottom,
	};
	if (user_IsRectEmpty(&r)) {
		memset(dst, 0, sizeof(*dst));
		return FALSE;
	}
	*dst = r;
	return TRUE;
}

WINFN(BOOL) user_UnionRect(LPRECT dst, const RECT *a, const RECT *b)
{
	if (!dst || !a || !b)
		return FALSE;
	if (user_IsRectEmpty(a) && user_IsRectEmpty(b)) {
		memset(dst, 0, sizeof(*dst));
		return FALSE;
	}
	if (user_IsRectEmpty(a)) {
		*dst = *b;
		return TRUE;
	}
	if (user_IsRectEmpty(b)) {
		*dst = *a;
		return TRUE;
	}
	RECT r = {
		a->left < b->left ? a->left : b->left,
		a->top < b->top ? a->top : b->top,
		a->right > b->right ? a->right : b->right,
		a->bottom > b->bottom ? a->bottom : b->bottom,
	};
	*dst = r;
	return TRUE;
}

WINFN(BOOL) user_PtInRect(const RECT *rc, POINT pt)
{
	return rc && pt.x >= rc->left && pt.x < rc->right && pt.y >= rc->top &&
	       pt.y < rc->bottom;
}

/*
 * Keyboard and mouse: no device, the cursor rests mid-screen. Each
 * thread's key state follows the keyboard messages it takes from its
 * queue (and SetKeyboardState), as Windows' does; the asynchronous state
 * of the (absent) hardware is always up.
 */

static __thread BYTE key_state[256];	/* 0x80 down, 0x01 toggled */

static void key_set(int vk, int down)
{
	if (down && !(key_state[vk] & 0x80))
		key_state[vk] ^= 0x01;
	key_state[vk] = (BYTE)((key_state[vk] & 0x01) | (down ? 0x80 : 0));
}

void user_key_message(const MSG *m)
{
	int down;
	switch (m->message) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		down = 1;
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		down = 0;
		break;
	default:
		return;
	}
	int vk = (int)(m->wParam & 0xff);
	key_set(vk, down);
	/* The left and right keys move the generic one too. */
	if (vk == 0xa0 || vk == 0xa1)
		key_set(VK_SHIFT, down);
	else if (vk == 0xa2 || vk == 0xa3)
		key_set(VK_CONTROL, down);
	else if (vk == 0xa4 || vk == 0xa5)
		key_set(VK_MENU, down);
}

WINFN(SHORT) user_GetKeyState(int key)
{
	BYTE s = key_state[key & 0xff];
	return (SHORT)(((s & 0x80) ? 0xff80 : 0) | (s & 0x01));
}

WINFN(SHORT) user_GetAsyncKeyState(int key) { return 0; }

WINFN(BOOL) user_GetKeyboardState(PBYTE keys)
{
	if (keys)
		memcpy(keys, key_state, 256);
	return keys != NULL;
}

WINFN(BOOL) user_SetKeyboardState(PBYTE keys)
{
	if (!keys)
		return FALSE;
	memcpy(key_state, keys, 256);
	return TRUE;
}

/* An enhanced 101/102-key keyboard: Delphi's start-up code asks, to see
 * whether it runs on a Japanese or Korean one. */
WINFN(int) user_GetKeyboardType(int what)
{
	switch (what) {
	case 0:
		return 4;	/* type: IBM enhanced */
	case 1:
		return 0;	/* OEM subtype */
	case 2:
		return 12;	/* function keys */
	default:
		return 0;
	}
}

WINFN(HANDLE) user_GetKeyboardLayout(DWORD tid)
{
	return (HANDLE)0x04090409;
}

/* One layout installed, the one GetKeyboardLayout reports. */
WINFN(int) user_GetKeyboardLayoutList(int n, HANDLE *list)
{
	if (n > 0 && list)
		list[0] = (HANDLE)0x04090409;
	return 1;
}

/* The cursor: in the middle of the screen until moved, kept on it. */
static int cursor_x = USER_SCREEN_WIDTH / 2, cursor_y = USER_SCREEN_HEIGHT / 2;

WINFN(BOOL) user_GetCursorPos(LPPOINT pt)
{
	if (!pt)
		return FALSE;
	pt->x = __atomic_load_n(&cursor_x, __ATOMIC_RELAXED);
	pt->y = __atomic_load_n(&cursor_y, __ATOMIC_RELAXED);
	return TRUE;
}

WINFN(BOOL) user_SetCursorPos(int x, int y)
{
	x = x < 0 ? 0 : x >= USER_SCREEN_WIDTH ? USER_SCREEN_WIDTH - 1 : x;
	y = y < 0 ? 0 : y >= USER_SCREEN_HEIGHT ? USER_SCREEN_HEIGHT - 1 : y;
	__atomic_store_n(&cursor_x, x, __ATOMIC_RELAXED);
	__atomic_store_n(&cursor_y, y, __ATOMIC_RELAXED);
	return TRUE;
}
WINFN(BOOL) user_ClipCursor(const RECT *rc) { return TRUE; }
WINFN(UINT) user_GetDoubleClickTime(void) { return 500; }

static HCURSOR current_cursor;
static int cursor_count;

WINFN(HCURSOR) user_SetCursor(HCURSOR c)
{
	return __atomic_exchange_n(&current_cursor, c, __ATOMIC_SEQ_CST);
}

WINFN(int) user_ShowCursor(BOOL show)
{
	return __atomic_add_fetch(&cursor_count, show ? 1 : -1, __ATOMIC_SEQ_CST);
}

/* Icons, cursors and bitmaps. */

/* A resource name argument as pe_find_resource takes it. */
static pe_resid res_name(const void *name, int wide, char *buf, size_t size)
{
	if (IS_INTRESOURCE(name))
		return (pe_resid)name;
	char *u = wide ? utf16_to_utf8_dup(name) : acp_to_utf8(name);
	snprintf(buf, size, "%s", u ? u : "");
	free(u);
	if (buf[0] == '#')
		return strtoul(buf + 1, NULL, 10);
	return (pe_resid)buf;
}

/* The data of a resource in inst's module, or NULL. */
static const void *resource(HINSTANCE inst, DWORD type, const void *name,
			    int wide, DWORD *size)
{
	struct module *m = mod_from_base(inst);
	struct pe_resource res;
	char buf[256];

	if (!m || !m->img ||
	    pe_find_resource(m->img, type, res_name(name, wide, buf, sizeof(buf)),
			     PE_RES_ANY_LANG, &res)) {
		win_set_error(ERROR_RESOURCE_NAME_NOT_FOUND);
		return NULL;
	}
	*size = res.size;
	return pe_ptr(m->img, res.data_rva, res.size);
}

static ULONG_PTR next_icon = 0x00200000;

/*
 * An icon or cursor handle: nothing is behind it, but it exists if the
 * resource (or, with no instance, the system one) does.
 */
static HICON load_icon(HINSTANCE inst, const void *name, DWORD group, int wide)
{
	DWORD size;

	if (inst && !resource(inst, group, name, wide, &size))
		return NULL;
	return (HICON)__atomic_add_fetch(&next_icon, 2, __ATOMIC_SEQ_CST);
}

WINFN(HICON) user_LoadIconA(HINSTANCE i, LPCSTR n) { return load_icon(i, n, RT_GROUP_ICON, 0); }
WINFN(HICON) user_LoadIconW(HINSTANCE i, LPCWSTR n) { return load_icon(i, n, RT_GROUP_ICON, 1); }
WINFN(HCURSOR) user_LoadCursorA(HINSTANCE i, LPCSTR n) { return load_icon(i, n, RT_GROUP_CURSOR, 0); }
WINFN(HCURSOR) user_LoadCursorW(HINSTANCE i, LPCWSTR n) { return load_icon(i, n, RT_GROUP_CURSOR, 1); }
WINFN(BOOL) user_DestroyIcon(HICON h) { return h != NULL; }
WINFN(BOOL) user_DestroyCursor(HCURSOR h) { return h != NULL; }

/* The pixels of a packed DIB (a BITMAPINFO followed by its bits). */
static const void *dib_bits(const BITMAPINFOHEADER *h, DWORD size)
{
	if (size < sizeof(*h) || h->biSize < sizeof(*h) || h->biSize > size)
		return NULL;
	DWORD off = h->biSize;
	if (h->biCompression == BI_BITFIELDS && h->biSize == sizeof(*h))
		off += 12;
	if (h->biBitCount <= 8)
		off += 4 * (h->biClrUsed ? h->biClrUsed : 1u << h->biBitCount);
	return off < size ? (const BYTE *)h + off : NULL;
}

/* A bitmap from a packed DIB: a DIB section, or one compatible with the display. */
static HBITMAP bitmap_from_dib(const BITMAPINFOHEADER *h, DWORD size, int dib)
{
	HBITMAP (WINAPI *create_ddb)(HDC, const BITMAPINFOHEADER *, DWORD,
				     LPCVOID, const BITMAPINFO *, UINT) =
		builtin_export(&builtin_gdi32, "CreateDIBitmap");
	HBITMAP (WINAPI *create_dib)(HDC, const BITMAPINFO *, UINT, void **,
				     HANDLE, DWORD) =
		builtin_export(&builtin_gdi32, "CreateDIBSection");
	const void *bits = dib_bits(h, size);
	HBITMAP bm = NULL;

	if (!bits) {
		win_set_error(ERROR_INVALID_DATA);
		return NULL;
	}
	HDC dc = gdi_display_dc(NULL);
	if (dib) {
		void *out;
		bm = create_dib(dc, (const BITMAPINFO *)h, DIB_RGB_COLORS, &out,
				NULL, 0);
		LONG height = h->biHeight < 0 ? -h->biHeight : h->biHeight;
		size_t n = (size_t)(((h->biWidth * h->biBitCount + 31) / 32) * 4) * height;
		if (bm && (const BYTE *)bits + n <= (const BYTE *)h + size)
			memcpy(out, bits, n);
	} else {
		bm = create_ddb(dc, h, CBM_INIT, bits, (const BITMAPINFO *)h,
				DIB_RGB_COLORS);
	}
	gdi_release_dc(NULL, dc);
	return bm;
}

static HBITMAP load_bitmap(HINSTANCE inst, const void *name, int wide, int dib)
{
	DWORD size;
	const void *data = inst ? resource(inst, RT_BITMAP, name, wide, &size) : NULL;

	if (!inst)
		win_set_error(ERROR_RESOURCE_NAME_NOT_FOUND);	/* no OEM bitmaps */
	return data ? bitmap_from_dib(data, size, dib) : NULL;
}

WINFN(HBITMAP) user_LoadBitmapA(HINSTANCE inst, LPCSTR name)
{
	return load_bitmap(inst, name, 0, 0);
}

WINFN(HBITMAP) user_LoadBitmapW(HINSTANCE inst, LPCWSTR name)
{
	return load_bitmap(inst, name, 1, 0);
}

/* A .bmp file named by a Windows path. */
static HBITMAP load_bitmap_file(const void *name, int wide, int dib)
{
	char *u = wide ? utf16_to_utf8_dup(name) : acp_to_utf8(name);
	char *path = u ? path_to_unix(u) : NULL;
	void *buf = NULL;
	size_t len;
	HBITMAP bm = NULL;

	free(u);
	if (!path || vfw_read_file(path, &buf, &len)) {
		win_set_error(ERROR_FILE_NOT_FOUND);
	} else if (len < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) ||
		   ((BYTE *)buf)[0] != 'B' || ((BYTE *)buf)[1] != 'M') {
		win_set_error(ERROR_INVALID_DATA);
	} else {
		bm = bitmap_from_dib((const BITMAPINFOHEADER *)((BYTE *)buf +
					sizeof(BITMAPFILEHEADER)),
				     len - sizeof(BITMAPFILEHEADER), dib);
	}
	free(path);
	free(buf);
	return bm;
}

static HANDLE load_image(HINSTANCE inst, const void *name, UINT type,
			 UINT flags, int wide)
{
	switch (type) {
	case IMAGE_BITMAP:
		if (flags & LR_LOADFROMFILE)
			return load_bitmap_file(name, wide, flags & LR_CREATEDIBSECTION);
		return load_bitmap(inst, name, wide, flags & LR_CREATEDIBSECTION);
	case IMAGE_ICON:
	case IMAGE_CURSOR:
		if (flags & LR_LOADFROMFILE)
			return load_icon(NULL, name, 0, wide);
		return load_icon(inst, name, type == IMAGE_ICON ? RT_GROUP_ICON :
					   RT_GROUP_CURSOR, wide);
	default:
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
}

WINFN(HANDLE) user_LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx,
			      int cy, UINT flags)
{
	return load_image(inst, name, type, flags, 0);
}

WINFN(HANDLE) user_LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx,
			      int cy, UINT flags)
{
	return load_image(inst, name, type, flags, 1);
}

/* Window stations, desktops and monitors: one of each, visible. */

#define WINSTA  ((HWINSTA)0x00000010)
#define DESKTOP ((HANDLE)0x00000014)
#define MONITOR ((HMONITOR)0x00010001)

WINFN(HWINSTA) user_GetProcessWindowStation(void) { return WINSTA; }
WINFN(HANDLE) user_GetThreadDesktop(DWORD tid) { return DESKTOP; }

static BOOL object_info(HANDLE h, int index, void *buf, DWORD len,
			LPDWORD needed, int wide)
{
	const char *s = NULL;
	USEROBJECTFLAGS flags = { FALSE, FALSE, WSF_VISIBLE };
	DWORD n;

	if (h != WINSTA && h != DESKTOP) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	switch (index) {
	case UOI_FLAGS:
		n = sizeof(flags);
		break;
	case UOI_NAME:
		s = h == WINSTA ? "WinSta0" : "Default";
		break;
	case UOI_TYPE:
		s = h == WINSTA ? "WindowStation" : "Desktop";
		break;
	default:
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (s)
		n = (strlen(s) + 1) * (wide ? sizeof(WCHAR) : 1);
	if (needed)
		*needed = n;
	if (!buf || len < n) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (!s)
		memcpy(buf, &flags, n);
	else if (wide)
		for (size_t i = 0; i <= strlen(s); i++)
			((WCHAR *)buf)[i] = s[i];
	else
		memcpy(buf, s, n);
	return TRUE;
}

WINFN(BOOL) user_GetUserObjectInformationA(HANDLE h, int index, PVOID buf,
					   DWORD len, LPDWORD needed)
{
	return object_info(h, index, buf, len, needed, 0);
}

WINFN(BOOL) user_GetUserObjectInformationW(HANDLE h, int index, PVOID buf,
					   DWORD len, LPDWORD needed)
{
	return object_info(h, index, buf, len, needed, 1);
}

WINFN(HMONITOR) user_MonitorFromWindow(HWND hwnd, DWORD flags) { return MONITOR; }
WINFN(HMONITOR) user_MonitorFromRect(const RECT *rc, DWORD flags) { return MONITOR; }

WINFN(HMONITOR) user_MonitorFromPoint(POINT pt, DWORD flags)
{
	int on = pt.x >= 0 && pt.y >= 0 && pt.x < USER_SCREEN_WIDTH &&
		 pt.y < USER_SCREEN_HEIGHT;
	return on || flags != MONITOR_DEFAULTTONULL ? MONITOR : NULL;
}

/* MONITORINFOEX adds the device name after MONITORINFO. */
static BOOL monitor_info(HMONITOR h, MONITORINFO *mi, int wide)
{
	static const char device[] = "\\\\.\\DISPLAY1";
	DWORD ex = sizeof(MONITORINFO) + 32 * (wide ? sizeof(WCHAR) : 1);

	if (h != MONITOR || !mi || mi->cbSize < sizeof(MONITORINFO)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	RECT screen = { 0, 0, USER_SCREEN_WIDTH, USER_SCREEN_HEIGHT };
	mi->rcMonitor = mi->rcWork = screen;
	mi->dwFlags = MONITORINFOF_PRIMARY;
	if (mi->cbSize >= ex) {
		BYTE *name = (BYTE *)(mi + 1);
		for (size_t i = 0; i < sizeof(device); i++) {
			if (wide)
				((WCHAR *)name)[i] = device[i];
			else
				name[i] = device[i];
		}
	}
	return TRUE;
}

WINFN(BOOL) user_GetMonitorInfoA(HMONITOR h, LPMONITORINFO mi) { return monitor_info(h, mi, 0); }
WINFN(BOOL) user_GetMonitorInfoW(HMONITOR h, LPMONITORINFO mi) { return monitor_info(h, mi, 1); }

/*
 * The one display has one mode, the screen GetSystemMetrics and
 * GetDeviceCaps describe; it is mode 0, the current and the registry's.
 * The device is the primary display, named or not.
 */
static BOOL display_mode(const WCHAR *device, DWORD mode, BYTE *out, int wide)
{
	static const char name[] = "\\\\.\\DISPLAY1";
	static const char gdi_name[] = "DISPLAY";
	DEVMODEW w;		/* the A layout's fields, widened */

	if (device && device[0]) {
		size_t i = 0;
		for (; name[i] && (device[i] | 0x20) == (name[i] | 0x20); i++)
			;
		if (name[i] || device[i]) {
			win_set_error(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
	}
	if (mode != ENUM_CURRENT_SETTINGS && mode != ENUM_REGISTRY_SETTINGS &&
	    mode != 0) {
		win_set_error(ERROR_INVALID_PARAMETER);	/* no more modes */
		return FALSE;
	}
	if (!out) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	memset(&w, 0, sizeof(w));
	for (size_t i = 0; i < sizeof(gdi_name); i++)
		w.dmDeviceName[i] = gdi_name[i];
	w.dmSpecVersion = DM_SPECVERSION;
	w.dmDriverVersion = DM_SPECVERSION;
	w.dmFields = DM_POSITION | DM_BITSPERPEL | DM_PELSWIDTH |
		     DM_PELSHEIGHT | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
	w.dmBitsPerPel = GDI_SCREEN_BPP;
	w.dmPelsWidth = USER_SCREEN_WIDTH;
	w.dmPelsHeight = USER_SCREEN_HEIGHT;
	w.dmDisplayFrequency = 60;

	/* The caller says how big its structure is; only that is written. */
	WORD size = ((const DEVMODEA *)out)->dmSize;
	if (wide) {
		w.dmSize = sizeof(DEVMODEW);
		memcpy(out, &w, size && size < sizeof(w) ? size : sizeof(w));
		return TRUE;
	}
	DEVMODEA a;
	memset(&a, 0, sizeof(a));
	for (size_t i = 0; i < CCHDEVICENAME; i++)
		a.dmDeviceName[i] = (BYTE)w.dmDeviceName[i];
	memcpy(&a.dmSpecVersion, &w.dmSpecVersion,
	       offsetof(DEVMODEA, dmFormName) - offsetof(DEVMODEA, dmSpecVersion));
	memcpy(&a.dmLogPixels, &w.dmLogPixels,
	       sizeof(a) - offsetof(DEVMODEA, dmLogPixels));
	a.dmSize = sizeof(DEVMODEA);
	memcpy(out, &a, size && size < sizeof(a) ? size : sizeof(a));
	return TRUE;
}

static WCHAR *device_w(const char *s, WCHAR *buf, size_t n)
{
	if (!s)
		return NULL;
	size_t i = 0;
	for (; s[i] && i + 1 < n; i++)
		buf[i] = (BYTE)s[i];
	buf[i] = 0;
	return buf;
}

WINFN(BOOL) user_EnumDisplaySettingsExW(LPCWSTR device, DWORD mode,
					DEVMODEW *dm, DWORD flags)
{
	return display_mode(device, mode, (BYTE *)dm, 1);
}

WINFN(BOOL) user_EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW *dm)
{
	return display_mode(device, mode, (BYTE *)dm, 1);
}

WINFN(BOOL) user_EnumDisplaySettingsExA(LPCSTR device, DWORD mode,
					DEVMODEA *dm, DWORD flags)
{
	WCHAR w[64];
	return display_mode(device_w(device, w, 64), mode, (BYTE *)dm, 0);
}

WINFN(BOOL) user_EnumDisplaySettingsA(LPCSTR device, DWORD mode, DEVMODEA *dm)
{
	return user_EnumDisplaySettingsExA(device, mode, dm, 0);
}

WINFN(DWORD) user_WaitForInputIdle(HANDLE process, DWORD ms) { return 0; }

/* Registered window messages and clipboard formats share one table. */

static char *registered[0x3fff];
static int nregistered;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

static UINT register_name(char *name)
{
	UINT id = 0;

	if (!name || !*name) {
		free(name);
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	pthread_mutex_lock(&reg_lock);
	for (int i = 0; i < nregistered && !id; i++)
		if (!strcasecmp(registered[i], name))
			id = 0xc000 + i;
	if (!id && nregistered < (int)(sizeof(registered) / sizeof(registered[0]))) {
		registered[nregistered] = name;
		id = 0xc000 + nregistered++;
		name = NULL;
	}
	pthread_mutex_unlock(&reg_lock);
	free(name);
	return id;
}

WINFN(UINT) user_RegisterWindowMessageA(LPCSTR name) { return register_name(acp_to_utf8(name)); }
WINFN(UINT) user_RegisterWindowMessageW(LPCWSTR name) { return register_name(utf16_to_utf8_dup(name)); }

/*
 * The clipboard, kept in the process: with no desktop to share it with,
 * what a DLL puts there is what it gets back. SetClipboardData hands the
 * data to the clipboard, as on Windows, which frees it when emptied (GDI
 * objects with DeleteObject, memory with GlobalFree; private formats stay
 * the application's). One thread at a time has it open. A NULL handle asks
 * for delayed rendering: GetClipboardData then sends the owner
 * WM_RENDERFORMAT, which puts the data there; an owner that is destroyed
 * is sent WM_RENDERALLFORMATS first, and what it leaves unrendered goes.
 * EmptyClipboard sends the previous owner WM_DESTROYCLIPBOARD.
 */
#define CLIP_FORMATS 16
#ifndef WM_RENDERFORMAT
#define WM_RENDERFORMAT 0x0305
#endif
#ifndef WM_RENDERALLFORMATS
#define WM_RENDERALLFORMATS 0x0306
#endif
#ifndef WM_DESTROYCLIPBOARD
#define WM_DESTROYCLIPBOARD 0x0307
#endif
#define CLIP_BITMAP 2
#define CLIP_PALETTE 9
#define CLIP_DSPBITMAP 0x82
#define CLIP_PRIVATEFIRST 0x0200
#define CLIP_PRIVATELAST 0x02ff
#define CLIP_GDIOBJFIRST 0x0300
#define CLIP_GDIOBJLAST 0x03ff
static struct { UINT format; HANDLE data; } clip[CLIP_FORMATS];
static pthread_mutex_t clip_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static DWORD clip_thread;		/* that has it open, or 0 */
static HWND clip_window;		/* that opened it */
static HWND clip_owner;			/* that last emptied it */
static UINT rendering;			/* the format WM_RENDERFORMAT asked for */
static DWORD clip_sequence;		/* changes with the contents */

static void clip_release(UINT format, HANDLE data)
{
	/* Private formats' handles stay the application's to free. */
	if (!data || (format >= CLIP_PRIVATEFIRST && format <= CLIP_PRIVATELAST))
		return;
	if (format == CLIP_BITMAP || format == CLIP_PALETTE ||
	    format == CLIP_DSPBITMAP ||
	    (format >= CLIP_GDIOBJFIRST && format <= CLIP_GDIOBJLAST)) {
		BOOL (WINAPI *del)(HGDIOBJ) =
			builtin_export(&builtin_gdi32, "DeleteObject");
		if (del)
			del(data);
	} else {
		global_free(data);
	}
}

static void clip_empty(void)
{
	for (int i = 0; i < CLIP_FORMATS; i++) {
		if (clip[i].format)
			clip_release(clip[i].format, clip[i].data);
		clip[i].format = 0;
		clip[i].data = NULL;
	}
}

/* 1 if this thread has the clipboard open (call with clip_lock held). */
static int clip_mine(void)
{
	return clip_thread && clip_thread == thread_current_id();
}

WINFN(BOOL) user_OpenClipboard(HWND owner)
{
	DWORD me = thread_current_id();
	BOOL ok = TRUE;

	pthread_mutex_lock(&clip_lock);
	if (clip_thread && clip_thread != me) {
		win_set_error(ERROR_ACCESS_DENIED);
		ok = FALSE;
	} else {
		clip_thread = me;
		clip_window = owner;
	}
	pthread_mutex_unlock(&clip_lock);
	return ok;
}

WINFN(BOOL) user_CloseClipboard(void)
{
	BOOL ok = TRUE;

	pthread_mutex_lock(&clip_lock);
	if (!clip_mine()) {
		win_set_error(ERROR_CLIPBOARD_NOT_OPEN);
		ok = FALSE;
	} else {
		clip_thread = 0;
		clip_window = NULL;
	}
	pthread_mutex_unlock(&clip_lock);
	return ok;
}

static LRESULT clip_send(HWND hwnd, UINT msg, WPARAM wp)
{
	LRESULT (WINAPI *send)(HWND, UINT, WPARAM, LPARAM) =
		builtin_export(builtin_find("user32.dll"), "SendMessageW");
	return send ? send(hwnd, msg, wp, 0) : 0;
}

WINFN(BOOL) user_EmptyClipboard(void)
{
	pthread_mutex_lock(&clip_lock);
	if (!clip_mine()) {
		pthread_mutex_unlock(&clip_lock);
		win_set_error(ERROR_CLIPBOARD_NOT_OPEN);
		return FALSE;
	}
	HWND old = clip_owner;
	pthread_mutex_unlock(&clip_lock);
	/* The previous owner frees what it kept for the data; it can't lose
	 * the clipboard meanwhile, as this thread has it open. */
	if (old)
		clip_send(old, WM_DESTROYCLIPBOARD, 0);
	pthread_mutex_lock(&clip_lock);
	clip_empty();
	clip_owner = clip_window;
	clip_sequence++;
	pthread_mutex_unlock(&clip_lock);
	return TRUE;
}

WINFN(DWORD) user_GetClipboardSequenceNumber(void)
{
	pthread_mutex_lock(&clip_lock);
	DWORD n = clip_sequence;
	pthread_mutex_unlock(&clip_lock);
	return n;
}

void user_clipboard_owner_destroyed(HWND hwnd)
{
	pthread_mutex_lock(&clip_lock);
	int owner = hwnd && clip_owner == hwnd;
	int promised = 0;
	for (int i = 0; owner && i < CLIP_FORMATS; i++)
		promised |= clip[i].format && !clip[i].data;
	pthread_mutex_unlock(&clip_lock);
	if (!owner)
		return;
	/* It renders what it promised (opening the clipboard to do so). */
	if (promised)
		clip_send(hwnd, WM_RENDERALLFORMATS, 0);
	pthread_mutex_lock(&clip_lock);
	if (clip_owner == hwnd) {
		int gone = 0;
		for (int i = 0; i < CLIP_FORMATS; i++)
			if (clip[i].format && !clip[i].data) {
				clip[i].format = 0;
				gone = 1;
			}
		if (gone)	/* the contents changed */
			clip_sequence++;
		clip_owner = NULL;
	}
	pthread_mutex_unlock(&clip_lock);
}

WINFN(HANDLE) user_SetClipboardData(UINT format, HANDLE data)
{
	int free_slot = -1, slot = -1;
	HANDLE ret = NULL;

	pthread_mutex_lock(&clip_lock);
	/* The owner answers WM_RENDERFORMAT without opening the clipboard. */
	int allowed = clip_mine() || (rendering && rendering == format);
	if (!allowed || !format) {
		win_set_error(allowed ? ERROR_INVALID_PARAMETER :
			      ERROR_CLIPBOARD_NOT_OPEN);
		goto out;
	}
	for (int i = 0; i < CLIP_FORMATS && slot < 0; i++) {
		if (clip[i].format == format)
			slot = i;
		else if (!clip[i].format && free_slot < 0)
			free_slot = i;
	}
	if (slot < 0 && (slot = free_slot) < 0) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		goto out;
	}
	int same = clip[slot].format == format && clip[slot].data == data;
	if (clip[slot].format == format && clip[slot].data != data)
		clip_release(format, clip[slot].data);
	clip[slot].format = format;
	clip[slot].data = data;
	/* A change, a delayed format's data arriving included, is a new
	 * sequence number; the same handle set again isn't. */
	if (!same)
		clip_sequence++;
	ret = data;	/* NULL for delayed rendering, as on Windows */
	win_set_error(ERROR_SUCCESS);
out:
	pthread_mutex_unlock(&clip_lock);
	return ret;
}

/* The slot holding format, or -1 (call with clip_lock held). */
static int clip_find(UINT format)
{
	for (int i = 0; i < CLIP_FORMATS; i++)
		if (clip[i].format && clip[i].format == format)
			return i;
	return -1;
}

WINFN(HANDLE) user_GetClipboardData(UINT format)
{
	pthread_mutex_lock(&clip_lock);
	int i = clip_find(format);
	HANDLE data = i >= 0 ? clip[i].data : NULL;
	HWND owner = clip_owner;
	pthread_mutex_unlock(&clip_lock);
	if (i < 0 || data || !owner)
		return data;

	/*
	 * Delayed: the owner renders it now, on its own thread, with the
	 * lock not held; while it does, it may set that format.
	 */
	pthread_mutex_lock(&clip_lock);
	UINT was = rendering;
	rendering = format;
	pthread_mutex_unlock(&clip_lock);
	clip_send(owner, WM_RENDERFORMAT, format);
	pthread_mutex_lock(&clip_lock);
	rendering = was;
	i = clip_find(format);
	data = i >= 0 ? clip[i].data : NULL;
	pthread_mutex_unlock(&clip_lock);
	return data;
}

/* Data there, or promised (delayed rendering), counts as available. */
WINFN(BOOL) user_IsClipboardFormatAvailable(UINT format)
{
	pthread_mutex_lock(&clip_lock);
	BOOL there = clip_find(format) >= 0;
	pthread_mutex_unlock(&clip_lock);
	return there;
}

WINFN(int) user_CountClipboardFormats(void)
{
	int n = 0;

	pthread_mutex_lock(&clip_lock);
	for (int i = 0; i < CLIP_FORMATS; i++)
		n += clip[i].format != 0;
	pthread_mutex_unlock(&clip_lock);
	return n;
}

WINFN(UINT) user_EnumClipboardFormats(UINT format)
{
	int seen = !format;
	UINT next = 0;

	pthread_mutex_lock(&clip_lock);
	for (int i = 0; i < CLIP_FORMATS && !next; i++) {
		if (!clip[i].format)
			continue;
		if (seen)
			next = clip[i].format;
		else
			seen = clip[i].format == format;
	}
	pthread_mutex_unlock(&clip_lock);
	return next;
}

WINFN(HWND) user_GetClipboardOwner(void)
{
	pthread_mutex_lock(&clip_lock);
	HWND h = clip_owner;
	pthread_mutex_unlock(&clip_lock);
	return h;
}

#define E(name) { #name, 0, (void *)user_##name }

/* ---- accelerator tables ---- */

struct accel_table {
	struct accel_table *next;
	int n;
	ACCEL e[];
};

static pthread_mutex_t accel_lock = PTHREAD_MUTEX_INITIALIZER;
static struct accel_table *accels;

/* Windows' largest accelerator table. */
#define ACCEL_MAX 32767

static HACCEL accel_new(const ACCEL *e, int n)
{
	struct accel_table *t;

	if (n <= 0 || n > ACCEL_MAX || !e) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	if (!(t = malloc(sizeof(*t) + (size_t)n * sizeof(ACCEL)))) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	t->n = n;
	for (int i = 0; i < n; i++) {
		t->e[i] = e[i];
		t->e[i].fVirt &= FVIRTKEY | FNOINVERT | FSHIFT | FCONTROL | FALT;
	}
	pthread_mutex_lock(&accel_lock);
	t->next = accels;
	accels = t;
	pthread_mutex_unlock(&accel_lock);
	return (HACCEL)t;
}

/* The live table h, with accel_lock held; NULL (and unlocked) if none. */
static struct accel_table *accel_get(HACCEL h)
{
	pthread_mutex_lock(&accel_lock);
	for (struct accel_table *t = accels; t; t = t->next)
		if ((HACCEL)t == h)
			return t;
	pthread_mutex_unlock(&accel_lock);
	win_set_error(ERROR_INVALID_HANDLE);
	return NULL;
}

/* A non-virtual key in ANSI, as the A functions take and give it. */
static WORD key_from_ansi(WORD k)
{
	char c = (char)k;
	WCHAR w = 0;
	return cp_to_utf16(CP_ACP, &c, 1, &w, 1, 0) == 1 ? w : k;
}

static WORD key_to_ansi(WORD k)
{
	WCHAR w = k;
	char c = 0;
	return cp_from_utf16(CP_ACP, &w, 1, &c, 1, '?', NULL) == 1 ?
		       (BYTE)c : k;
}

WINFN(HACCEL) user_CreateAcceleratorTableW(LPACCEL e, int n)
{
	return accel_new(e, n);
}

WINFN(HACCEL) user_CreateAcceleratorTableA(LPACCEL e, int n)
{
	if (n <= 0 || n > ACCEL_MAX || !e) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	ACCEL *w = malloc((size_t)n * sizeof(*w));
	if (!w)
		return NULL;
	for (int i = 0; i < n; i++) {
		w[i] = e[i];
		if (!(w[i].fVirt & FVIRTKEY))
			w[i].key = key_from_ansi(w[i].key);
	}
	HACCEL h = accel_new(w, n);
	free(w);
	return h;
}

WINFN(BOOL) user_DestroyAcceleratorTable(HACCEL h)
{
	pthread_mutex_lock(&accel_lock);
	for (struct accel_table **p = &accels; *p; p = &(*p)->next)
		if ((HACCEL)*p == h) {
			struct accel_table *t = *p;
			*p = t->next;
			pthread_mutex_unlock(&accel_lock);
			free(t);
			return TRUE;
		}
	pthread_mutex_unlock(&accel_lock);
	return FALSE;
}

static int accel_copy(HACCEL h, LPACCEL dst, int n, int ansi)
{
	struct accel_table *t = accel_get(h);
	if (!t)
		return 0;
	int count = t->n;
	if (dst) {
		count = n < t->n ? n : t->n;
		for (int i = 0; i < count; i++) {
			dst[i] = t->e[i];
			if (ansi && !(dst[i].fVirt & FVIRTKEY))
				dst[i].key = key_to_ansi(dst[i].key);
		}
	}
	pthread_mutex_unlock(&accel_lock);
	return count < 0 ? 0 : count;
}

WINFN(int) user_CopyAcceleratorTableW(HACCEL h, LPACCEL dst, int n)
{
	return accel_copy(h, dst, n, 0);
}

WINFN(int) user_CopyAcceleratorTableA(HACCEL h, LPACCEL dst, int n)
{
	return accel_copy(h, dst, n, 1);
}

/* RT_ACCELERATOR: 8-byte entries, the last flagged 0x80. */
static HACCEL load_accel(HINSTANCE inst, const void *name, int wide)
{
	DWORD size;
	const BYTE *p = resource(inst, RT_ACCELERATOR, name, wide, &size);
	int n = 0;

	if (!p)
		return NULL;
	ACCEL *e = malloc((size / 8 + 1) * sizeof(*e));
	if (!e)
		return NULL;
	for (DWORD at = 0; at + 8 <= size; at += 8) {
		e[n].fVirt = p[at];
		e[n].key = (WORD)(p[at + 2] | p[at + 3] << 8);
		e[n].cmd = (WORD)(p[at + 4] | p[at + 5] << 8);
		n++;
		if (p[at] & 0x80)
			break;
	}
	HACCEL h = n ? accel_new(e, n) : NULL;
	if (!n)
		win_set_error(ERROR_RESOURCE_DATA_NOT_FOUND);
	free(e);
	return h;
}

WINFN(HACCEL) user_LoadAcceleratorsW(HINSTANCE inst, LPCWSTR name)
{
	return load_accel(inst, name, 1);
}

WINFN(HACCEL) user_LoadAcceleratorsA(HINSTANCE inst, LPCSTR name)
{
	return load_accel(inst, name, 0);
}

/*
 * The command a keyboard message is the accelerator for, or -1. Virtual
 * keys match WM_KEYDOWN/WM_SYSKEYDOWN with the shift states; characters
 * match WM_CHAR/WM_SYSCHAR, ALT being bit 29 of a system message.
 */
int user_accel_match(const ACCEL *e, int n, const MSG *msg)
{
	int virt, alt = (msg->message == WM_SYSKEYDOWN ||
			 msg->message == WM_SYSCHAR) &&
			(msg->lParam & 0x20000000);

	switch (msg->message) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		virt = 1;
		break;
	case WM_CHAR:
	case WM_SYSCHAR:
		virt = 0;
		break;
	default:
		return -1;
	}
	int shift = user_GetKeyState(VK_SHIFT) < 0;
	int ctrl = user_GetKeyState(VK_CONTROL) < 0;
	for (int i = 0; i < n; i++) {
		if (!!(e[i].fVirt & FVIRTKEY) != virt ||
		    e[i].key != (WORD)msg->wParam)
			continue;
		if (!!(e[i].fVirt & FALT) != alt)
			continue;
		if (virt && (!!(e[i].fVirt & FSHIFT) != shift ||
			     !!(e[i].fVirt & FCONTROL) != ctrl))
			continue;
		return e[i].cmd;
	}
	return -1;
}

static int translate_accel(HWND hwnd, HACCEL h, const MSG *msg)
{
	if (!msg)
		return 0;
	struct accel_table *t = accel_get(h);
	if (!t)
		return 0;
	int cmd = user_accel_match(t->e, t->n, msg);
	pthread_mutex_unlock(&accel_lock);
	if (cmd < 0)
		return 0;
	LRESULT (WINAPI *send)(HWND, UINT, WPARAM, LPARAM) =
		builtin_export(&builtin_user32, "SendMessageW");
	if (send)
		send(hwnd, WM_COMMAND, (WPARAM)(DWORD)(cmd | 0x10000), 0);
	return 1;
}

WINFN(int) user_TranslateAcceleratorW(HWND hwnd, HACCEL h, LPMSG msg)
{
	return translate_accel(hwnd, h, msg);
}

WINFN(int) user_TranslateAcceleratorA(HWND hwnd, HACCEL h, LPMSG msg)
{
	if (!msg || (msg->message != WM_CHAR && msg->message != WM_SYSCHAR))
		return translate_accel(hwnd, h, msg);
	MSG w = *msg;
	w.wParam = key_from_ansi((WORD)msg->wParam);
	return translate_accel(hwnd, h, &w);
}

static const struct builtin_export exports[] = {
	E(BeginPaint),
	E(CopyAcceleratorTableA),
	E(CopyAcceleratorTableW),
	E(CreateAcceleratorTableA),
	E(CreateAcceleratorTableW),
	E(DestroyAcceleratorTable),
	E(LoadAcceleratorsA),
	E(LoadAcceleratorsW),
	E(TranslateAcceleratorA),
	E(TranslateAcceleratorW),
	E(CloseClipboard),
	E(CountClipboardFormats),
	E(EmptyClipboard),
	E(EnumClipboardFormats),
	E(GetClipboardData),
	E(GetClipboardOwner),
	E(GetClipboardSequenceNumber),
	E(IsClipboardFormatAvailable),
	E(OpenClipboard),
	E(SetClipboardData),
	E(ClipCursor),
	E(CopyRect),
	E(DestroyCursor),
	E(DestroyIcon),
	E(EndPaint),
	E(EqualRect),
	E(FillRect),
	E(GetAsyncKeyState),
	E(GetCursorPos),
	E(GetDC),
	E(GetDCEx),
	E(GetDoubleClickTime),
	E(GetKeyState),
	E(GetKeyboardLayout),
	E(GetKeyboardLayoutList),
	E(GetKeyboardState),
	E(SetKeyboardState),
	E(GetKeyboardType),
	E(EnumDisplaySettingsA),
	E(EnumDisplaySettingsExA),
	E(EnumDisplaySettingsExW),
	E(EnumDisplaySettingsW),
	E(GetMonitorInfoA),
	E(GetMonitorInfoW),
	E(GetProcessWindowStation),
	E(GetSysColor),
	E(GetSysColorBrush),
	E(GetSystemMetrics),
	E(GetThreadDesktop),
	E(GetUserObjectInformationA),
	E(GetUserObjectInformationW),
	E(GetWindowDC),
	E(InflateRect),
	E(IntersectRect),
	E(IsRectEmpty),
	E(LoadBitmapA),
	E(LoadBitmapW),
	E(LoadCursorA),
	E(LoadCursorW),
	E(LoadIconA),
	E(LoadIconW),
	E(LoadImageA),
	E(LoadImageW),
	E(MessageBeep),
	E(MessageBoxA),
	E(MessageBoxExA),
	E(MessageBoxExW),
	E(MessageBoxW),
	E(MonitorFromPoint),
	E(MonitorFromRect),
	E(MonitorFromWindow),
	E(OffsetRect),
	E(PtInRect),
	{ "RegisterClipboardFormatA", 0, (void *)user_RegisterWindowMessageA },
	{ "RegisterClipboardFormatW", 0, (void *)user_RegisterWindowMessageW },
	E(RegisterWindowMessageA),
	E(RegisterWindowMessageW),
	E(ReleaseDC),
	E(SetCursor),
	E(SetCursorPos),
	E(SetRect),
	E(SetRectEmpty),
	E(ShowCursor),
	E(SystemParametersInfoA),
	E(SystemParametersInfoW),
	E(UnionRect),
	E(WaitForInputIdle),
	BUILTIN_END
};

extern const struct builtin_export u32_window_exports[];
extern const struct builtin_export u32_string_exports[];
extern const struct builtin_export u32_legacy_exports[];
extern const struct builtin_export u32_dialog_exports[];

static const struct builtin_export *const tables[] = {
	exports, u32_window_exports, u32_string_exports, u32_legacy_exports,
	u32_dialog_exports, NULL
};

const struct builtin_dll builtin_user32 = { "user32.dll", NULL, tables, 0 };
