/* SPDX-License-Identifier: GPL-2.0-only */
/* winspool.drv's headless, no-printers behaviour. */
#include "test.h"

#include "runtime/builtin.h"
#include "runtime/runtime.h"
#include "win32/internal.h"

typedef BOOL (WINAPI *open_fn)(const char *, HANDLE *, void *);
typedef BOOL (WINAPI *close_fn)(HANDLE);
typedef LONG (WINAPI *properties_fn)(HWND, HANDLE, char *, void *, void *,
				     DWORD);

int main(void)
{
	CHECK_EQ(runtime_enter(), 0);
	const struct builtin_dll *dll = builtin_find("WINSPOOL.DRV");
	CHECK(dll != NULL);
	if (!dll)
		return test_summary("test_winspool");

	open_fn open = builtin_export(dll, "OpenPrinterA");
	close_fn close = builtin_export(dll, "ClosePrinter");
	properties_fn properties = builtin_export(dll, "DocumentPropertiesA");
	CHECK(open && close && properties);
	if (open && close && properties) {
		HANDLE printer = (HANDLE)1;
		CHECK(!open("missing printer", &printer, NULL));
		CHECK(printer == NULL);
		CHECK_EQ(win_get_error(), ERROR_INVALID_PRINTER_NAME);
		CHECK_EQ(properties(NULL, NULL, NULL, NULL, NULL, 0), -1);
		CHECK_EQ(win_get_error(), ERROR_INVALID_HANDLE);
		CHECK(!close(NULL));
		CHECK_EQ(win_get_error(), ERROR_INVALID_HANDLE);
	}

	return test_summary("test_winspool");
}
