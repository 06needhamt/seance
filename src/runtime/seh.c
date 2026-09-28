/*
 * Structured exception handling: dispatch through the fs:[0] chain of
 * EXCEPTION_REGISTRATION_RECORDs.
 *
 * A hardware fault reaches us in on_fault() (crash.c), on the thread's
 * alternate signal stack with the faulting signal blocked. Handlers in the
 * chain are arbitrary DLL code (typically the compiler's own
 * _except_handler3/4, which is free to do a non-local jump into a
 * __except body once it decides to), so they cannot safely run there: a
 * jump away would abandon the kernel's signal frame with the signal still
 * blocked forever. seh_take_fault() instead rewrites the saved context so
 * that, once the signal handler returns normally (letting the kernel's own
 * sigreturn restore the signal mask, as it always does), execution resumes
 * in seh_run_chain() on the thread's own stack, in ordinary call context -
 * where a handler's jump behaves exactly like a longjmp.
 *
 * RaiseException and RtlUnwind use the same chain-walking loop, but need
 * none of this: they already run in ordinary context.
 */
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>

#include "loader/modules.h"
#include "runtime/crash.h"
#include "runtime/seh.h"
#include "runtime/teb.h"

/* What a fault becomes, until seh_run_chain() on the thread's own stack has
 * dealt with it. _Thread_local: the same thread reads it back after the
 * signal handler returns it there. */
static _Thread_local struct {
	int sig;
	EXCEPTION_RECORD rec;
	CONTEXT ctx;
} pending;

static _Thread_local int dispatching;	/* seh_run_chain() is on the stack */

static volatile LPTOP_LEVEL_EXCEPTION_FILTER top_filter;

static DWORD code_from_fpe(int si_code)
{
	switch (si_code) {
	case FPE_INTDIV: return EXCEPTION_INT_DIVIDE_BY_ZERO;
	case FPE_INTOVF: return EXCEPTION_INT_OVERFLOW;
	case FPE_FLTDIV: return EXCEPTION_FLT_DIVIDE_BY_ZERO;
	case FPE_FLTOVF: return EXCEPTION_FLT_OVERFLOW;
	case FPE_FLTUND: return EXCEPTION_FLT_UNDERFLOW;
	case FPE_FLTRES: return EXCEPTION_FLT_INEXACT_RESULT;
	case FPE_FLTINV: return EXCEPTION_FLT_INVALID_OPERATION;
	case FPE_FLTSUB: return EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
	default:         return EXCEPTION_FLT_INVALID_OPERATION;
	}
}

/* A frame pointer worth dereferencing: inside the thread's own stack, with
 * room for the fixed part of an EXCEPTION_REGISTRATION_RECORD. A corrupted
 * chain ends dispatch instead of faulting the dispatcher itself. */
static int frame_plausible(const struct win_teb *teb,
			   const PEXCEPTION_REGISTRATION_RECORD frame)
{
	uintptr_t f = (uintptr_t)frame;
	uintptr_t lo = (uintptr_t)teb->StackLimit, hi = (uintptr_t)teb->StackBase;

	return frame && (void *)frame != SEH_CHAIN_END && f >= lo &&
	       f + sizeof(*frame) <= hi && frame->Handler;
}

/*
 * A CONTEXT for the calling frame, as RtlCaptureContext would give: enough
 * for a handler evaluating a filter or running a __finally during an
 * RtlUnwind. Segment registers and the floating-point/SSE state are left
 * zero; nothing in the chain-walking contract needs them.
 */
static void __attribute__((noinline)) capture_context(CONTEXT *c)
{
	DWORD eflags;

	memset(c, 0, sizeof(*c));
	c->ContextFlags = CONTEXT_FULL;
	__asm__ __volatile__(
		"movl %%edi, %0\n\t"
		"movl %%esi, %1\n\t"
		"movl %%ebx, %2\n\t"
		"movl %%edx, %3\n\t"
		"movl %%ecx, %4\n\t"
		"movl %%eax, %5\n\t"
		"movl %%ebp, %6\n\t"
		"movl %%esp, %7\n\t"
		"pushfl\n\t"
		"popl %8\n\t"
		: "=m"(c->Edi), "=m"(c->Esi), "=m"(c->Ebx), "=m"(c->Edx),
		  "=m"(c->Ecx), "=m"(c->Eax), "=m"(c->Ebp), "=m"(c->Esp),
		  "=r"(eflags));
	c->EFlags = eflags;
	c->Eip = (DWORD)(uintptr_t)__builtin_return_address(0);
}

