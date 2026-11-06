/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Structured exception handling on x86-64.
 *
 * Win64 exceptions are table-based: there is no fs:[0] chain. A dispatcher
 * finds each frame's language handler by unwinding the stack with the
 * modules' .pdata/.xdata (unwind_x64.c), calling it with the exception, the
 * frame, the context of the exception and a DISPATCHER_CONTEXT. The
 * handler most code has is __C_specific_handler, which walks the
 * function's scope table: filters in the search phase, __finally blocks
 * and the jump to a chosen __except in the unwind phase (RtlUnwindEx).
 *
 * A hardware fault reaches us in on_fault() (crash.c) on the alternate
 * signal stack. As on i386 (seh.c) the dispatch itself must not run
 * there, since a handler may jump away: seh_take_fault() rewrites the
 * saved context so the signal handler returns into seh_run_fault() on the
 * thread's own stack, with the signal mask restored. RaiseException and
 * RtlUnwindEx are entered from DLL code already.
 *
 * What can't be unwound: a frame in code the harness itself runs (a
 * built-in DLL, a trampoline) has no unwind data, so the walk ends there.
 */
#if defined(__x86_64__)

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>

#include "common/log.h"
#include "loader/modules.h"
#include "runtime/crash.h"
#include "runtime/seh.h"
#include "runtime/teb.h"

#define OFF(f) __builtin_offsetof(CONTEXT, f)
_Static_assert(OFF(ContextFlags) == 0x30 && OFF(MxCsr) == 0x34, "CONTEXT");
_Static_assert(OFF(SegCs) == 0x38 && OFF(SegSs) == 0x42, "CONTEXT");
_Static_assert(OFF(EFlags) == 0x44, "CONTEXT.EFlags");
_Static_assert(OFF(Rax) == 0x78 && OFF(Rcx) == 0x80 && OFF(Rdx) == 0x88 &&
	       OFF(Rbx) == 0x90 && OFF(Rsp) == 0x98 && OFF(Rbp) == 0xa0 &&
	       OFF(Rsi) == 0xa8 && OFF(Rdi) == 0xb0, "CONTEXT regs");
_Static_assert(OFF(R8) == 0xb8 && OFF(R10) == 0xc8 && OFF(R11) == 0xd0 &&
	       OFF(R15) == 0xf0, "CONTEXT r8-r15");

#define STATUS_UNWIND 0xC0000027

/*
 * RtlCaptureContext(ctx): the caller's registers, as they will be once it
 * returns (rsp past the return address, rip the return address), the
 * segment registers, flags and the FXSAVE state.
 *
 * vfw_restore_context(ctx): the reverse, never returning. It builds a
 * flags/return-address pair below the target stack and ends in popfq; ret,
 * so no register is needed for the jump; what it writes lies in space the
 * target stack has already given up.
 */
