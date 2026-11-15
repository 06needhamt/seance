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

/* Whether the instruction at pc is one only the kernel may run. */
static inline int seh_privileged_instr(const BYTE *pc)
{
	for (int i = 0; i < 15; i++, pc++) {
		switch (*pc) {
		case 0x26: case 0x2e: case 0x36: case 0x3e: case 0x64:
		case 0x65: case 0x66: case 0x67: case 0xf2: case 0xf3:
			continue;	/* prefixes */
		case 0xf4:		/* hlt */
		case 0xfa:		/* cli */
		case 0xfb:		/* sti */
			return 1;
		case 0x0f:
			switch (pc[1]) {
			case 0x06:	/* clts */
			case 0x08:	/* invd */
			case 0x09:	/* wbinvd */
			case 0x20: case 0x21: case 0x22: case 0x23: /* mov cr, dr */
			case 0x30:	/* wrmsr */
			case 0x32:	/* rdmsr */
				return 1;
			case 0x00:	/* lldt, ltr */
				return ((pc[2] >> 3) & 7) == 2 || ((pc[2] >> 3) & 7) == 3;
			case 0x01: {	/* lgdt, lidt, lmsw, invlpg */
				int reg = (pc[2] >> 3) & 7;
				if (reg == 6)
					return 1;
				return (pc[2] & 0xc0) != 0xc0 &&
				       (reg == 2 || reg == 3 || reg == 7);
			}
			}
			return 0;
		default:
			return 0;
		}
	}
	return 0;
}

/*
 * The exception Windows reports for a hardware fault, from the signal and
 * the CPU's trap number and error code (as Wine has them). *pc and *eflags
 * are the context's: a breakpoint is reported at itself, and a single step
 * with the trap flag clear.
 */
static inline void seh_fault_record(EXCEPTION_RECORD *rec, int sig,
				    const siginfo_t *si, unsigned long trapno,
				    unsigned long err, ULONG_PTR *pc,
				    DWORD *eflags)
{
	rec->ExceptionAddress = (void *)*pc;
	switch (sig) {
	case SIGSEGV:
	case SIGBUS:
		rec->ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
		rec->NumberParameters = 2;
		if (trapno == 13) {
			/*
			 * A general-protection fault: an instruction only the
			 * kernel may run, or one it refuses ("int 1", a
			 * non-canonical address), which Windows reports as
			 * reading -1.
			 */
			if (!err && seh_privileged_instr((const BYTE *)*pc)) {
				rec->ExceptionCode = EXCEPTION_PRIV_INSTRUCTION;
				rec->NumberParameters = 0;
			} else {
				rec->ExceptionInformation[1] = (ULONG_PTR)-1;
			}
			break;
		}
		/* bit 1 of the page-fault error code: 1 for a write. */
		rec->ExceptionInformation[0] = (err & 2) ? 1 : 0;
		rec->ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
		break;
	case SIGILL:
		rec->ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
		break;
	case SIGTRAP:
		if (trapno == 1 || (trapno != 3 && si->si_code == TRAP_TRACE)) {
			/* The trap flag, or icebp (F1): past the instruction. */
			rec->ExceptionCode = EXCEPTION_SINGLE_STEP;
			*eflags &= ~0x100u;
		} else {
			/*
			 * int3, which Windows reports at the breakpoint itself
			 * (a handler steps over it with Eip++); "int 3" (CD 03)
			 * gets the same, one byte back, as there.
			 */
			rec->ExceptionCode = EXCEPTION_BREAKPOINT;
			rec->NumberParameters = 1;
			(*pc)--;
			rec->ExceptionAddress = (void *)*pc;
		}
		break;
	default:
		rec->ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
		break;
	}
}

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

/* Drops what the C++ runtime cached about functions of an unloaded image. */
void cxx_forget_image(const void *base, size_t len);
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
