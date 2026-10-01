/*
 * C++ exceptions as MSVC compiles them for a DLL with a dynamic CRT,
 * against msvcrt's __CxxFrameHandler and _CxxThrowException. mingw has
 * no MSVC-style C++ EH, so the functions below are written out the way
 * cl emits them: the frame with its state variable, the FuncInfo tables
 * (unwind map, try blocks, catch blocks), catch blocks as pieces of the
 * function run with its %ebp that return where to carry on, and
 * destructors in the unwind map. Thrown objects live in the throwing C
 * function's frame, as a throw's temporary does.
 *
 * cxx_run returns 0, or the number of the first check that failed.
 * cxx_unhandled and cxx_noexcept end the process, for a test to watch.
 */
#include "fixture.h"

#define THISCALL __attribute__((thiscall))

/* The compiler's tables, as msvcrt reads them. */
struct type_desc {
	const void *vtable;
	char *name;
	char mangled[24];
};

struct unwind_entry {
	int prev;
	void *handler;
};

struct catch_block {
	UINT flags;
	const struct type_desc *type;
	int offset;
	void *handler;
};

struct try_block {
	int start, end, catch_level;
	UINT ncatch;
	const struct catch_block *catches;
};

struct func_info {
	UINT magic;
	UINT nunwind;
	const struct unwind_entry *unwind;
	UINT ntry;
	const struct try_block *tries;
	UINT nipmap;
	const void *ipmap;
	const void *expect;
	UINT flags;
};

struct cxx_type {
	UINT flags;
	const struct type_desc *type;
	int this_offset, vbase_descr, vbase_offset;
	UINT size;
	void *copy_ctor;
};

struct type_table {
	UINT count;
	const struct cxx_type *types[2];
};

struct throw_info {
	UINT flags;
	void *dtor;
	void *custom;
	const struct type_table *table;
};

#define VC6 0x19930520
#define VC8 0x19930522
#define REF 8
#define SIMPLE 1

__declspec(dllimport) void __stdcall _CxxThrowException(void *, const void *);
extern const void *const type_info_vtable __asm__("\"__imp_??_7type_info@@6B@\"");

/* What happened, in order. */
char trail[32];
int dtors, copies, uncaught_in_dtor, dtors_in_catch;
void *thrown_at, *caught_at;

void note(int c)
{
	int n = 0;
	while (trail[n] && n < 30)
		n++;
	trail[n] = (char)c;
	trail[n + 1] = 0;
}

static int (__cdecl *uncaught)(void);

void check_uncaught(void)
{
	uncaught_in_dtor = uncaught();
}

/* An object whose base class sits 4 bytes in. */
struct derived {
	int tag;
	int value;	/* the base */
};

static void THISCALL derived_dtor(struct derived *d)
{
	dtors++;
	note(d->tag);
}

static void THISCALL derived_copy(struct derived *dst, const struct derived *src)
{
	*dst = *src;
	copies++;
}

static struct type_desc td_int = { NULL, NULL, ".H" };
static struct type_desc td_char = { NULL, NULL, ".D" };
static struct type_desc td_derived = { NULL, NULL, ".?AVDerived@@" };
static struct type_desc td_base = { NULL, NULL, ".?AVBase@@" };
/* Another descriptor for the same type, as another module has: matched by name. */
static struct type_desc td_base2 = { NULL, NULL, ".?AVBase@@" };

static const struct cxx_type ct_int = { SIMPLE, &td_int, 0, -1, 0, 4, NULL };
static const struct type_table tt_int = { 1, { &ct_int } };
static const struct throw_info ti_int = { 0, NULL, NULL, &tt_int };

static const struct cxx_type ct_derived = {
	0, &td_derived, 0, -1, 0, 8, (void *)derived_copy };
static const struct cxx_type ct_base = { 0, &td_base, 4, -1, 0, 4, NULL };
static const struct type_table tt_derived = { 2, { &ct_derived, &ct_base } };
static const struct throw_info ti_derived = {
	0, (void *)derived_dtor, NULL, &tt_derived };

void throw_int(int v)
{
	int object = v;
	_CxxThrowException(&object, &ti_int);
}

void throw_derived(int tag, int value)
{
	struct derived d = { tag, value };
	thrown_at = &d;
	_CxxThrowException(&d, &ti_derived);
}

static const struct throw_info ti_const_int = { 1, NULL, NULL, &tt_int };

void throw_const_int(int v)
{
	const int object = v;
	_CxxThrowException((void *)&object, &ti_const_int);
}

