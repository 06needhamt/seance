/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oledlg.dll: the OLE common dialogs (Busy, Insert Object, Paste Special,
 * ...). Codecs built with MFC link them (vp7vfw.dll imports OleUIBusyA by
 * ordinal) and never get as far as showing one. With nobody to answer,
 * every dialog is cancelled, as a user closing it would.
 */
#include "win32/internal.h"

#define OLEUI_CANCEL 2

WINFN(BOOL) dlg_OleUIAddVerbMenuA(void *object, LPCSTR short_type, HMENU menu,
				  UINT pos, UINT first, UINT last, BOOL convert,
				  UINT convert_id, HMENU *popup)
{
	if (popup)
		*popup = NULL;
	return FALSE;
}

WINFN(BOOL) dlg_OleUIAddVerbMenuW(void *object, LPCWSTR short_type,
				  HMENU menu, UINT pos, UINT first, UINT last,
				  BOOL convert, UINT convert_id, HMENU *popup)
{
	if (popup)
		*popup = NULL;
	return FALSE;
}

WINFN(BOOL) dlg_OleUICanConvertOrActivateAs(const GUID *clsid, BOOL linked,
					    WORD format)
{
	return FALSE;
}

WINFN(BOOL) dlg_OleUIUpdateLinksA(void *container, HWND parent, LPSTR title,
				  int links)
{
	return FALSE;
}

WINFN(BOOL) dlg_OleUIUpdateLinksW(void *container, HWND parent, LPWSTR title,
				  int links)
{
	return FALSE;
}

/* cdecl, as it takes the template's arguments after the parent. */
static INT CDECL dlg_OleUIPromptUserA(INT template, HWND parent, ...)
{
	return OLEUI_CANCEL;
}

static INT CDECL dlg_OleUIPromptUserW(INT template, HWND parent, ...)
{
	return OLEUI_CANCEL;
}

/* The dialogs that take one structure describing themselves. */
#define DIALOG(name) \
	WINFN(UINT) dlg_##name(void *info) { return OLEUI_CANCEL; }

DIALOG(OleUIInsertObjectA)
DIALOG(OleUIInsertObjectW)
DIALOG(OleUIPasteSpecialA)
DIALOG(OleUIPasteSpecialW)
DIALOG(OleUIEditLinksA)
DIALOG(OleUIEditLinksW)
DIALOG(OleUIChangeIconA)
DIALOG(OleUIChangeIconW)
DIALOG(OleUIConvertA)
DIALOG(OleUIConvertW)
DIALOG(OleUIBusyA)
DIALOG(OleUIBusyW)
DIALOG(OleUIObjectPropertiesA)
DIALOG(OleUIObjectPropertiesW)
DIALOG(OleUIChangeSourceA)
DIALOG(OleUIChangeSourceW)

#define O(name, n) { #name, n, (void *)dlg_##name }

/* The ANSI functions' ordinals are as Windows numbers them. */
static const struct builtin_export exports[] = {
	O(OleUIAddVerbMenuA, 1),
	O(OleUICanConvertOrActivateAs, 2),
	O(OleUIInsertObjectA, 3),
	O(OleUIPasteSpecialA, 4),
	O(OleUIEditLinksA, 5),
	O(OleUIChangeIconA, 6),
	O(OleUIConvertA, 7),
	O(OleUIBusyA, 8),
	O(OleUIUpdateLinksA, 9),
	O(OleUIPromptUserA, 10),
	O(OleUIObjectPropertiesA, 11),
	O(OleUIChangeSourceA, 12),
	O(OleUIAddVerbMenuW, 0),
	O(OleUIBusyW, 0),
	O(OleUIChangeIconW, 0),
	O(OleUIChangeSourceW, 0),
	O(OleUIConvertW, 0),
	O(OleUIEditLinksW, 0),
	O(OleUIInsertObjectW, 0),
	O(OleUIObjectPropertiesW, 0),
	O(OleUIPasteSpecialW, 0),
	O(OleUIPromptUserW, 0),
	O(OleUIUpdateLinksW, 0),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_oledlg = { "oledlg.dll", NULL, tables, 0 };
