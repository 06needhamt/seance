/*
 * __try/__except/__finally as MSVC compiles them for a DLL with a dynamic
 * CRT, against msvcrt's _except_handler3 (VC6-style frames) and
 * _except_handler4_common (VC8-style, with the scope table XORed with a
 * cookie). mingw has no __try, so the functions below are written out the
 * way cl emits them: the frame layout, try-level updates, scope tables,
 * filters and __finally blocks as pieces of the function run with its
 * %ebp, __except blocks that restore %esp themselves, and
 * _local_unwind2/4 for a return out of a __try.
 *
 * Each case can be triggered by an access violation or by RaiseException
 * (sehcrt_run's argument), the latter needing no signal handling.
 * sehcrt_run returns 0, or the number of the first check that failed.
 */
#include "fixture.h"

int use_raise;
int cont_value = 77;
int nest_count, nest_abnormal;
int inner_count, inner_abnormal;
int ret_count, ret_abnormal;
int ft_count, ft_abnormal;
UINT_PTR eh4_cookie = 0x5eed1234;

#define RAISED_CODE 0xE0001234

/* The frame of a function with __try, as cl builds it. */
__asm__(
	".macro EH3_PROLOGUE table\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"pushl $-1\n\t"				/* ebp-0x04: try level */
	"pushl $\\table\n\t"			/* ebp-0x08: scope table */
	"pushl __imp___except_handler3\n\t"	/* ebp-0x0c: handler */
	"movl %fs:0, %eax\n\t"
	"pushl %eax\n\t"			/* ebp-0x10: next frame */
	"movl %esp, %fs:0\n\t"
	"subl $8, %esp\n\t"			/* ebp-0x14, ebp-0x18 */
	"pushl %ebx\n\t"
	"pushl %esi\n\t"
	"pushl %edi\n\t"
	"movl %esp, -0x18(%ebp)\n"
	".endm\n"

	".macro EH4_PROLOGUE table\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"pushl $-2\n\t"
	"pushl $\\table\n\t"
	"pushl $_eh4_handler\n\t"
	"movl %fs:0, %eax\n\t"
	"pushl %eax\n\t"
	"movl _eh4_cookie, %eax\n\t"
	"xorl %eax, -8(%ebp)\n\t"
	"movl %esp, %fs:0\n\t"
	"subl $8, %esp\n\t"
	"pushl %ebx\n\t"
	"pushl %esi\n\t"
	"pushl %edi\n\t"
	"movl %esp, -0x18(%ebp)\n"
	".endm\n"

	".macro EH_EPILOGUE\n\t"
	"leal -0x24(%ebp), %esp\n\t"
	"movl -0x10(%ebp), %ecx\n\t"
	"movl %ecx, %fs:0\n\t"
	"popl %edi\n\t"
	"popl %esi\n\t"
	"popl %ebx\n\t"
	"movl %ebp, %esp\n\t"
	"popl %ebp\n\t"
	"ret\n"
	".endm\n"

	".text\n"
	/* Faults reading NULL, or raises RAISED_CODE; returns %eax. */
	"_trigger:\n\t"
	"cmpl $0, _use_raise\n\t"
	"jne 1f\n\t"
	"xorl %eax, %eax\n\t"
	"movl (%eax), %eax\n\t"
	"ret\n"
	"1:\n\t"
	"pushl $0\n\t"
	"pushl $0\n\t"
	"pushl $0\n\t"
	"pushl $0xE0001234\n\t"
	"call *__imp__RaiseException@16\n\t"
	"movl _cont_value, %eax\n\t"
	"ret\n"

	/* Filters, run with their function's %ebp. */
	"_filter_ours:\n\t"		/* the exception trigger makes */
	"movl -0x14(%ebp), %eax\n\t"
	"movl (%eax), %eax\n\t"
	"movl (%eax), %eax\n\t"
	"cmpl $0xC0000005, %eax\n\t"
	"je 1f\n\t"
	"cmpl $0xE0001234, %eax\n\t"
	"je 1f\n\t"
	"xorl %eax, %eax\n\t"
	"ret\n"
	"1:\n\t"
	"movl $1, %eax\n\t"
	"ret\n"
	"_filter_decline:\n\t"
	"xorl %eax, %eax\n\t"
	"ret\n"
	"_filter_accept:\n\t"
	"movl $1, %eax\n\t"
	"ret\n"
	/* Points the faulting read at cont_value and resumes it. */
	"_filter_continue:\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"movl 4(%eax), %eax\n\t"		/* ContextRecord */
	"movl $_cont_value, 0xb0(%eax)\n\t"	/* ->Eax */
	"movl $-1, %eax\n\t"
	"ret\n"

	/* int eh3_catch(void): __try { trigger(); return 0; }
	 * __except (ours) { return 1; } */
	".globl _eh3_catch\n"
	"_eh3_catch:\n\t"
	"EH3_PROLOGUE t_catch\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _trigger\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"xorl %eax, %eax\n\t"
	"jmp 1f\n"
	"h_catch:\n\t"
	"movl -0x18(%ebp), %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $1, %eax\n"
	"1:\n\t"
	"EH_EPILOGUE\n"

	/* int eh3_continue(void): __try { return trigger(); }
	 * __except (fix it up, EXCEPTION_CONTINUE_EXECUTION) { return -2; } */
	".globl _eh3_continue\n"
	"_eh3_continue:\n\t"
	"EH3_PROLOGUE t_cont\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _trigger\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"jmp 1f\n"
	"h_cont:\n\t"
	"movl -0x18(%ebp), %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-2, %eax\n"
	"1:\n\t"
	"EH_EPILOGUE\n"

	/*
	 * int eh3_nested(void):
	 * __try {					level 0
	 *	__try {					level 1
	 *		__try { trigger(); }		level 2
	 *		__finally { nest_abnormal = AbnormalTermination();
	 *			    nest_count++; }
	 *	} __except (EXCEPTION_CONTINUE_SEARCH) { return -5; }
	 * } __except (EXCEPTION_EXECUTE_HANDLER) {
	 *	return nest_count * 10 + nest_abnormal;
	 * }
	 */
	".globl _eh3_nested\n"
	"_eh3_nested:\n\t"
	"EH3_PROLOGUE t_nest\n\t"
	"movl $0, -4(%ebp)\n\t"
	"movl $1, -4(%ebp)\n\t"
	"movl $2, -4(%ebp)\n\t"
	"call _trigger\n\t"
	"movl $1, -4(%ebp)\n\t"
	"call f_nest\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-6, %eax\n\t"
	"jmp 1f\n"
	"f_nest:\n\t"
	"call *__imp___abnormal_termination\n\t"
	"movl %eax, _nest_abnormal\n\t"
	"incl _nest_count\n\t"
	"ret\n"
	"h_nest1:\n\t"
	"movl -0x18(%ebp), %esp\n\t"
	"movl $0, -4(%ebp)\n\t"
	"movl $-5, %eax\n\t"
	"jmp 1f\n"
	"h_nest0:\n\t"
	"movl -0x18(%ebp), %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"imull $10, _nest_count, %eax\n\t"
	"addl _nest_abnormal, %eax\n"
	"1:\n\t"
	"EH_EPILOGUE\n"

	/*
	 * int eh3_outer(void): __try { eh3_inner(); return -7; }
	 * __except (EXCEPTION_EXECUTE_HANDLER) {
	 *	return inner_count * 10 + inner_abnormal;
	 * }
	 * with eh3_inner: __try { trigger(); }
	 * __finally { inner_abnormal = AbnormalTermination(); inner_count++; }
	 */
	".globl _eh3_outer\n"
	"_eh3_outer:\n\t"
	"EH3_PROLOGUE t_outer\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _eh3_inner\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-7, %eax\n\t"
	"jmp 1f\n"
	"h_outer:\n\t"
	"movl -0x18(%ebp), %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"imull $10, _inner_count, %eax\n\t"
	"addl _inner_abnormal, %eax\n"
	"1:\n\t"
	"EH_EPILOGUE\n"
	"_eh3_inner:\n\t"
	"EH3_PROLOGUE t_inner\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _trigger\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"call f_inner\n\t"
	"EH_EPILOGUE\n"
	"f_inner:\n\t"
	"call *__imp___abnormal_termination\n\t"
	"movl %eax, _inner_abnormal\n\t"
	"incl _inner_count\n\t"
	"ret\n"

	/* int eh3_return(void): __try { return 5; }
	 * __finally { ret_abnormal = AbnormalTermination(); ret_count++; } */
	".globl _eh3_return\n"
	"_eh3_return:\n\t"
	"EH3_PROLOGUE t_ret\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $-1\n\t"
	"leal -0x10(%ebp), %eax\n\t"
	"pushl %eax\n\t"
	"call *__imp___local_unwind2\n\t"
	"addl $8, %esp\n\t"
	"movl $5, %eax\n\t"
	"EH_EPILOGUE\n"
	"f_ret:\n\t"
	"call *__imp___abnormal_termination\n\t"
	"movl %eax, _ret_abnormal\n\t"
	"incl _ret_count\n\t"
	"ret\n"

	/* int eh3_fallthrough(void): __try { } __finally
	 * { ft_abnormal = AbnormalTermination(); ft_count++; } return 6; */
	".globl _eh3_fallthrough\n"
	"_eh3_fallthrough:\n\t"
	"EH3_PROLOGUE t_ft\n\t"
	"movl $0, -4(%ebp)\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"call f_ft\n\t"
	"movl $6, %eax\n\t"
	"EH_EPILOGUE\n"
	"f_ft:\n\t"
	"call *__imp___abnormal_termination\n\t"
	"movl %eax, _ft_abnormal\n\t"
	"incl _ft_count\n\t"
	"ret\n"

	/* The DLL's own _except_handler4, as the static part of the CRT
	 * links it into every /MD image. */
	"_eh4_handler:\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"pushl 20(%ebp)\n\t"
	"pushl 16(%ebp)\n\t"
	"pushl 12(%ebp)\n\t"
	"pushl 8(%ebp)\n\t"
	"pushl $_eh4_check\n\t"
	"pushl $_eh4_cookie\n\t"
	"call *__imp___except_handler4_common\n\t"
	"addl $24, %esp\n\t"
	"popl %ebp\n\t"
	"ret\n"
	"_eh4_check:\n\t"
	"ret\n"

	/* int eh4_catch(void): as eh3_catch. */
	".globl _eh4_catch\n"
	"_eh4_catch:\n\t"
	"EH4_PROLOGUE t4_catch\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _trigger\n\t"
	"movl $-2, -4(%ebp)\n\t"
	"xorl %eax, %eax\n\t"
	"jmp 1f\n"
	"h4_catch:\n\t"
	"movl -0x18(%ebp), %esp\n\t"
	"movl $-2, -4(%ebp)\n\t"
	"movl $1, %eax\n"
	"1:\n\t"
	"EH_EPILOGUE\n"

	/* int eh4_nested(void): as eh3_nested, counting into the same
	 * variables. */
	".globl _eh4_nested\n"
	"_eh4_nested:\n\t"
	"EH4_PROLOGUE t4_nest\n\t"
	"movl $0, -4(%ebp)\n\t"
	"movl $1, -4(%ebp)\n\t"
	"movl $2, -4(%ebp)\n\t"
	"call _trigger\n\t"
	"movl $1, -4(%ebp)\n\t"
	"call f_nest\n\t"
	"movl $-2, -4(%ebp)\n\t"
	"movl $-6, %eax\n\t"
	"jmp 1f\n"
	"h4_nest1:\n\t"
	"movl -0x18(%ebp), %esp\n\t"
	"movl $0, -4(%ebp)\n\t"
	"movl $-5, %eax\n\t"
	"jmp 1f\n"
	"h4_nest0:\n\t"
	"movl -0x18(%ebp), %esp\n\t"
	"movl $-2, -4(%ebp)\n\t"
	"imull $10, _nest_count, %eax\n\t"
	"addl _nest_abnormal, %eax\n"
	"1:\n\t"
	"EH_EPILOGUE\n"

	/* int eh4_return(void): as eh3_return, through _local_unwind4. */
	".globl _eh4_return\n"
	"_eh4_return:\n\t"
	"EH4_PROLOGUE t4_ret\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $-2\n\t"
	"leal -0x10(%ebp), %eax\n\t"
	"pushl %eax\n\t"
	"pushl $_eh4_cookie\n\t"
	"call *__imp___local_unwind4\n\t"
	"addl $12, %esp\n\t"
	"movl $5, %eax\n\t"
	"EH_EPILOGUE\n"

	/* Scope tables: enclosing level, filter (0 for __finally), block. */
	".section .rdata\n"
	".align 4\n"
	"t_catch: .long -1, _filter_ours, h_catch\n"
	"t_cont: .long -1, _filter_continue, h_cont\n"
	"t_nest: .long -1, _filter_accept, h_nest0\n\t"
	".long 0, _filter_decline, h_nest1\n\t"
	".long 1, 0, f_nest\n"
	"t_outer: .long -1, _filter_accept, h_outer\n"
	"t_inner: .long -1, 0, f_inner\n"
	"t_ret: .long -1, 0, f_ret\n"
	"t_ft: .long -1, 0, f_ft\n"
	/* EH4: GS cookie offset (-2: none), GS XOR, EH cookie, EH XOR. */
	"t4_catch: .long -2, 0, -0x1c, 0\n\t"
	".long -2, _filter_ours, h4_catch\n"
	"t4_nest: .long -2, 0, -0x1c, 0\n\t"
	".long -2, _filter_accept, h4_nest0\n\t"
	".long 0, _filter_decline, h4_nest1\n\t"
	".long 1, 0, f_nest\n"
	"t4_ret: .long -2, 0, -0x1c, 0\n\t"
	".long -2, 0, f_ret\n"
	".text\n"
);