void raise_seh(void)
{
	RaiseException(0xE0000001, 0, 0, NULL);
}

/* The functions, as cl lays them out. */
__asm__(
	".macro CXX_PROLOGUE handler\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"pushl $-1\n\t"			/* ebp-0x04: state */
	"pushl $\\handler\n\t"		/* ebp-0x08: handler */
	"movl %fs:0, %eax\n\t"
	"pushl %eax\n\t"		/* ebp-0x0c: next frame */
	"movl %esp, %fs:0\n\t"
	"subl $0x20, %esp\n\t"		/* ebp-0x10 saved esp, locals */
	"pushl %ebx\n\t"
	"pushl %esi\n\t"
	"pushl %edi\n\t"
	"movl %esp, -0x10(%ebp)\n"
	".endm\n"

	".macro CXX_EPILOGUE\n\t"
	"movl -0xc(%ebp), %ecx\n\t"
	"movl %ecx, %fs:0\n\t"
	"leal -0x38(%ebp), %esp\n\t"
	"popl %edi\n\t"
	"popl %esi\n\t"
	"popl %ebx\n\t"
	"movl %ebp, %esp\n\t"
	"popl %ebp\n\t"
	"ret\n"
	".endm\n"

	".macro CXX_HANDLER name, info, handler=__imp____CxxFrameHandler\n"
	"\\name:\n\t"
	"movl $\\info, %eax\n\t"
	"jmp *\\handler\n"
	".endm\n"

	".text\n"

	/* int catch_int(int v): try { throw_int(v); } catch (int e) { return e + 1; } */
	".globl _catch_int\n"
	"_catch_int:\n\t"
	"CXX_PROLOGUE h_int\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl 8(%ebp)\n\t"
	"call _throw_int\n\t"
	"addl $4, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_int_catch:\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"incl %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_int, _fi_int\n"

	/*
	 * int catch_base(void): try { throw_derived('D', 7); }
	 * catch (Base &b) { caught_at = &b; return b.value * 10 + dtors; }
	 * (through __CxxFrameHandler3, as VC8 on uses)
	 */
	".globl _catch_base\n"
	"_catch_base:\n\t"
	"CXX_PROLOGUE h_base\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $7\n\t"
	"pushl $68\n\t"
	"call _throw_derived\n\t"
	"addl $8, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_base_catch:\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"movl %eax, _caught_at\n\t"
	"imull $10, (%eax), %eax\n\t"
	"addl _dtors, %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_base, _fi_base, __imp____CxxFrameHandler3\n"

	/* int catch_copy(void): try { throw_derived('C', 5); }
	 * catch (Derived d) { return d.value * 10 + copies; } */
	".globl _catch_copy\n"
	"_catch_copy:\n\t"
	"CXX_PROLOGUE h_copy\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $5\n\t"
	"pushl $67\n\t"
	"call _throw_derived\n\t"
	"addl $8, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_copy_catch:\n\t"
	"imull $10, -0x1c(%ebp), %eax\n\t"	/* the copy's value */
	"addl _copies, %eax\n\t"
	"movl %eax, -0x28(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x28(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_copy, _fi_copy\n"

	/*
	 * void unwind_mid(void): Obj a('1'); Obj b('2'); throw_int(5);
	 * - no try block: its destructors run as the exception passes.
	 */
	"_unwind_mid:\n\t"
	"CXX_PROLOGUE h_mid\n\t"
	"movl $0, -4(%ebp)\n\t"		/* a constructed */
	"movl $1, -4(%ebp)\n\t"		/* b constructed */
	"pushl $5\n\t"
	"call _throw_int\n\t"
	"addl $4, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"CXX_EPILOGUE\n"
	"_mid_dtor_a:\n\t"
	"pushl $49\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"ret\n"
	"_mid_dtor_b:\n\t"
	"call _check_uncaught\n\t"
	"pushl $50\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"ret\n"
	"CXX_HANDLER h_mid, _fi_mid\n"

	/* int catch_unwind(void): try { unwind_mid(); } catch (...) { note('C'); return 1; } */
	".globl _catch_unwind\n"
	"_catch_unwind:\n\t"
	"CXX_PROLOGUE h_unw\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _unwind_mid\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_unw_catch:\n\t"
	"pushl $67\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $1, %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_unw, _fi_unw\n"

	/* void no_match(void): try { throw_int(3); } catch (char) { note('x'); } */
	"_no_match:\n\t"
	"CXX_PROLOGUE h_nom\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $3\n\t"
	"call _throw_int\n\t"
	"addl $4, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"jmp 1f\n"
	"_nom_catch:\n\t"
	"pushl $120\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"movl $1f, %eax\n\t"
	"ret\n"
	"1:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_nom, _fi_nom\n"

	/* int catch_outer(void): try { no_match(); } catch (int e) { return e; } */
	".globl _catch_outer\n"
	"_catch_outer:\n\t"
	"CXX_PROLOGUE h_out\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _no_match\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_out_catch:\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_out, _fi_out\n"

	/* void rethrower(void): try { throw_derived('R', 4); } catch (...) { note('r'); throw; } */
	"_rethrower:\n\t"
	"CXX_PROLOGUE h_rth\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $4\n\t"
	"pushl $82\n\t"
	"call _throw_derived\n\t"
	"addl $8, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"CXX_EPILOGUE\n"
	"_rth_catch:\n\t"
	"pushl $114\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"pushl $0\n\t"
	"pushl $0\n\t"
	"call *__imp___CxxThrowException@8\n\t"
	"ud2\n"
	"CXX_HANDLER h_rth, _fi_rth\n"

	/*
	 * int catch_rethrown(void): try { rethrower(); }
	 * catch (Base &b) { caught_at = &b; dtors_in_catch = dtors; return b.value; }
	 */
	".globl _catch_rethrown\n"
	"_catch_rethrown:\n\t"
	"CXX_PROLOGUE h_crt\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _rethrower\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_crt_catch:\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"movl %eax, _caught_at\n\t"
	"movl _dtors, %ecx\n\t"
	"movl %ecx, _dtors_in_catch\n\t"
	"movl (%eax), %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_crt, _fi_crt\n"

	/* void thrower_in_catch(void): try { throw_derived('T', 1); } catch (Base &) { throw_int(9); } */
	"_thrower_in_catch:\n\t"
	"CXX_PROLOGUE h_tic\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $1\n\t"
	"pushl $84\n\t"
	"call _throw_derived\n\t"
	"addl $8, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"CXX_EPILOGUE\n"
	"_tic_catch:\n\t"
	"pushl $9\n\t"
	"call _throw_int\n\t"
	"ud2\n"
	"CXX_HANDLER h_tic, _fi_tic\n"

	/* int catch_new(void): try { thrower_in_catch(); }
	 * catch (int e) { dtors_in_catch = dtors; return e; } */
	".globl _catch_new\n"
	"_catch_new:\n\t"
	"CXX_PROLOGUE h_cnw\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _thrower_in_catch\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_cnw_catch:\n\t"
	"movl _dtors, %ecx\n\t"
	"movl %ecx, _dtors_in_catch\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_cnw, _fi_cnw\n"

	/*
	 * int catch_in_catch(void):
	 * try { throw_int(1); }				state 0
	 * catch (int e) {					state 1
	 *	try { throw_int(20); }				state 2
	 *	catch (int e2) { r = e * 100 + e2; }		state 3
	 * }
	 * return r;
	 */
	".globl _catch_in_catch\n"
	"_catch_in_catch:\n\t"
	"CXX_PROLOGUE h_cic\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $1\n\t"
	"call _throw_int\n\t"
	"addl $4, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_cic_outer:\n\t"
	"movl $2, -4(%ebp)\n\t"
	"movl %esp, -0x10(%ebp)\n\t"	/* for after the inner catch */
	"pushl $20\n\t"
	"call _throw_int\n\t"
	"ud2\n"
	"_cic_inner:\n\t"
	"movl $3f, %eax\n\t"
	"ret\n"
	"3:\n\t"			/* back in the outer catch block */
	"movl $1, -4(%ebp)\n\t"
	"imull $100, -0x14(%ebp), %eax\n\t"
	"addl -0x24(%ebp), %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"			/* %esp as the body had it */
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n\t"
	"leal -0x38(%ebp), %ecx\n\t"
	"cmpl %ecx, %esp\n\t"
	"je 1f\n\t"
	"movl $-7, %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_cic, _fi_cic\n"

	/*
	 * int sync_only(void), /EHs (VC8, synchronous):
	 * try { raise_seh(); } catch (...) { note('s'); } - which doesn't
	 * take an SEH exception, so it goes on.
	 */
	"_sync_only:\n\t"
	"CXX_PROLOGUE h_syn\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _raise_seh\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"jmp 1f\n"
	"_syn_catch:\n\t"
	"pushl $115\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"movl $1f, %eax\n\t"
	"ret\n"
	"1:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_syn, _fi_syn\n"

	/* int catch_seh(void), /EHa: try { sync_only(); } catch (...) { note('a'); return 9; } */
	".globl _catch_seh\n"
	"_catch_seh:\n\t"
	"CXX_PROLOGUE h_seh\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _sync_only\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_seh_typed:\n\t"		/* catch (int): never for an SEH exception */
	"pushl $105\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"_seh_catch:\n\t"
	"pushl $97\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $9, %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_seh, _fi_seh\n"

	/*
	 * int catch_across(void):
	 * try {						state 0
	 *	try { throw_derived('X', 1); }			state 1
	 *	catch (Base &) { throw_int(8); }		state 2
	 * } catch (int e) { dtors_in_catch = dtors; return e; }	state 3
	 * - the exception out of the inner catch block is the outer try's.
	 */
	".globl _catch_across\n"
	"_catch_across:\n\t"
	"CXX_PROLOGUE h_acr\n\t"
	"movl $0, -4(%ebp)\n\t"
	"movl $1, -4(%ebp)\n\t"
	"pushl $1\n\t"
	"pushl $88\n\t"
	"call _throw_derived\n\t"
	"addl $8, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_acr_inner:\n\t"
	"pushl $8\n\t"
	"call _throw_int\n\t"
	"ud2\n"
	"_acr_outer:\n\t"
	"movl _dtors, %ecx\n\t"
	"movl %ecx, _dtors_in_catch\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_acr, _fi_acr\n"

	/*
	 * int catch_local(void): try { Obj a('L'); throw_int(5); }
	 * catch (...) { note('C'); return 1; } - a goes before the catch runs.
	 */
	".globl _catch_local\n"
	"_catch_local:\n\t"
	"CXX_PROLOGUE h_loc\n\t"
	"movl $0, -4(%ebp)\n\t"
	"movl $1, -4(%ebp)\n\t"		/* a constructed */
	"pushl $5\n\t"
	"call _throw_int\n\t"
	"addl $4, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_loc_dtor:\n\t"
	"pushl $76\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"ret\n"
	"_loc_catch:\n\t"
	"pushl $67\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $1, %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_loc, _fi_loc\n"

	/*
	 * int catch_const(void): try { throw (const int)6; }
	 * catch (int &) { note('n'); } catch (const int &e) { return e; }
	 */
	".globl _catch_const\n"
	"_catch_const:\n\t"
	"CXX_PROLOGUE h_cst\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $6\n\t"
	"call _throw_const_int\n\t"
	"addl $4, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_cst_plain:\n\t"
	"pushl $110\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"movl $-5, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"_cst_const:\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"movl (%eax), %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_cst, _fi_cst\n"

	/* void noexcept_fn(void) noexcept (VC8): throw_int(1); */
	".globl _noexcept_fn\n"
	"_noexcept_fn:\n\t"
	"CXX_PROLOGUE h_nex\n\t"
	"pushl $1\n\t"
	"call _throw_int\n\t"
	"addl $4, %esp\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_nex, _fi_nex\n"
);

