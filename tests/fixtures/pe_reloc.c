/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Holds absolute pointers that are only correct after relocation. Built
 * twice: pe_reloc.dll keeps its .reloc section, pe_noreloc.dll strips it.
 */
#include "fixture.h"

static int value_a = 11;
static int value_b = 31;
static const char message[] = "relocated";

static int twice(int x)
{
	return 2 * x;
}

/* volatile, so the compiler cannot fold the pointers away. */
static int *volatile values[] = { &value_a, &value_b };
static int (*volatile funcs[])(int) = { twice };
static const char *volatile strings[] = { message };

/* Returns 2 * (value_a + value_b) = 84 when every pointer is right. */
EXPORT int reloc_check(void)
{
	if (strings[0][0] != 'r' || strings[0][8] != 'd')
		return -1;
	return funcs[0](*values[0] + *values[1]);
}

EXPORT const char *reloc_message(void)
{
	return strings[0];
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
