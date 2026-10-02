/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Imports from pe_basic.dll and from zz_nosuch.dll, which does not exist, so
 * loading it must fail and release pe_basic again.
 */
#include "fixture.h"

int basic_add(int a, int b);
int nosuch_function(void);

EXPORT int missing_call(void)
{
	return basic_add(1, 2) + nosuch_function();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