extern char int_catch[], base_catch[], copy_catch[], mid_dtor_a[],
	mid_dtor_b[], unw_catch[], nom_catch[], out_catch[], rth_catch[],
	crt_catch[], tic_catch[], cnw_catch[], cic_outer[], cic_inner[],
	syn_catch[], seh_catch[], seh_typed[], acr_inner[], acr_outer[],
	loc_dtor[], loc_catch[], cst_plain[], cst_const[];

/* A try at state 0 whose catch blocks run at state 1. */
static const struct unwind_entry unwind2[] = { { -1, NULL }, { -1, NULL } };

static const struct catch_block cb_int[] = { { 0, &td_int, -0x14, int_catch } };
static const struct try_block tb_int[] = { { 0, 0, 1, 1, cb_int } };
const struct func_info fi_int = { VC6, 2, unwind2, 1, tb_int };

static const struct catch_block cb_base[] = {
	{ 0, &td_int, -0x14, NULL },		/* not this one */
	{ REF, &td_base2, -0x14, base_catch },
};
static const struct try_block tb_base[] = { { 0, 0, 1, 2, cb_base } };
const struct func_info fi_base = { VC8, 2, unwind2, 1, tb_base, 0, NULL,
				   NULL, 1 };

static const struct catch_block cb_copy[] = {
	{ 0, &td_derived, -0x20, copy_catch } };
