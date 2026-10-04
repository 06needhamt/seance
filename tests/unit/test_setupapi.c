/* SPDX-License-Identifier: GPL-2.0-only */
/* setupapi.dll's headless, empty-device-set behaviour. */
#include "test.h"

#include "runtime/builtin.h"
#include "runtime/runtime.h"
#include "win32/internal.h"

typedef HANDLE (WINAPI *get_fn)(const GUID *, const char *, HWND, DWORD);
typedef BOOL (WINAPI *enum_fn)(HANDLE, DWORD, void *);
typedef BOOL (WINAPI *property_fn)(HANDLE, void *, DWORD, DWORD *, BYTE *,
				   DWORD, DWORD *);
typedef BOOL (WINAPI *destroy_fn)(HANDLE);

int main(void)
{
	CHECK_EQ(runtime_enter(), 0);
	const struct builtin_dll *dll = builtin_find("SETUPAPI.DLL");
	CHECK(dll != NULL);
	if (!dll)
		return test_summary("test_setupapi");

	get_fn get = builtin_export(dll, "SetupDiGetClassDevsA");
	enum_fn enumerate = builtin_export(dll, "SetupDiEnumDeviceInfo");
	property_fn property = builtin_export(
		dll, "SetupDiGetDeviceRegistryPropertyA");
	destroy_fn destroy = builtin_export(dll, "SetupDiDestroyDeviceInfoList");
	CHECK(get && enumerate && property && destroy);
	if (get && enumerate && property && destroy) {
		HANDLE set = get(NULL, NULL, NULL, 0);
		CHECK(set && set != INVALID_HANDLE_VALUE);
		CHECK(!enumerate(set, 0, NULL));
		CHECK_EQ(win_get_error(), ERROR_NO_MORE_ITEMS);
		DWORD needed = 42;
		CHECK(!property(set, NULL, 0, NULL, NULL, 0, &needed));
		CHECK_EQ(needed, 0);
		CHECK_EQ(win_get_error(), ERROR_INVALID_DATA);
		CHECK(destroy(set));
		CHECK(!destroy(INVALID_HANDLE_VALUE));
		CHECK_EQ(win_get_error(), ERROR_INVALID_HANDLE);
	}

	return test_summary("test_setupapi");
}
