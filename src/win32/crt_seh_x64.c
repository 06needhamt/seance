/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt on x86-64: _setjmp and longjmp, with Win64's _JUMP_BUFFER.
 *
 *    0 Frame   8 Rbx   16 Rsp   24 Rbp   32 Rsi   40 Rdi
 *   48 R12    56 R13   64 R14   72 R15   80 Rip
 *   88 MxCsr (4)  92 FpCsr (2)  94 Spare (2)
 *   96 Xmm6 ... 240 Xmm15 (16 each)            256 in all
 *
 * Code calls _setjmp(buf, frame), frame being its own frame (mingw passes
 * it; cl's _setjmpex reads it itself). On Windows a longjmp with a frame
 * unwinds the frames it leaves, running their __finally blocks and C++
 * destructors; without the table-based unwinder (docs/X64_PLAN.md, phase
 * 4) this one restores the registers only, which is all C code needs.
 */
#if defined(__x86_64__)

#include "runtime/builtin.h"
#include "win32/internal.h"

__asm__(
	".text\n"
	".globl vfw_x64_setjmp\n"
	".type vfw_x64_setjmp, @function\n"
	"vfw_x64_setjmp:\n"		/* ms_abi: rcx = buf, rdx = frame */
	"	movq %rdx, 0(%rcx)\n"
	"	movq %rbx, 8(%rcx)\n"
	"	leaq 8(%rsp), %rax\n"	/* the caller's rsp, once we return */
	"	movq %rax, 16(%rcx)\n"
	"	movq %rbp, 24(%rcx)\n"
	"	movq %rsi, 32(%rcx)\n"
	"	movq %rdi, 40(%rcx)\n"
	"	movq %r12, 48(%rcx)\n"
	"	movq %r13, 56(%rcx)\n"
	"	movq %r14, 64(%rcx)\n"
	"	movq %r15, 72(%rcx)\n"
	"	movq (%rsp), %rax\n"	/* where to come back to */
	"	movq %rax, 80(%rcx)\n"
	"	stmxcsr 88(%rcx)\n"
	"	fnstcw 92(%rcx)\n"
	"	movdqu %xmm6, 96(%rcx)\n"
	"	movdqu %xmm7, 112(%rcx)\n"
	"	movdqu %xmm8, 128(%rcx)\n"
	"	movdqu %xmm9, 144(%rcx)\n"
	"	movdqu %xmm10, 160(%rcx)\n"
	"	movdqu %xmm11, 176(%rcx)\n"
	"	movdqu %xmm12, 192(%rcx)\n"
	"	movdqu %xmm13, 208(%rcx)\n"
	"	movdqu %xmm14, 224(%rcx)\n"
	"	movdqu %xmm15, 240(%rcx)\n"
	"	xorl %eax, %eax\n"
	"	ret\n"
	".size vfw_x64_setjmp, .-vfw_x64_setjmp\n"

	".globl vfw_x64_longjmp\n"
	".type vfw_x64_longjmp, @function\n"
	"vfw_x64_longjmp:\n"		/* rcx = buf, edx = value (0 means 1) */
	"	movl %edx, %eax\n"
	"	testl %eax, %eax\n"
	"	jnz 1f\n"
	"	incl %eax\n"
	"1:	movq 8(%rcx), %rbx\n"
	"	movq 24(%rcx), %rbp\n"
	"	movq 32(%rcx), %rsi\n"
	"	movq 40(%rcx), %rdi\n"
	"	movq 48(%rcx), %r12\n"
	"	movq 56(%rcx), %r13\n"
	"	movq 64(%rcx), %r14\n"
	"	movq 72(%rcx), %r15\n"
	"	ldmxcsr 88(%rcx)\n"
	"	fnclex\n"
	"	fldcw 92(%rcx)\n"
	"	movdqu 96(%rcx), %xmm6\n"
	"	movdqu 112(%rcx), %xmm7\n"
	"	movdqu 128(%rcx), %xmm8\n"
	"	movdqu 144(%rcx), %xmm9\n"
	"	movdqu 160(%rcx), %xmm10\n"
	"	movdqu 176(%rcx), %xmm11\n"
	"	movdqu 192(%rcx), %xmm12\n"
	"	movdqu 208(%rcx), %xmm13\n"
	"	movdqu 224(%rcx), %xmm14\n"
	"	movdqu 240(%rcx), %xmm15\n"
	"	movq 16(%rcx), %rsp\n"
	"	jmpq *80(%rcx)\n"
	".size vfw_x64_longjmp, .-vfw_x64_longjmp\n");

extern void vfw_x64_setjmp(void);
extern void vfw_x64_longjmp(void);

#define EASM(name, fn) { name, 0, (void *)fn }

const struct builtin_export crt_seh_exports[] = {
	EASM("_setjmp", vfw_x64_setjmp),
	EASM("_setjmpex", vfw_x64_setjmp),
	EASM("longjmp", vfw_x64_longjmp),
	BUILTIN_END
};

#endif /* __x86_64__ */
