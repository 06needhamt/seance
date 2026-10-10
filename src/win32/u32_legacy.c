/* SPDX-License-Identifier: GPL-2.0-only */
/* Headless implementations of legacy user32 entry points. */
#include <stdlib.h>
#include <string.h>

#include "win32/codepage.h"
#include "win32/gdi.h"
#include "win32/internal.h"
#include "win32/user.h"

typedef LRESULT (CALLBACK *HOOKPROC)(int, WPARAM, LPARAM);
typedef HANDLE HHOOK;

typedef struct {
	UINT length, flags, showCmd;
	POINT min, max;
	RECT normal;
} WINDOWPLACEMENT;

WINFN(BOOL) user_AdjustWindowRectEx(LPRECT rc, DWORD style, BOOL menu,
				     DWORD ex_style)
{
	int x, y;

	if (!rc)
		return FALSE;
	x = style & 0x00c00000 ? 4 : 0;
	y = x + (menu ? 19 : 0);
	rc->left -= x;
	rc->right += x;
	rc->top -= y;
	rc->bottom += x;
	return TRUE;
}

WINFN(LRESULT) user_CallNextHookEx(HHOOK hook, int code, WPARAM wparam,
				    LPARAM lparam)
{
	return 0;
}

WINFN(DWORD) user_CheckMenuItem(HMENU menu, UINT id, UINT flags)
{
	return (DWORD)-1;
}

WINFN(BOOL) user_CheckRadioButton(HWND hwnd, int first, int last, int check)
{
	return TRUE;
}

WINFN(HWND) user_ChildWindowFromPoint(HWND parent, POINT point)
{
	return user_is_window(parent) ? parent : NULL;
}

/*
 * Menus are only names: nothing is shown, so a new one is a handle that
 * items can be inserted into and that can be destroyed.
 */
static ULONG_PTR next_menu = 0x00100001;

WINFN(HMENU) user_CreateMenu(void)
{
	return (HMENU)(next_menu += 4);
}

WINFN(HMENU) user_CreatePopupMenu(void)
{
	return user_CreateMenu();
}

WINFN(BOOL) user_InsertMenuItemW(HMENU menu, UINT item, BOOL by_position,
				 const void *info)
{
	if (!menu || !info) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return TRUE;
}

WINFN(BOOL) user_DestroyMenu(HMENU menu)
{
	return menu != NULL;
}

/*
 * DrawText: the text in lines, broken at line breaks and, with
 * DT_WORDBREAK, between words to fit the rectangle's width; each drawn as
 * ExtTextOut draws it, left, centred or right in rc, clipped to it unless
 * DT_NOCLIP; a single line centred or at the bottom if asked. "&x" is x
 * (the prefix underline isn't drawn) unless DT_NOPREFIX, and tabs are
 * eight characters with DT_EXPANDTABS. DT_CALCRECT measures instead,
 * setting rc's right and bottom. The text's height is returned; with no
 * DC it is measured in the system font, and not drawn.
 */
static int draw_text(HDC hdc, const WCHAR *in, int n, RECT *rc, UINT fmt)
{
	int cw, ch;

	if (!in || !rc || !gdi_text_width(hdc, 1, &cw, &ch))
		return 0;
	if (n < 0)
		for (n = 0; in[n]; n++)
			;
	/* The text as it is drawn: prefixes and tabs dealt with. */
	WCHAR *text = malloc(((size_t)n * 8 + 1) * sizeof(WCHAR));
	if (!text)
		return 0;
	int len = 0;
	for (int i = 0; i < n; i++) {
		if (in[i] == '&' && !(fmt & DT_NOPREFIX) && i + 1 < n) {
			text[len++] = in[++i];
		} else if (in[i] == '\t' && (fmt & DT_EXPANDTABS)) {
			do
				text[len++] = ' ';
			while (len % 8);
		} else {
			text[len++] = in[i];
		}
	}

	int room = rc->right - rc->left;
	int max = 0, lines = 0, pos = 0;
	int single = fmt & DT_SINGLELINE;
	struct { int start, length; } *line = malloc(((size_t)len + 1) * sizeof(*line));
	if (!line) {
		free(text);
		return 0;
	}
	while (pos < len || !lines) {
		int end = pos;
		while (end < len && (single || (text[end] != '\n' && text[end] != '\r')))
			end++;
		/* Broken between words to fit, at least a word a line. */
		if ((fmt & DT_WORDBREAK) && !single && room > 0 &&
		    (end - pos) * cw > room) {
			int cut = pos + room / cw;
			while (cut > pos && text[cut] != ' ')
				cut--;
			if (cut == pos) {
				cut = pos;
				while (cut < end && text[cut] != ' ')
					cut++;
			}
			end = cut;
		}
		line[lines].start = pos;
		line[lines].length = end - pos;
		if ((end - pos) * cw > max)
			max = (end - pos) * cw;
		lines++;
		pos = end;
		if (pos < len && text[pos] == ' ' && (fmt & DT_WORDBREAK))
			pos++;
		else if (pos < len && text[pos] == '\r')
			pos += pos + 1 < len && text[pos + 1] == '\n' ? 2 : 1;
		else if (pos < len && text[pos] == '\n')
			pos++;
		if (pos >= len)
			break;
	}
	int height = lines * ch;

	if (fmt & DT_CALCRECT) {
		rc->right = rc->left + max;
		rc->bottom = rc->top + height;
	} else if (hdc) {
		int top = rc->top;
		if (single && (fmt & DT_VCENTER))
			top = rc->top + (rc->bottom - rc->top - ch) / 2;
		else if (single && (fmt & DT_BOTTOM))
			top = rc->bottom - ch;
		for (int l = 0; l < lines; l++) {
			int w = line[l].length * cw, x = rc->left;
			if (fmt & DT_CENTER)
				x = rc->left + (room - w) / 2;
			else if (fmt & DT_RIGHT)
				x = rc->right - w;
			gdi_text_out(hdc, x, top + l * ch,
				     fmt & DT_NOCLIP ? 0 : ETO_CLIPPED, rc,
				     text + line[l].start, line[l].length, NULL);
		}
	}
	free(line);
	free(text);
	return height;
}

