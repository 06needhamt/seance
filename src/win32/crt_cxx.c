/*
 * msvcrt: C++ exceptions as MSVC compiles them for i386, after Wine's
 * except_i386.c and cppexcept.c, for DLLs built with a dynamic CRT (/MD,
 * msvcrt.dll to vcruntime140.dll).
 *
 * throw X calls _CxxThrowException(object, ThrowInfo), which raises SEH
 * exception 0xE06D7363 with the object and its type. Each function with a
 * try block or an object to destroy has a frame much like EH3's, from its
 * %ebp down:
 *
 *   ebp-0x04  state: where in the function the code is (-1 outside all)
 *   ebp-0x08  the handler: "mov eax, FuncInfo; jmp __CxxFrameHandler"
 *   ebp-0x0c  the next frame on the fs:[0] chain ("frame" below)
 *   ebp-0x10  %esp as the body left it (with try blocks), for after a catch
 *
 * FuncInfo lists, per state, the state it unwinds to and the destructor
 * (a piece of the function, run with its %ebp) that gets there; and the
 * try blocks, each a range of states with its catch blocks. A catch block
 * is also a piece of the function: called with the function's %ebp, it
 * returns the address to carry on at, with %esp from ebp-0x10.
 *
 * The frame handler finds a catch block whose type the exception's type
 * list has (so a class caught by its base class, a pointer by a base
 * pointer), copies the object into the catch's variable, unwinds every
 * frame above (RtlUnwind: their destructors run) and its own states down
 * to the try, then calls the catch block under a frame of its own, which
 * sees rethrows (throw;) and exceptions thrown out of it. When the catch
 * block ends, the exception object is destroyed, unless an outer catch
 * block still running holds the same one.
 *
 * catch (...) also takes SEH exceptions (an access violation, say) in
 * code built with /EHa, as on Windows; /EHs code (FuncInfo's synchronous
 * flag) only takes C++ ones. A noexcept function an exception would leave
 * calls terminate().
 *
 * Not done: _set_se_translator, exception specifications (throw(X)),
 * managed (custom) exception handlers, __CxxExceptionFilter and
 * __CxxDetectRethrow, RTTI casts (__RTDynamicCast and friends), and the
 * std::exception subclasses (bad_cast and so on).
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime/seh.h"
#include "runtime/teb.h"
#include "win32/internal.h"

#define CRTFN(ret) static ret STUB_CDECL
#define THISCALL __attribute__((thiscall, force_align_arg_pointer))

#define CXX_EXCEPTION          0xE06D7363
#define CXX_FRAME_MAGIC_VC6    0x19930520
#define CXX_FRAME_MAGIC_VC7    0x19930521
#define CXX_FRAME_MAGIC_VC8    0x19930522

#define FUNC_DESCR_SYNCHRONOUS 1	/* /EHs: C++ exceptions only */
#define FUNC_DESCR_NOEXCEPT    4

#define TYPE_FLAG_CONST        1
#define TYPE_FLAG_VOLATILE     2
#define TYPE_FLAG_REFERENCE    8

#define CLASS_IS_SIMPLE_TYPE          1
#define CLASS_HAS_VIRTUAL_BASE_CLASS  4

/* The compiler's tables. */

struct type_info {
	const void *vtable;
	char *name;		/* undecorated, made on first use */
	char mangled[1];	/* ".?AVfoo@@", as long as it is */
};

struct unwind_entry {
	int prev;		/* the state this one unwinds to */
	void *handler;		/* what gets there, or NULL */
};

struct catch_block {
	UINT flags;		/* TYPE_FLAG_* */
	const struct type_info *type;	/* NULL for catch (...) */
	int offset;		/* of the catch variable from %ebp, or 0 */
	void *handler;
};

struct try_block {
	int start_level, end_level;	/* the states of the try */
	int catch_level;	/* the highest state in its catch blocks */
	UINT ncatch;
	const struct catch_block *catches;
};

struct func_descr {
	UINT magic;
	UINT nunwind;
	const struct unwind_entry *unwind;
	UINT ntry;
	const struct try_block *tries;
	UINT nipmap;
	const void *ipmap;
	const void *expect_list;	/* VC7 on */
	UINT flags;			/* VC8 on */
};

struct this_offsets {
	int this_offset;	/* of the base in the object */
	int vbase_descr;	/* of the virtual base table pointer, or -1 */
	int vbase_offset;	/* of this base's entry in that table */
};

