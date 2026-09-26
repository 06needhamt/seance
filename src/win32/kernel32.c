/*
 * kernel32.dll
 */
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "common/log.h"
#include "runtime/builtin.h"
#include "runtime/teb.h"

#define K32(ret) static ret STUB_WINAPI

/* Errors. */

K32(DWORD) k32_GetLastError(void)
{
	struct win_teb *teb = teb_current();
	return teb ? teb->LastErrorValue : 0;
}

K32(void) k32_SetLastError(DWORD err)
{
	struct win_teb *teb = teb_current();
	if (teb)
		teb->LastErrorValue = err;
}

/* Processes and threads. */

K32(DWORD) k32_GetCurrentProcessId(void)
{
	return getpid();
}

K32(DWORD) k32_GetCurrentThreadId(void)
{
	return syscall(SYS_gettid);
}

/* Time. */

K32(DWORD) k32_GetTickCount(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (DWORD)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000);
}

K32(void) k32_Sleep(DWORD ms)
{
	TRACE("Sleep(%u)", ms);
	if (!ms) {
		sched_yield();
		return;
	}
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts))
		;
}

#define E(name) { #name, 0, (void *)k32_##name }

/* Sorted by name. */
static const struct builtin_export exports[] = {
	E(GetCurrentProcessId),
	E(GetCurrentThreadId),
	E(GetLastError),
	E(GetTickCount),
	E(SetLastError),
	E(Sleep),
};

const struct builtin_dll builtin_kernel32 = {
	"kernel32.dll", NULL, BUILTIN_EXPORTS(exports)
};