WINFN(int) user_DrawTextW(HDC hdc, LPCWSTR text, int count, LPRECT rc,
			  UINT format)
{
	return draw_text(hdc, text, count, rc, format);
}

WINFN(int) user_DrawTextA(HDC hdc, LPCSTR text, int count, LPRECT rc,
			  UINT format)
{
	if (!text)
		return 0;
	if (count < 0)
		count = (int)strlen(text);
	int wn = count ? cp_to_utf16(CP_ACP, text, count, NULL, 0, 0) : 0;
	WCHAR *w = wn >= 0 ? malloc(((size_t)wn + 1) * sizeof(WCHAR)) : NULL;
	if (!w)
		return 0;
	if (wn)
		cp_to_utf16(CP_ACP, text, count, w, wn, 0);
	int r = draw_text(hdc, w, wn, rc, format);
	free(w);
	return r;
}

WINFN(int) user_DrawTextExA(HDC hdc, LPSTR text, int count, LPRECT rc,
			    UINT format, void *params)
{
	return user_DrawTextA(hdc, text, count, rc, format);
}

WINFN(int) user_DrawTextExW(HDC hdc, LPWSTR text, int count, LPRECT rc,
			    UINT format, void *params)
{
	return user_DrawTextW(hdc, text, count, rc, format);
}

WINFN(BOOL) user_EnableMenuItem(HMENU menu, UINT id, UINT flags)
{
	return (UINT)-1;
}

WINFN(DWORD) user_GetClassLongA(HWND hwnd, int index)
{
	return 0;
}

WINFN(HCURSOR) user_GetCursor(void)
{
	return NULL;
}

WINFN(HMENU) user_GetMenu(HWND hwnd)
{
	return NULL;
}

WINFN(LONG) user_GetMenuCheckMarkDimensions(void)
{
	return (13 & 0xffff) | (13 << 16);
}

WINFN(int) user_GetMenuItemCount(HMENU menu)
{
	return -1;
}

WINFN(UINT) user_GetMenuItemID(HMENU menu, int position)
{
	return (UINT)-1;
}

WINFN(UINT) user_GetMenuState(HMENU menu, UINT id, UINT flags)
{
	return (UINT)-1;
}

WINFN(HWND) user_GetNextDlgTabItem(HWND dialog, HWND control, BOOL previous)
{
	return NULL;
}

WINFN(int) user_GetScrollPos(HWND hwnd, int bar)
{
	return 0;
}

WINFN(BOOL) user_GetScrollRange(HWND hwnd, int bar, int *min, int *max)
{
	(void)bar;
	if (!user_is_window(hwnd)) return FALSE;
	if (min) *min = 0;
	if (max) *max = 100;
	return TRUE;
}

WINFN(WORD) user_GetWindowWord(HWND hwnd, int index)
{
	if (!user_is_window(hwnd)) return 0;
	/* Positive offsets are application-owned extra bytes, which this
	 * headless window model does not allocate.  Negative indices are the
	 * low word of their GetWindowLong counterparts. */
	(void)index;
	return 0;
}

WINFN(HMENU) user_GetSubMenu(HMENU menu, int position)
{
	return NULL;
}

WINFN(HWND) user_GetTopWindow(HWND hwnd)
{
	return NULL;
}

WINFN(BOOL) user_GetUpdateRect(HWND hwnd, LPRECT rc, BOOL erase)
{
	if (rc)
		*rc = (RECT){ 0 };
	return FALSE;
}

WINFN(HWND) user_GetWindow(HWND hwnd, UINT command)
{
	return NULL;
}