struct cxx_type {
	UINT flags;		/* CLASS_* */
	const struct type_info *type;
	struct this_offsets offsets;
	UINT size;
	void *copy_ctor;
};

struct cxx_type_table {
	UINT count;
	const struct cxx_type *types[];	/* the class, then its bases */
};

struct exception_type {		/* ThrowInfo */
	UINT flags;		/* TYPE_FLAG_* */
	void *destructor;
	void *custom_handler;
	const struct cxx_type_table *table;
};

/* The frame, from its fs:[0] record up to the function's saved %ebp. */
struct cxx_frame {
	EXCEPTION_REGISTRATION_RECORD reg;
	int trylevel;
	DWORD ebp;
};

typedef void (THISCALL *dtor_fn)(void *);
typedef void (THISCALL *copy_fn)(void *, const void *);
typedef void (THISCALL *copy_vbase_fn)(void *, const void *, int);
typedef void (STUB_CDECL *void_fn)(void);

/*
 * The frame a running catch block is called under: an fs:[0] record whose
 * handler sees exceptions leaving the block, and a link in the thread's
 * list of the exceptions its catch blocks are handling.
 */
struct catch_frame {
	EXCEPTION_REGISTRATION_RECORD reg;	/* first */
	struct cxx_frame *frame;
	const struct func_descr *descr;
	const struct try_block *try;
	EXCEPTION_RECORD *rec;
	CONTEXT *ctx;
	void *object;		/* NULL for a non-C++ exception */
	const struct exception_type *type;
	struct catch_frame *outer;
	EXCEPTION_RECORD *prev_rec;
	CONTEXT *prev_ctx;
	int left;
};

static _Thread_local struct {
	EXCEPTION_RECORD *rec;	/* what the innermost catch block handles */
	CONTEXT *ctx;
	struct catch_frame *catching;
	int processing_throw;	/* inside a frame handler's search */
	void_fn terminate, unexpected;
} td;

static void_fn purecall_handler;
static int (STUB_CDECL *new_handler)(size_t);
static int new_mode;

/*
 * Calls a piece of a function (a destructor or catch block) with %ebp set
 * to the function's frame; returns what it leaves in %eax. It may use
 * %ebx, %esi and %edi as its function does, so they are saved around it.
 */
__asm__(
	".text\n"
	".type cxx_call_ebp, @function\n"
	"cxx_call_ebp:\n\t"
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
	".size cxx_call_ebp, .-cxx_call_ebp\n"
	/* Carries on after a catch block: %esp as the body saved it. */
	".type cxx_continue, @function\n"
	"cxx_continue:\n\t"
	"movl 4(%esp), %edx\n\t"
	"movl 8(%esp), %eax\n\t"
	"movl -4(%edx), %esp\n\t"
	"leal 12(%edx), %ebp\n\t"
	"jmp *%eax\n\t"
	".size cxx_continue, .-cxx_continue\n"
);
void *cxx_call_ebp(void *fn, void *ebp) __asm__("cxx_call_ebp")
	__attribute__((cdecl));
void cxx_continue(struct cxx_frame *frame, void *addr) __asm__("cxx_continue")
	__attribute__((cdecl, noreturn));

static void *frame_ebp(struct cxx_frame *f)
{
	return &f->ebp;
}

/* abort, terminate and unexpected. */

CRTFN(void) crt_abort(void)
{
	fputs("vfw-harness: crash: a DLL called abort()\n", stderr);
	fflush(stderr);
	_exit(3);	/* as msvcrt's abort exits */
}

CRTFN(void) crt_terminate(void)
{
	void_fn f = td.terminate;

	if (f)
		f();
	fputs("vfw-harness: crash: terminate() called (an unhandled C++ "
	      "exception, or one where none may be)\n", stderr);
	crt_abort();
}

CRTFN(void) crt_unexpected(void)
{
	void_fn f = td.unexpected;

	if (f)
		f();
	crt_terminate();
}

CRTFN(void_fn) crt_set_terminate(void_fn f)
{
	void_fn old = td.terminate;
	td.terminate = f;
	return old;
}

CRTFN(void_fn) crt_set_unexpected(void_fn f)
{
	void_fn old = td.unexpected;
	td.unexpected = f;
	return old;
}

CRTFN(void_fn) crt__get_terminate(void) { return td.terminate; }
CRTFN(void_fn) crt__get_unexpected(void) { return td.unexpected; }