/*
 * Loads c's general registers and jumps to c->Eip with c->Esp, as if the
 * function that owned that context had just resumed there. Never returns.
 * %edi holds c throughout, read one field at a time so nothing it still
 * needs is clobbered before it is used; c->Edi itself, read through c, is
 * loaded last of all.
 */
static void __attribute__((noreturn, noinline)) resume_context(const CONTEXT *c)
{
	__asm__ __volatile__(
		"movl %c[esp](%%edi), %%eax\n\t"
		"movl %c[eip](%%edi), %%ecx\n\t"
		"movl %c[ebp](%%edi), %%ebx\n\t"
		"pushl %c[eflags](%%edi)\n\t"
		"popfl\n\t"
		"movl %%eax, %%esp\n\t"
		"pushl %%ecx\n\t"
		"movl %%ebx, %%ebp\n\t"
		"movl %c[esi](%%edi), %%esi\n\t"
		"movl %c[ebx](%%edi), %%ebx\n\t"
		"movl %c[edx](%%edi), %%edx\n\t"
		"movl %c[ecx](%%edi), %%ecx\n\t"
		"movl %c[eax](%%edi), %%eax\n\t"
		"movl %c[edi](%%edi), %%edi\n\t"
		"ret\n\t"
		:
		: "D"(c),
		  [esp] "i"(offsetof(CONTEXT, Esp)), [eip] "i"(offsetof(CONTEXT, Eip)),
		  [ebp] "i"(offsetof(CONTEXT, Ebp)), [eflags] "i"(offsetof(CONTEXT, EFlags)),
		  [esi] "i"(offsetof(CONTEXT, Esi)), [ebx] "i"(offsetof(CONTEXT, Ebx)),
		  [edx] "i"(offsetof(CONTEXT, Edx)), [ecx] "i"(offsetof(CONTEXT, Ecx)),
		  [eax] "i"(offsetof(CONTEXT, Eax)), [edi] "i"(offsetof(CONTEXT, Edi))
		: "memory", "cc");
	__builtin_unreachable();
}

/*
 * Calls a chain handler as Windows' own dispatcher does, restoring %esp
 * afterwards: MSVC's _except_handler3/4 (the handler behind almost every
 * frame, from msvcrt or statically linked into the DLL) are cdecl and
 * leave their arguments on the stack, while hand-written handlers are
 * often stdcall and pop them. %ebx, %esi and %edi are the handler's to
 * preserve, as the ABI says either way.
 */
__asm__(
	".text\n"
	".type seh_call_handler, @function\n"
	"seh_call_handler:\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"pushl 24(%ebp)\n\t"		/* dispatcher context */
	"pushl 20(%ebp)\n\t"		/* context */
	"pushl 16(%ebp)\n\t"		/* frame */
	"pushl 12(%ebp)\n\t"		/* record */
	"call *8(%ebp)\n\t"
	"movl %ebp, %esp\n\t"
	"popl %ebp\n\t"
	"ret\n\t"
	".size seh_call_handler, .-seh_call_handler\n"
);
EXCEPTION_DISPOSITION seh_call_handler(PEXCEPTION_ROUTINE handler,
				       EXCEPTION_RECORD *rec, void *frame,
				       CONTEXT *ctx, void *dispatch)
	__asm__("seh_call_handler") __attribute__((cdecl));

/* Walks teb's fs:[0] chain, calling each handler until one returns
 * ExceptionContinueExecution (then resumes ctx, never returning) or the
 * chain ends. Returns if nothing handled it, for the caller to report. */
