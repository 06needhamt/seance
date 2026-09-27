/*
 * Structured exception handling: the i386 fs:[0] chain of
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
void WINAPI seh_RtlUnwind(PEXCEPTION_REGISTRATION_RECORD target_frame,
			  void *target_ip, EXCEPTION_RECORD *rec,
			  void *retval);
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
seh_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
LONG WINAPI seh_UnhandledExceptionFilter(EXCEPTION_POINTERS *ep);

#endif /* VFW_RUNTIME_SEH_H */
