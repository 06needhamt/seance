/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FOURCCs kept where codecs keep them, for the search of a DLL for its
 * handlers: one compared with ('CMPA'), one stored by its code ('MOVB'),
 * a string that begins with one ('STRC codec'), and, in its version
 * resource, a word that is one ("seance VERD fixture").
 */
#include "fixture.h"

static const char description[] = "STRC codec";

EXPORT const char *fourccs_description(void)
{
	return description;
}

EXPORT int fourccs_check(DWORD handler)
{
	/* Kept in memory, it is stored by a mov, not compared with as such. */
	volatile DWORD stored = 'B' << 24 | 'V' << 16 | 'O' << 8 | 'M';

	return handler == ('A' << 24 | 'P' << 16 | 'M' << 8 | 'C') ? 1 :
	       handler == stored ? 2 : 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
