/* SPDX-License-Identifier: GPL-2.0-only */
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
 * EH4's handler also checks the frame's EH cookie, and its GS cookie when
 * it has one, through the DLL's own __security_check_cookie (which ends
 * the process on a mismatch: a smashed stack), before it trusts the frame
 * and again after running code of the function's.
 *
 * Also setjmp and longjmp, which take part: longjmp unwinds the frames
 * set up since its setjmp (their __finally blocks and destructors run),
 * and that frame's own levels down to setjmp's, through what _setjmp3 was
 * told to call (_seh_longjmp_unwind or _seh_longjmp_unwind4 for a __try
 * frame, __CxxLongjmpUnwind for a C++ one).
 *
 * An exception out of a __finally that an unwind runs is dispatched as
 * any other (see runtime/seh.c); when it is caught, the abandoned
 * __finally is forgotten for _abnormal_termination's sake.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "runtime/seh.h"
#include "runtime/teb.h"
#include "win32/crt.h"
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
	int gs_cookie_offset;	/* from %ebp, or EH4_NO_GS */
	int gs_cookie_xor_offset;
	int eh_cookie_offset;
	int eh_cookie_xor_offset;
	struct scope_entry entries[];
};

#define EH4_NO_GS (-2)

/* The DLL's __security_check_cookie. */
typedef void (__attribute__((fastcall)) *cookie_check_fn)(UINT_PTR);

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

/*
 * Control goes on in the frame at ebp (an __except block, a catch block,
 * longjmp's target): a __finally still listed for it or a deeper frame
 * was left by an exception out of it, and is over.
 */
void crt_finally_abandon(const void *ebp)
{
	while (nunwinding &&
	       (uintptr_t)unwinding[nunwinding - 1] <= (uintptr_t)ebp)
		nunwinding--;
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

/* EH4: the frame's cookies, each XORed with an address in the frame. */
struct cookies {
	const struct eh4_table *table;
	cookie_check_fn check;
};

static void check_cookies(const struct cookies *c, BYTE *ebp)
{
	if (!c || !c->check)
		return;
	const struct eh4_table *t = c->table;
	if (t->gs_cookie_offset != EH4_NO_GS)
		c->check(*(UINT_PTR *)(ebp + t->gs_cookie_offset) ^
			 (UINT_PTR)(ebp + t->gs_cookie_xor_offset));
	c->check(*(UINT_PTR *)(ebp + t->eh_cookie_offset) ^
		 (UINT_PTR)(ebp + t->eh_cookie_xor_offset));
}

/*
 * The search for an __except that takes the exception, shared by both
 * versions: innermost try level outwards, each filter deciding. One that
 * says EXCEPTION_EXECUTE_HANDLER gets every frame above this one unwound,
 * then this one's inner __finally blocks, then control, for good.
 */
static EXCEPTION_DISPOSITION handle(EXCEPTION_RECORD *rec, struct eh_frame *f,
				    CONTEXT *ctx, const struct scope_entry *table,
				    int none, const struct cookies *cookies)
{
	void *ebp = frame_ebp(f);

	check_cookies(cookies, ebp);
	if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
		if (f->try_level != none) {
			local_unwind(f, table, none, none);
			check_cookies(cookies, ebp);
		}
		return ExceptionContinueSearch;
	}

	/*
	 * For GetExceptionInformation() in the filters. It outlives this call
	 * only as a dangling pointer into the dead dispatcher stack, as on
	 * Windows, where it is equally only good while the filters run.
	 */
	EXCEPTION_POINTERS xp = { rec, ctx };
	*(volatile UINT_PTR *)((BYTE *)f - sizeof(void *)) = (UINT_PTR)&xp;

	EXCEPTION_DISPOSITION disposition = ExceptionContinueSearch;
	int ran = 0;
	for (int level = f->try_level; level != none;
	     level = table[level].enclosing) {
		const struct scope_entry *e = &table[level];
		if (!e->filter)
			continue;
		int r = crt_seh_call(e->filter, ebp);
		ran = 1;
		if (r < 0) {
			disposition = ExceptionContinueExecution;
			break;
		}
		if (r > 0) {
			seh_RtlUnwind((PEXCEPTION_REGISTRATION_RECORD)f, NULL,
				      rec, NULL);
			local_unwind(f, table, level, none);
			f->try_level = e->enclosing;
			check_cookies(cookies, ebp);
			crt_finally_abandon(ebp);
			crt_seh_jump(e->handler, ebp);
		}
	}
	if (ran)
		check_cookies(cookies, ebp);
	return disposition;
}