CRTFN(void) crt__purecall(void)
{
	void_fn f = purecall_handler;

	if (f)
		f();
	fputs("vfw-harness: crash: a DLL called a pure virtual function\n",
	      stderr);
	crt_abort();
}

CRTFN(void_fn) crt__set_purecall_handler(void_fn f)
{
	void_fn old = purecall_handler;
	purecall_handler = f;
	return old;
}

CRTFN(void_fn) crt__get_purecall_handler(void) { return purecall_handler; }

CRTFN(int) crt___uncaught_exception(void) { return td.processing_throw != 0; }
CRTFN(int) crt___uncaught_exceptions(void) { return td.processing_throw; }
CRTFN(int *) crt___processing_throw(void) { return &td.processing_throw; }
CRTFN(void **) crt___current_exception(void) { return (void **)&td.rec; }
CRTFN(void **) crt___current_exception_context(void) { return (void **)&td.ctx; }

/* The frame handler. */

/* Where a base class is in an object, virtual bases included. */
static void *this_pointer(const struct this_offsets *off, void *object)
{
	if (!object)
		return NULL;
	if (off->vbase_descr >= 0) {
		BYTE *p = (BYTE *)object + off->vbase_descr;
		p += *(const int *)(*(BYTE **)p + off->vbase_offset);
		object = p;
	}
	return (BYTE *)object + off->this_offset;
}

/* The type in the exception's list a catch block takes, or NULL. */
static const struct cxx_type *caught_type(const struct exception_type *e,
					  const struct type_info *want,
					  UINT flags)
{
	for (UINT i = 0; i < e->table->count; i++) {
		const struct cxx_type *t = e->table->types[i];
		if (!want || !want->mangled[0])
			return t;	/* catch (...) */
		if (want != t->type &&
		    strcmp(want->mangled, t->type->mangled))
			continue;
		/* A const object only binds to a const catch, likewise volatile. */
		if ((e->flags & TYPE_FLAG_CONST) && !(flags & TYPE_FLAG_CONST))
			continue;
		if ((e->flags & TYPE_FLAG_VOLATILE) &&
		    !(flags & TYPE_FLAG_VOLATILE))
			continue;
		return t;
	}
	return NULL;
}

/* Puts the object into the catch block's variable. */
static void copy_exception(void *object, struct cxx_frame *frame,
			   const struct catch_block *c, const struct cxx_type *t)
{
	if (!c->type || !c->type->mangled[0] || !c->offset)
		return;
	void **dest = (void **)((BYTE *)frame_ebp(frame) + c->offset);

	if (c->flags & TYPE_FLAG_REFERENCE) {
		*dest = this_pointer(&t->offsets, object);
	} else if (t->flags & CLASS_IS_SIMPLE_TYPE) {
		memmove(dest, object, t->size);
		/* A pointer to a class, caught as a pointer to its base. */
		if (t->size == sizeof(void *))
			*dest = this_pointer(&t->offsets, *dest);
	} else if (t->copy_ctor) {
		void *src = this_pointer(&t->offsets, object);
		if (t->flags & CLASS_HAS_VIRTUAL_BASE_CLASS)
			((copy_vbase_fn)t->copy_ctor)(dest, src, 1);
		else
			((copy_fn)t->copy_ctor)(dest, src);
	} else {
		memmove(dest, this_pointer(&t->offsets, object), t->size);
	}
}

/* Runs the destructors from the frame's state down to last. */
static void local_unwind(struct cxx_frame *frame, const struct func_descr *d,
			 int last)
{
	int level = frame->trylevel;

	while (level != last) {
		if (level < 0 || level >= (int)d->nunwind) {
			fprintf(stderr, "vfw-harness: C++ unwind: bad state %d "
				"(of %u)\n", level, d->nunwind);
			crt_terminate();
		}
		const struct unwind_entry *u = &d->unwind[level];
		/* Moved on first, so a destructor that throws isn't rerun. */
		level = frame->trylevel = u->prev;
		if (u->handler)
			cxx_call_ebp(u->handler, frame_ebp(frame));
	}
}

/* Whether a running catch block other than c holds object. */
static int held(const struct catch_frame *c, const void *object)
{
	for (const struct catch_frame *o = td.catching; o; o = o->outer)
		if (o != c && o->object == object)
			return 1;
	return 0;
}

