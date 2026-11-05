/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Structured exception handling on x86-64, so far without handlers.
 *
 * Win64 exceptions are table-based: there is no fs:[0] chain, and a
 * dispatcher finds each frame's handler by unwinding the stack with the
 * modules' .pdata and .xdata. Until that unwinder exists (phase 4 of
 * docs/X64_PLAN.md), an exception goes straight to the top-level filter
 * (SetUnhandledExceptionFilter) and is otherwise reported as unhandled,
 * as it would be on Windows with no __try around it.
 */
#if defined(__x86_64__)

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "common/log.h"
#include "loader/modules.h"
#include "runtime/crash.h"
#include "runtime/seh.h"
#include "runtime/teb.h"

static volatile LPTOP_LEVEL_EXCEPTION_FILTER top_filter;

static void report_unhandled_exception(const EXCEPTION_RECORD *rec)
{
	char where[160], line[256];

	mod_describe_address(rec->ExceptionAddress, where, sizeof(where));
	snprintf(line, sizeof(line), "seance: crash: unhandled exception "
		 "0x%08lX at %s\n", (unsigned long)rec->ExceptionCode, where);
	fputs(line, stderr);
	fflush(stderr);
}

/*
 * A fault goes to crash.c's report: with no handlers to run there is
 * nothing to gain from leaving the signal handler first.
 */
int seh_take_fault(int sig, siginfo_t *si, void *ucontext)
{
	return 0;
}

void WINAPI seh_RaiseException(DWORD code, DWORD flags, DWORD nargs,
			       const DWORD_PTR *args)
{
	EXCEPTION_RECORD rec;
	CONTEXT ctx;

	memset(&rec, 0, sizeof(rec));
	rec.ExceptionCode = code;
	rec.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
	rec.ExceptionAddress = __builtin_return_address(0);
	rec.NumberParameters = nargs > EXCEPTION_MAXIMUM_PARAMETERS ?
				EXCEPTION_MAXIMUM_PARAMETERS : nargs;
	for (DWORD i = 0; i < rec.NumberParameters; i++)
		rec.ExceptionInformation[i] = args[i];

	memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_CONTROL;
	ctx.Rip = (uintptr_t)rec.ExceptionAddress;
	ctx.Rsp = (uintptr_t)__builtin_frame_address(0);
	TRACE("exception 0x%08lX (raised), no x86-64 handler dispatch yet",
	      (unsigned long)code);

	EXCEPTION_POINTERS ep = { &rec, &ctx };
	LPTOP_LEVEL_EXCEPTION_FILTER f = top_filter;
	if (f && f(&ep) == EXCEPTION_CONTINUE_EXECUTION)
		return;

	report_unhandled_exception(&rec);
	crash_exit(SIGABRT);
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
seh_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
	LPTOP_LEVEL_EXCEPTION_FILTER prev = top_filter;
	top_filter = filter;
	return prev;
}

LONG WINAPI seh_UnhandledExceptionFilter(EXCEPTION_POINTERS *ep)
{
	LPTOP_LEVEL_EXCEPTION_FILTER f = top_filter;
	return f ? f(ep) : EXCEPTION_CONTINUE_SEARCH;
}

#endif /* __x86_64__ */