__asm__(
	".text\n"
	".globl seh_RtlCaptureContext\n"
	".type seh_RtlCaptureContext, @function\n"
	"seh_RtlCaptureContext:\n"
	"	pushfq\n"
	"	movq %rax, 0x78(%rcx)\n"
	"	movq %rcx, 0x80(%rcx)\n"
	"	movq %rdx, 0x88(%rcx)\n"
	"	movq %rbx, 0x90(%rcx)\n"
	"	leaq 16(%rsp), %rax\n"
	"	movq %rax, 0x98(%rcx)\n"
	"	movq %rbp, 0xa0(%rcx)\n"
	"	movq %rsi, 0xa8(%rcx)\n"
	"	movq %rdi, 0xb0(%rcx)\n"
	"	movq %r8, 0xb8(%rcx)\n"
	"	movq %r9, 0xc0(%rcx)\n"
	"	movq %r10, 0xc8(%rcx)\n"
	"	movq %r11, 0xd0(%rcx)\n"
	"	movq %r12, 0xd8(%rcx)\n"
	"	movq %r13, 0xe0(%rcx)\n"
	"	movq %r14, 0xe8(%rcx)\n"
	"	movq %r15, 0xf0(%rcx)\n"
	"	movq 8(%rsp), %rax\n"
	"	movq %rax, 0xf8(%rcx)\n"
	"	popq %rax\n"
	"	movl %eax, 0x44(%rcx)\n"
	"	movw %cs, 0x38(%rcx)\n"
	"	movw %ds, 0x3a(%rcx)\n"
	"	movw %es, 0x3c(%rcx)\n"
	"	movw %fs, 0x3e(%rcx)\n"
	"	movw %gs, 0x40(%rcx)\n"
	"	movw %ss, 0x42(%rcx)\n"
	"	stmxcsr 0x34(%rcx)\n"
	"	fxsave 0x100(%rcx)\n"
	"	movl $0x10000f, 0x30(%rcx)\n"
	"	ret\n"
	".size seh_RtlCaptureContext, .-seh_RtlCaptureContext\n"

	".globl vfw_restore_context\n"
	".type vfw_restore_context, @function\n"
	"vfw_restore_context:\n"
	"	movq 0x98(%rcx), %r10\n"
	"	subq $16, %r10\n"
	"	movl 0x44(%rcx), %r11d\n"
	"	movq %r11, (%r10)\n"
	"	movq 0xf8(%rcx), %r11\n"
	"	movq %r11, 8(%r10)\n"
	"	testl $8, 0x30(%rcx)\n"
	"	jz 1f\n"
	"	fxrstor 0x100(%rcx)\n"
	"	ldmxcsr 0x34(%rcx)\n"
	"1:	movq 0x88(%rcx), %rdx\n"
	"	movq 0x90(%rcx), %rbx\n"
	"	movq 0xa0(%rcx), %rbp\n"
	"	movq 0xa8(%rcx), %rsi\n"
	"	movq 0xb0(%rcx), %rdi\n"
	"	movq 0xb8(%rcx), %r8\n"
	"	movq 0xc0(%rcx), %r9\n"
	"	movq 0xd0(%rcx), %r11\n"
	"	movq 0xd8(%rcx), %r12\n"
	"	movq 0xe0(%rcx), %r13\n"
	"	movq 0xe8(%rcx), %r14\n"
	"	movq 0xf0(%rcx), %r15\n"
	"	movq %r10, %rsp\n"
	"	movq 0xc8(%rcx), %r10\n"
	"	movq 0x78(%rcx), %rax\n"
	"	movq 0x80(%rcx), %rcx\n"
	"	popfq\n"
	"	ret\n"
	".size vfw_restore_context, .-vfw_restore_context\n"

	/*
	 * RaiseException(code, flags, nargs, args), and RtlUnwindEx(target
	 * frame, target ip, record, return value, ...) with RtlUnwind: each
	 * captures its caller's context, then continues in C on the same
	 * stack (the System V functions below). Neither ever returns to
	 * here: the C side ends in vfw_restore_context, which also gives
	 * rsi, rdi and xmm6-15 back to the caller, as the ms ABI wants.
	 */
	".globl seh_RaiseException\n"
	".type seh_RaiseException, @function\n"
	"seh_RaiseException:\n"
	"	subq $0x4d8, %rsp\n"
	"	movq %rcx, %r10\n"
	"	movq %rdx, %r11\n"
	"	movq %rsp, %rcx\n"
	"	call seh_RtlCaptureContext\n"
	"	leaq 0x4e0(%rsp), %rax\n"
	"	movq %rax, 0x98(%rsp)\n"
	"	movq 0x4d8(%rsp), %rax\n"
	"	movq %rax, 0xf8(%rsp)\n"
	"	movl %r10d, %edi\n"
	"	movl %r11d, %esi\n"
	"	movl %r8d, %edx\n"
	"	movq %r9, %rcx\n"
	"	movq %rsp, %r8\n"
	"	call vfw_raise_c\n"
	"	ud2\n"
	".size seh_RaiseException, .-seh_RaiseException\n"

	".globl seh_RtlUnwindEx\n"
	".globl seh_RtlUnwind\n"
	".type seh_RtlUnwindEx, @function\n"
	"seh_RtlUnwindEx:\n"
	"seh_RtlUnwind:\n"
	"	subq $0x4d8, %rsp\n"
	"	movq %rcx, %r10\n"
	"	movq %rsp, %rcx\n"
	"	call seh_RtlCaptureContext\n"
	"	leaq 0x4e0(%rsp), %rax\n"
	"	movq %rax, 0x98(%rsp)\n"
	"	movq 0x4d8(%rsp), %rax\n"
	"	movq %rax, 0xf8(%rsp)\n"
	"	movq %r10, %rdi\n"
	"	movq %rdx, %rsi\n"
	"	movq %r8, %rdx\n"
	"	movq %r9, %rcx\n"
	"	movq %rsp, %r8\n"
	"	call vfw_unwind_c\n"
	"	ud2\n"
	".size seh_RtlUnwindEx, .-seh_RtlUnwindEx\n");