/*
 * A catch block is over: off the thread's list, and its exception object
 * destroyed unless it is still in use (being thrown again, or held by an
 * outer catch block).
 */
static void catch_leave(struct catch_frame *c, const void *in_flight)
{
	if (c->left)
		return;
	c->left = 1;
	for (struct catch_frame **p = &td.catching; *p; p = &(*p)->outer)
		if (*p == c) {
			*p = c->outer;
			break;
		}
	td.rec = c->prev_rec;
	td.ctx = c->prev_ctx;
	if (c->object && c->type && c->type->destructor &&
	    c->object != in_flight && !held(c, c->object))
		((dtor_fn)c->type->destructor)(c->object);
}

static int is_cxx(const EXCEPTION_RECORD *rec)
{
	return rec->ExceptionCode == CXX_EXCEPTION &&
	       rec->NumberParameters >= 3 &&
	       rec->ExceptionInformation[0] >= CXX_FRAME_MAGIC_VC6 &&
	       rec->ExceptionInformation[0] <= CXX_FRAME_MAGIC_VC8;
}

static EXCEPTION_DISPOSITION STUB_CDECL catch_handler(EXCEPTION_RECORD *rec,
						      void *frame, CONTEXT *ctx,
						      void *dispatch);

/*
 * Looks for a catch block in frame that takes the exception and, finding
 * one, runs it and carries on after it, never returning. Inside a
 * running catch block (nested), only try blocks within it count.
 *
 * Its catch_frame goes on the fs:[0] chain, which dispatch only follows
 * within the thread's stack: AddressSanitizer's fake stack, where it
 * would otherwise put locals, is kept away from it.
 */
static void __attribute__((no_sanitize_address)) find_catch(EXCEPTION_RECORD *rec, CONTEXT *ctx,
		       struct cxx_frame *frame, const struct func_descr *d,
		       struct catch_frame *nested,
		       const struct exception_type *type)
{
	void *object = type ? (void *)rec->ExceptionInformation[1] : NULL;
	int level = frame->trylevel;

	td.processing_throw++;
	for (UINT i = 0; i < d->ntry; i++) {
		const struct try_block *t = &d->tries[i];
		/* Inside a catch block: only the try blocks within it. */
		if (nested && t->start_level <= nested->try->end_level)
			continue;
		if (level < t->start_level || level > t->end_level)
			continue;
		for (UINT j = 0; j < t->ncatch; j++) {
			const struct catch_block *c = &t->catches[j];
			if (type) {
				const struct cxx_type *ct =
					caught_type(type, c->type, c->flags);
				if (!ct)
					continue;
				copy_exception(object, frame, c, ct);
			} else if (c->type && c->type->mangled[0]) {
				continue;	/* only catch (...) takes these */
			}

			/* Everything above, then this function down to the try. */
			seh_RtlUnwind(nested ? &nested->reg : &frame->reg, NULL,
				      rec, NULL);
			local_unwind(frame, d, t->start_level);
			frame->trylevel = t->end_level + 1;

			struct catch_frame cf;
			memset(&cf, 0, sizeof(cf));
			cf.reg.Handler = (PEXCEPTION_ROUTINE)(void *)catch_handler;
			cf.frame = frame;
			cf.descr = d;
			cf.try = t;
			cf.rec = rec;
			cf.ctx = ctx;
			cf.object = object;
			cf.type = type;
			cf.outer = td.catching;
			cf.prev_rec = td.rec;
			cf.prev_ctx = td.ctx;
			td.catching = &cf;
			td.rec = rec;
			td.ctx = ctx;
			td.processing_throw--;

			struct win_teb *teb = teb_current();
			cf.reg.Next = teb->ExceptionList;
			teb->ExceptionList = &cf.reg;
			DWORD saved_esp = ((DWORD *)frame)[-1];
			void *cont = cxx_call_ebp(c->handler, frame_ebp(frame));
			teb->ExceptionList = cf.reg.Next;
			((DWORD *)frame)[-1] = saved_esp;
			catch_leave(&cf, NULL);
			cxx_continue(frame, cont);
		}
	}
	td.processing_throw--;
}

