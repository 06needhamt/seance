/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * x86-64 C++ exceptions, laid out as cl compiles them: every function is
 * written in assembly with .seh_* unwind data, __CxxFrameHandler3 as its
 * language handler and a FuncInfo (state maps, try and catch tables, all
 * RVAs) as handler data; catch blocks and destructors are funclets taking
 * the function's frame in rdx; ThrowInfo and the catchable-type tables
 * describe what is thrown.
 *
 *   thrower_int      throw 5-style int            (ThrowInfo: int)
 *   thrower_derived  throw Derived{pad, base}     (Derived, then Base)
 *   thrower_obj      throw ExcObj, which has a destructor
 *   mid_*            a function with a local to destroy (state 0) between
 *   catcher macro    try { callee(v) } catch (type) { ... }
 *
 * The exported functions return what their catch block saw, or 0.
 */
#include "fixture.h"

volatile LONG dtor_count, obj_dtor_count;

EXPORT LONG cxx64_counter(int which, int clear)
{
	volatile LONG *p = which ? &obj_dtor_count : &dtor_count;
	LONG v = *p;

	if (clear)
		*p = 0;
	return v;
}

__asm__(
	".text\n"

	/* The unwind funclet of a local: count it. */
	"dtor_action:\n"
	"	incl dtor_count(%rip)\n"
	"	ret\n"
	/* ExcObj's destructor (this in rcx). */
	"obj_dtor:\n"
	"	incl obj_dtor_count(%rip)\n"
	"	ret\n"

	/* A function that throws: sub; stores its argument in a temp. */
	".macro thrower name, ti, pad\n"
	".seh_proc \\name\n"
	"\\name:\n"
	"	subq $56, %rsp\n"
	"	.seh_stackalloc 56\n"
	"	.seh_endprologue\n"
	"	.if \\pad\n"
	"	movl $0x1234, 32(%rsp)\n"
	"	movl %ecx, 36(%rsp)\n"
	"	.else\n"
	"	movl %ecx, 32(%rsp)\n"
	"	.endif\n"
	"	leaq 32(%rsp), %rcx\n"
	"	leaq \\ti(%rip), %rdx\n"
	"	call *__imp__CxxThrowException(%rip)\n"
	"	int3\n"
	"	.seh_endproc\n"
	".endm\n"

	/* A function with a local (state 0) around its call. */
	".macro mid name, callee\n"
	".seh_proc \\name\n"
	"\\name:\n"
	"	subq $56, %rsp\n"
	"	.seh_stackalloc 56\n"
	"	.seh_endprologue\n"
	"	movq $-2, 40(%rsp)\n"
	".Lmb\\@:\n"
	"	call \\callee\n"
	"	nop\n"
	".Lme\\@:\n"
	"	addq $56, %rsp\n"
	"	ret\n"
	"	.seh_handler __CxxFrameHandler3, @unwind, @except\n"
	"	.seh_handlerdata\n"
	"	.rva .Lmf\\@\n"
	"	.text\n"
	"	.seh_endproc\n"
	"	.section .rdata,\"dr\"\n"
	".Lmf\\@:\n"
	"	.long 0x19930522, 1\n"
	"	.rva .Lmu\\@\n"
	"	.long 0, 0, 2\n"
	"	.rva .Lmi\\@\n"
	"	.long 40, 0, 1\n"
	".Lmu\\@:\n"
	"	.long -1\n"
	"	.rva dtor_action\n"
	".Lmi\\@:\n"
	"	.rva .Lmb\\@\n"
	"	.long 0\n"
	"	.rva .Lme\\@\n"
	"	.long -1\n"
	"	.text\n"
	".endm\n"

	/*
	 * try { callee(v); } catch (type) { ... }: adj is the catch's flags,
	 * objoff where its variable is, kind what the function returns once
	 * caught (0 the int in the variable, 1 the int the variable points
	 * to, 2 the constant 77); rethrow makes the catch block throw;.
	 */
	".macro catcher name, callee, type, adj, objoff, kind, rethrow\n"
	".seh_proc \\name\n"
	"\\name:\n"
	"	subq $56, %rsp\n"
	"	.seh_stackalloc 56\n"
	"	.seh_endprologue\n"
	"	movq $-2, 40(%rsp)\n"
	"	movq $0, 32(%rsp)\n"
	".Ltry\\@:\n"
	"	call \\callee\n"
	"	nop\n"
	".Lend\\@:\n"
	"	xorl %eax, %eax\n"
	"	jmp .Lout\\@\n"
	".Lcont\\@:\n"
	"	.if \\kind == 0\n"
	"	movl 32(%rsp), %eax\n"
	"	.elseif \\kind == 1\n"
	"	movq 32(%rsp), %rax\n"
	"	movl (%rax), %eax\n"
	"	.else\n"
	"	movl $77, %eax\n"
	"	.endif\n"
	".Lout\\@:\n"
	"	addq $56, %rsp\n"
	"	ret\n"
	"	.seh_handler __CxxFrameHandler3, @unwind, @except\n"
	"	.seh_handlerdata\n"
	"	.rva .Lfi\\@\n"
	"	.text\n"
	"	.seh_endproc\n"
	/* The catch funclet. */
	".seh_proc cfun\\@\n"
	"cfun\\@:\n"
	"	subq $40, %rsp\n"
	"	.seh_stackalloc 40\n"
	"	.seh_endprologue\n"
	"	.if \\rethrow\n"
	"	xorl %ecx, %ecx\n"
	"	xorl %edx, %edx\n"
	"	call *__imp__CxxThrowException(%rip)\n"
	"	int3\n"
	"	.endif\n"
	"	leaq .Lcont\\@(%rip), %rax\n"
	"	addq $40, %rsp\n"
	"	ret\n"
	"	.seh_endproc\n"
	"	.section .rdata,\"dr\"\n"
	".Lfi\\@:\n"
	"	.long 0x19930522, 2\n"
	"	.rva .Lum\\@\n"
	"	.long 1\n"
	"	.rva .Ltm\\@\n"
	"	.long 2\n"
	"	.rva .Lim\\@\n"
	"	.long 40, 0, 1\n"
	".Lum\\@:\n"
	"	.long -1, 0, -1, 0\n"
	".Ltm\\@:\n"
	"	.long 0, 0, 1, 1\n"
	"	.rva .Lcb\\@\n"
	".Lcb\\@:\n"
	"	.long \\adj\n"
	"	.ifnb \\type\n"
	"	.rva \\type\n"
	"	.else\n"
	"	.long 0\n"
	"	.endif\n"
	"	.long \\objoff\n"
	"	.rva cfun\\@\n"
	"	.long 0\n"
	".Lim\\@:\n"
	"	.rva .Ltry\\@\n"
	"	.long 0\n"
	"	.rva .Lend\\@\n"
	"	.long -1\n"
	"	.text\n"
	".endm\n"

	"thrower thrower_int, ThrowInt, 0\n"
	"thrower thrower_derived, ThrowDerived, 1\n"
	"thrower thrower_obj, ThrowObj, 0\n"
	"mid mid_int, thrower_int\n"
	"mid mid_derived, thrower_derived\n"
	"mid mid_obj, thrower_obj\n"

	".globl cxx64_int\n"
	"catcher cxx64_int, mid_int, TdInt, 0, 32, 0, 0\n"
	".globl cxx64_all\n"
	"catcher cxx64_all, mid_int, , 0, 0, 2, 0\n"
	".globl cxx64_base\n"
	"catcher cxx64_base, mid_derived, TdBase, 8, 32, 1, 0\n"
	".globl cxx64_obj\n"
	"catcher cxx64_obj, mid_obj, TdObj, 8, 32, 1, 0\n"
	/* The inner catch matches and rethrows; the outer one gets it. */
	"catcher inner_rethrow, mid_int, TdInt, 0, 32, 0, 1\n"
	".globl cxx64_rethrow\n"
	"catcher cxx64_rethrow, inner_rethrow, TdInt, 0, 32, 0, 0\n"
	"catcher inner_rethrow_obj, mid_obj, TdObj, 8, 32, 1, 1\n"
	".globl cxx64_rethrow_obj\n"
	"catcher cxx64_rethrow_obj, inner_rethrow_obj, TdObj, 8, 32, 1, 0\n"
	/* The inner catch is for another type: the outer one gets it. */
	"catcher inner_wrong, mid_int, TdChar, 0, 32, 0, 0\n"
	".globl cxx64_wrongtype\n"
	"catcher cxx64_wrongtype, inner_wrong, TdInt, 0, 32, 0, 0\n"

	/*
	 * RTTI: Base and Derived (single inheritance, with vftables and
	 * locators), and calls of __RTtypeid, __RTDynamicCast and
	 * __RTCastToVoid on them. rt_* return 1 when they got the right answer.
	 */
	".macro rtcall name, body\n"
	".globl \\name\n"
	".seh_proc \\name\n"
	"\\name:\n"
	"	subq $56, %rsp\n"
	"	.seh_stackalloc 56\n"
	"	.seh_endprologue\n"
	"	\\body\n"
	"	addq $56, %rsp\n"
	"	ret\n"
	"	.seh_endproc\n"
	".endm\n"
	"rtcall rt_typeid, \"leaq ObjDerived(%rip), %rcx; call *__imp___RTtypeid(%rip); leaq TdDerived(%rip), %rdx; xorl %ecx, %ecx; cmpq %rdx, %rax; sete %cl; movl %ecx, %eax\"\n"
	"rtcall rt_cast_ok, \"leaq ObjDerived(%rip), %rcx; xorl %edx, %edx; leaq TdBase(%rip), %r8; leaq TdDerived(%rip), %r9; movl $0, 32(%rsp); call *__imp___RTDynamicCast(%rip); leaq ObjDerived(%rip), %rdx; xorl %ecx, %ecx; cmpq %rdx, %rax; sete %cl; movl %ecx, %eax\"\n"
	"rtcall rt_cast_null, \"leaq ObjBase(%rip), %rcx; xorl %edx, %edx; leaq TdBase(%rip), %r8; leaq TdDerived(%rip), %r9; movl $0, 32(%rsp); call *__imp___RTDynamicCast(%rip); xorl %ecx, %ecx; testq %rax, %rax; sete %cl; movl %ecx, %eax\"\n"
	"rtcall rt_cast_void, \"leaq ObjDerived(%rip), %rcx; call *__imp___RTCastToVoid(%rip); leaq ObjDerived(%rip), %rdx; xorl %ecx, %ecx; cmpq %rdx, %rax; sete %cl; movl %ecx, %eax\"\n"
	/* Callees that throw from msvcrt: a failed reference cast, typeid(NULL). */
	"rtcall rt_cast_ref, \"leaq ObjBase(%rip), %rcx; xorl %edx, %edx; leaq TdBase(%rip), %r8; leaq TdDerived(%rip), %r9; movl $1, 32(%rsp); call *__imp___RTDynamicCast(%rip)\"\n"
	"rtcall rt_typeid_null, \"xorl %ecx, %ecx; call *__imp___RTtypeid(%rip)\"\n"
	".macro xcall name, imp\n"
	".globl \\name\n"
	".seh_proc \\name\n"
	"\\name:\n"
	"	subq $56, %rsp\n"
	"	.seh_stackalloc 56\n"
	"	.seh_endprologue\n"
	"	leaq XMsg(%rip), %rcx\n"
	"	call *\"\\imp\"(%rip)\n"
	"	int3\n"
	"	.seh_endproc\n"
	".endm\n"
	"xcall rt_xrange, \"__imp_?_Xout_of_range@std@@YAXPEBD@Z\"\n"
	"xcall rt_xruntime, \"__imp_?_Xruntime_error@std@@YAXPEBD@Z\"\n"
	"xcall rt_xalloc, \"__imp_?_Xbad_alloc@std@@YAXXZ\"\n"
	".globl cxx64_xrange\n"
	"catcher cxx64_xrange, rt_xrange, TdLogic, 8, 32, 2, 0\n"
	".globl cxx64_xruntime\n"
	"catcher cxx64_xruntime, rt_xruntime, TdRuntime, 8, 32, 2, 0\n"
	".globl cxx64_xalloc\n"
	"catcher cxx64_xalloc, rt_xalloc, TdExc, 8, 32, 2, 0\n"
	".globl cxx64_badcast\n"
	"catcher cxx64_badcast, rt_cast_ref, TdBadCast, 8, 32, 2, 0\n"
	/* ... caught as their base, std::exception. */
	".globl cxx64_badtypeid\n"
	"catcher cxx64_badtypeid, rt_typeid_null, TdExc, 8, 32, 2, 0\n"

	".section .rdata,\"dr\"\n"
	".balign 8\n"
	".balign 8\n"
	"TdInt:  .quad 0, 0\n .asciz \".H\"\n"
	".balign 8\n"
	"TdChar: .quad 0, 0\n .asciz \".D\"\n"
	".balign 8\n"
	"TdBase: .quad 0, 0\n .asciz \".?AUBase@@\"\n"
	".balign 8\n"
	"TdDerived: .quad 0, 0\n .asciz \".?AUDerived@@\"\n"
	".balign 8\n"
	"TdObj:  .quad 0, 0\n .asciz \".?AUExcObj@@\"\n"
	".balign 4\n"
	"TdBadCast: .quad 0, 0\n .asciz \".?AVbad_cast@std@@\"\n"
	".balign 8\n"
	"TdExc:  .quad 0, 0\n .asciz \".?AVexception@std@@\"\n"
	".balign 8\n"
	"TdLogic:   .quad 0, 0\n .asciz \".?AVlogic_error@std@@\"\n"
	".balign 8\n"
	"TdRuntime: .quad 0, 0\n .asciz \".?AVruntime_error@std@@\"\n"
	"XMsg: .asciz \"x\"\n"
	".balign 4\n"
	/* RTTI of Base and Derived. */
	"ColBase:    .long 1, 0, 0\n .rva TdBase\n .rva ChdBase\n .rva ColBase\n"
	"ChdBase:    .long 0, 0, 1\n .rva BcaBase\n"
	"BcaBase:    .rva BcdBase0\n"
	"BcdBase0:   .rva TdBase\n .long 0, 0, -1, 0, 0\n .rva ChdBase\n"
	"ColDerived: .long 1, 0, 0\n .rva TdDerived\n .rva ChdDerived\n .rva ColDerived\n"
	"ChdDerived: .long 0, 0, 2\n .rva BcaDerived\n"
	"BcaDerived: .rva BcdD0\n .rva BcdD1\n"
	"BcdD0:      .rva TdDerived\n .long 1, 0, -1, 0, 0\n .rva ChdDerived\n"
	"BcdD1:      .rva TdBase\n .long 0, 0, -1, 0, 0\n .rva ChdBase\n"
	".balign 8\n"
	"VtBase:     .quad ColBase\n"
	"VtBase1:    .quad 0\n"
	"VtDerived:  .quad ColDerived\n"
	"VtDerived1: .quad 0\n"
	".data\n"
	".balign 8\n"
	"ObjBase:    .quad VtBase1\n"
	"ObjDerived: .quad VtDerived1\n"
	".section .rdata,\"dr\"\n"
	/* CatchableType: flags, type, {mdisp, pdisp, vdisp}, size, copy. */
	"CtInt:     .long 1\n .rva TdInt\n .long 0, -1, 0, 4, 0\n"
	"CtDerived: .long 0\n .rva TdDerived\n .long 0, -1, 0, 8, 0\n"
	"CtBase:    .long 0\n .rva TdBase\n .long 4, -1, 0, 4, 0\n"
	"CtObj:     .long 0\n .rva TdObj\n .long 0, -1, 0, 4, 0\n"
	"CtaInt:     .long 1\n .rva CtInt\n"
	"CtaDerived: .long 2\n .rva CtDerived\n .rva CtBase\n"
	"CtaObj:     .long 1\n .rva CtObj\n"
	/* ThrowInfo: flags, destructor, custom handler, type array. */
	"ThrowInt:     .long 0, 0, 0\n .rva CtaInt\n"
	"ThrowDerived: .long 0, 0, 0\n .rva CtaDerived\n"
	"ThrowObj:     .long 0\n .rva obj_dtor\n .long 0\n .rva CtaObj\n"
	".text\n"

	".section .drectve\n"
	"	.ascii \" -export:cxx64_int -export:cxx64_all\"\n"
	"	.ascii \" -export:cxx64_base -export:cxx64_obj\"\n"
	"	.ascii \" -export:cxx64_rethrow -export:cxx64_rethrow_obj\"\n"
	"	.ascii \" -export:cxx64_wrongtype -export:cxx64_badcast\"\n"
	"	.ascii \" -export:cxx64_badtypeid -export:rt_typeid\"\n"
	"	.ascii \" -export:rt_cast_ok -export:rt_cast_null\"\n"
	"	.ascii \" -export:rt_cast_void -export:cxx64_xrange\"\n"
	"	.ascii \" -export:cxx64_xruntime -export:cxx64_xalloc\"\n"
	".text\n");

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
