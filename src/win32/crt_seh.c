/*
 * msvcrt: the handlers behind MSVC's __try/__except/__finally on i386,
 * for DLLs built with a dynamic CRT (/MD). Such a DLL puts msvcrt's
 * _except_handler3 (VC6 to VC7.1) in each __try frame, or, from VC8 on,
 * its own small _except_handler4 that calls msvcrt's
 * _except_handler4_common. A DLL with a static CRT carries all of this
 * itself.
 *
 * The frame a function with __try pushes, from its %ebp down:
 *
 *   ebp-0x04  try level: the scope-table index of the innermost __try
 *             the code is in, or -1 (EH3) / -2 (EH4) outside any
 *   ebp-0x08  the scope table (EH4: XORed with the image's cookie)
 *   ebp-0x0c  the handler      } the EXCEPTION_REGISTRATION_RECORD on
 *   ebp-0x10  the next frame   } the fs:[0] chain ("frame" below)
 *   ebp-0x14  EXCEPTION_POINTERS *, for GetExceptionInformation()
 *   ebp-0x18  %esp as the body left it, which an __except block restores
 *
 * Each scope-table entry holds the enclosing try level, a filter, and the
 * __except block to jump to; a NULL filter marks a __finally, whose block
 * is called and returns. Filters and __finally blocks are pieces of the
 * function itself, so they run with %ebp set to the function's frame.
 *
 * Not done: the EH4 GS and EH cookie checks (a mismatch would mean a
 * smashed stack, which the DLL's own checks catch on return anyway), and
 * a nested exception during an unwind, which Windows handles with its own
 * frame around each __finally.
 */
#include <stdint.h>

#include "runtime/seh.h"
#include "win32/internal.h"

#define CRTFN(ret) static ret STUB_CDECL

#define EH3_NONE ((int)-1)
#define EH4_NONE ((int)-2)

struct scope_entry {
	int enclosing;
	void *filter;		/* NULL for a __finally */
	void *handler;
};

/* EH4's table, after the cookie offsets that check its frame. */
struct eh4_table {
	int gs_cookie_offset;
	DWORD gs_cookie_xor_offset;
	DWORD eh_cookie_offset;
	DWORD eh_cookie_xor_offset;
	struct scope_entry entries[];
};

struct eh_frame {
	struct eh_frame *next;
	void *handler;
	UINT_PTR scope_table;
	int try_level;
};

/* The function's %ebp: just above its registration record. */
static void *frame_ebp(struct eh_frame *f)
{
	return (BYTE *)f + sizeof(*f);
}

/*
 * Calls a filter or __finally block with %ebp set to its function's
 * frame; returns what it leaves in %eax. The block may use %ebx, %esi and
 * %edi as its function does, so they are saved around it.
 */
__asm__(
	".text\n"
	".type crt_seh_call, @function\n"
	"crt_seh_call:\n\t"
	"pushl %ebp\n\t"
	"pushl %ebx\n\t"
	"pushl %esi\n\t"
	"pushl %edi\n\t"
	"movl 20(%esp), %eax\n\t"
	"movl 24(%esp), %ebp\n\t"
	"call *%eax\n\t"
	"popl %edi\n\t"
	"popl %esi\n\t"
	"popl %ebx\n\t"
	"popl %ebp\n\t"
	"ret\n\t"
	".size crt_seh_call, .-crt_seh_call\n"
	/* Jumps into an __except block, which restores %esp itself. */
	".type crt_seh_jump, @function\n"
	"crt_seh_jump:\n\t"
	"movl 4(%esp), %eax\n\t"
	"movl 8(%esp), %ebp\n\t"
	"jmp *%eax\n\t"
	".size crt_seh_jump, .-crt_seh_jump\n"
);
int crt_seh_call(void *fn, void *ebp) __asm__("crt_seh_call")
	__attribute__((cdecl));
void crt_seh_jump(void *fn, void *ebp) __asm__("crt_seh_jump")
	__attribute__((cdecl, noreturn));

/*
 * The %ebp of each __finally block running because of an unwind, so
 * _abnormal_termination() can tell it from one reached by falling out of
 * its __try.
 */
#define MAX_UNWINDS 16
static _Thread_local void *unwinding[MAX_UNWINDS];
static _Thread_local int nunwinding;

static void call_finally(void *fn, void *ebp)
{
	int slot = nunwinding < MAX_UNWINDS ? nunwinding++ : -1;

	if (slot >= 0)
		unwinding[slot] = ebp;
	crt_seh_call(fn, ebp);
	if (slot >= 0)
		nunwinding = slot;
}

/* Runs the __finally blocks from f's current try level out to stop. */
static void local_unwind(struct eh_frame *f, const struct scope_entry *table,
			 int stop, int none)
{
	while (f->try_level != stop && f->try_level != none) {
		const struct scope_entry *e = &table[f->try_level];
		f->try_level = e->enclosing;
		if (!e->filter)
			call_finally(e->handler, frame_ebp(f));
	}
}