static EXCEPTION_DISPOSITION frame_handler(EXCEPTION_RECORD *rec,
					   struct cxx_frame *frame,
					   CONTEXT *ctx,
					   const struct func_descr *d,
					   struct catch_frame *nested)
{
	if (d->magic < CXX_FRAME_MAGIC_VC6 || d->magic > CXX_FRAME_MAGIC_VC8)
		return ExceptionContinueSearch;
	if (d->magic >= CXX_FRAME_MAGIC_VC8 &&
	    (d->flags & FUNC_DESCR_SYNCHRONOUS) &&
	    rec->ExceptionCode != CXX_EXCEPTION)
		return ExceptionContinueSearch;

	if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
		if (d->nunwind && !nested)
			local_unwind(frame, d, -1);
		return ExceptionContinueSearch;
	}

	/* throw; (no object, no type): the exception being handled, again. */
	if (rec->ExceptionCode == CXX_EXCEPTION && rec->NumberParameters >= 3 &&
	    !rec->ExceptionInformation[1] && !rec->ExceptionInformation[2]) {
		if (!td.rec)
			crt_terminate();	/* nothing to rethrow */
		*rec = *td.rec;
		rec->ExceptionFlags &= ~EXCEPTION_UNWINDING;
	}

	const struct exception_type *type = is_cxx(rec) ?
		(const struct exception_type *)rec->ExceptionInformation[2] :
		NULL;
	if (d->ntry)
		find_catch(rec, ctx, frame, d, nested, type);

	if (!nested && d->magic >= CXX_FRAME_MAGIC_VC8 &&
	    (d->flags & FUNC_DESCR_NOEXCEPT))
		crt_terminate();
	return ExceptionContinueSearch;
}

/*
 * The handler of a running catch block's frame. An exception leaving the
 * block ends it; one thrown inside it may yet be caught by a try block
 * within it.
 */
static EXCEPTION_DISPOSITION STUB_CDECL catch_handler(EXCEPTION_RECORD *rec,
						      void *frame, CONTEXT *ctx,
						      void *dispatch)
{
	struct catch_frame *c = frame;

	if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
		catch_leave(c, is_cxx(rec) ?
			    (void *)rec->ExceptionInformation[1] : NULL);
		return ExceptionContinueSearch;
	}
	return frame_handler(rec, c->frame, ctx, c->descr, c);
}

/* Entered from "mov eax, FuncInfo; jmp __CxxFrameHandler". */
EXCEPTION_DISPOSITION STUB_CDECL crt_cxx_frame_handler(EXCEPTION_RECORD *rec,
						       void *frame, CONTEXT *ctx,
						       void *dispatch,
						       const struct func_descr *d)
	__attribute__((used, visibility("hidden")));
EXCEPTION_DISPOSITION STUB_CDECL crt_cxx_frame_handler(EXCEPTION_RECORD *rec,
						       void *frame, CONTEXT *ctx,
						       void *dispatch,
						       const struct func_descr *d)
{
	return frame_handler(rec, frame, ctx, d, NULL);
}

__asm__(
	".text\n"
	".type crt___CxxFrameHandler, @function\n"
	"crt___CxxFrameHandler:\n\t"
	"pushl %eax\n\t"		/* the FuncInfo */
	"pushl 20(%esp)\n\t"		/* dispatcher context */
	"pushl 20(%esp)\n\t"		/* context */
	"pushl 20(%esp)\n\t"		/* frame */
	"pushl 20(%esp)\n\t"		/* record */
	"call crt_cxx_frame_handler\n\t"
	"addl $20, %esp\n\t"
	"ret\n\t"
	".size crt___CxxFrameHandler, .-crt___CxxFrameHandler\n"
);
EXCEPTION_DISPOSITION crt___CxxFrameHandler(void) __asm__("crt___CxxFrameHandler");

static void WINAPI __attribute__((force_align_arg_pointer))
crt__CxxThrowException(void *object, const struct exception_type *type)
{
	DWORD_PTR args[3] = { CXX_FRAME_MAGIC_VC6, (DWORD_PTR)object,
			      (DWORD_PTR)type };

	seh_RaiseException(CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE, 3, args);
}

/* operator new and delete. */

static void *cxx_new(size_t size)
{
	for (;;) {
		void *p = malloc(size ? size : 1);
		if (p || !new_handler || !new_handler(size))
			return p;
	}
}

CRTFN(void *) crt_operator_new(size_t size) { return cxx_new(size); }
CRTFN(void) crt_operator_delete(void *p) { free(p); }

typedef int (STUB_CDECL *new_handler_fn)(size_t);

