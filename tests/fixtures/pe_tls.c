/*
 * Supplies its own TLS directory (normally from the CRT's tlssup.c) with one
 * callback, so the loader's TLS index and callback handling can be checked.
 */
#include "fixture.h"

static DWORD callback_reasons;	/* bit per reason seen */
static DWORD entry_saw_callback;

/*
 * Read-only on purpose: the loader must store the index before it applies
 * page protections. volatile, so reads see the value the loader wrote.
 */
const volatile ULONG _tls_index = 0xdeadbeef;

static void WINAPI tls_callback(PVOID dll, DWORD reason, PVOID reserved)
{
	callback_reasons |= 1u << reason;
}

__attribute__((section(".tls"))) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;

__attribute__((section(".CRT$XLB"), used))
static const PIMAGE_TLS_CALLBACK tls_callbacks[] = { tls_callback, 0 };

const IMAGE_TLS_DIRECTORY _tls_used = {
	(DWORD)&_tls_start, (DWORD)&_tls_end, (DWORD)&_tls_index,
	(DWORD)tls_callbacks, 0, 0
};

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
	return TRUE;
}