extern void WINAPI vfw_restore_context(const CONTEXT *c)
	__attribute__((noreturn));

/*
 * The dispatches under way on this thread, outermost first, each marked by
 * where its frame is on the stack. RtlUnwindEx, called by a handler from
 * inside one, starts from that dispatch's original context (the handler's
 * own frames hold no unwind data we could walk); and it ends every
 * dispatch below the frame it resumes in, which are abandoned. One that
 * is left behind by a jump is dropped once the stack is back above it.
 */
#define MAX_DISPATCH 16
static _Thread_local struct {
	uintptr_t marker;
	const CONTEXT *ctx;
} live[MAX_DISPATCH];
static _Thread_local int nlive;

static void drop_below(uintptr_t sp)
{
	while (nlive && live[nlive - 1].marker < sp)
		nlive--;
}

static int dispatch_begin(uintptr_t marker, const CONTEXT *ctx)
{
	drop_below(marker);
	if (nlive == MAX_DISPATCH)
		return -1;
	live[nlive].marker = marker;
	live[nlive].ctx = ctx;
	return nlive++;
}

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

/* With VFW_TRACE: each exception as it is dispatched. */
static void trace_exception(const EXCEPTION_RECORD *rec, const char *how)
{
	char where[160];

	if (!vfw_trace_enabled)
		return;
	if (!mod_describe_address_try(rec->ExceptionAddress, where,
				      sizeof(where)))
		snprintf(where, sizeof(where), "%p", rec->ExceptionAddress);
	if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
	    rec->NumberParameters >= 2)
		TRACE("exception 0x%08lX (%s) at %s: %s %#lx",
		      (unsigned long)rec->ExceptionCode, how, where,
		      rec->ExceptionInformation[0] ? "writing" : "reading",
		      (unsigned long)rec->ExceptionInformation[1]);
	else
		TRACE("exception 0x%08lX (%s) at %s",
		      (unsigned long)rec->ExceptionCode, how, where);
}

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
 * Host frames in the middle of a walk. A C++ catch block is called from
 * our runtime, which has no unwind data; its caller registers a link so
 * that an exception leaving the block continues in the context of the
 * function whose catch it is. `unwound` is called when an unwind crosses
 * the link (the block is being left by an exception).
 */
static _Thread_local struct seh_link *links;

void seh_link_push(struct seh_link *l)
{
	l->marker = (uintptr_t)l;
	l->next = links;
	links = l;
}

void seh_link_pop(struct seh_link *l)
{
	for (struct seh_link **p = &links; *p; p = &(*p)->next)
		if (*p == l) {
			*p = l->next;
			return;
		}
}

static void links_prune(uintptr_t sp)
{
	while (links && links->marker < sp)
		links = links->next;
}

/* c is in host code: continue in the parent of the innermost link above it. */
static int host_link(CONTEXT *c, EXCEPTION_RECORD *unwinding)
{
	for (struct seh_link *l = links; l; l = l->next) {
		if (l->marker < c->Rsp)
			continue;
		*c = l->parent;
		if (unwinding && l->unwound)
			l->unwound(l->arg, unwinding);
		return 1;
	}
	return 0;
}

/*
 * seh_guarded: fn(arg) as __try { fn(arg); } __except (an access violation
 * in host code) {}. Host code has no unwind data to find a handler by, so
 * dispatch looks here first for a fault whose pc is outside every module.
 */
#include <setjmp.h>

static _Thread_local struct seh_guard {
	struct seh_guard *next;
	jmp_buf jump;
} *guards;

int __attribute__((no_sanitize_address))
seh_guarded(void (*fn)(void *), void *arg)
{
	struct seh_guard g;

	g.next = guards;
	if (setjmp(g.jump)) {
		guards = g.next;
		return 1;
	}
	guards = &g;
	fn(arg);
	guards = g.next;
	return 0;
}

static void guard_take(const EXCEPTION_RECORD *rec, const CONTEXT *ctx)
{
	if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !guards ||
	    mod_from_address((const void *)(uintptr_t)ctx->Rip))
		return;
	while (guards && (uintptr_t)guards < ctx->Rsp)
		guards = guards->next;		/* stale */
	if (guards)
		longjmp(guards->jump, 1);
}