CRTFN(new_handler_fn) crt__set_new_handler(new_handler_fn f)
{
	new_handler_fn old = new_handler;
	new_handler = f;
	return old;
}

CRTFN(new_handler_fn) crt__query_new_handler(void) { return new_handler; }

CRTFN(int) crt__set_new_mode(int mode)
{
	int old = new_mode;
	new_mode = mode;
	return old;
}

CRTFN(int) crt__query_new_mode(void) { return new_mode; }

/* type_info. */

static pthread_mutex_t name_lock = PTHREAD_MUTEX_INITIALIZER;

struct out {
	char buf[256];
	size_t n;
	int bad;
};

static void put(struct out *o, const char *s, size_t len)
{
	if (o->n + len >= sizeof(o->buf)) {
		o->bad = 1;
		return;
	}
	memcpy(o->buf + o->n, s, len);
	o->n += len;
	o->buf[o->n] = '\0';
}

static void puts_(struct out *o, const char *s)
{
	put(o, s, strlen(s));
}

/* A qualified name, "B@A@@" -> "A::B"; advances *p past it. */
static void qualified(struct out *o, const char **p)
{
	const char *parts[8];
	size_t lens[8];
	int n = 0;

	while (**p && **p != '@') {
		const char *s = *p;
		if (*s == '?' || (*s >= '0' && *s <= '9')) {
			o->bad = 1;	/* templates, back references */
			return;
		}
		while (**p && **p != '@')
			(*p)++;
		if (!**p || n == 8) {
			o->bad = 1;
			return;
		}
		parts[n] = s;
		lens[n++] = (size_t)(*p - s);
		(*p)++;
	}
	if (**p != '@' || !n) {
		o->bad = 1;
		return;
	}
	(*p)++;
	for (int i = n - 1; i >= 0; i--) {
		put(o, parts[i], lens[i]);
		if (i)
			puts_(o, "::");
	}
}

/* A type in MSVC's mangling, the common forms; o->bad if not one. */
static void type_name(struct out *o, const char **p)
{
	static const struct { char c; const char *name; } basic[] = {
		{ 'C', "signed char" }, { 'D', "char" }, { 'E', "unsigned char" },
		{ 'F', "short" }, { 'G', "unsigned short" }, { 'H', "int" },
		{ 'I', "unsigned int" }, { 'J', "long" }, { 'K', "unsigned long" },
		{ 'M', "float" }, { 'N', "double" }, { 'O', "long double" },
		{ 'X', "void" },
	};
	static const struct { char c; const char *name; } ext[] = {
		{ 'N', "bool" }, { 'J', "__int64" }, { 'K', "unsigned __int64" },
		{ 'W', "wchar_t" },
	};
	char c = *(*p)++;

	switch (c) {
	case 'V': puts_(o, "class "); qualified(o, p); return;
	case 'U': puts_(o, "struct "); qualified(o, p); return;
	case 'T': puts_(o, "union "); qualified(o, p); return;
	case 'W':
		if (*(*p)++ != '4') {
			o->bad = 1;
			return;
		}
		puts_(o, "enum ");
		qualified(o, p);
		return;
	case 'P':	/* pointer: A plain, B const, C volatile, D both */
	case 'Q': {
		char q = *(*p)++;
		if (q < 'A' || q > 'D') {
			o->bad = 1;
			return;
		}
		type_name(o, p);
		if (q == 'B' || q == 'D')
			puts_(o, " const");
		if (q == 'C' || q == 'D')
			puts_(o, " volatile");
		puts_(o, " *");
		return;
	}
	case '_':
		c = *(*p)++;
		for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
			if (ext[i].c == c) {
				puts_(o, ext[i].name);
				return;
			}
		break;
	default:
		for (size_t i = 0; i < sizeof(basic) / sizeof(basic[0]); i++)
			if (basic[i].c == c) {
				puts_(o, basic[i].name);
				return;
			}
	}
	o->bad = 1;
}

/*
 * The undecorated name of a type descriptor's ".?AVfoo@@" (class foo) or
 * ".H" (int); unknown forms give the decorated name without its dot.
 */
static char *undecorate(const char *mangled)
{
	struct out o = { .n = 0 };
	const char *p = mangled;

	if (*p == '.')
		p++;
	if (p[0] == '?' && p[1] == 'A')
		p += 2;
	type_name(&o, &p);
	return strdup(o.bad || *p ? (mangled[0] == '.' ? mangled + 1 : mangled) :
		      o.buf);
}

