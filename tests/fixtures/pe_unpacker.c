/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Binds an import late, as UPX's stub binds a packed DLL's (scmpack.dll,
 * DivX520.dll): its DllMain is in a section both writable and executable,
 * and fails if GetProcAddress gives NULL for a function kernel32 doesn't
 * have. Code elsewhere probing for the same function gets NULL.
 */
#include "fixture.h"

/* Declared before the compiler's own use of it, so these flags stand. */
__asm__(".section .unp,\"xw\"\n\t.text");

static FARPROC late;

__attribute__((section(".unp")))
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		late = GetProcAddress(GetModuleHandleA("kernel32.dll"),
				      "SeanceNoSuchFunction");
		if (!late)
			return FALSE;
	}
	return TRUE;
}

/* Whether DllMain bound the function. */
EXPORT int up_bound(void)
{
	return late != NULL;
}

/* 1 if a probe from ordinary code gets NULL for it. */
EXPORT int up_probe(void)
{
	return GetProcAddress(GetModuleHandleA("kernel32.dll"),
			      "SeanceNoSuchFunction") == NULL;
}