CRTFN(EXCEPTION_DISPOSITION) crt__except_handler3(EXCEPTION_RECORD *rec,
						  void *frame, CONTEXT *ctx,
						  void *dispatch)
{
	struct eh_frame *f = frame;

	return handle(rec, f, ctx, (const struct scope_entry *)f->scope_table,
		      EH3_NONE, NULL);
}

static const struct eh4_table *eh4_table(const UINT_PTR *cookie,
					  const struct eh_frame *f)
{
	return (const struct eh4_table *)(f->scope_table ^ *cookie);
}

static const struct scope_entry *eh4_entries(const UINT_PTR *cookie,
					     const struct eh_frame *f)
{
	return eh4_table(cookie, f)->entries;
}

/* check is the DLL's __security_check_cookie; see the top of the file. */
CRTFN(EXCEPTION_DISPOSITION) crt__except_handler4_common(
	UINT_PTR *cookie, void *check, EXCEPTION_RECORD *rec, void *frame,
	CONTEXT *ctx, void *dispatch)
{
	struct eh_frame *f = frame;
	struct cookies c = { eh4_table(cookie, f), (cookie_check_fn)check };

	return handle(rec, f, ctx, c.table->entries, EH4_NONE, &c);
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

/* setjmp and longjmp. */

/*
 * _setjmp and _setjmp3 save the registers as their caller has them, then
 * go on to their C part, which sees the same arguments and returns 0 to
 * that caller.
 */
#define SETJMP_ENTRY(name)						\
	".type crt_" #name ", @function\n"				\
	"crt_" #name ":\n\t"						\
	"movl 4(%esp), %ecx\n\t"					\
	"movl %ebp, 0(%ecx)\n\t"					\
	"movl %ebx, 4(%ecx)\n\t"					\
	"movl %edi, 8(%ecx)\n\t"					\
	"movl %esi, 12(%ecx)\n\t"					\
	"movl %esp, 16(%ecx)\n\t"					\
	"movl 0(%esp), %eax\n\t"					\
	"movl %eax, 20(%ecx)\n\t"	/* where to return to */	\
	"jmp crt_" #name "_c\n\t"					\
	".size crt_" #name ", .-crt_" #name "\n"

__asm__(
	".text\n"
	SETJMP_ENTRY(_setjmp)
	SETJMP_ENTRY(_setjmp3)
	/* Back to where setjmp returned, returning value this time. */
	".type crt_longjmp_jump, @function\n"
	"crt_longjmp_jump:\n\t"
	"movl 4(%esp), %ecx\n\t"
	"movl 8(%esp), %eax\n\t"
	"movl 0(%ecx), %ebp\n\t"
	"movl 4(%ecx), %ebx\n\t"
	"movl 8(%ecx), %edi\n\t"
	"movl 12(%ecx), %esi\n\t"
	"movl 16(%ecx), %esp\n\t"
	"addl $4, %esp\n\t"		/* setjmp's return address */
	"jmp *20(%ecx)\n\t"
	".size crt_longjmp_jump, .-crt_longjmp_jump\n"
);
int crt__setjmp(void *jmp) __asm__("crt__setjmp") __attribute__((cdecl));
int crt__setjmp3(void *jmp, int n, ...) __asm__("crt__setjmp3")
	__attribute__((cdecl));
void crt_longjmp_jump(const struct jump_buffer *jmp, int value)
	__asm__("crt_longjmp_jump") __attribute__((cdecl, noreturn));

/* The try level of the frame at the chain's head, read as EH3's. */
static long head_level(unsigned long reg)
{
	return reg == (unsigned long)SEH_CHAIN_END ? EH3_NONE :
	       ((const struct eh_frame *)reg)->try_level;
}

/* VC6's: the frame at the chain's head, unwound as an EH3 one. */
int STUB_CDECL crt__setjmp_c(struct jump_buffer *jmp)
	__attribute__((used, visibility("hidden")));
int STUB_CDECL crt__setjmp_c(struct jump_buffer *jmp)
{
	jmp->registration = (unsigned long)teb_current()->ExceptionList;
	jmp->trylevel = head_level(jmp->registration);
	jmp->cookie = 0;
	jmp->unwind_func = 0;
	return 0;
}

/* From VC7: then n arguments, the unwind function, try level and data. */
int STUB_CDECL crt__setjmp3_c(struct jump_buffer *jmp, int n, ...)
	__attribute__((used, visibility("hidden")));
int STUB_CDECL crt__setjmp3_c(struct jump_buffer *jmp, int n, ...)
{
	va_list ap;

	jmp->cookie = JMP_COOKIE;
	jmp->unwind_func = 0;
	jmp->registration = (unsigned long)teb_current()->ExceptionList;
	jmp->trylevel = head_level(jmp->registration);
	if (jmp->registration == (unsigned long)SEH_CHAIN_END)
		return 0;
	va_start(ap, n);
	if (n > 0)
		jmp->unwind_func = va_arg(ap, unsigned long);
	if (n > 1)
		jmp->trylevel = va_arg(ap, long);
	for (int i = 0; i < 6 && i < n - 2; i++)
		jmp->unwind_data[i] = va_arg(ap, unsigned long);
	va_end(ap);
	return 0;
}

typedef void (WINAPI *jmp_unwind_fn)(const struct jump_buffer *);

CRTFN(void) crt_longjmp(const struct jump_buffer *jmp, int value)
{
	struct win_teb *teb = teb_current();
	void *reg = (void *)jmp->registration;

	/* The frames set up since, then that frame's own levels. */
	if (teb->ExceptionList != reg)
		seh_RtlUnwind(reg, NULL, NULL, NULL);
	if (reg && reg != SEH_CHAIN_END) {
		if (jmp->cookie != JMP_COOKIE)
			crt__local_unwind2(reg, (int)jmp->trylevel);
		else if (jmp->unwind_func)
			((jmp_unwind_fn)jmp->unwind_func)(jmp);
	}
	crt_finally_abandon((const void *)jmp->ebp);
	crt_longjmp_jump(jmp, value ? value : 1);
}

/* _setjmp3's unwind function for a __try frame: its __finally blocks. */
static void WINAPI __attribute__((force_align_arg_pointer))
crt__seh_longjmp_unwind(const struct jump_buffer *jmp)
{
	crt__local_unwind2((void *)jmp->registration, (int)jmp->trylevel);
}

/*
 * The same for an EH4 frame, whose scope table needs the image's cookie:
 * the compiler passes its address after the try level (unwind_data[0]),
 * as it passes it to _local_unwind4. That is checked against the frame's
 * EH cookie before the table is trusted (a table is 4-byte aligned, so a
 * wrong cookie usually shows before anything is read); if it doesn't
 * match, or can't be read, nothing is unwound and a warning says so.
 */
struct eh4_probe {
	struct eh_frame *frame;
	const UINT_PTR *cookie;
	int ok;
};

static void probe_eh4(void *arg)
{
	struct eh4_probe *p = arg;
	const struct eh4_table *t = eh4_table(p->cookie, p->frame);
	BYTE *ebp = frame_ebp(p->frame);

	if ((UINT_PTR)t & 3)
		return;
	p->ok = (*(UINT_PTR *)(ebp + t->eh_cookie_offset) ^
		 (UINT_PTR)(ebp + t->eh_cookie_xor_offset)) == *p->cookie;
}

static void WINAPI __attribute__((force_align_arg_pointer))
crt__seh_longjmp_unwind4(const struct jump_buffer *jmp)
{
	struct eh4_probe p = { (struct eh_frame *)jmp->registration,
			       (const UINT_PTR *)jmp->unwind_data[0], 0 };

	if (!p.cookie || crt_guarded(probe_eh4, &p) || !p.ok) {
		fputs("seance: warning: longjmp: no cookie for an EH4 "
		      "frame's __try levels, which stay as they are\n", stderr);
		return;
	}
	crt__local_unwind4((UINT_PTR *)p.cookie, p.frame, (int)jmp->trylevel);
}

#define E(name) { #name, 0, crt_##name }

const struct builtin_export crt_seh_exports[] = {
	E(_except_handler3),
	E(_except_handler4_common),
	E(_local_unwind2),
	E(_local_unwind4),
	E(_global_unwind2),
	E(_abnormal_termination),
	E(_setjmp),
	E(_setjmp3),
	E(longjmp),
	{ "_longjmpex", 0, crt_longjmp },
	{ "__intrinsic_setjmp", 0, crt__setjmp },
	E(_seh_longjmp_unwind),
	E(_seh_longjmp_unwind4),
	BUILTIN_END
};
