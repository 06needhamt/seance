/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt: the compiler-generated helpers MSVC emits calls to for 64-bit
 * integer arithmetic and float-to-int truncation on i386, where there's no
 * single instruction for them. Every 32-bit MSVC codec using __int64 or
 * truncating a float to an integer needs these, whether or not it links
 * msvcrt.dll for anything else - the compiler calls them unconditionally.
 *
 * _allmul/_alldiv/_aulldiv/_allrem/_aullrem take their two __int64
 * arguments the normal (cdecl) way, one 32-bit half at a time, but - since
 * they were written before that convention had a name - the callee pops
 * them, like stdcall; declaring them stdcall with four 32-bit parameters
 * (the push order is identical either way) gets that for free, with the
 * actual arithmetic done in portable C. _allshl/_allshr/_aullshr instead
 * take their __int64 already in EDX:EAX and a shift count in CL, entirely
 * in registers, which no C calling convention expresses, so those three
 * are hand-written asm.
 */
#include "runtime/builtin.h"
#include "win32/internal.h"

#define CRTFN(ret) static ret STUB_WINAPI

static long long pack64(int lo, int hi)
{
	/* Left-shifting a negative value is undefined; build the bit
	 * pattern unsigned, then reinterpret it as signed (well defined on
	 * a two's complement target, which is all this harness runs on). */
	return (long long)(((unsigned long long)(unsigned int)hi << 32) |
			    (unsigned int)lo);
}

CRTFN(long long) crt__allmul(int a_lo, int a_hi, int b_lo, int b_hi)
{
	/* Signed overflow is undefined, but a 64x64->64 multiply is exactly
	 * a truncation to the low 64 bits of the product, which the
	 * hardware (and this helper's real counterpart) does unconditionally;
	 * unsigned multiplication gives that truncation without invoking it. */
	return (long long)((unsigned long long)pack64(a_lo, a_hi) *
			    (unsigned long long)pack64(b_lo, b_hi));
}

CRTFN(long long) crt__alldiv(int a_lo, int a_hi, int b_lo, int b_hi)
{
	return pack64(a_lo, a_hi) / pack64(b_lo, b_hi);
}

CRTFN(unsigned long long) crt__aulldiv(int a_lo, int a_hi, int b_lo, int b_hi)
{
	return (unsigned long long)pack64(a_lo, a_hi) / (unsigned long long)pack64(b_lo, b_hi);
}

CRTFN(long long) crt__allrem(int a_lo, int a_hi, int b_lo, int b_hi)
{
	return pack64(a_lo, a_hi) % pack64(b_lo, b_hi);
}

CRTFN(unsigned long long) crt__aullrem(int a_lo, int a_hi, int b_lo, int b_hi)
{
	return (unsigned long long)pack64(a_lo, a_hi) % (unsigned long long)pack64(b_lo, b_hi);
}

/*
 * _allshl/_allshr/_aullshr: EDX:EAX in, CL (0-63) shift count in, EDX:EAX
 * out, no stack arguments and nothing popped - the classic MASM-runtime
 * implementations, reproduced from their long-published disassembly.
 *
 * This file is compiled for the harness itself (native Linux/ELF), which,
 * unlike PE, does not prepend an underscore to a C symbol's name - so the
 * bare labels below (_ftol, not __ftol) are exactly what the extern
 * declarations after this block, and hence the export table's C-side
 * references, need; the leading underscore in each is simply part of the
 * name (matching msvcrt.dll's real, separate, export-table spelling).
 */
__asm__(
	".text\n"
	".globl _ftol\n"
	"_ftol:\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"subl $12, %esp\n\t"
	"fnstcw -2(%ebp)\n\t"
	"movzwl -2(%ebp), %eax\n\t"
	"orl $0xc00, %eax\n\t"
	"movw %ax, -4(%ebp)\n\t"
	"fldcw -4(%ebp)\n\t"
	"fistpll -12(%ebp)\n\t"
	"fldcw -2(%ebp)\n\t"
	"movl -12(%ebp), %eax\n\t"
	"movl -8(%ebp), %edx\n\t"
	"movl %ebp, %esp\n\t"
	"popl %ebp\n\t"
	"ret\n\t"

	".globl _ftol2\n"
	"_ftol2:\n\t"
	"jmp _ftol\n\t"

	".globl _allshl\n"
	"_allshl:\n\t"
	"cmpb $32, %cl\n\t"
	"jae 1f\n\t"
	"shldl %cl, %eax, %edx\n\t"
	"shll %cl, %eax\n\t"
	"ret\n\t"
	"1:\n\t"
	"movl %eax, %edx\n\t"
	"xorl %eax, %eax\n\t"
	"andb $31, %cl\n\t"
	"shll %cl, %edx\n\t"
	"ret\n\t"

	".globl _allshr\n"
	"_allshr:\n\t"
	"cmpb $32, %cl\n\t"
	"jae 1f\n\t"
	"shrdl %cl, %edx, %eax\n\t"
	"sarl %cl, %edx\n\t"
	"ret\n\t"
	"1:\n\t"
	"movl %edx, %eax\n\t"
	"sarl $31, %edx\n\t"
	"andb $31, %cl\n\t"
	"sarl %cl, %eax\n\t"
	"ret\n\t"

	".globl _aullshr\n"
	"_aullshr:\n\t"
	"cmpb $32, %cl\n\t"
	"jae 1f\n\t"
	"shrdl %cl, %edx, %eax\n\t"
	"shrl %cl, %edx\n\t"
	"ret\n\t"
	"1:\n\t"
	"movl %edx, %eax\n\t"
	"xorl %edx, %edx\n\t"
	"andb $31, %cl\n\t"
	"shrl %cl, %eax\n\t"
	"ret\n\t"
);

/* The asm-only helpers above, as the harness's own (native ELF, so
 * unmangled) symbol names. */
extern void _ftol(void);
extern void _ftol2(void);
extern void _allshl(void);
extern void _allshr(void);
extern void _aullshr(void);

#define E(name) { #name, 0, (void *)crt_##name }
#define EASM(name) { #name, 0, (void *)name }

const struct builtin_export crt_helpers_exports[] = {
	E(_allmul), E(_alldiv), E(_aulldiv), E(_allrem), E(_aullrem),
	EASM(_ftol), EASM(_ftol2), EASM(_allshl), EASM(_allshr), EASM(_aullshr),
	BUILTIN_END
};
