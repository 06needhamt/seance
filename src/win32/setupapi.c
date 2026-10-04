/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * setupapi.dll, headless device enumeration.  The harness exposes no host
 * hardware to Windows codecs, so every successfully created information set
 * is empty.  This is enough for codecs which merely check for capture cards.
 */
#include <stdint.h>

#include "win32/internal.h"

#define EMPTY_DEVICE_SET ((HANDLE)(uintptr_t)1)

WINFN(HANDLE) setup_SetupDiGetClassDevsA(const GUID *class_guid,
					 const char *enumerator, HWND parent,
					 DWORD flags)
{
	return EMPTY_DEVICE_SET;
}

WINFN(HANDLE) setup_SetupDiGetClassDevsW(const GUID *class_guid,
					 const WCHAR *enumerator, HWND parent,
					 DWORD flags)
{
	return EMPTY_DEVICE_SET;
}

WINFN(BOOL) setup_SetupDiEnumDeviceInfo(HANDLE set, DWORD index, void *info)
{
	if (set != EMPTY_DEVICE_SET) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	win_set_error(ERROR_NO_MORE_ITEMS);
	return FALSE;
}

WINFN(BOOL) setup_SetupDiGetDeviceRegistryPropertyA(HANDLE set, void *info,
						     DWORD property, DWORD *type,
						     BYTE *buffer, DWORD size,
						     DWORD *needed)
{
	if (needed)
		*needed = 0;
	win_set_error(set == EMPTY_DEVICE_SET ? ERROR_INVALID_DATA :
						ERROR_INVALID_HANDLE);
	return FALSE;
}

WINFN(BOOL) setup_SetupDiGetDeviceRegistryPropertyW(HANDLE set, void *info,
						     DWORD property, DWORD *type,
						     BYTE *buffer, DWORD size,
						     DWORD *needed)
{
	return setup_SetupDiGetDeviceRegistryPropertyA(set, info, property, type,
						       buffer, size, needed);
}

WINFN(BOOL) setup_SetupDiDestroyDeviceInfoList(HANDLE set)
{
	if (set == EMPTY_DEVICE_SET)
		return TRUE;
	win_set_error(ERROR_INVALID_HANDLE);
	return FALSE;
}

#define E(name) { #name, 0, (void *)setup_##name }

static const struct builtin_export exports[] = {
	E(SetupDiDestroyDeviceInfoList),
	E(SetupDiEnumDeviceInfo),
	E(SetupDiGetClassDevsA),
	E(SetupDiGetClassDevsW),
	E(SetupDiGetDeviceRegistryPropertyA),
	E(SetupDiGetDeviceRegistryPropertyW),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_setupapi = { "setupapi.dll", NULL, tables, 0 };
