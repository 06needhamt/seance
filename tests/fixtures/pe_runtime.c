/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Checks the runtime from the Win32 side: the TEB behind %fs, GetLastError
 * through it, the x87 control word, and imports of unimplemented functions.
 */
#include "fixture.h"

static DWORD fs_read(DWORD offset)
{
	DWORD v;
	__asm__ volatile("movl %%fs:(%1), %0" : "=r"(v) : "r"(offset));
	return v;
}

EXPORT DWORD rt_fs(DWORD offset)
{
	return fs_read(offset);
}

/* SetLastError must land in the TEB, where inline code reads it. */
EXPORT DWORD rt_last_error(DWORD value)
{
	SetLastError(value);
	if (fs_read(0x34) != value)
		return 0;
	return GetLastError();
}

EXPORT DWORD rt_thread_id(void)
{
	return GetCurrentThreadId();
}

EXPORT WORD rt_fpu_control(void)
{
	WORD cw;
	__asm__ volatile("fnstcw %0" : "=m"(cw));
	return cw;
}

/* Declared by vfw_missing.def: kernel32 has no such function. */
DWORD WINAPI SeanceTestMissing(void);

EXPORT DWORD rt_call_missing(void)
{
	/* Not a tail call, so the trap sees a return address in this DLL. */
	return SeanceTestMissing() + 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