/* Is there any hope of unwinding from this pc: a function in a module? */
static PRUNTIME_FUNCTION frame_entry(const CONTEXT *c, ULONGLONG *base,
				     int *ok)
{
	PRUNTIME_FUNCTION fe = seh_RtlLookupFunctionEntry(c->Rip, base, NULL);

	*ok = fe || mod_from_address((const void *)(uintptr_t)c->Rip);
	return fe;
}

/*
 * The search phase: each frame's language handler, from the one that
 * faulted outward, until one resolves the exception. Resumes ctx and does
 * not return if one does; otherwise reports the exception as unhandled
 * (after the top-level filter), exiting.
 */
static void __attribute__((noreturn, no_sanitize_address))
dispatch_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx, int sig)
{
	struct win_teb *teb = teb_current();
	int level = dispatch_begin((uintptr_t)__builtin_frame_address(0), ctx);

	trace_exception(rec, sig ? "fault" : "raised");
	guard_take(rec, ctx);
	if (level >= 0) {
		CONTEXT c = *ctx;

		for (int depth = 0; depth < 4096; depth++) {
			ULONGLONG base = 0, frame = 0;
			void *hdata = NULL;
			int ok;
			PRUNTIME_FUNCTION fe = frame_entry(&c, &base, &ok);
			CONTEXT n = c;
			PEXCEPTION_ROUTINE h;

			if (vfw_trace_enabled) {
				char where[160];

				if (!mod_describe_address_try((void *)(uintptr_t)c.Rip,
							      where, sizeof(where)))
					snprintf(where, sizeof(where), "%#llx",
						 (unsigned long long)c.Rip);
				TRACE("  frame %s rsp %#llx%s", where,
				      (unsigned long long)c.Rsp, fe ? "" : " (no unwind data)");
			}
			if (!ok) {
				if (host_link(&c, NULL))
					continue;
				break;
			}
			h = seh_RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, c.Rip,
						 fe, &n, &hdata, &frame, NULL);
			if (h) {
				DISPATCHER_CONTEXT dc;

				memset(&dc, 0, sizeof(dc));
				dc.ControlPc = c.Rip;
				dc.ImageBase = base;
				dc.FunctionEntry = fe;
				dc.EstablisherFrame = frame;
				dc.ContextRecord = ctx;
				dc.LanguageHandler = h;
				dc.HandlerData = hdata;
				/*
				 * An exception raised inside the handler (libgcc
				 * starts its second phase that way) goes on, from
				 * here, from the context of this one.
				 */
				struct seh_link hl;
				EXCEPTION_DISPOSITION d;

				memset(&hl, 0, sizeof(hl));
				hl.parent = *ctx;
				seh_link_push(&hl);
				d = h(rec, (void *)(uintptr_t)frame, ctx, &dc);
				seh_link_pop(&hl);

				if (vfw_trace_enabled) {
					char where[160];

					if (!mod_describe_address_try((void *)(uintptr_t)c.Rip,
								      where, sizeof(where)))
						snprintf(where, sizeof(where), "%#llx",
							 (unsigned long long)c.Rip);
					TRACE("  handler of %s (frame %#llx) -> %d",
					      where, (unsigned long long)frame, (int)d);
				}
				if (d == ExceptionContinueExecution &&
				    !(rec->ExceptionFlags &
				      EXCEPTION_NONCONTINUABLE)) {
					nlive = level;
					links_prune(ctx->Rsp);
					vfw_restore_context(ctx);
				}
			}
			if (n.Rsp <= c.Rsp || !n.Rip ||
			    (teb && n.Rsp > (uintptr_t)teb->StackBase))
				break;
			c = n;
		}
		nlive = level;
	}

	EXCEPTION_POINTERS ep = { rec, ctx };
	LPTOP_LEVEL_EXCEPTION_FILTER f = top_filter;
	if (f && f(&ep) == EXCEPTION_CONTINUE_EXECUTION &&
	    !(rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE))
		vfw_restore_context(ctx);

	if (sig)
		crash_report_and_die(sig, (const void *)(uintptr_t)ctx->Rip,
				     (const void *)(uintptr_t)
				     rec->ExceptionInformation[1]);
	report_unhandled_exception(rec);
	crash_exit(SIGABRT);
}