int eh3_catch(void);
int eh3_continue(void);
int eh3_nested(void);
int eh3_outer(void);
int eh3_return(void);
int eh3_fallthrough(void);
int eh4_catch(void);
int eh4_nested(void);
int eh4_return(void);

static void *chain_head(void)
{
	void *p;

	__asm__ __volatile__("movl %%fs:0, %0" : "=r"(p));
	return p;
}

EXPORT int sehcrt_run(int raise)
{
	void *head = chain_head();

	use_raise = raise;
	if (eh3_catch() != 1)
		return 1;
	if (eh3_continue() != 77)
		return 2;
	nest_count = nest_abnormal = 0;
	if (eh3_nested() != 11)
		return 3;
	inner_count = inner_abnormal = 0;
	if (eh3_outer() != 11)
		return 4;
	ret_count = ret_abnormal = 0;
	if (eh3_return() != 5 || ret_count != 1 || ret_abnormal != 1)
		return 5;
	ft_count = 0;
	ft_abnormal = 1;
	if (eh3_fallthrough() != 6 || ft_count != 1 || ft_abnormal != 0)
		return 6;
	if (eh4_catch() != 1)
		return 7;
	nest_count = nest_abnormal = 0;
	if (eh4_nested() != 11)
		return 8;
	ret_count = ret_abnormal = 0;
	if (eh4_return() != 5 || ret_count != 1 || ret_abnormal != 1)
		return 9;
	if (chain_head() != head)
		return 10;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