static const struct try_block tb_copy[] = { { 0, 0, 1, 1, cb_copy } };
const struct func_info fi_copy = { VC6, 2, unwind2, 1, tb_copy };

static const struct unwind_entry unwind_mid_map[] = {
	{ -1, mid_dtor_a }, { 0, mid_dtor_b } };
const struct func_info fi_mid = { VC6, 2, unwind_mid_map, 0, NULL };

static const struct catch_block cb_all_unw[] = { { 0, NULL, 0, unw_catch } };
static const struct try_block tb_unw[] = { { 0, 0, 1, 1, cb_all_unw } };
const struct func_info fi_unw = { VC6, 2, unwind2, 1, tb_unw };

static const struct catch_block cb_nom[] = { { 0, &td_char, -0x14, nom_catch } };
static const struct try_block tb_nom[] = { { 0, 0, 1, 1, cb_nom } };
const struct func_info fi_nom = { VC6, 2, unwind2, 1, tb_nom };

static const struct catch_block cb_out[] = { { 0, &td_int, -0x14, out_catch } };
static const struct try_block tb_out[] = { { 0, 0, 1, 1, cb_out } };
const struct func_info fi_out = { VC6, 2, unwind2, 1, tb_out };

static const struct catch_block cb_rth[] = { { 0, NULL, 0, rth_catch } };
static const struct try_block tb_rth[] = { { 0, 0, 1, 1, cb_rth } };
const struct func_info fi_rth = { VC6, 2, unwind2, 1, tb_rth };

