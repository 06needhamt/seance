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
 * Also an SE translator, an exception specification, an exception with
 * a handler of its own (a later compiler's), the RTTI exceptions the CRT
 * throws, and longjmp out of and within C++ frames.
 *
 * cxx_run returns 0, or the number of the first check that failed.
 * cxx_unhandled and cxx_noexcept end the process, for a test to watch;
 * cxx_rtti_fault needs the harness's fault handler.
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
#define VC7 0x19930521
#define VC8 0x19930522
#define REF 8
#define SIMPLE 1

__declspec(dllimport) void __stdcall _CxxThrowException(void *, const void *);
extern const void *const type_info_vtable __asm__("\"__imp_??_7type_info@@6B@\"");
__declspec(dllimport) const void *__cdecl __RTtypeid(void *);
__declspec(dllimport) void *__cdecl __RTDynamicCast(void *, int, void *, void *,
						     int);
__declspec(dllimport) int __cdecl _setjmp3(void *, int, ...)
	__attribute__((returns_twice));
__declspec(dllimport) void __cdecl longjmp(void *, int)
	__attribute__((noreturn));

/* An exception specification. */
struct es_list {
	int count;
	const struct catch_block *types;
};

/* RTTI: what the slot before a vftable points to. */
struct rtti_base {
	const struct type_desc *type;
	int nbases;
	int this_offset, vbase_descr, vbase_offset;
	UINT attributes;
};

struct rtti_hierarchy {
	UINT signature, attributes;
	int nbases;
	const struct rtti_base *const *bases;
};

struct rtti_locator {
	UINT signature;
	int offset;
	UINT cd_offset;
	const struct type_desc *type;
	const struct rtti_hierarchy *hierarchy;
};

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

static const struct cxx_type ct_char = { SIMPLE, &td_char, 0, -1, 0, 1, NULL };
static const struct type_table tt_char = { 1, { &ct_char } };
static const struct throw_info ti_char = { 0, NULL, NULL, &tt_char };

void throw_char(char v)
{
	char object = v;
	_CxxThrowException(&object, &ti_char);
}

/* The SE translator: turns 0xE0000001 into Derived('t', 77). */
int translations;

void tr_throw(void);

static void __cdecl translator(unsigned code, EXCEPTION_POINTERS *ep)
{
	translations++;
	if (code == 0xE0000001 && ep->ExceptionRecord->ExceptionCode == code)
		tr_throw();	/* through a frame of its own */
}

/* unexpected(): may throw something the specification allows. */
static void __cdecl on_unexpected(void)
{
	note('u');
	throw_char('q');
}

/* ... or one it doesn't allow either: terminate(). */
static void __cdecl on_unexpected_again(void)
{
	note('v');
	throw_int(6);
}

/* An exception from a later compiler, magic past VC8: its handler's. */
int custom_calls;
void *custom_frame, *custom_expect;
const void *custom_descr;

static EXCEPTION_DISPOSITION __cdecl custom_handler(EXCEPTION_RECORD *rec,
						    void *frame, CONTEXT *ctx,
						    void *dispatch,
						    const void *descr,
						    int trylevel, void *nested,
						    DWORD unused)
{
	custom_calls++;
	custom_frame = frame;
	custom_descr = descr;
	return ExceptionContinueExecution;
}

static const struct throw_info ti_custom = {
	0, NULL, (void *)custom_handler, &tt_int };

void raise_custom(void)
{
	int object = 3;
	ULONG_PTR args[3] = { 0x19930523, (ULONG_PTR)&object,
			      (ULONG_PTR)&ti_custom };
	RaiseException(0xE06D7363, 0, 3, args);
}

/* RTTI: an A, which no B is. */
static struct type_desc td_A = { NULL, NULL, ".?AVA@@" };
static struct type_desc td_B = { NULL, NULL, ".?AVB@@" };
static const struct rtti_base rb_A = { &td_A, 0, 0, -1, 0, 0 };
static const struct rtti_base *const rh_A_bases[] = { &rb_A };
static const struct rtti_hierarchy rh_A = { 0, 0, 1, rh_A_bases };
static const struct rtti_locator rl_A = { 0, 0, 0, &td_A, &rh_A };
static const void *const vt_A[2] = { &rl_A, NULL };
static struct { const void *const *vtable; } a_obj = { &vt_A[1] };
/* An object whose vftable pointer points nowhere: no RTTI to read. */
static struct { const void *vtable; } bad_obj = { (void *)0x10 };

/* The exceptions the CRT throws, caught by their names (see h_rti). */
static struct type_desc td_bad_cast = { NULL, NULL, ".?AVbad_cast@std@@" };
static struct type_desc td_non_rtti = { NULL, NULL, ".?AV__non_rtti_object@@" };
static struct type_desc td_exception = { NULL, NULL, ".?AVexception@@" };
static struct type_desc td_exception_std = { NULL, NULL, ".?AVexception@std@@" };

char caught_what[40];

/* In a catch block: what() of the std::exception caught. */
void grab(void *e)
{
	const char *(THISCALL *what)(void *) = (*(void *const *const *)e)[1];
	const char *w = what(e);
	int i;

	for (i = 0; i < 39 && w[i]; i++)
		caught_what[i] = w[i];
	caught_what[i] = 0;
	caught_at = e;
}

void rtti_thrower(int which)
{
	if (which == 0)	/* dynamic_cast<B &>(an A) */
		__RTDynamicCast(&a_obj, 0, &td_A, &td_B, 1);
	else if (which == 1)	/* typeid(*(A *)NULL) */
		__RTtypeid(NULL);
	else
		__RTtypeid(&bad_obj);
}

int sj_buf[16], sj_buf2[16];

void sj_jump(void)
{
	longjmp(sj_buf, 4);
}

void sj_jump2(void)
{
	longjmp(sj_buf2, 6);
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

	/*
	 * int catch_translated(void), /EHa: try { raise_seh(); }
	 * catch (Base &b) { return b.value; } - which the translator makes.
	 */
	".globl _catch_translated\n"
	"_catch_translated:\n\t"
	"CXX_PROLOGUE h_trn\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _raise_seh\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_trn_catch:\n\t"
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
	"CXX_HANDLER h_trn, _fi_trn\n"

	/* void tr_nomatch(void), /EHa: try { raise_seh(); } catch (char) { note('x'); } */
	"_tr_nomatch:\n\t"
	"CXX_PROLOGUE h_trm\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _raise_seh\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"jmp 1f\n"
	"_trm_catch:\n\t"
	"pushl $120\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"movl $1f, %eax\n\t"
	"ret\n"
	"1:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_trm, _fi_trm\n"

	/* int catch_tr_outer(void): try { tr_nomatch(); }
	 * catch (Base &b) { return b.value * 10 + translations; } */
	".globl _catch_tr_outer\n"
	"_catch_tr_outer:\n\t"
	"CXX_PROLOGUE h_tro\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _tr_nomatch\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_tro_catch:\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"imull $10, (%eax), %eax\n\t"
	"addl _translations, %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_tro, _fi_tro\n"

	/*
	 * void es_fn(int kind) throw(char) (VC7 tables): Obj e('E');
	 * kind ? throw_int(5) : throw_char('c');
	 */
	"_es_fn:\n\t"
	"CXX_PROLOGUE h_esf\n\t"
	"movl $0, -4(%ebp)\n\t"
	"cmpl $0, 8(%ebp)\n\t"
	"je 2f\n\t"
	"pushl $5\n\t"
	"call _throw_int\n\t"
	"ud2\n"
	"2:\n\t"
	"pushl $99\n\t"
	"call _throw_char\n\t"
	"ud2\n"
	"_esf_dtor:\n\t"
	"pushl $69\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"ret\n"
	"CXX_HANDLER h_esf, _fi_esf\n"

	/* int catch_es(int kind): try { es_fn(kind); }
	 * catch (char c) { return c; } catch (int e) { return -e; } */
	".globl _catch_es\n"
	"_catch_es:\n\t"
	"CXX_PROLOGUE h_ces\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl 8(%ebp)\n\t"
	"call _es_fn\n\t"
	"ud2\n"
	"_ces_char:\n\t"
	"movsbl -0x14(%ebp), %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"_ces_int:\n\t"
	"movl -0x14(%ebp), %eax\n\t"
	"negl %eax\n\t"
	"movl %eax, -0x18(%ebp)\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_ces, _fi_ces\n"

	/* int catch_custom(void): try { raise_custom(); return 1; }
	 * catch (...) { return -1; } - which its handler resumes. */
	".globl _catch_custom\n"
	"_catch_custom:\n\t"
	"CXX_PROLOGUE h_cus\n\t"
	"leal -0xc(%ebp), %eax\n\t"
	"movl %eax, _custom_expect\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _raise_custom\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $1, %eax\n\t"
	"jmp 1f\n"
	"_cus_catch:\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-1, %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_cus, _fi_cus\n"

	/*
	 * int catch_rtti(int which): try { rtti_thrower(which); }
	 * catch (std::bad_cast &e) { grab(e); return 1; }
	 * catch (__non_rtti_object &e) { grab(e); return 3; }
	 * catch (std::exception &e) { grab(e); return 2; }
	 */
	".globl _catch_rtti\n"
	"_catch_rtti:\n\t"
	"CXX_PROLOGUE h_rti\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl 8(%ebp)\n\t"
	"call _rtti_thrower\n\t"
	"addl $4, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_rti_cast:\n\t"
	"movl $1, -0x18(%ebp)\n\t"
	"jmp 3f\n"
	"_rti_non:\n\t"
	"movl $3, -0x18(%ebp)\n\t"
	"jmp 3f\n"
	"_rti_exc:\n\t"
	"movl $2, -0x18(%ebp)\n"
	"3:\n\t"
	"pushl -0x14(%ebp)\n\t"
	"call _grab\n\t"
	"addl $4, %esp\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl -0x18(%ebp), %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_rti, _fi_rti\n"

	/* void sj_mid(void): Obj m('M'); sj_jump(); - longjmp out past it. */
	"_sj_mid:\n\t"
	"CXX_PROLOGUE h_sjm\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _sj_jump\n\t"
	"ud2\n"
	"_sjm_dtor:\n\t"
	"pushl $77\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"ret\n"
	"CXX_HANDLER h_sjm, _fi_sjm\n"

	/*
	 * int sj_local(void): if (int r = setjmp(sj_buf2)) return r * 10 +
	 * (state is back to -1); Obj a('A'); sj_jump2(); - cl's setjmp in a
	 * C++ function, _setjmp3 with __CxxLongjmpUnwind and its FuncInfo.
	 */
	".globl _sj_local\n"
	"_sj_local:\n\t"
	"CXX_PROLOGUE h_sjl\n\t"
	"pushl $_fi_sjl\n\t"
	"pushl $-1\n\t"
	"pushl __imp____CxxLongjmpUnwind\n\t"
	"pushl $3\n\t"
	"pushl $_sj_buf2\n\t"
	"call *__imp___setjmp3\n\t"
	"addl $20, %esp\n\t"
	"testl %eax, %eax\n\t"
	"jnz 2f\n\t"
	"movl $0, -4(%ebp)\n\t"
	"call _sj_jump2\n\t"
	"ud2\n"
	"2:\n\t"
	"imull $10, %eax, %eax\n\t"
	"cmpl $-1, -4(%ebp)\n\t"
	"jne 1f\n\t"
	"incl %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"_sjl_dtor:\n\t"
	"pushl $65\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"ret\n"
	"CXX_HANDLER h_sjl, _fi_sjl\n"

	/* void tr_throw(void): Obj k('k'); throw_derived('t', 77); */
	".globl _tr_throw\n"
	"_tr_throw:\n\t"
	"CXX_PROLOGUE h_trt\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl $77\n\t"
	"pushl $116\n\t"
	"call _throw_derived\n\t"
	"ud2\n"
	"_trt_dtor:\n\t"
	"pushl $107\n\t"
	"call _note\n\t"
	"addl $4, %esp\n\t"
	"ret\n"
	"CXX_HANDLER h_trt, _fi_trt\n"

	/* int catch_any_exc(int which): try { rtti_thrower(which); }
	 * catch (exception &e) { grab(e); return 2; } - msvcrt's name */
	".globl _catch_any_exc\n"
	"_catch_any_exc:\n\t"
	"CXX_PROLOGUE h_cax\n\t"
	"movl $0, -4(%ebp)\n\t"
	"pushl 8(%ebp)\n\t"
	"call _rtti_thrower\n\t"
	"addl $4, %esp\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $-99, %eax\n\t"
	"jmp 1f\n"
	"_cax_catch:\n\t"
	"pushl -0x14(%ebp)\n\t"
	"call _grab\n\t"
	"addl $4, %esp\n\t"
	"movl $2f, %eax\n\t"
	"ret\n"
	"2:\n\t"
	"movl $-1, -4(%ebp)\n\t"
	"movl $2, %eax\n"
	"1:\n\t"
	"CXX_EPILOGUE\n"
	"CXX_HANDLER h_cax, _fi_cax\n"

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
	loc_dtor[], loc_catch[], cst_plain[], cst_const[], trn_catch[],
	trm_catch[], tro_catch[], esf_dtor[], ces_char[], ces_int[],
	cus_catch[], rti_cast[], rti_non[], rti_exc[], sjm_dtor[], sjl_dtor[],
	trt_dtor[], cax_catch[];

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

static const struct catch_block cb_trn[] = { { REF, &td_base, -0x14, trn_catch } };
static const struct try_block tb_trn[] = { { 0, 0, 1, 1, cb_trn } };
const struct func_info fi_trn = { VC8, 2, unwind2, 1, tb_trn };

static const struct catch_block cb_trm[] = { { 0, &td_char, -0x14, trm_catch } };
static const struct try_block tb_trm[] = { { 0, 0, 1, 1, cb_trm } };
const struct func_info fi_trm = { VC6, 2, unwind2, 1, tb_trm };

static const struct catch_block cb_tro[] = { { REF, &td_base, -0x14, tro_catch } };
static const struct try_block tb_tro[] = { { 0, 0, 1, 1, cb_tro } };
const struct func_info fi_tro = { VC6, 2, unwind2, 1, tb_tro };

static const struct unwind_entry unwind_esf[] = { { -1, esf_dtor } };
static const struct catch_block es_char_types[] = { { 0, &td_char, 0, NULL } };
static const struct es_list es_char = { 1, es_char_types };
const struct func_info fi_esf = { VC7, 1, unwind_esf, 0, NULL, 0, NULL, &es_char };

static const struct catch_block cb_ces[] = {
	{ 0, &td_char, -0x14, ces_char }, { 0, &td_int, -0x14, ces_int } };
static const struct try_block tb_ces[] = { { 0, 0, 1, 2, cb_ces } };
const struct func_info fi_ces = { VC6, 2, unwind2, 1, tb_ces };

static const struct catch_block cb_cus[] = { { 0, NULL, 0, cus_catch } };
static const struct try_block tb_cus[] = { { 0, 0, 1, 1, cb_cus } };
const struct func_info fi_cus = { VC6, 2, unwind2, 1, tb_cus };

static const struct catch_block cb_rti[] = {
	{ REF, &td_bad_cast, -0x14, rti_cast },
	{ REF, &td_non_rtti, -0x14, rti_non },
	{ REF, &td_exception_std, -0x14, rti_exc },
};
static const struct try_block tb_rti[] = { { 0, 0, 1, 3, cb_rti } };
const struct func_info fi_rti = { VC6, 2, unwind2, 1, tb_rti };

static const struct unwind_entry unwind_trt[] = { { -1, trt_dtor } };
const struct func_info fi_trt = { VC6, 1, unwind_trt, 0, NULL };

static const struct catch_block cb_cax[] = {
	{ REF, &td_exception, -0x14, cax_catch } };
static const struct try_block tb_cax[] = { { 0, 0, 1, 1, cb_cax } };
const struct func_info fi_cax = { VC6, 2, unwind2, 1, tb_cax };

static const struct unwind_entry unwind_sjm[] = { { -1, sjm_dtor } };
const struct func_info fi_sjm = { VC6, 1, unwind_sjm, 0, NULL };

static const struct unwind_entry unwind_sjl[] = { { -1, sjl_dtor } };
const struct func_info fi_sjl = { VC6, 1, unwind_sjl, 0, NULL };

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
int catch_translated(void);
int catch_tr_outer(void);
int catch_es(int kind);
int catch_custom(void);
int catch_rtti(int which);
int catch_any_exc(int which);
void sj_mid(void);
int sj_local(void);

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
	translations = 0;
	caught_what[0] = 0;
}

typedef void (__cdecl *translator_fn)(unsigned, EXCEPTION_POINTERS *);

/* longjmp out past a C++ frame: its destructors run. */
static int sj_outer(void)
{
	if (_setjmp3(sj_buf, 0))
		return streq(trail, "M") ? 4 : -2;
	sj_mid();
	return -1;
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

	/*
	 * The SE translator: its exception goes to the frame's catch blocks
	 * (/EHa only), and one none takes is destroyed there, the SEH one
	 * going on to be translated again.
	 */
	translator_fn (__cdecl *set_translator)(translator_fn) =
		(void *)GetProcAddress(crt, "?_set_se_translator@@YAP6AXIPAU"
				       "_EXCEPTION_POINTERS@@@ZP6AXI0@Z@Z");
	if (!set_translator || set_translator(translator))
		return 16;
	reset();
	if (catch_translated() != 77 || translations != 1 || dtors != 1)
		return 17;
	reset();
	if (catch_tr_outer() != 772 || translations != 2 || dtors != 2 ||
	    !streq(trail, "ktkt"))
		return 18;
	reset();
	if (catch_seh() != 9 || translations != 1 || !streq(trail, "kat"))
		return 19;
	if (set_translator(NULL) != translator)
		return 20;

	/* An exception specification lets char out; int calls unexpected(). */
	void (__cdecl *(__cdecl *set_unexpected)(void (__cdecl *)(void)))(void) =
		(void *)GetProcAddress(crt, "?set_unexpected@@YAP6AXXZP6AXXZ@Z");
	if (!set_unexpected)
		return 21;
	set_unexpected(on_unexpected);
	reset();
	if (catch_es(0) != 'c' || !streq(trail, "E"))
		return 22;
	reset();
	if (catch_es(1) != 'q' || !streq(trail, "Eu"))
		return 23;
	reset();	/* again: nothing left over from the first time */
	if (catch_es(1) != 'q' || !streq(trail, "Eu"))
		return 31;
	set_unexpected(NULL);

	/* A later compiler's exception: its own handler, with this frame. */
	custom_calls = 0;
	if (catch_custom() != 1 || custom_calls != 1 ||
	    custom_frame != custom_expect || custom_descr != &fi_cus)
		return 24;

	/* The RTTI exceptions, caught by either name. */
	td_A.vtable = td_B.vtable = td_bad_cast.vtable = td_non_rtti.vtable =
		td_exception.vtable = td_exception_std.vtable = type_info_vtable;
	reset();
	if (catch_rtti(0) != 1 || !streq(caught_what, "Bad dynamic_cast!"))
		return 25;
	reset();
	if (catch_rtti(1) != 2 ||
	    !streq(caught_what, "Attempted a typeid of NULL pointer!"))
		return 26;
	if (catch_any_exc(0) != 2 || catch_any_exc(1) != 2)
		return 30;
	if (__RTtypeid(&a_obj) != &td_A ||
	    __RTDynamicCast(&a_obj, 0, &td_A, &td_A, 1) != &a_obj ||
	    __RTDynamicCast(&a_obj, 0, &td_A, &td_B, 0))
		return 27;

	/* longjmp: past a C++ frame, and within one, both unwound. */
	reset();
	if (sj_outer() != 4)
		return 28;
	reset();
	if (sj_local() != 61 || !streq(trail, "A"))
		return 29;

	/* And after all that, catching still works (nothing left behind). */
	reset();
	if (catch_int(1) != 2 || catch_base() != 70)
		return 12;
	return 0;
}

/* RTTI that isn't there: reading it faults, which becomes __non_rtti_object. */
EXPORT int cxx_rtti_fault(void)
{
	td_non_rtti.vtable = td_bad_cast.vtable = td_exception.vtable =
		td_exception_std.vtable = type_info_vtable;
	reset();
	if (catch_rtti(2) != 3 ||
	    !streq(caught_what, "Bad read pointer - no RTTI data!"))
		return 1;
	if (catch_any_exc(2) != 2)	/* and as a std::exception */
		return 2;
	return 0;
}

static void __cdecl on_terminate(void);

/* unexpected() throwing another type the specification doesn't allow. */
EXPORT void cxx_es_terminate(void)
{
	HMODULE crt = GetModuleHandleA("msvcrt.dll");
	void (__cdecl *(__cdecl *set_terminate)(void (__cdecl *)(void)))(void) =
		(void *)GetProcAddress(crt, "?set_terminate@@YAP6AXXZP6AXXZ@Z");
	void (__cdecl *(__cdecl *set_unexpected)(void (__cdecl *)(void)))(void) =
		(void *)GetProcAddress(crt, "?set_unexpected@@YAP6AXXZP6AXXZ@Z");
	td_int.vtable = td_char.vtable = type_info_vtable;
	if (set_terminate && set_unexpected) {
		set_terminate(on_terminate);
		set_unexpected(on_unexpected_again);
	}
	catch_es(1);
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