/* From seh_RaiseException's stub, with the caller's context captured. */
void __attribute__((used, noreturn))
vfw_raise_c(DWORD code, DWORD flags, DWORD nargs, const DWORD_PTR *args,
	    CONTEXT *ctx)
{
	EXCEPTION_RECORD rec;

	memset(&rec, 0, sizeof(rec));
	rec.ExceptionCode = code;
	rec.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
	rec.ExceptionAddress = (void *)(uintptr_t)ctx->Rip;
	rec.NumberParameters = args && nargs <= EXCEPTION_MAXIMUM_PARAMETERS ?
			       nargs : 0;
	for (DWORD i = 0; i < rec.NumberParameters; i++)
		rec.ExceptionInformation[i] = args[i];
	dispatch_exception(&rec, ctx, 0);
}

/*
 * The unwind phase: each frame from the exception outward, its language
 * handler called with EXCEPTION_UNWINDING (and EXCEPTION_TARGET_UNWIND
 * at target) so __finally blocks run, then the registers of the target
 * frame resumed at target_ip with retval in rax. For a longjmp
 * (STATUS_LONGJUMP, the jump buffer in the record's first parameter) the
 * registers come from the buffer instead.
 */
/*
 * The unwind phase's walk: from start outward, each frame's language
 * handler called with EXCEPTION_UNWINDING (and EXCEPTION_TARGET_UNWIND at
 * target), so __finally blocks and destructors run. Returns 1 with the
 * context of the target frame in *out if it was reached, else 0. The
 * stack is left as it is: C++ catch blocks run on top of it.
 */
int __attribute__((no_sanitize_address))
seh_unwind_run(void *target, void *target_ip, EXCEPTION_RECORD *rec,
		   const CONTEXT *start, CONTEXT *out)
{
	struct win_teb *teb = teb_current();
	EXCEPTION_RECORD ur;
	CONTEXT c = *start;

	if (rec) {
		ur = *rec;
	} else {
		memset(&ur, 0, sizeof(ur));
		ur.ExceptionCode = STATUS_UNWIND;
	}
	ur.ExceptionFlags |= EXCEPTION_UNWINDING;
	if (!target)
		ur.ExceptionFlags |= EXCEPTION_EXIT_UNWIND;

	for (int depth = 0; depth < 4096; depth++) {
		ULONGLONG base = 0, frame = 0;
		void *hdata = NULL;
		int ok;
		PRUNTIME_FUNCTION fe = frame_entry(&c, &base, &ok);
		CONTEXT n = c;
		PEXCEPTION_ROUTINE h;
		int is_target;
		struct seh_link hl;

		if (!ok) {
			if (host_link(&c, &ur))
				continue;
			break;
		}
		h = seh_RtlVirtualUnwind(UNW_FLAG_UHANDLER, base, c.Rip, fe,
					 &n, &hdata, &frame, NULL);
		is_target = target && frame == (ULONGLONG)(uintptr_t)target;
		if (target && !is_target && frame > (ULONGLONG)(uintptr_t)target)
			break;		/* it isn't on the stack */
		if (h) {
			DISPATCHER_CONTEXT dc;

			memset(&dc, 0, sizeof(dc));
			ur.ExceptionFlags &= ~EXCEPTION_TARGET_UNWIND;
			if (is_target)
				ur.ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
			dc.ControlPc = c.Rip;
			dc.ImageBase = base;
			dc.FunctionEntry = fe;
			dc.EstablisherFrame = frame;
			dc.TargetIp = (ULONGLONG)(uintptr_t)target_ip;
			dc.ContextRecord = &c;
			dc.LanguageHandler = h;
			dc.HandlerData = hdata;
			memset(&hl, 0, sizeof(hl));
			hl.parent = *start;
			seh_link_push(&hl);
			h(&ur, (void *)(uintptr_t)frame, &c, &dc);
			seh_link_pop(&hl);
		}
		if (is_target) {
			*out = c;
			return 1;
		}
		if (n.Rsp <= c.Rsp || !n.Rip ||
		    (teb && n.Rsp > (uintptr_t)teb->StackBase))
			break;
		c = n;
	}
	return 0;
}

/* Resumes ctx for good: the dispatches and links below it are over. */
void __attribute__((noreturn)) seh_resume(CONTEXT *c)
{
	drop_below(c->Rsp);
	links_prune(c->Rsp);
	vfw_restore_context(c);
}