static const struct catch_block cb_crt[] = { { REF, &td_base, -0x14, crt_catch } };
static const struct try_block tb_crt[] = { { 0, 0, 1, 1, cb_crt } };
const struct func_info fi_crt = { VC6, 2, unwind2, 1, tb_crt };

static const struct catch_block cb_tic[] = { { REF, &td_base, -0x14, tic_catch } };
static const struct try_block tb_tic[] = { { 0, 0, 1, 1, cb_tic } };
const struct func_info fi_tic = { VC6, 2, unwind2, 1, tb_tic };

static const struct catch_block cb_cnw[] = { { 0, &td_int, -0x14, cnw_catch } };
static const struct try_block tb_cnw[] = { { 0, 0, 1, 1, cb_cnw } };
const struct func_info fi_cnw = { VC6, 2, unwind2, 1, tb_cnw };

static const struct unwind_entry unwind_cic[] = {
	{ -1, NULL }, { -1, NULL }, { 1, NULL }, { 1, NULL } };
static const struct catch_block cb_cic_inner[] = {
	{ 0, &td_int, -0x24, cic_inner } };
static const struct catch_block cb_cic_outer[] = {
	{ 0, &td_int, -0x14, cic_outer } };
static const struct try_block tb_cic[] = {	/* innermost first */
	{ 2, 2, 3, 1, cb_cic_inner },
	{ 0, 0, 3, 1, cb_cic_outer },
};
const struct func_info fi_cic = { VC6, 4, unwind_cic, 2, tb_cic };

static const struct catch_block cb_syn[] = { { 0, NULL, 0, syn_catch } };
static const struct try_block tb_syn[] = { { 0, 0, 1, 1, cb_syn } };
const struct func_info fi_syn = { VC8, 2, unwind2, 1, tb_syn, 0, NULL, NULL, 1 };

static const struct catch_block cb_seh[] = {
	{ 0, &td_int, -0x14, seh_typed }, { 0, NULL, 0, seh_catch } };
static const struct try_block tb_seh[] = { { 0, 0, 1, 2, cb_seh } };
const struct func_info fi_seh = { VC6, 2, unwind2, 1, tb_seh };

static const struct unwind_entry unwind_acr[] = {
	{ -1, NULL }, { 0, NULL }, { 0, NULL }, { -1, NULL } };
static const struct catch_block cb_acr_inner[] = {
	{ REF, &td_base, -0x14, acr_inner } };
static const struct catch_block cb_acr_outer[] = {
	{ 0, &td_int, -0x14, acr_outer } };
static const struct try_block tb_acr[] = {
	{ 1, 1, 2, 1, cb_acr_inner },
	{ 0, 2, 3, 1, cb_acr_outer },
};
const struct func_info fi_acr = { VC6, 4, unwind_acr, 2, tb_acr };

static const struct unwind_entry unwind_loc[] = {
	{ -1, NULL }, { 0, loc_dtor }, { -1, NULL } };
