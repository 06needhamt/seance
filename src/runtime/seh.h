/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Structured exception handling (seh.c; on x86-64, seh_x64.c): the i386
 * fs:[0] chain of
 * EXCEPTION_REGISTRATION_RECORDs that __try/__except and __finally compile
 * to, and the three ways code drives it (a hardware fault, RaiseException,
 * RtlUnwind).
 *
 * A handler in the chain is an arbitrary function pointer following the
 * raw ABI (PEXCEPTION_ROUTINE): what runs the compiler's own filter and
 * __except/__finally logic (e.g. msvcrt's _except_handler3) is not our
 * concern here. Our job is only what an OS normally does: build the
 * EXCEPTION_RECORD/CONTEXT, call each handler in turn, and act on what it
 * returns.
 */
#ifndef VFW_RUNTIME_SEH_H
#define VFW_RUNTIME_SEH_H

#include <signal.h>

#include "win32.h"

/*
 * Called from the SIGSEGV/SIGBUS/SIGILL/SIGFPE handler, on its alternate
 * stack, before it reports anything. If the faulting thread has a TEB
 * (so it may be running DLL code with SEH frames), this rewrites *ucontext
 * so that, once the signal handler returns, execution resumes in a
 * trampoline on the thread's own stack with its signal mask restored -
 * where the fs:[0] chain is walked in ordinary (non-signal) context, so a
 * handler that jumps away (as one that decides to run a __except body
 * does) behaves exactly like a longjmp. Returns 1 if it did this (the
 * caller must not touch the signal any further) or 0 if there is no TEB to
 * dispatch through, in which case the caller reports the crash as usual.
 */
int seh_take_fault(int sig, siginfo_t *si, void *ucontext);

/* RaiseException, RtlUnwind, and Set/UnhandledExceptionFilter, exported by
 * kernel32.dll (RtlUnwind is also exported, as a forward, by ntdll.dll). */
void WINAPI seh_RaiseException(DWORD code, DWORD flags, DWORD nargs,
			       const DWORD_PTR *args);
#if !defined(__x86_64__)	/* the fs:[0] chain's */
void WINAPI seh_RtlUnwind(PEXCEPTION_REGISTRATION_RECORD target_frame,
			  void *target_ip, EXCEPTION_RECORD *rec,
			  void *retval);
#endif
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
seh_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
LONG WINAPI seh_UnhandledExceptionFilter(EXCEPTION_POINTERS *ep);

#if defined(__x86_64__)
/* The x86-64 unwinder (unwind_x64.c), exported by ntdll.dll and kernel32.dll. */
PRUNTIME_FUNCTION WINAPI seh_RtlLookupFunctionEntry(ULONGLONG pc,
						    ULONGLONG *image_base,
						    void *history);
PEXCEPTION_ROUTINE WINAPI
seh_RtlVirtualUnwind(DWORD type, ULONGLONG base, ULONGLONG pc,
		     PRUNTIME_FUNCTION fe, CONTEXT *ctx, void **handler_data,
		     ULONGLONG *frame_ret, KNONVOLATILE_CONTEXT_POINTERS *ptrs);
BOOLEAN WINAPI seh_RtlAddFunctionTable(PRUNTIME_FUNCTION entries, DWORD count,
				       ULONGLONG base);
BOOLEAN WINAPI seh_RtlDeleteFunctionTable(PRUNTIME_FUNCTION entries);
void WINAPI seh_RtlCaptureContext(CONTEXT *ctx);
void WINAPI seh_RtlRestoreContext(CONTEXT *ctx, EXCEPTION_RECORD *rec);
void WINAPI seh_RtlUnwindEx(void *target_frame, void *target_ip,
			    EXCEPTION_RECORD *rec, void *retval,
			    CONTEXT *ctx, void *history);
void WINAPI seh_RtlUnwind(void *target_frame, void *target_ip,
			  EXCEPTION_RECORD *rec, void *retval);
PVOID WINAPI seh_RtlPcToFileHeader(PVOID pc, PVOID *base);

/* For the C++ runtime (crt_cxx_x64.c). */
struct seh_link {
	struct seh_link *next;
	uintptr_t marker;
	CONTEXT parent;		/* where an exception leaving the host frame goes on */
	void (*unwound)(void *arg, EXCEPTION_RECORD *unwinding);
	void *arg;
};
void seh_link_push(struct seh_link *l);
void seh_link_pop(struct seh_link *l);
int seh_unwind_run(void *target, void *target_ip, EXCEPTION_RECORD *rec,
		   const CONTEXT *start, CONTEXT *out);
int seh_guarded(void (*fn)(void *), void *arg);
void seh_resume(CONTEXT *c) __attribute__((noreturn));
void seh_dispatch(EXCEPTION_RECORD *rec, CONTEXT *ctx)
	__attribute__((noreturn));
EXCEPTION_DISPOSITION WINAPI
seh_C_specific_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx,
		       DISPATCHER_CONTEXT *dc);
#endif

#endif /* VFW_RUNTIME_SEH_H */
