/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Supplies its own TLS directory (normally from the CRT's tlssup.c) with one
 * callback and some implicit TLS data, so the loader's TLS index, callback
 * handling, per-thread TLS blocks and thread notifications can be checked.
 */
#include "fixture.h"

static DWORD callback_reasons;	/* bit per reason seen */
static DWORD entry_saw_callback;
static volatile LONG callback_count[4], entry_count[4];	/* by reason */

/*
 * Read-only on purpose: the loader must store the index before it applies
 * page protections. volatile, so reads see the value the loader wrote.
 */
const volatile ULONG _tls_index = 0xdeadbeef;

static void WINAPI tls_callback(PVOID dll, DWORD reason, PVOID reserved)
{
	callback_reasons |= 1u << reason;
	if (reason < 4)
		__atomic_add_fetch(&callback_count[reason], 1, __ATOMIC_SEQ_CST);
}

__attribute__((section(".tls"))) char _tls_start = 0;
/* A __declspec(thread) variable: each thread gets its own copy. */
__attribute__((section(".tls$BBB"))) int tls_value = 1234;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;

#define ZERO_FILL 16

__attribute__((section(".CRT$XLB"), used))
static const PIMAGE_TLS_CALLBACK tls_callbacks[] = { tls_callback, 0 };

const IMAGE_TLS_DIRECTORY _tls_used = {
	(DWORD)&_tls_start, (DWORD)&_tls_end, (DWORD)&_tls_index,
	(DWORD)tls_callbacks, ZERO_FILL, 0
};

/* This thread's TLS block, the way MSVC code finds it. */
static BYTE *tls_block(void)
{
	BYTE **array;

	__asm__ volatile("movl %%fs:0x2c, %0" : "=r"(array));
	return array ? array[_tls_index] : 0;
}

/* This thread's tls_value, or -1 if it has no TLS block. */
EXPORT int tls_read(void)
{
	BYTE *b = tls_block();
	return b ? *(int *)(b + ((BYTE *)&tls_value - (BYTE *)&_tls_start)) : -1;
}

EXPORT void tls_write(int v)
{
	BYTE *b = tls_block();
	if (b)
		*(int *)(b + ((BYTE *)&tls_value - (BYTE *)&_tls_start)) = v;
}

/* Whether this thread's block holds the template (bar tls_value) and zeros. */
EXPORT int tls_block_ok(void)
{
	BYTE *b = tls_block();
	DWORD size = (DWORD)(&_tls_end - &_tls_start), i;
	DWORD off = (DWORD)((char *)&tls_value - &_tls_start);

	if (!b)
		return 0;
	for (i = 0; i < size; i++)
		if ((i < off || i >= off + sizeof(int)) &&
		    b[i] != (BYTE)(&_tls_start)[i])
			return 0;
	for (i = size; i < size + ZERO_FILL; i++)
		if (b[i])
			return 0;
	return 1;
}

/* TLS callback and DllMain calls so far for a reason (DLL_THREAD_*...). */
EXPORT LONG tls_callback_count(DWORD reason)
{
	return reason < 4 ? callback_count[reason] : -1;
}

EXPORT LONG tls_entry_count(DWORD reason)
{
	return reason < 4 ? entry_count[reason] : -1;
}

EXPORT DWORD tls_reasons(void)
{
	return callback_reasons;
}

EXPORT ULONG tls_index(void)
{
	return _tls_index;
}

/* Non-zero if the PROCESS_ATTACH callback ran before DllMain. */
EXPORT DWORD tls_callback_first(void)
{
	return entry_saw_callback;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		entry_saw_callback = callback_reasons & (1u << DLL_PROCESS_ATTACH);
	if (reason < 4)
		__atomic_add_fetch(&entry_count[reason], 1, __ATOMIC_SEQ_CST);
	return TRUE;
}