static const struct catch_block cb_loc[] = { { 0, NULL, 0, loc_catch } };
static const struct try_block tb_loc[] = { { 0, 1, 2, 1, cb_loc } };
const struct func_info fi_loc = { VC6, 3, unwind_loc, 1, tb_loc };

static const struct catch_block cb_cst[] = {
	{ REF, &td_int, -0x14, cst_plain },
	{ REF | 1, &td_int, -0x14, cst_const },
};
static const struct try_block tb_cst[] = { { 0, 0, 1, 2, cb_cst } };
const struct func_info fi_cst = { VC6, 2, unwind2, 1, tb_cst };

const struct func_info fi_nex = { VC8, 0, NULL, 0, NULL, 0, NULL, NULL, 4 };

int catch_int(int v);
int catch_base(void);
int catch_copy(void);
int catch_unwind(void);
int catch_outer(void);
int catch_rethrown(void);
int catch_new(void);
int catch_in_catch(void);
int catch_seh(void);
int catch_across(void);
int catch_local(void);
int catch_const(void);
void noexcept_fn(void);

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static void reset(void)
{
	trail[0] = 0;
	dtors = copies = uncaught_in_dtor = dtors_in_catch = 0;
	thrown_at = caught_at = NULL;
}

EXPORT int cxx_run(void)
{
	HMODULE crt = GetModuleHandleA("msvcrt.dll");
	uncaught = (int (__cdecl *)(void))GetProcAddress(crt,
							 "__uncaught_exception");
	if (!uncaught || !type_info_vtable)
		return 1;
	td_int.vtable = td_char.vtable = td_derived.vtable = td_base.vtable =
		td_base2.vtable = type_info_vtable;

	reset();
	if (catch_int(41) != 42 || catch_int(-1) != 0)
		return 2;

	/* By a base-class reference: this adjusted, destroyed once, after. */
	reset();
	if (catch_base() != 70 || dtors != 1 || !streq(trail, "D") ||
	    caught_at != (char *)thrown_at + 4)
		return 3;

	/* By value: copied with the copy constructor. */
	reset();
	if (catch_copy() != 51 || copies != 1 || dtors != 1)
		return 4;

	/* Destructors of the frames in between, innermost state first. */
	reset();
	if (catch_unwind() != 1 || !streq(trail, "21C") || !uncaught_in_dtor)
		return 5;
	if (uncaught())
		return 6;

	/* A catch block for another type passes it on. */
	reset();
	if (catch_outer() != 3 || trail[0])
		return 7;

	/* throw; in a catch block: the same object, destroyed once, at the end. */
	reset();
	if (catch_rethrown() != 4 || !streq(trail, "rR") || dtors != 1 ||
	    dtors_in_catch != 0 || caught_at != (char *)thrown_at + 4)
		return 8;

	/* A new exception out of a catch block ends the old one there. */
	reset();
	if (catch_new() != 9 || dtors != 1 || dtors_in_catch != 1 ||
	    !streq(trail, "T"))
		return 9;

	/* A try block inside a catch block. */
	reset();
	if (catch_in_catch() != 120)
		return 10;

	/* SEH exceptions: catch (...) takes them only without /EHs. */
	reset();
	if (catch_seh() != 9 || !streq(trail, "a"))
		return 11;

	/* Out of an inner catch block, to the try around it. */
	reset();
	if (catch_across() != 8 || dtors_in_catch != 1 || !streq(trail, "X"))
		return 13;

	/* The catching function's own objects in the try go first. */
	reset();
	if (catch_local() != 1 || !streq(trail, "LC"))
		return 14;

	/* A const object only by a const catch. */
	reset();
	if (catch_const() != 6 || trail[0])
		return 15;

	/* And after all that, catching still works (nothing left behind). */
	reset();
	if (catch_int(1) != 2 || catch_base() != 70)
		return 12;
	return 0;
}

/* No catch anywhere: the harness reports it and aborts. */
EXPORT void cxx_unhandled(void)
{
	throw_int(1);
}

static void __cdecl on_terminate(void)
{
	ExitProcess(42);
}

/* Out of a noexcept function: terminate(), here with a handler of ours. */
EXPORT void cxx_noexcept(void)
{
	HMODULE crt = GetModuleHandleA("msvcrt.dll");
	void (__cdecl *(__cdecl *set_terminate)(void (__cdecl *)(void)))(void) =
		(void *)GetProcAddress(crt, "?set_terminate@@YAP6AXXZP6AXXZ@Z");
	if (set_terminate)
		set_terminate(on_terminate);
	noexcept_fn();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
