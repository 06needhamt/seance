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
 * EH4 frames carry the GS and EH cookies, which _except_handler4_common
 * has the DLL's own check function (here, one that counts) look at; and
 * setjmp/longjmp unwind __try frames, through _seh_longjmp_unwind or, for
 * the old _setjmp, on their own.
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
int eh4_checks, eh4_bad;
int sj_count, sj_inner_count;
int sj_buf[16];

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

	/* EH4's has the GS cookie at ebp-0x1c, XORed with ebp+8, and the EH
	 * one at ebp-0x2c, XORed with ebp+4 (their tables say so). */
	".macro EH4_PROLOGUE table\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"pushl $-2\n\t"
	"pushl $\\table\n\t"
	"pushl $_eh4_handler\n\t"
	"movl %fs:0, %eax\n\t"
	"pushl %eax\n\t"
	"subl $12, %esp\n\t"
	"pushl %ebx\n\t"
	"pushl %esi\n\t"
	"pushl %edi\n\t"
	"movl _eh4_cookie, %eax\n\t"
	"xorl %eax, -8(%ebp)\n\t"
	"leal 8(%ebp), %edx\n\t"
	"xorl %eax, %edx\n\t"
	"movl %edx, -0x1c(%ebp)\n\t"
	"leal 4(%ebp), %edx\n\t"
	"xorl %edx, %eax\n\t"
	"pushl %eax\n\t"
	"leal -0x10(%ebp), %eax\n\t"
	"movl %eax, %fs:0\n\t"
	"movl %esp, -0x18(%ebp)\n"
	".endm\n"

	".macro EH4_EPILOGUE\n\t"
	"leal -0x28(%ebp), %esp\n\t"
	"movl -0x10(%ebp), %ecx\n\t"
	"movl %ecx, %fs:0\n\t"
	"popl %edi\n\t"
	"popl %esi\n\t"
	"popl %ebx\n\t"
	"movl %ebp, %esp\n\t"
	"popl %ebp\n\t"
	"ret\n"
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
	/* __security_check_cookie (fastcall): counts instead of ending it all. */
	"_eh4_check:\n\t"
	"incl _eh4_checks\n\t"
	"cmpl _eh4_cookie, %ecx\n\t"
	"je 1f\n\t"
	"incl _eh4_bad\n"
	"1:\n\t"
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
	"EH4_EPILOGUE\n"

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
	"EH4_EPILOGUE\n"

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
	"EH4_EPILOGUE\n"

	/* int eh4_smashed(int offset): as eh4_catch, the cookie at
	 * ebp+offset (GS -0x1c, EH -0x2c) overwritten. */
	".globl _eh4_smashed\n"
	"_eh4_smashed:\n\t"
	"EH4_PROLOGUE t4_smash\n\t"
	"movl 8(%ebp), %eax\n\t"
	"notl (%ebp,%eax)\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _trigger\n\t"
	"movl $-2, -4(%ebp)\n\t"
	"xorl %eax, %eax\n\t"
	"jmp 1f\n"
	"h4_smashed:\n\t"
	"movl -0x18(%ebp), %esp\n\t"
	"movl $-2, -4(%ebp)\n\t"
	"movl $1, %eax\n"
	"1:\n\t"
	"EH4_EPILOGUE\n"

	/*
	 * int eh3_setjmp(int old):
	 * __try {						level 0
	 *	int r = old ? _setjmp(sj_buf) : setjmp(sj_buf);	(_setjmp3, 2 args)
	 *	if (r) { r = r * 10 + sj_count + 100 * sj_inner_count;
	 *		 (still at level 0) } else
	 *	__try { sj_inner(); } __finally { sj_count++; }	level 1
	 * } __finally { sj_count += 1000; }
	 * return r + 1000;	(once the outer __finally has run)
	 * with sj_inner: __try { longjmp(sj_buf, 3); } __finally { sj_inner_count++; }
	 */
	".globl _eh3_setjmp\n"
	"_eh3_setjmp:\n\t"
	"EH3_PROLOGUE t_sj\n\t"
	"movl $0, -4(%ebp)\n\t"
	"cmpl $0, 8(%ebp)\n\t"
	"je 2f\n\t"
	"pushl $_sj_buf\n\t"
	"call *__imp___setjmp\n\t"
	"addl $4, %esp\n\t"
	"jmp 3f\n"
	"2:\n\t"
	"pushl $0\n\t"
	"pushl __imp___seh_longjmp_unwind\n\t"
	"pushl $2\n\t"
	"pushl $_sj_buf\n\t"
	"call *__imp___setjmp3\n\t"
	"addl $16, %esp\n"
	"3:\n\t"
	"testl %eax, %eax\n\t"
	"jnz 4f\n\t"
	"movl $1, -4(%ebp)\n\t"
	"call _sj_inner\n\t"
	"movl $-9, %eax\n\t"
	"jmp 5f\n"
	"4:\n\t"
	"imull $10, %eax, %eax\n\t"
	"addl _sj_count, %eax\n\t"
	"imull $100, _sj_inner_count, %ecx\n\t"
	"addl %ecx, %eax\n\t"
	"cmpl $0, -4(%ebp)\n\t"
	"je 6f\n\t"
	"movl $-8, %eax\n\t"
	"jmp 5f\n"
	"6:\n\t"
	"movl %eax, %esi\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"call f_sj0\n\t"
	"movl %esi, %eax\n\t"
	"addl _sj_count, %eax\n\t"
	"subl $1, %eax\n"		/* sj_count is 1001 here */
	"5:\n\t"
	"EH_EPILOGUE\n"
	"f_sj:\n\t"
	"incl _sj_count\n\t"
	"ret\n"
	"f_sj0:\n\t"
	"addl $1000, _sj_count\n\t"
	"ret\n"
	"_sj_inner:\n\t"
	"EH3_PROLOGUE t_sji\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $3\n\t"
	"pushl $_sj_buf\n\t"
	"call *__imp__longjmp\n\t"
	"ud2\n"
	"f_sji:\n\t"
	"incl _sj_inner_count\n\t"
	"ret\n"

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
	"t_sj: .long -1, 0, f_sj0\n\t"
	".long 0, 0, f_sj\n"
	"t_sji: .long -1, 0, f_sji\n"
	/* EH4: GS cookie offset (-2: none), GS XOR, EH cookie, EH XOR. */
	"t4_catch: .long -0x1c, 8, -0x2c, 4\n\t"
	".long -2, _filter_ours, h4_catch\n"
	"t4_smash: .long -0x1c, 8, -0x2c, 4\n\t"
	".long -2, _filter_ours, h4_smashed\n"
	"t4_nest: .long -2, 0, -0x2c, 4\n\t"
	".long -2, _filter_accept, h4_nest0\n\t"
	".long 0, _filter_decline, h4_nest1\n\t"
	".long 1, 0, f_nest\n"
	"t4_ret: .long -2, 0, -0x2c, 4\n\t"
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
int eh4_smashed(int offset);
int eh3_setjmp(int old);

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
	eh4_checks = eh4_bad = 0;
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
	/* Every check the cookies passed, then one smashed fails. */
	if (eh4_checks < 4 || eh4_bad)
		return 11;
	if (eh4_smashed(-0x2c) != 1 || !eh4_bad)
		return 12;
	eh4_bad = 0;
	if (eh4_smashed(-0x1c) != 1 || !eh4_bad)
		return 15;
	/* longjmp runs the __finally blocks it leaves, either setjmp. */
	sj_count = sj_inner_count = 0;
	if (eh3_setjmp(0) != 1131 || sj_count != 1001)
		return 13;
	sj_count = sj_inner_count = 0;
	if (eh3_setjmp(1) != 1131 || sj_count != 1001)
		return 14;
	if (chain_head() != head)
		return 10;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