/* For the C++ runtime: dispatches a record raised with ctx (never returns). */
void __attribute__((noreturn)) seh_dispatch(EXCEPTION_RECORD *rec, CONTEXT *ctx)
{
	dispatch_exception(rec, ctx, 0);
}

/*
 * RtlUnwindEx: seh_unwind_run, then the registers of the target frame
 * resumed at target_ip with retval in rax. For a longjmp
 * (STATUS_LONGJUMP, the jump buffer in the record's first parameter) the
 * registers come from the buffer instead.
 */
void __attribute__((used, noreturn))
vfw_unwind_c(void *target, void *target_ip, EXCEPTION_RECORD *rec,
	     void *retval, CONTEXT *captured)
{
	CONTEXT c;
	int longjmp_ = rec && rec->ExceptionCode == STATUS_LONGJUMP;
	int found;

	drop_below(captured->Rsp);
	found = seh_unwind_run(target, target_ip, rec,
			       nlive ? live[nlive - 1].ctx : captured, &c);

	if (!found && !longjmp_) {
		if (target) {
			fputs("seance: crash: unwind target is not on the "
			      "stack\n", stderr);
			fflush(stderr);
			crash_exit(SIGABRT);
		}
		/* An unwind of everything: back to the caller. */
		nlive = 0;
		vfw_restore_context(captured);
	}

	if (longjmp_) {
		if (!found)
			c = *captured;
		/* Without the frame on the stack this is a plain longjmp. */
		const BYTE *jb = (const BYTE *)(uintptr_t)rec->ExceptionInformation[0];

		memcpy(&c.Rbx, jb + 8, 8);
		memcpy(&c.Rsp, jb + 16, 8);
		memcpy(&c.Rbp, jb + 24, 8);
		memcpy(&c.Rsi, jb + 32, 8);
		memcpy(&c.Rdi, jb + 40, 8);
		memcpy(&c.R12, jb + 48, 32);
		memcpy(&c.Rip, jb + 80, 8);
		memcpy(&c.MxCsr, jb + 88, 4);
		c.FltSave.MxCsr = c.MxCsr;
		memcpy(&c.Xmm6, jb + 96, 160);
		c.ContextFlags |= CONTEXT_FLOATING_POINT;
		c.Rax = (ULONGLONG)(uintptr_t)retval;
	} else {
		c.Rip = (ULONGLONG)(uintptr_t)target_ip;
		c.Rax = (ULONGLONG)(uintptr_t)retval;
	}
	seh_resume(&c);
}

/*
 * __C_specific_handler: the language handler for C's __try. HandlerData is
 * the scope table, a count followed by {Begin, End, Handler, Target} RVAs:
 * Target is the __except body (Handler its filter, or 1 for "always"), or 0
 * for a __finally block (Handler its funclet).
 */
struct scope {
	DWORD Begin, End, Handler, Target;
};

typedef LONG (WINAPI *filter_fn)(EXCEPTION_POINTERS *, void *frame);
typedef void (WINAPI *finally_fn)(BOOLEAN abnormal, void *frame);

EXCEPTION_DISPOSITION WINAPI
seh_C_specific_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx,
		       DISPATCHER_CONTEXT *dc)
{
	const DWORD *table = dc->HandlerData;
	const struct scope *scopes = (const struct scope *)(table + 1);
	DWORD count = table ? table[0] : 0;
	ULONGLONG base = dc->ImageBase;
	DWORD rva = (DWORD)(dc->ControlPc - base);

	if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
		for (DWORD i = dc->ScopeIndex; i < count; i++) {
			const struct scope *s = &scopes[i];

			if (rva < s->Begin || rva >= s->End)
				continue;
			if (s->Target) {
				/* An __except: nothing to run, but the one
				 * being jumped to ends the unwind here. */
				if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) &&
				    dc->TargetIp == base + s->Target)
					break;
				continue;
			}
			dc->ScopeIndex = i + 1;
			((finally_fn)(uintptr_t)(base + s->Handler))(1, frame);
		}
		return ExceptionContinueSearch;
	}

	EXCEPTION_POINTERS ep = { rec, ctx };

	for (DWORD i = dc->ScopeIndex; i < count; i++) {
		const struct scope *s = &scopes[i];
		LONG r;

		if (rva < s->Begin || rva >= s->End || !s->Target)
			continue;
		r = s->Handler == 1 ? EXCEPTION_EXECUTE_HANDLER :
		    ((filter_fn)(uintptr_t)(base + s->Handler))(&ep, frame);
		if (r < 0)
			return ExceptionContinueExecution;
		if (r > 0) {
			seh_RtlUnwindEx(frame, (void *)(uintptr_t)(base + s->Target),
					rec, (void *)(uintptr_t)rec->ExceptionCode,
					ctx, dc->HistoryTable);
			break;		/* the target wasn't there */
		}
	}
	return ExceptionContinueSearch;
}

