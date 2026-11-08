/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * x86-64 structured exception handling, as a compiler lays it out: each
 * function below is written in assembly with .seh_* directives (so GNU as
 * emits the .pdata/.xdata unwind tables) and __C_specific_handler with a
 * scope table, i.e. what cl makes of __try/__except/__finally.
 *
 *   inner_fault   saves rbx, r12 and xmm6 (PUSH_NONVOL, SAVE_XMM128),
 *                 clobbers them and writes to address 0.
 *   catcher(...)  keeps its own values in them, calls a function inside a
 *                 __try with the given filter, and checks after the
 *                 __except body that unwinding gave them back.
 *   fin_inner     a __finally (scope with no target) around inner_fault.
 *   seh64_fpreg   the same through a frame pointer (SET_FPREG), with an
 *                 alloca-like adjustment the tables don't describe.
 *   seh64_longjmp a longjmp out of a __finally scope.
 *
 * Unwind data for the exported functions are in this DLL's .pdata; the
 * C functions (filters, the __finally body) are leaves.
 */
#include "fixture.h"

volatile LONG fin_ran, fin_abnormal, filt_calls, top_calls;
void *resume_addr;

/* __except(filter): execute for a null access violation, else not. */
LONG WINAPI filt_av(EXCEPTION_POINTERS *ep, void *frame)
{
	return ep->ExceptionRecord->ExceptionCode ==
		EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER :
					     EXCEPTION_CONTINUE_SEARCH;
}

/* __except(...) that resumes where the exception was raised. */
LONG WINAPI filt_continue(EXCEPTION_POINTERS *ep, void *frame)
{
	filt_calls++;
	return EXCEPTION_CONTINUE_EXECUTION;
}

/* The __finally body. */
void WINAPI fin_funclet(BOOLEAN abnormal, void *frame)
{
	fin_ran++;
	fin_abnormal = abnormal;
}

/* The top-level filter: carry on at the label the test function named. */
static LONG WINAPI top_filter(EXCEPTION_POINTERS *ep)
{
	top_calls++;
	ep->ContextRecord->Rip = (ULONG_PTR)resume_addr;
	return EXCEPTION_CONTINUE_EXECUTION;
}

/* A catcher: seh64_<name>, calling <callee> inside a __try whose filter
 * (1 for always, else a function) is <filter>. Returns 1 if the __except
 * body ran with rbx, r12 and xmm6 as they were, 0 if the exception was
 * not caught, -1 if the registers were wrong. */