static void walk_chain(struct win_teb *teb, EXCEPTION_RECORD *rec, CONTEXT *ctx)
{
	PEXCEPTION_REGISTRATION_RECORD frame = teb->ExceptionList;

	while (frame_plausible(teb, frame)) {
		PEXCEPTION_REGISTRATION_RECORD next = frame->Next;
		EXCEPTION_DISPOSITION d =
			seh_call_handler(frame->Handler, rec, frame, ctx, NULL);

		if (d == ExceptionContinueExecution) {
			/*
			 * Leaving SEH dispatch for good: resume_context()
			 * never returns here, so this cannot be left to a
			 * caller further up to reset once we're back - a
			 * later, unrelated fault on this thread would
			 * otherwise find `dispatching` stuck set and refuse
			 * to be dispatched at all.
			 */
			dispatching = 0;
			resume_context(ctx);
		}
		/* ExceptionContinueSearch, or anything else: try the next. */
		frame = next;
	}
}

static void report_unhandled_exception(const EXCEPTION_RECORD *rec)
{
	char where[160], line[256];

	mod_describe_address(rec->ExceptionAddress, where, sizeof(where));
	snprintf(line, sizeof(line), "vfw-harness: crash: unhandled exception "
		 "0x%08lX at %s\n", (unsigned long)rec->ExceptionCode, where);
	fputs(line, stderr);
	fflush(stderr);
}

/* Runs on the faulting thread's own stack once seh_take_fault() has
 * redirected it there. Reads the fault from `pending`, as its caller (the
 * signal handler) has already returned and cannot pass it any other way. */
static void __attribute__((noreturn)) seh_run_chain(void)
{
	struct win_teb *teb = teb_current();

	dispatching = 1;
	walk_chain(teb, &pending.rec, &pending.ctx);
	dispatching = 0;

	EXCEPTION_POINTERS ep = { &pending.rec, &pending.ctx };
	LPTOP_LEVEL_EXCEPTION_FILTER f = top_filter;
	if (f && f(&ep) == EXCEPTION_CONTINUE_EXECUTION)
		resume_context(&pending.ctx);

	crash_report_and_die(pending.sig, (const void *)(uintptr_t)pending.ctx.Eip,
			     (const void *)(uintptr_t)pending.rec.ExceptionInformation[1]);
}

int seh_take_fault(int sig, siginfo_t *si, void *ucontext)
{
	ucontext_t *uc = ucontext;
	struct win_teb *teb = teb_current();

	if (!teb || dispatching)
		return 0;

	greg_t *g = uc->uc_mcontext.gregs;
	memset(&pending.ctx, 0, sizeof(pending.ctx));
	pending.sig = sig;
	pending.ctx.ContextFlags = CONTEXT_FULL;
	pending.ctx.SegGs = (DWORD)g[REG_GS];
	pending.ctx.SegFs = (DWORD)g[REG_FS];
	pending.ctx.SegEs = (DWORD)g[REG_ES];
	pending.ctx.SegDs = (DWORD)g[REG_DS];
	pending.ctx.Edi = (DWORD)g[REG_EDI];
	pending.ctx.Esi = (DWORD)g[REG_ESI];
	pending.ctx.Ebx = (DWORD)g[REG_EBX];
	pending.ctx.Edx = (DWORD)g[REG_EDX];
	pending.ctx.Ecx = (DWORD)g[REG_ECX];
	pending.ctx.Eax = (DWORD)g[REG_EAX];
	pending.ctx.Ebp = (DWORD)g[REG_EBP];
	pending.ctx.Eip = (DWORD)g[REG_EIP];
	pending.ctx.SegCs = (DWORD)g[REG_CS];
	pending.ctx.EFlags = (DWORD)g[REG_EFL];
	pending.ctx.Esp = (DWORD)g[REG_UESP];
	pending.ctx.SegSs = (DWORD)g[REG_SS];

	memset(&pending.rec, 0, sizeof(pending.rec));
	pending.rec.ExceptionAddress = (void *)(uintptr_t)pending.ctx.Eip;
	switch (sig) {
	case SIGSEGV:
	case SIGBUS:
		pending.rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
		pending.rec.NumberParameters = 2;
		/* bit 1 of the page-fault error code: 1 for a write. */
		pending.rec.ExceptionInformation[0] = (g[REG_ERR] & 2) ? 1 : 0;
		pending.rec.ExceptionInformation[1] = (DWORD_PTR)(uintptr_t)si->si_addr;
		break;
	case SIGILL:
		pending.rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
		break;
	case SIGFPE:
		pending.rec.ExceptionCode = code_from_fpe(si->si_code);
		break;
	default:
		pending.rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
		break;
	}

	/* Reserve some room below the faulting stack, as the CPU itself
	 * would for its own trap frame, then hand the thread to
	 * seh_run_chain() once the signal handler returns. */
	uintptr_t stack = ((uintptr_t)g[REG_UESP] - 512) & ~(uintptr_t)15;
	g[REG_UESP] = (greg_t)stack;
	g[REG_ESP] = (greg_t)stack;
	g[REG_EIP] = (greg_t)(uintptr_t)seh_run_chain;
	return 1;
}