static const char *ti_name(struct type_info *ti)
{
	pthread_mutex_lock(&name_lock);
	if (!ti->name)
		ti->name = undecorate(ti->mangled);
	pthread_mutex_unlock(&name_lock);
	return ti->name;
}

static THISCALL const char *crt_ti_name(struct type_info *ti)
{
	return ti_name(ti);
}

static THISCALL const char *crt_ti_raw_name(struct type_info *ti)
{
	return ti->mangled;
}

static THISCALL int crt_ti_equal(const struct type_info *a,
				 const struct type_info *b)
{
	return a == b || !strcmp(a->mangled + 1, b->mangled + 1);
}

static THISCALL int crt_ti_not_equal(const struct type_info *a,
				     const struct type_info *b)
{
	return !crt_ti_equal(a, b);
}

static THISCALL int crt_ti_before(const struct type_info *a,
				  const struct type_info *b)
{
	return strcmp(a->mangled + 1, b->mangled + 1) < 0;
}

/* The names were never the DLL's to free; they stay. */
static THISCALL void crt_ti_dtor(struct type_info *ti)
{
}

static THISCALL void *crt_ti_vector_dtor(struct type_info *ti, unsigned flags)
{
	if (flags & 1)
		free(ti);
	return ti;
}

/* A vftable, with the RTTI locator slot MSVC keeps before it. */
struct vtable {
	const void *locator;
	const void *fn[2];
};

static const struct vtable ti_vtable = { NULL, { (void *)crt_ti_vector_dtor } };

/* vcruntime140's type_info helpers, on the part after the vtable. */
struct std_type_info_data {
	char *name;
	char mangled[1];
};

CRTFN(const char *) crt___std_type_info_name(struct std_type_info_data *d,
					     void *list)
{
	return ti_name((struct type_info *)((BYTE *)d - sizeof(void *)));
}

CRTFN(int) crt___std_type_info_compare(const struct std_type_info_data *a,
				       const struct std_type_info_data *b)
{
	return a == b ? 0 : strcmp(a->mangled + 1, b->mangled + 1);
}

CRTFN(size_t) crt___std_type_info_hash(const struct std_type_info_data *d)
{
	size_t h = 2166136261u;	/* FNV-1a, as vcruntime's */
	for (const char *p = d->mangled + 1; *p; p++)
		h = (h ^ (BYTE)*p) * 16777619u;
	return h;
}

CRTFN(void) crt___std_type_info_destroy_list(void *list)
{
}

/* std::exception, as msvcrt lays it out. */

struct exception {
	const void *vtable;
	char *name;
	int do_free;
};

static const struct vtable exception_vtable;

static void exception_init(struct exception *e, const char *name, int copy)
{
	e->vtable = exception_vtable.fn;
	e->name = NULL;
	e->do_free = 0;
	if (name && copy) {
		e->name = strdup(name);
		e->do_free = e->name != NULL;
	} else {
		e->name = (char *)name;
	}
}

static THISCALL struct exception *crt_exc_ctor(struct exception *e)
{
	exception_init(e, NULL, 0);
	return e;
}

static THISCALL struct exception *crt_exc_ctor_name(struct exception *e,
						    const char *const *name)
{
	exception_init(e, name ? *name : NULL, 1);
	return e;
}

static THISCALL struct exception *crt_exc_copy(struct exception *e,
					       const struct exception *rhs)
{
	exception_init(e, rhs->name, rhs->do_free);
	return e;
}

static THISCALL void crt_exc_dtor(struct exception *e)
{
	if (e->do_free)
		free(e->name);
	e->name = NULL;
	e->do_free = 0;
}

static THISCALL struct exception *crt_exc_assign(struct exception *e,
						 const struct exception *rhs)
{
	if (e != rhs) {
		crt_exc_dtor(e);
		exception_init(e, rhs->name, rhs->do_free);
	}
	return e;
}

static THISCALL const char *crt_exc_what(const struct exception *e)
{
	return e->name ? e->name : "Unknown exception";
}

static THISCALL void *crt_exc_vector_dtor(struct exception *e, unsigned flags)
{
	crt_exc_dtor(e);
	if (flags & 1)
		free(e);
	return e;
}

static const struct vtable exception_vtable = { NULL, { (void *)crt_exc_vector_dtor,
			       (void *)crt_exc_what } };

