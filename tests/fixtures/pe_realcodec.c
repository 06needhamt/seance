/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A DLL with the entry points of a RealPlayer video codec (rv203260.dll's
 * RV20toYUV420*), which is no VfW driver, DirectShow filter or DMO.
 */
#include "fixture.h"

EXPORT int RV20toYUV420Init(void *init, void **ctx)
{
	return -1;
}

EXPORT int RV20toYUV420Free(void *ctx)
{
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
