/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Delay-loads, from pe_basic.dll, basic_add by name and by ordinal and a
 * function it doesn't have, and from zz_nosuch.dll, which doesn't exist,
 * one more. The harness binds delay imports when the DLL loads, so the
 * delay-load helper (here: what the IAT starts out pointing to) must never
 * run; the two that can't be had get traps.
 *
 * GNU ld doesn't build the delay import directory, so the tables are
 * written here in assembly (.rva gives image-relative addresses) and
 * exported as delay_directory; the test points the PE header's delay
 * import directory at them before loading the DLL.
 */
#include "fixture.h"

typedef int (*add_fn)(int, int);
typedef int (*void_fn)(void);

extern FARPROC delay_iat_basic[3], delay_iat_nosuch[1];
extern HMODULE delay_hmod_basic, delay_hmod_nosuch;

static LONG helper_calls;

/* What each IAT slot holds until bound: the lazy path, which must not run. */
int delay_unbound(void)
{
	helper_calls++;
	return -1;
}

__asm__(
	".data\n"
	".balign 4\n"
	".globl _delay_directory\n"
	"_delay_directory:\n"
	/* pe_basic.dll */
	"	.long 1\n"			/* Attributes: RVA-based */
	"	.rva name_basic\n"
	"	.rva _delay_hmod_basic\n"
	"	.rva _delay_iat_basic\n"
	"	.rva int_basic\n"
	"	.long 0, 0, 0\n"
	/* zz_nosuch.dll */
	"	.long 1\n"
	"	.rva name_nosuch\n"
	"	.rva _delay_hmod_nosuch\n"
	"	.rva _delay_iat_nosuch\n"
	"	.rva int_nosuch\n"
	"	.long 0, 0, 0\n"
	/* the end */
	"	.long 0, 0, 0, 0, 0, 0, 0, 0\n"
	".globl _delay_hmod_basic\n"
	"_delay_hmod_basic: .long 0\n"
	".globl _delay_hmod_nosuch\n"
	"_delay_hmod_nosuch: .long 0\n"
	".globl _delay_iat_basic\n"
	"_delay_iat_basic:\n"
	"	.long _delay_unbound, _delay_unbound, _delay_unbound, 0\n"
	".globl _delay_iat_nosuch\n"
	"_delay_iat_nosuch:\n"
	"	.long _delay_unbound, 0\n"
	"int_basic:\n"
	"	.rva hint_add\n"
	"	.long 0x80000001\n"		/* basic_add, by ordinal */
	"	.rva hint_gone\n"
	"	.long 0\n"
	"int_nosuch:\n"
	"	.rva hint_nosuch\n"
	"	.long 0\n"
	"hint_add: .short 0\n .asciz \"basic_add\"\n"
	".balign 2\n"
	"hint_gone: .short 0\n .asciz \"basic_gone\"\n"
	".balign 2\n"
	"hint_nosuch: .short 0\n .asciz \"nosuch_function\"\n"
	"name_basic: .asciz \"PE_BASIC.DLL\"\n"
	"name_nosuch: .asciz \"zz_nosuch.dll\"\n"
	/* Exported as data, so the test can find it in the file. */
	".section .drectve\n"
	"	.ascii \" -export:delay_directory,data\"\n"
	".text\n");

EXPORT int delay_add(int a, int b)
{
	return ((add_fn)delay_iat_basic[0])(a, b);
}

EXPORT int delay_add_by_ordinal(int a, int b)
{
	return ((add_fn)delay_iat_basic[1])(a, b);
}

/* Not tail calls, so a trap names this DLL as the caller. */
EXPORT int delay_gone(void)
{
	volatile int r = ((void_fn)delay_iat_basic[2])();
	return r;
}

EXPORT int delay_nosuch(void)
{
	volatile int r = ((void_fn)delay_iat_nosuch[0])();
	return r;
}

EXPORT HMODULE delay_module(int which)
{
	return which ? delay_hmod_nosuch : delay_hmod_basic;
}

EXPORT LONG delay_helper_calls(void)
{
	return helper_calls;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
