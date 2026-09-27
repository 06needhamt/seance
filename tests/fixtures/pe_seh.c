/*
 * Exercises the harness's SEH dispatch: installing frames on the fs:[0]
 * chain by hand (the compiler's own __try/__except needs a CRT we don't
 * link here), faulting under them, and checking that a handler runs and
 * can resume execution, that a chain of several frames is walked in
 * order, that RtlUnwind visits the frames between the current one and its
 * target, that RaiseException reaches an installed handler, and that an
 * unhandled fault still crashes normally.
 *
 * A handler's ABI (PEXCEPTION_ROUTINE) doesn't depend on how either side
 * spells the types, only that both agree it is
 * stdcall(EXCEPTION_RECORD*, void*, CONTEXT*, void*) -> LONG, so the frame
 * struct and handler prototype are declared locally rather than pulled
 * from <windows.h>, which gates them behind facilities this freestanding
 * build doesn't have.
 */
#include <stddef.h>

#include "fixture.h"

struct seh_frame {
	struct seh_frame *Next;
	LONG(WINAPI *Handler)
	(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp);
};

#define DISP_CONTINUE_EXECUTION 0
#define DISP_CONTINUE_SEARCH    1

static VOID(WINAPI *RtlUnwindProc)(void *target, void *target_ip,
				   EXCEPTION_RECORD *rec, void *retval);

/* A tiny freestanding routine (no CRT, no compiler-generated frame): faults
 * writing through a NULL pointer, or, if resumed at vfw_seh_recovered,
 * returns 1 instead. */
__asm__(
	".text\n"
	".globl vfw_seh_try_write\n"
	"vfw_seh_try_write:\n\t"
	"xorl %eax, %eax\n\t"
	"movl $0x2a, (%eax)\n\t"
	"movl $0, %eax\n\t"		/* not reached */
	"ret\n\t"
	".globl vfw_seh_recovered\n"
	"vfw_seh_recovered:\n\t"
	"movl $1, %eax\n\t"
	"ret\n\t"
);
extern int vfw_seh_try_write(void) asm("vfw_seh_try_write");
extern char vfw_seh_recovered[] asm("vfw_seh_recovered");

static struct seh_frame *chain_push(struct seh_frame *f,
				    LONG(WINAPI *handler)(EXCEPTION_RECORD *,
							  void *, CONTEXT *, void *))
{
	void *prev;

	__asm__ __volatile__("movl %%fs:0, %0" : "=r"(prev));
	f->Next = prev;
	f->Handler = handler;
	__asm__ __volatile__("movl %0, %%fs:0" : : "r"(f) : "memory");
	return f;
}

static void chain_pop(struct seh_frame *f)
{
	__asm__ __volatile__("movl %0, %%fs:0" : : "r"(f->Next) : "memory");
}

/* Recovers a single-frame access violation by resuming at the fixed-up
 * recovery point. */
static LONG WINAPI recover_handler(EXCEPTION_RECORD *rec, void *frame,
				   CONTEXT *ctx, void *disp)
{
	if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return DISP_CONTINUE_SEARCH;
	if (rec->NumberParameters != 2 || rec->ExceptionInformation[0] != 1 ||
	    rec->ExceptionInformation[1] != 0)
		return DISP_CONTINUE_SEARCH;	/* not the write-to-NULL we expect */
	ctx->Eip = (DWORD)(ULONG_PTR)vfw_seh_recovered;
	return DISP_CONTINUE_EXECUTION;
}

EXPORT int seh_recover(void)
{
	struct seh_frame f;
	int r;

	chain_push(&f, recover_handler);
	r = vfw_seh_try_write();
	chain_pop(&f);
	return r == 1;
}

/* Three frames: the innermost declines, the middle one only notices it was
 * unwound past (EXCEPTION_UNWINDING), the outer one handles the fault by
 * calling RtlUnwind to the middle frame's target then resuming. */
static int unwind_seen;

static LONG WINAPI decline_handler(EXCEPTION_RECORD *rec, void *frame,
				   CONTEXT *ctx, void *disp)
{
	if (rec->ExceptionFlags & EXCEPTION_UNWINDING)
		unwind_seen = 1;
	return DISP_CONTINUE_SEARCH;
}

static LONG WINAPI outer_handler(EXCEPTION_RECORD *rec, void *frame,
				 CONTEXT *ctx, void *disp)
{
	if (rec->ExceptionFlags & EXCEPTION_UNWINDING)
		return DISP_CONTINUE_SEARCH;
	if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return DISP_CONTINUE_SEARCH;
	RtlUnwindProc(frame, NULL, rec, NULL);
	ctx->Eip = (DWORD)(ULONG_PTR)vfw_seh_recovered;
	return DISP_CONTINUE_EXECUTION;
}

