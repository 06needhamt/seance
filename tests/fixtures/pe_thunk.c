/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A Windows 9x DLL built on flat thunks, as Mvicod32.dll is: its DllMain
 * connects to its 16-bit half. It is refused before DllMain runs; were it
 * run, the unimplemented ThunkConnect32 would be reported instead.
 */
#include "fixture.h"

BOOL WINAPI ThunkConnect32(void *data, LPSTR dll16, LPSTR dll32,
			   HINSTANCE inst, DWORD reason, DWORD unused);

static char thunk_data[64];

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH || reason == DLL_PROCESS_DETACH)
		return ThunkConnect32(thunk_data, "PE_TH16.DLL", "PE_THUNK.DLL",
				      inst, reason, 0);
	return TRUE;
}