/*
 * The search for an __except that takes the exception, shared by both
 * versions: innermost try level outwards, each filter deciding. One that
 * says EXCEPTION_EXECUTE_HANDLER gets every frame above this one unwound,
 * then this one's inner __finally blocks, then control, for good.
 */
static EXCEPTION_DISPOSITION handle(EXCEPTION_RECORD *rec, struct eh_frame *f,
				    CONTEXT *ctx, const struct scope_entry *table,
				    int none)
{
	void *ebp = frame_ebp(f);

	if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
		local_unwind(f, table, none, none);
		return ExceptionContinueSearch;
	}

	/*
	 * For GetExceptionInformation() in the filters. It outlives this call
	 * only as a dangling pointer into the dead dispatcher stack, as on
	 * Windows, where it is equally only good while the filters run.
	 */
	EXCEPTION_POINTERS xp = { rec, ctx };
	*(volatile UINT_PTR *)((BYTE *)f - sizeof(void *)) = (UINT_PTR)&xp;

	for (int level = f->try_level; level != none;
	     level = table[level].enclosing) {
		const struct scope_entry *e = &table[level];
		if (!e->filter)
			continue;
		int r = crt_seh_call(e->filter, ebp);
		if (r < 0)
			return ExceptionContinueExecution;
		if (r > 0) {
			seh_RtlUnwind((PEXCEPTION_REGISTRATION_RECORD)f, NULL,
				      rec, NULL);
			local_unwind(f, table, level, none);
			f->try_level = e->enclosing;
			crt_seh_jump(e->handler, ebp);
		}
	}
	return ExceptionContinueSearch;
}

CRTFN(EXCEPTION_DISPOSITION) crt__except_handler3(EXCEPTION_RECORD *rec,
						  void *frame, CONTEXT *ctx,
						  void *dispatch)
{
	struct eh_frame *f = frame;

	return handle(rec, f, ctx, (const struct scope_entry *)f->scope_table,
		      EH3_NONE);
}

static const struct scope_entry *eh4_entries(const UINT_PTR *cookie,
					     const struct eh_frame *f)
{
	const struct eh4_table *t =
		(const struct eh4_table *)(f->scope_table ^ *cookie);
	return t->entries;
}

/* check is the DLL's __security_check_cookie; see the top of the file. */
CRTFN(EXCEPTION_DISPOSITION) crt__except_handler4_common(
	UINT_PTR *cookie, void *check, EXCEPTION_RECORD *rec, void *frame,
	CONTEXT *ctx, void *dispatch)
{
	struct eh_frame *f = frame;

	return handle(rec, f, ctx, eh4_entries(cookie, f), EH4_NONE);
}

/* Leaving __try blocks by return, goto or __leave: runs their __finally. */
CRTFN(void) crt__local_unwind2(void *frame, int stop)
{
	struct eh_frame *f = frame;

	local_unwind(f, (const struct scope_entry *)f->scope_table, stop,
		     EH3_NONE);
}

CRTFN(void) crt__local_unwind4(UINT_PTR *cookie, void *frame, int stop)
{
	struct eh_frame *f = frame;

	local_unwind(f, eh4_entries(cookie, f), stop, EH4_NONE);
}

/* Unwinds every frame above frame, which becomes the chain's head. */
CRTFN(void) crt__global_unwind2(void *frame)
{
	seh_RtlUnwind(frame, NULL, NULL, NULL);
}

/*
 * Called from a __finally block, with its function's %ebp still in %ebp:
 * the asm entry passes that on, since a C function can't read its
 * caller's %ebp reliably.
 */
int STUB_CDECL crt_abnormal_termination_c(void *ebp)
	__attribute__((used, visibility("hidden")));
int STUB_CDECL crt_abnormal_termination_c(void *ebp)
{
	return nunwinding > 0 && unwinding[nunwinding - 1] == ebp;
}

__asm__(
	".text\n"
	".type crt__abnormal_termination, @function\n"
	"crt__abnormal_termination:\n\t"
	"pushl %ebp\n\t"
	"call crt_abnormal_termination_c\n\t"
	"addl $4, %esp\n\t"
	"ret\n\t"
	".size crt__abnormal_termination, .-crt__abnormal_termination\n"
);
int crt__abnormal_termination(void) __asm__("crt__abnormal_termination");

#define E(name) { #name, 0, crt_##name }

const struct builtin_export crt_seh_exports[] = {
	E(_except_handler3),
	E(_except_handler4_common),
	E(_local_unwind2),
	E(_local_unwind4),
	E(_global_unwind2),
	E(_abnormal_termination),
	BUILTIN_END
};
