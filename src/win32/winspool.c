/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * winspool.drv, headless.  There are no printers, matching comdlg32's
 * printing dialogs.  Codecs commonly link these entry points only for a
 * configuration page; failing them normally lets the codec keep decoding.
 */
#include "win32/internal.h"

WINFN(BOOL) spool_OpenPrinterA(const char *name, HANDLE *printer, void *defaults)
{
	if (printer)
		*printer = NULL;
	win_set_error(ERROR_INVALID_PRINTER_NAME);
	return FALSE;
}

WINFN(BOOL) spool_OpenPrinterW(const WCHAR *name, HANDLE *printer, void *defaults)
{
	return spool_OpenPrinterA(NULL, printer, defaults);
}

WINFN(BOOL) spool_ClosePrinter(HANDLE printer)
{
	win_set_error(ERROR_INVALID_HANDLE);
	return FALSE;
}

WINFN(LONG) spool_DocumentPropertiesA(HWND parent, HANDLE printer,
				      char *device, void *out, void *in,
				      DWORD mode)
{
	win_set_error(ERROR_INVALID_HANDLE);
	return -1;
}

WINFN(LONG) spool_DocumentPropertiesW(HWND parent, HANDLE printer,
				      WCHAR *device, void *out, void *in,
				      DWORD mode)
{
	return spool_DocumentPropertiesA(parent, printer, NULL, out, in, mode);
}

#define E(name) { #name, 0, (void *)spool_##name }

static const struct builtin_export exports[] = {
	E(ClosePrinter),
	E(DocumentPropertiesA),
	E(DocumentPropertiesW),
	E(OpenPrinterA),
	E(OpenPrinterW),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_winspool = { "winspool.drv", NULL, tables, 0 };