WINFN(BOOL) user_GetWindowPlacement(HWND hwnd, WINDOWPLACEMENT *placement)
{
	if (!placement || !user_is_window(hwnd))
		return FALSE;
	placement->flags = 0;
	placement->showCmd = 1;
	placement->min = (POINT){ -1, -1 };
	placement->max = (POINT){ -1, -1 };
	return user_client_rect(hwnd, &placement->normal);
}

WINFN(BOOL) user_GrayStringA(HDC hdc, HBRUSH brush, void *callback,
			      LPARAM data, int count, int x, int y, int cx, int cy)
{
	return TRUE;
}

WINFN(BOOL) user_IsDialogMessageA(HWND dialog, LPMSG message)
{
	return FALSE;
}

WINFN(BOOL) user_MapDialogRect(HWND dialog, LPRECT rc)
{
	if (!rc)
		return FALSE;
	rc->left = rc->left * 8 / 4;
	rc->right = rc->right * 8 / 4;
	rc->top = rc->top * 16 / 8;
	rc->bottom = rc->bottom * 16 / 8;
	return TRUE;
}

WINFN(int) user_MapWindowPoints(HWND from, HWND to, LPPOINT points, UINT count)
{
	return 0;
}

WINFN(BOOL) user_ModifyMenuA(HMENU menu, UINT position, UINT flags,
			      UINT_PTR id, LPCSTR text)
{
	return FALSE;
}

WINFN(BOOL) user_RedrawWindow(HWND hwnd, const RECT *update, HRGN region,
			      UINT flags)
{
	return !hwnd || user_is_window(hwnd);
}

WINFN(BOOL) user_SetMenuItemBitmaps(HMENU menu, UINT position, UINT flags,
				     HBITMAP unchecked, HBITMAP checked)
{
	return FALSE;
}

WINFN(int) user_SetScrollPos(HWND hwnd, int bar, int position, BOOL redraw)
{
	return position;
}

WINFN(BOOL) user_SetScrollRange(HWND hwnd, int bar, int min, int max,
				 BOOL redraw)
{
	return TRUE;
}

WINFN(HHOOK) user_SetWindowsHookExA(int id, HOOKPROC callback,
				     HINSTANCE module, DWORD thread)
{
	win_set_error(ERROR_CALL_NOT_IMPLEMENTED);
	return NULL;
}

WINFN(LONG) user_TabbedTextOutA(HDC hdc, int x, int y, LPCSTR text,
				 int count, int ntabs, const int *tabs, int origin)
{
	if (count < 0)
		count = strlen(text);
	return (count * 8 & 0xffff) | (16 << 16);
}

WINFN(BOOL) user_UnhookWindowsHookEx(HHOOK hook)
{
	return FALSE;
}

WINFN(BOOL) user_WinHelpA(HWND hwnd, LPCSTR file, UINT command,
			   ULONG_PTR data)
{
	return FALSE;
}

/* No menus are shown, so a popup menu is dismissed without a choice. */
WINFN(BOOL) user_TrackPopupMenu(HMENU menu, UINT flags, int x, int y,
				int reserved, HWND hwnd, const RECT *rc)
{
	return FALSE;
}

/* There is no shell window. */
WINFN(HWND) user_GetShellWindow(void)
{
	return NULL;
}

#define E(name) { #name, 0, (void *)user_##name }

const struct builtin_export u32_legacy_exports[] = {
	E(AdjustWindowRectEx),
	E(CallNextHookEx),
	E(CheckMenuItem),
	E(CheckRadioButton),
	E(ChildWindowFromPoint),
	E(CreateMenu),
	E(CreatePopupMenu),
	E(DestroyMenu),
	E(DrawTextA),
	E(DrawTextExA),
	E(DrawTextExW),
	E(DrawTextW),
	E(EnableMenuItem),
	E(GetClassLongA),
	E(GetCursor),
	E(GetMenu),
	E(GetMenuCheckMarkDimensions),
	E(GetMenuItemCount),
	E(GetMenuItemID),
	E(GetMenuState),
	E(GetNextDlgTabItem),
	E(GetScrollPos),
	E(GetScrollRange),
	E(GetShellWindow),
	E(GetSubMenu),
	E(GetTopWindow),
	E(GetUpdateRect),
	E(GetWindow),
	E(GetWindowPlacement),
	E(GetWindowWord),
	E(GrayStringA),
	E(InsertMenuItemW),
	E(IsDialogMessageA),
	E(MapDialogRect),
	E(MapWindowPoints),
	E(ModifyMenuA),
	E(RedrawWindow),
	E(SetMenuItemBitmaps),
	E(SetScrollPos),
	E(SetScrollRange),
	E(SetWindowsHookExA),
	E(TabbedTextOutA),
	E(TrackPopupMenu),
	E(UnhookWindowsHookEx),
	E(WinHelpA),
	BUILTIN_END
};
