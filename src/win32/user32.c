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
#include "win32/path.h"
#include "win32/user.h"

#define IS_INTRESOURCE(p) (((ULONG_PTR)(p) >> 16) == 0)

extern const struct builtin_dll builtin_gdi32;

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

/* Keyboard and mouse: nothing is pressed, the cursor rests mid-screen. */

WINFN(SHORT) user_GetKeyState(int key) { return 0; }
WINFN(SHORT) user_GetAsyncKeyState(int key) { return 0; }

WINFN(BOOL) user_GetKeyboardState(PBYTE keys)
{
	if (keys)
		memset(keys, 0, 256);
	return keys != NULL;
}

WINFN(HANDLE) user_GetKeyboardLayout(DWORD tid)
{
	return (HANDLE)0x04090409;
}

WINFN(BOOL) user_GetCursorPos(LPPOINT pt)
{
	if (!pt)
		return FALSE;
	pt->x = USER_SCREEN_WIDTH / 2;
	pt->y = USER_SCREEN_HEIGHT / 2;
	return TRUE;
}

WINFN(BOOL) user_SetCursorPos(int x, int y) { return TRUE; }
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

#define E(name) { #name, 0, (void *)user_##name }

static const struct builtin_export exports[] = {
	E(BeginPaint),
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
	E(GetKeyboardState),
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

static const struct builtin_export *const tables[] = {
	exports, u32_window_exports, u32_string_exports, NULL
};

const struct builtin_dll builtin_user32 = { "user32.dll", NULL, tables, 0 };