__asm__(
	".macro catcher name, callee, filter\n"
	".globl \\name\n"
	".seh_proc \\name\n"
	"\\name:\n"
	"	pushq %rbx\n"
	"	.seh_pushreg %rbx\n"
	"	pushq %r12\n"
	"	.seh_pushreg %r12\n"
	"	subq $56, %rsp\n"
	"	.seh_stackalloc 56\n"
	"	movaps %xmm6, 32(%rsp)\n"
	"	.seh_savexmm %xmm6, 32\n"
	"	.seh_endprologue\n"
	"	movl $0x3333, %ebx\n"
	"	movl $0x4444, %r12d\n"
	"	movl $0x55, %eax\n"
	"	movq %rax, %xmm6\n"
	".Ltb\\@:\n"
	"	call \\callee\n"
	"	nop\n"
	".Lte\\@:\n"
	"	xorl %eax, %eax\n"
	"	jmp .Lout\\@\n"
	".Lcatch\\@:\n"
	"	cmpl $0x3333, %ebx\n"
	"	jne .Lbad\\@\n"
	"	cmpl $0x4444, %r12d\n"
	"	jne .Lbad\\@\n"
	"	movq %xmm6, %rax\n"
	"	cmpl $0x55, %eax\n"
	"	jne .Lbad\\@\n"
	"	movl $1, %eax\n"
	"	jmp .Lout\\@\n"
	".Lbad\\@:\n"
	"	movl $-1, %eax\n"
	".Lout\\@:\n"
	"	movaps 32(%rsp), %xmm6\n"
	"	addq $56, %rsp\n"
	"	popq %r12\n"
	"	popq %rbx\n"
	"	ret\n"
	"	.seh_handler __C_specific_handler, @unwind, @except\n"
	"	.seh_handlerdata\n"
	"	.long 1\n"
	"	.rva .Ltb\\@\n"
	"	.rva .Lte\\@\n"
	"	.ifc \\filter, 1\n"
	"	.long 1\n"
	"	.else\n"
	"	.rva \\filter\n"
	"	.endif\n"
	"	.rva .Lcatch\\@\n"
	"	.text\n"
	"	.seh_endproc\n"
	".endm\n"

	".text\n"

	/* Saves and clobbers the nonvolatile registers, then faults. */
	".globl inner_fault\n"
	".seh_proc inner_fault\n"
	"inner_fault:\n"
	"	pushq %rbx\n"
	"	.seh_pushreg %rbx\n"
	"	pushq %r12\n"
	"	.seh_pushreg %r12\n"
	"	subq $56, %rsp\n"
	"	.seh_stackalloc 56\n"
	"	movaps %xmm6, 32(%rsp)\n"
	"	.seh_savexmm %xmm6, 32\n"
	"	.seh_endprologue\n"
	"	movl $0x1111, %ebx\n"
	"	movl $0x2222, %r12d\n"
	"	pxor %xmm6, %xmm6\n"
	"	xorl %eax, %eax\n"
	"	movl $1, (%rax)\n"
	"	movaps 32(%rsp), %xmm6\n"
	"	addq $56, %rsp\n"
	"	popq %r12\n"
	"	popq %rbx\n"
	"	ret\n"
	"	.seh_endproc\n"

	/* Raises 0xE0001234 with no parameters. */
	".globl do_raise\n"
	".seh_proc do_raise\n"
	"do_raise:\n"
	"	subq $40, %rsp\n"
	"	.seh_stackalloc 40\n"
	"	.seh_endprologue\n"
	"	movl $0xE0001234, %ecx\n"
	"	xorl %edx, %edx\n"
	"	xorl %r8d, %r8d\n"
	"	xorl %r9d, %r9d\n"
	"	call *__imp_RaiseException(%rip)\n"
	"	addq $40, %rsp\n"
	"	ret\n"
	"	.seh_endproc\n"

	/* inner_fault under a __finally. */
	".globl fin_inner\n"
	".seh_proc fin_inner\n"
	"fin_inner:\n"
	"	subq $40, %rsp\n"
	"	.seh_stackalloc 40\n"
	"	.seh_endprologue\n"
	".Lf1b:\n"
	"	call inner_fault\n"
	"	nop\n"
	".Lf1e:\n"
	"	addq $40, %rsp\n"
	"	ret\n"
	"	.seh_handler __C_specific_handler, @unwind\n"
	"	.seh_handlerdata\n"
	"	.long 1\n"
	"	.rva .Lf1b\n"
	"	.rva .Lf1e\n"
	"	.rva fin_funclet\n"
	"	.long 0\n"
	"	.text\n"
	"	.seh_endproc\n"

	/* A __try/__except(filt_av) that does not catch what do_raise raises. */
	"catcher seh64_reject, do_raise, filt_av\n"

	"catcher seh64_except, inner_fault, 1\n"
	"catcher seh64_finally, fin_inner, 1\n"
	"catcher seh64_filter, inner_fault, filt_av\n"
	"catcher seh64_nested, seh64_reject, 1\n"
	"catcher seh64_continue, do_raise, filt_continue\n"

	/* The same through a frame pointer. */
	".globl seh64_fpreg\n"
	".seh_proc seh64_fpreg\n"
	"seh64_fpreg:\n"
	"	pushq %rbp\n"
	"	.seh_pushreg %rbp\n"
	"	pushq %rbx\n"
	"	.seh_pushreg %rbx\n"
	"	subq $40, %rsp\n"
	"	.seh_stackalloc 40\n"
	"	leaq 32(%rsp), %rbp\n"
	"	.seh_setframe %rbp, 32\n"
	"	.seh_endprologue\n"
	"	movl $0x3333, %ebx\n"
	"	subq $16, %rsp\n"
	".Lpb:\n"
	"	call inner_fault\n"
	"	nop\n"
	".Lpe:\n"
	"	xorl %eax, %eax\n"
	"	jmp .Lpout\n"
	".Lpcatch:\n"
	"	leaq -32(%rbp), %rsp\n"	/* the frame's own rsp again */
	"	xorl %eax, %eax\n"
	"	cmpl $0x3333, %ebx\n"
	"	jne .Lpout\n"
	"	movl $1, %eax\n"
	".Lpout:\n"
	"	leaq -32(%rbp), %rsp\n"
	"	addq $40, %rsp\n"
	"	popq %rbx\n"
	"	popq %rbp\n"
	"	ret\n"
	"	.seh_handler __C_specific_handler, @except\n"
	"	.seh_handlerdata\n"
	"	.long 1\n"
	"	.rva .Lpb\n"
	"	.rva .Lpe\n"
	"	.long 1\n"
	"	.rva .Lpcatch\n"
	"	.text\n"
	"	.seh_endproc\n"

	/* A longjmp out of a __finally scope, to a setjmp with a frame. */
	".globl lj_inner\n"
	".seh_proc lj_inner\n"
	"lj_inner:\n"
	"	subq $40, %rsp\n"
	"	.seh_stackalloc 40\n"
	"	.seh_endprologue\n"
	".Lj1b:\n"
	"	movl $7, %edx\n"
	"	call *__imp_longjmp(%rip)\n"
	"	nop\n"
	".Lj1e:\n"
	"	addq $40, %rsp\n"
	"	ret\n"
	"	.seh_handler __C_specific_handler, @unwind\n"
	"	.seh_handlerdata\n"
	"	.long 1\n"
	"	.rva .Lj1b\n"
	"	.rva .Lj1e\n"
	"	.rva fin_funclet\n"
	"	.long 0\n"
	"	.text\n"
	"	.seh_endproc\n"

	".globl seh64_longjmp\n"
	".seh_proc seh64_longjmp\n"
	"seh64_longjmp:\n"
	"	subq $296, %rsp\n"
	"	.seh_stackalloc 296\n"
	"	.seh_endprologue\n"
	"	leaq 32(%rsp), %rcx\n"
	"	movq %rsp, %rdx\n"
	"	call *__imp__setjmp(%rip)\n"
	"	testl %eax, %eax\n"
	"	jnz .Ljback\n"
	"	leaq 32(%rsp), %rcx\n"
	"	call lj_inner\n"
	"	xorl %eax, %eax\n"
	".Ljback:\n"
	"	addq $296, %rsp\n"
	"	ret\n"
	"	.seh_endproc\n"

	/* A fault nothing handles, which the top-level filter resumes at
	 * .Lresumed. */
	".globl seh64_top\n"
	".seh_proc seh64_top\n"
	"seh64_top:\n"
	"	subq $40, %rsp\n"
	"	.seh_stackalloc 40\n"
	"	.seh_endprologue\n"
	"	leaq .Lresumed(%rip), %rax\n"
	"	movq %rax, resume_addr(%rip)\n"
	"	xorl %eax, %eax\n"
	"	movl $1, (%rax)\n"
	"	xorl %eax, %eax\n"
	"	jmp .Ltout\n"
	".Lresumed:\n"
	"	movl $1, %eax\n"
	".Ltout:\n"
	"	addq $40, %rsp\n"
	"	ret\n"
	"	.seh_endproc\n"

	/* A fault nothing handles, and no top-level filter. */
	".globl seh_unhandled_crash\n"
	".seh_proc seh_unhandled_crash\n"
	"seh_unhandled_crash:\n"
	"	subq $40, %rsp\n"
	"	.seh_stackalloc 40\n"
	"	.seh_endprologue\n"
	"	xorl %eax, %eax\n"
	"	movl $1, (%rax)\n"
	"	addq $40, %rsp\n"
	"	ret\n"
	"	.seh_endproc\n"

	".section .drectve\n"
	"	.ascii \" -export:seh64_except -export:seh64_finally\"\n"
	"	.ascii \" -export:seh64_filter -export:seh64_nested\"\n"
	"	.ascii \" -export:seh64_continue -export:seh64_fpreg\"\n"
	"	.ascii \" -export:seh64_longjmp -export:seh64_top\"\n"
	"	.ascii \" -export:seh_unhandled_crash\"\n"
	".text\n");

EXPORT void seh64_install_top(void)
{
	SetUnhandledExceptionFilter(top_filter);
}

EXPORT void seh64_remove_top(void)
{
	SetUnhandledExceptionFilter(NULL);
}

/* The counters, so the test can read and clear them. */
EXPORT LONG seh64_counter(int which, int clear)
{
	volatile LONG *p = which == 0 ? &fin_ran : which == 1 ? &fin_abnormal :
			   which == 2 ? &filt_calls : &top_calls;
	LONG v = *p;

	if (clear)
		*p = 0;
	return v;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