void WINAPI seh_RaiseException(DWORD code, DWORD flags, DWORD nargs,
			       const DWORD_PTR *args)
{
	struct win_teb *teb = teb_current();
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

	capture_context(&ctx);
	ctx.Eip = (DWORD)(uintptr_t)rec.ExceptionAddress;

	if (teb && !dispatching) {
		dispatching = 1;
		PEXCEPTION_REGISTRATION_RECORD frame = teb->ExceptionList;
		while (frame_plausible(teb, frame)) {
			PEXCEPTION_REGISTRATION_RECORD next = frame->Next;
			EXCEPTION_DISPOSITION d = seh_call_handler(
				frame->Handler, &rec, frame, &ctx, NULL);
			if (d == ExceptionContinueExecution) {
				dispatching = 0;
				return;
			}
			frame = next;
		}
		dispatching = 0;
	}

	EXCEPTION_POINTERS ep = { &rec, &ctx };
	LPTOP_LEVEL_EXCEPTION_FILTER f = top_filter;
	if (f && f(&ep) == EXCEPTION_CONTINUE_EXECUTION)
		return;

	report_unhandled_exception(&rec);
	raise(SIGABRT);
	_exit(134);
}

void WINAPI seh_RtlUnwind(PEXCEPTION_REGISTRATION_RECORD target_frame,
			  void *target_ip, EXCEPTION_RECORD *rec, void *retval)
{
	struct win_teb *teb = teb_current();
	EXCEPTION_RECORD unwind_rec;
	CONTEXT ctx;

	(void)target_ip;
	(void)retval;
	if (!teb)
		return;

	capture_context(&ctx);
	if (rec)
		unwind_rec = *rec;
	else
		memset(&unwind_rec, 0, sizeof(unwind_rec));
	unwind_rec.ExceptionFlags |= target_frame ? EXCEPTION_UNWINDING :
				    (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND);

	/*
	 * Runs each frame's handler with EXCEPTION_UNWINDING set (so a
	 * __finally along the way runs), then hands the chain to
	 * target_frame - what a compiler-generated handler that decided to
	 * run a __except body calls this for, right before it makes its own
	 * jump into that body with the registers it saved when the __try
	 * was entered; target_ip and retval exist for that caller's own
	 * bookkeeping; we do not need to interpret them.
	 */
	PEXCEPTION_REGISTRATION_RECORD frame = teb->ExceptionList;
	while (frame_plausible(teb, frame) && frame != target_frame) {
		PEXCEPTION_REGISTRATION_RECORD next = frame->Next;
		seh_call_handler(frame->Handler, &unwind_rec, frame, &ctx,
				 target_frame);
		frame = next;
	}
	teb->ExceptionList = target_frame ? target_frame : SEH_CHAIN_END;

	/*
	 * A compiler exception handler calls RtlUnwind immediately before it
	 * restores the protected frame and jumps into the selected __except
	 * body.  That non-local transfer means neither walk_chain() nor its
	 * caller gets control back to clear this guard.  End the dispatch here,
	 * once the unwind has finished, so a later fault in the same thread can
	 * start a fresh dispatch.
	 */
	dispatching = 0;
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