/* What a fault becomes, until seh_run_fault() has dealt with it. */
static _Thread_local struct {
	int sig;
	EXCEPTION_RECORD rec;
	CONTEXT ctx;
} pending;

/* On the faulting thread's own stack, once seh_take_fault() redirected it. */
static void __attribute__((noreturn)) seh_run_fault(void)
{
	int sig = pending.sig;
	EXCEPTION_RECORD rec = pending.rec;
	CONTEXT ctx = pending.ctx;

	dispatch_exception(&rec, &ctx, sig);
}

int seh_take_fault(int sig, siginfo_t *si, void *ucontext)
{
	ucontext_t *uc = ucontext;
	struct win_teb *teb = teb_current();
	greg_t *g = uc->uc_mcontext.gregs;
	CONTEXT *c = &pending.ctx;

	if (!teb)
		return 0;
	drop_below((uintptr_t)g[REG_RSP]);

	memset(c, 0, sizeof(*c));
	pending.sig = sig;
	c->ContextFlags = CONTEXT_FULL | CONTEXT_SEGMENTS;
	c->SegCs = (WORD)(g[REG_CSGSFS] & 0xffff);
	c->SegGs = (WORD)((g[REG_CSGSFS] >> 16) & 0xffff);
	c->SegFs = (WORD)((g[REG_CSGSFS] >> 32) & 0xffff);
	c->EFlags = (DWORD)g[REG_EFL];
	c->Rax = g[REG_RAX];
	c->Rcx = g[REG_RCX];
	c->Rdx = g[REG_RDX];
	c->Rbx = g[REG_RBX];
	c->Rsp = g[REG_RSP];
	c->Rbp = g[REG_RBP];
	c->Rsi = g[REG_RSI];
	c->Rdi = g[REG_RDI];
	c->R8 = g[REG_R8];
	c->R9 = g[REG_R9];
	c->R10 = g[REG_R10];
	c->R11 = g[REG_R11];
	c->R12 = g[REG_R12];
	c->R13 = g[REG_R13];
	c->R14 = g[REG_R14];
	c->R15 = g[REG_R15];
	c->Rip = g[REG_RIP];
	if (uc->uc_mcontext.fpregs) {
		memcpy(&c->FltSave, uc->uc_mcontext.fpregs, sizeof(c->FltSave));
		c->MxCsr = c->FltSave.MxCsr;
	} else {
		c->ContextFlags &= ~(DWORD)0x8;
	}

	memset(&pending.rec, 0, sizeof(pending.rec));
	pending.rec.ExceptionAddress = (void *)(uintptr_t)c->Rip;
	switch (sig) {
	case SIGSEGV:
	case SIGBUS:
		pending.rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
		pending.rec.NumberParameters = 2;
		pending.rec.ExceptionInformation[0] = (g[REG_ERR] & 2) ? 1 : 0;
		pending.rec.ExceptionInformation[1] =
			(DWORD_PTR)(uintptr_t)si->si_addr;
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

	/* Room below the faulting stack (past any red zone), laid out as if
	 * seh_run_fault had been called. */
	uintptr_t stack = (((uintptr_t)g[REG_RSP] - 512) & ~(uintptr_t)15) - 8;

	g[REG_RSP] = (greg_t)stack;
	g[REG_RIP] = (greg_t)(uintptr_t)seh_run_fault;
	return 1;
}

void WINAPI seh_RtlRestoreContext(CONTEXT *ctx, EXCEPTION_RECORD *rec)
{
	vfw_restore_context(ctx);
}

PVOID WINAPI seh_RtlPcToFileHeader(PVOID pc, PVOID *base)
{
	struct module *m = mod_from_address(pc);

	*base = m ? m->base : NULL;
	return *base;
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