/* vcruntime140's: the what() string of std::exception, copied or not. */
struct std_exception_data {
	const char *what;
	BYTE do_free;
};

CRTFN(void) crt___std_exception_copy(const struct std_exception_data *src,
				     struct std_exception_data *dst)
{
	if (src->do_free && src->what) {
		dst->what = strdup(src->what);
		dst->do_free = dst->what != NULL;
	} else {
		dst->what = src->what;
		dst->do_free = 0;
	}
}

CRTFN(void) crt___std_exception_destroy(struct std_exception_data *d)
{
	if (d->do_free)
		free((char *)d->what);
	d->what = NULL;
	d->do_free = 0;
}

#define E(name) { #name, 0, (void *)crt_##name }
#define M(mangled, fn) { mangled, 0, (void *)fn }

const struct builtin_export crt_cxx_exports[] = {
	/* The EH core. */
	{ "_CxxThrowException", 0, (void *)crt__CxxThrowException },
	{ "__CxxFrameHandler", 0, (void *)crt___CxxFrameHandler },
	{ "__CxxFrameHandler2", 0, (void *)crt___CxxFrameHandler },
	{ "__CxxFrameHandler3", 0, (void *)crt___CxxFrameHandler },
	E(__uncaught_exception),
	E(__uncaught_exceptions),
	E(__processing_throw),
	E(__current_exception),
	E(__current_exception_context),
	{ "__std_terminate", 0, (void *)crt_terminate },
	M("?terminate@@YAXXZ", crt_terminate),
	M("?unexpected@@YAXXZ", crt_unexpected),
	M("?set_terminate@@YAP6AXXZP6AXXZ@Z", crt_set_terminate),
	M("?set_unexpected@@YAP6AXXZP6AXXZ@Z", crt_set_unexpected),
	{ "terminate", 0, (void *)crt_terminate },
	{ "set_terminate", 0, (void *)crt_set_terminate },
	E(_get_terminate),
	E(_get_unexpected),
	E(abort),
	E(_purecall),
	E(_set_purecall_handler),
	E(_get_purecall_handler),
	/* operator new and delete. */
	M("??2@YAPAXI@Z", crt_operator_new),
	M("??_U@YAPAXI@Z", crt_operator_new),
	M("??3@YAXPAX@Z", crt_operator_delete),
	M("??_V@YAXPAX@Z", crt_operator_delete),
	M("?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z", crt__set_new_handler),
	M("?_query_new_handler@@YAP6AHI@ZXZ", crt__query_new_handler),
	M("?_set_new_mode@@YAHH@Z", crt__set_new_mode),
	M("?_query_new_mode@@YAHXZ", crt__query_new_mode),
	E(_set_new_handler),
	E(_set_new_mode),
	E(_query_new_handler),
	E(_query_new_mode),
	/* type_info. */
	M("??_7type_info@@6B@", ti_vtable.fn),
	M("??1type_info@@UAE@XZ", crt_ti_dtor),
	M("?_type_info_dtor_internal_method@type_info@@QAEXXZ", crt_ti_dtor),
	M("??8type_info@@QBEHABV0@@Z", crt_ti_equal),
	M("??9type_info@@QBEHABV0@@Z", crt_ti_not_equal),
	M("?before@type_info@@QBEHABV0@@Z", crt_ti_before),
	M("?name@type_info@@QBEPBDXZ", crt_ti_name),
	M("?raw_name@type_info@@QBEPBDXZ", crt_ti_raw_name),
	E(__std_type_info_name),
	E(__std_type_info_compare),
	E(__std_type_info_hash),
	E(__std_type_info_destroy_list),
	/* std::exception. */
	M("??_7exception@@6B@", exception_vtable.fn),
	M("??0exception@@QAE@XZ", crt_exc_ctor),
	M("??0exception@@QAE@ABQBD@Z", crt_exc_ctor_name),
	M("??0exception@@QAE@ABV0@@Z", crt_exc_copy),
	M("??1exception@@UAE@XZ", crt_exc_dtor),
	M("??4exception@@QAEAAV0@ABV0@@Z", crt_exc_assign),
	M("?what@exception@@UBEPBDXZ", crt_exc_what),
	M("??_Eexception@@UAEPAXI@Z", crt_exc_vector_dtor),
	M("??_Gexception@@UAEPAXI@Z", crt_exc_vector_dtor),
	E(__std_exception_copy),
	E(__std_exception_destroy),
	BUILTIN_END
};