EXPORT int seh_chain_unwind(void)
{
	struct seh_frame outer, middle;
	int r;

	RtlUnwindProc = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
					       "RtlUnwind");
	if (!RtlUnwindProc)
		return -1;

	unwind_seen = 0;
	chain_push(&outer, outer_handler);
	chain_push(&middle, decline_handler);
	r = vfw_seh_try_write();
	chain_pop(&middle);
	chain_pop(&outer);
	return r == 1 && unwind_seen;
}

/* Model the transfer made by _except_handler3 after it accepts a fault:
 * RtlUnwind returns to the handler, which restores the protected stack and
 * jumps directly to the __except body instead of returning to the dispatcher.
 * Doing this twice catches dispatch state left behind by the first jump. */
static LONG WINAPI nonlocal_handler(EXCEPTION_RECORD *rec, void *frame,
				    CONTEXT *ctx, void *disp)
{
	if (rec->ExceptionFlags & EXCEPTION_UNWINDING ||
	    rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return DISP_CONTINUE_SEARCH;

	RtlUnwindProc(frame, NULL, rec, NULL);
	__asm__ __volatile__(
		/* The non-local transfer must restore the protected function's
		 * callee-saved registers as well as its stack.  In particular, GCC
		 * addresses this function's locals through EBP, so retaining the
		 * dispatcher's EBP makes the recovery return corrupt its caller. */
		"movl %c[ebp](%%eax), %%ebp\n\t"
		"movl %c[ebx](%%eax), %%ebx\n\t"
		"movl %c[esi](%%eax), %%esi\n\t"
		"movl %c[edi](%%eax), %%edi\n\t"
		"movl %c[esp](%%eax), %%esp\n\t"
		"jmp vfw_seh_recovered\n\t"
		:
		: "a"(ctx),
		  [ebp] "i"(offsetof(CONTEXT, Ebp)),
		  [ebx] "i"(offsetof(CONTEXT, Ebx)),
		  [esi] "i"(offsetof(CONTEXT, Esi)),
		  [edi] "i"(offsetof(CONTEXT, Edi)),
		  [esp] "i"(offsetof(CONTEXT, Esp))
		: "memory");
	__builtin_unreachable();
}

EXPORT int seh_nonlocal_twice(void)
{
	struct seh_frame f;
	int first, second;

	RtlUnwindProc = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
					       "RtlUnwind");
	if (!RtlUnwindProc)
		return -1;

	chain_push(&f, nonlocal_handler);
	first = vfw_seh_try_write();
	chain_pop(&f);
	chain_push(&f, nonlocal_handler);
	second = vfw_seh_try_write();
	chain_pop(&f);
	return first == 1 && second == 1;
}

/* RaiseException reaching an installed frame, with its code and one
 * argument intact, and resuming normally afterwards. */
static DWORD raised_seen;
static DWORD_PTR raised_arg;

static LONG WINAPI raise_handler(EXCEPTION_RECORD *rec, void *frame,
				 CONTEXT *ctx, void *disp)
{
	if (rec->ExceptionCode < 0xE0000000 || rec->ExceptionCode > 0xE0000fff)
		return DISP_CONTINUE_SEARCH;
	raised_seen = rec->ExceptionCode;
	raised_arg = rec->NumberParameters == 1 ? rec->ExceptionInformation[0] : 0;
	return DISP_CONTINUE_EXECUTION;
}

EXPORT int seh_raise_test(void)
{
	struct seh_frame f;
	DWORD_PTR arg = 0x1234;

	raised_seen = 0;
	raised_arg = 0;
	chain_push(&f, raise_handler);
	RaiseException(0xE0000042, 0, 1, &arg);
	chain_pop(&f);
	return raised_seen == 0xE0000042 && raised_arg == 0x1234;
}

/* An otherwise-unhandled fault, caught only by a top-level filter. */
static LONG WINAPI top_filter(EXCEPTION_POINTERS *ep)
{
	ep->ContextRecord->Eip = (DWORD)(ULONG_PTR)vfw_seh_recovered;
	return -1;	/* EXCEPTION_CONTINUE_EXECUTION */
}

EXPORT int seh_unhandled_filter(void)
{
	int r;

	SetUnhandledExceptionFilter(top_filter);
	r = vfw_seh_try_write();
	SetUnhandledExceptionFilter(NULL);
	return r == 1;
}

/* No frame, no filter: still faults normally, for the harness's crash
 * report to catch. Never returns. */
EXPORT int seh_unhandled_crash(void)
{
	return vfw_seh_try_write();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
