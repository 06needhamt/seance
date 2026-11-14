/* SPDX-License-Identifier: GPL-2.0-only */
#include "runtime/gate.h"

#if !defined(__x86_64__)
/*
 * uintptr_t gate(const void *fn, unsigned n, const uintptr_t *args), cdecl:
 * pushes args[n - 1] ... args[0], calls fn, and puts %esp back from %ebp
 * however many bytes fn popped. %ebx, %esi and %edi are saved too, for
 * routines that clobber them. Position-independent, so a copy runs anywhere.
 */
__asm__(
	".text\n"
	".globl vfw_gate_start\n"
	".hidden vfw_gate_start\n"
	".type vfw_gate_start, @function\n"
	"vfw_gate_start:\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"pushl %ebx\n\t"
	"pushl %esi\n\t"
	"pushl %edi\n\t"
	"movl 12(%ebp), %ecx\n\t"	/* n */
	"movl 16(%ebp), %esi\n\t"	/* args */
	/* %esp 16-byte aligned once the arguments are pushed, as gcc
	 * keeps it at a call (code built with SSE relies on it). */
	"leal 0(,%ecx,4), %eax\n\t"
	"subl %eax, %esp\n\t"
	"andl $-16, %esp\n\t"
	"addl %eax, %esp\n\t"
	"1:\n\t"
	"testl %ecx, %ecx\n\t"
	"jz 2f\n\t"
	"pushl -4(%esi,%ecx,4)\n\t"
	"decl %ecx\n\t"
	"jmp 1b\n\t"
	"2:\n\t"
	"call *8(%ebp)\n\t"
	"leal -12(%ebp), %esp\n\t"
	"popl %edi\n\t"
	"popl %esi\n\t"
	"popl %ebx\n\t"
	"popl %ebp\n\t"
	"ret\n\t"
	".globl vfw_gate_end\n"
	".hidden vfw_gate_end\n"
	"vfw_gate_end:\n\t"
	".size vfw_gate_start, .-vfw_gate_start\n"
);
#else
/*
 * Win64 has one calling convention, in which the caller owns the stack, so
 * the gate only gives the routine a return address in kernel32's image:
 * sub rsp, 0x38 (shadow space plus two stack arguments, keeping 16-byte
 * alignment); move args[4..5] to the stack and args[0..3] to rcx, rdx, r8,
 * r9; call fn; add rsp back. SysV entry: rdi = fn, rsi = n (unused: the
 * caller zero-fills to GATE_MAX_ARGS), rdx = args.
 */
__asm__(
	".text\n"
	".globl vfw_gate_start\n"
	".hidden vfw_gate_start\n"
	".type vfw_gate_start, @function\n"
	"vfw_gate_start:\n\t"
	"pushq %rbp\n\t"
	"movq %rsp, %rbp\n\t"
	"pushq %rsi\n\t"		/* Win64 callee-saved, SysV not */
	"pushq %rdi\n\t"
	"subq $0x30, %rsp\n\t"
	"movq %rdi, %rax\n\t"
	"movq %rdx, %r10\n\t"
	"movq 32(%r10), %r11\n\t"
	"movq %r11, 32(%rsp)\n\t"
	"movq 40(%r10), %r11\n\t"
	"movq %r11, 40(%rsp)\n\t"
	"movq 0(%r10), %rcx\n\t"
	"movq 8(%r10), %rdx\n\t"
	"movq 16(%r10), %r8\n\t"
	"movq 24(%r10), %r9\n\t"
	"callq *%rax\n\t"
	"leaq -16(%rbp), %rsp\n\t"
	"popq %rdi\n\t"
	"popq %rsi\n\t"
	"popq %rbp\n\t"
	"retq\n\t"
	".globl vfw_gate_end\n"
	".hidden vfw_gate_end\n"
	"vfw_gate_end:\n\t"
	".size vfw_gate_start, .-vfw_gate_start\n"
);
#endif

extern const char vfw_gate_start[], vfw_gate_end[];

typedef uintptr_t (*gate_fn)(const void *fn, unsigned n, const uintptr_t *args);

static void *copy;

size_t gate_code_size(void)
{
	return (size_t)(vfw_gate_end - vfw_gate_start);
}

const void *gate_code(void)
{
	return vfw_gate_start;
}

void gate_set_copy(void *code)
{
	__atomic_store_n(&copy, code, __ATOMIC_RELEASE);
}

const void *gate_copy(void)
{
	return __atomic_load_n(&copy, __ATOMIC_ACQUIRE);
}

uintptr_t gate_call(const void *fn, unsigned n, const uintptr_t *args)
{
	uintptr_t a[GATE_MAX_ARGS] = { 0 };
	const void *code = gate_copy();

	if (n > GATE_MAX_ARGS)
		n = GATE_MAX_ARGS;
	for (unsigned i = 0; i < n; i++)
		a[i] = args[i];
	gate_fn g = (gate_fn)(uintptr_t)(code ? code : (const void *)vfw_gate_start);
	return g(fn, n, a);
}
