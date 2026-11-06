/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt on x86-64: C++ exceptions as cl compiles them (__CxxFrameHandler3
 * and _CxxThrowException), operator new and delete, _purecall.
 *
 * throw X calls _CxxThrowException(object, ThrowInfo), which raises SEH
 * exception 0xE06D7363 with four parameters: the magic, the object, the
 * ThrowInfo and the base of the image it is in (everything in the tables
 * below is an RVA from that base, or from the base of the image the
 * function is in).
 *
 * A function with a try block or objects to destroy has __CxxFrameHandler3
 * as its language handler, with the RVA of a FuncInfo as handler data. The
 * state the function is in is found from where its control pc lies
 * (IpToStateMap), not kept in the frame as on i386. The UnwindMap gives
 * each state the one it unwinds to and an action: a funclet, called with
 * the function's frame in rdx, that destroys what the state made. Catch
 * blocks are funclets too, returning the address to carry on at.
 *
 * On finding a catch block the handler copies the exception into its
 * variable, unwinds the frames above (seh_unwind_run; the stack stays as
 * it is, and with it the exception object, which lives in the thrower's
 * frame), runs the function's own states down to the try, then calls the
 * catch funclet from here and resumes the function at the address it
 * returns. An exception leaving the funclet is handed on from the
 * function's context (a seh_link), in the state of the catch block, and
 * ends the block.
 *
 * Not done yet (docs/X64_PLAN.md, phase 5): __CxxFrameHandler4, try
 * blocks inside catch funclets (cl gives these funclets their own handler
 * data), exception specifications, the SE translator, and RTTI.
 */
#if defined(__x86_64__)

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/builtin.h"
#include "runtime/crash.h"
#include "runtime/seh.h"
#include "win32/crt.h"
#include "win32/internal.h"

#define CRTFN(ret) static ret STUB_CDECL

#define CXX_EXCEPTION          0xE06D7363
#define CXX_FRAME_MAGIC_VC6    0x19930520
#define CXX_FRAME_MAGIC_VC8    0x19930522

#define FUNC_DESCR_SYNCHRONOUS 1	/* /EHs: C++ exceptions only */
#define FUNC_DESCR_NOEXCEPT    4

#define TYPE_FLAG_CONST        1
#define TYPE_FLAG_VOLATILE     2
#define TYPE_FLAG_REFERENCE    8

#define CLASS_IS_SIMPLE_TYPE          1
#define CLASS_HAS_VIRTUAL_BASE_CLASS  4

/* The compiler's tables; every DWORD called an RVA is from the image base. */

struct type_desc {
	const void *vtable;
	void *spare;
	char name[];		/* ".?AVfoo@@" */
};

struct unwind_entry {
	int to_state;
	DWORD action;		/* RVA of a funclet, or 0 */
};

struct catch_block {
	UINT adjectives;	/* TYPE_FLAG_* */
	DWORD type;		/* RVA of a type_desc, 0 for catch (...) */
	int obj_offset;		/* of the catch variable from the frame, or 0 */
	DWORD handler;		/* RVA of the catch funclet */
	int frame_offset;
};

struct try_block {
	int try_low, try_high;	/* the states of the try */
	int catch_high;		/* the highest state in its catch blocks */
	int ncatch;
	DWORD catches;		/* RVA of catch_block[ncatch] */
};

struct ip_state {
	DWORD ip;		/* RVA */
	int state;
};

struct func_info {
	UINT magic;
	int max_state;
	DWORD unwind_map;	/* RVA of unwind_entry[max_state] */
	UINT ntry;
	DWORD try_map;		/* RVA of try_block[ntry] */
	UINT nip;
	DWORD ip_map;		/* RVA of ip_state[nip], sorted */
	int unwind_help;
	DWORD es_list;
	UINT flags;
};

struct this_offsets {
	int this_offset;	/* of the base in the object */
	int vbase_descr;	/* of the virtual base table pointer, or -1 */
	int vbase_offset;	/* of this base's entry in that table */
};

struct cxx_type {		/* CatchableType */
	UINT flags;		/* CLASS_* */
	DWORD type;		/* RVA of a type_desc */
	struct this_offsets offsets;
	UINT size;
	DWORD copy_ctor;	/* RVA, or 0 */
};

struct cxx_type_table {		/* CatchableTypeArray */
	int count;
	DWORD types[];		/* RVAs of cxx_type */
};

struct throw_info {
	UINT flags;		/* TYPE_FLAG_* */
	DWORD destructor;	/* RVA */
	DWORD custom_handler;
	DWORD table;		/* RVA of a cxx_type_table */
};

typedef void (WINAPI *dtor_fn)(void *);
typedef void (WINAPI *copy_fn)(void *, const void *);
typedef void (WINAPI *copy_vbase_fn)(void *, const void *, int);
typedef void (WINAPI *funclet_fn)(void *unused, void *frame);
typedef void *(WINAPI *catch_fn)(void *unused, void *frame);

/* terminate() and its handler. */
static void (STUB_CDECL *terminate_handler)(void);

CRTFN(void) crt_terminate(void)
{
	if (terminate_handler)
		terminate_handler();
	fprintf(stderr, "seance: terminate called\n");
	fflush(stderr);
	crash_exit(SIGABRT);
}

CRTFN(void *) crt_set_terminate(void (STUB_CDECL *f)(void))
{
	void *prev = (void *)terminate_handler;

	terminate_handler = f;
	return prev;
}

/*
 * The catch blocks running on this thread, innermost first: each is an
 * exception being handled (what throw; throws again), the state its
 * function is in while it runs, and the object to destroy when it ends.
 */
struct catch_rec {
	struct catch_rec *outer;
	struct seh_link link;
	EXCEPTION_RECORD *rec;
	void *object;		/* NULL for a non-C++ exception */
	const struct throw_info *ti;
	ULONGLONG ibase;
	ULONGLONG frame;
	ULONGLONG pc;		/* the function's context while the block runs */
	int state;		/* ... and its state there */
	int left;
};

static _Thread_local struct catch_rec *catching;

/* The frame and state the unwind to a catching function ends at. */
static _Thread_local struct {
	ULONGLONG frame;
	int last;
} target;

static int ip_state(const struct func_info *d, ULONGLONG base, DWORD rva)
{
	const struct ip_state *m = (const void *)(uintptr_t)(base + d->ip_map);
	int state = -1;

	for (UINT i = 0; i < d->nip && m[i].ip <= rva; i++)
		state = m[i].state;
	return state;
}

/* The state the function is in at pc: its catch block's, if one is running. */
static int state_at(const struct func_info *d, ULONGLONG base,
		    ULONGLONG frame, ULONGLONG pc)
{
	for (struct catch_rec *c = catching; c; c = c->outer)
		if (c->frame == frame && c->pc == pc)
			return c->state;
	return ip_state(d, base, (DWORD)(pc - base));
}

/* Runs the destroying actions from state cur down to last. */
static void local_unwind(const struct func_info *d, ULONGLONG base,
			 void *frame, int cur, int last)
{
	const struct unwind_entry *u =
		(const void *)(uintptr_t)(base + d->unwind_map);

	while (cur != last) {
		if (cur < 0 || cur >= d->max_state)
			return;
		int next = u[cur].to_state;

		if (u[cur].action)
			((funclet_fn)(uintptr_t)(base + u[cur].action))(NULL,
									frame);
		cur = next;
	}
}

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

/* The type in the exception's list that a catch of want takes, or NULL. */
static const struct cxx_type *caught_type(const struct throw_info *e,
					  ULONGLONG ibase,
					  const struct type_desc *want,
					  UINT flags)
{
	const struct cxx_type_table *t =
		(const void *)(uintptr_t)(ibase + e->table);

	for (int i = 0; i < t->count; i++) {
		const struct cxx_type *ct =
			(const void *)(uintptr_t)(ibase + t->types[i]);
		const struct type_desc *td =
			(const void *)(uintptr_t)(ibase + ct->type);

		if (!want)
			return ct;		/* catch (...) */
		if (want != td && strcmp(want->name, td->name))
			continue;
		/* A const object only binds to a const catch, likewise volatile. */
		if ((e->flags & TYPE_FLAG_CONST) && !(flags & TYPE_FLAG_CONST))
			continue;
		if ((e->flags & TYPE_FLAG_VOLATILE) &&
		    !(flags & TYPE_FLAG_VOLATILE))
			continue;
		return ct;
	}
	return NULL;
}

/* Puts the object, as type t, where a catch of flags puts it. */
static void copy_object(void *object, void **dest, UINT flags,
			const struct cxx_type *t, ULONGLONG ibase)
{
	if (flags & TYPE_FLAG_REFERENCE) {
		*dest = this_pointer(&t->offsets, object);
	} else if (t->flags & CLASS_IS_SIMPLE_TYPE) {
		memmove(dest, object, t->size);
		/* A pointer to a class, caught as a pointer to its base. */
		if (t->size == sizeof(void *))
			*dest = this_pointer(&t->offsets, *dest);
	} else if (t->copy_ctor) {
		void *src = this_pointer(&t->offsets, object);
		void *fn = (void *)(uintptr_t)(ibase + t->copy_ctor);

		if (t->flags & CLASS_HAS_VIRTUAL_BASE_CLASS)
			((copy_vbase_fn)fn)(dest, src, 1);
		else
			((copy_fn)fn)(dest, src);
	} else {
		memmove(dest, this_pointer(&t->offsets, object), t->size);
	}
}

static int is_cxx(const EXCEPTION_RECORD *rec)
{
	return rec->ExceptionCode == CXX_EXCEPTION &&
	       rec->NumberParameters >= 4 &&
	       rec->ExceptionInformation[0] >= CXX_FRAME_MAGIC_VC6 &&
	       rec->ExceptionInformation[0] <= CXX_FRAME_MAGIC_VC8;
}

/* Whether a running catch block other than c holds object. */
static int held(const struct catch_rec *c, const void *object)
{
	for (const struct catch_rec *o = catching; o; o = o->outer)
		if (o != c && o->object == object)
			return 1;
	return 0;
}

/*
 * A catch block is over: off the thread's list, and its exception object
 * destroyed unless it is still in use (being thrown again, or held by an
 * outer catch block).
 */
static void catch_leave(struct catch_rec *c, const void *in_flight)
{
	if (c->left)
		return;
	c->left = 1;
	for (struct catch_rec **p = &catching; *p; p = &(*p)->outer)
		if (*p == c) {
			*p = c->outer;
			break;
		}
	if (c->object && c->ti && c->ti->destructor && c->object != in_flight &&
	    !held(c, c->object))
		((dtor_fn)(uintptr_t)(c->ibase + c->ti->destructor))(c->object);
}

/* An exception is leaving the catch block (see seh_link). */
static void catch_unwound(void *arg, EXCEPTION_RECORD *unwinding)
{
	catch_leave(arg, is_cxx(unwinding) ?
		    (void *)unwinding->ExceptionInformation[1] : NULL);
}

/*
 * Looks for a catch block in the function that takes the exception and,
 * finding one, runs it and carries on after it, never returning.
 *
 * Its catch_rec holds a link whose place on the stack is compared with
 * the unwinder's: AddressSanitizer's fake stack, where it would otherwise
 * put locals, is kept away from it.
 */
static void __attribute__((no_sanitize_address))
find_catch(EXCEPTION_RECORD *rec, CONTEXT *ctx, void *frame,
		       DISPATCHER_CONTEXT *dc, const struct func_info *d,
		       int state)
{
	ULONGLONG base = dc->ImageBase;
	const struct throw_info *ti = is_cxx(rec) ?
		(const void *)(uintptr_t)rec->ExceptionInformation[2] : NULL;
	ULONGLONG ibase = ti ? rec->ExceptionInformation[3] : 0;
	void *object = ti ? (void *)rec->ExceptionInformation[1] : NULL;
	const struct try_block *tries =
		(const void *)(uintptr_t)(base + d->try_map);

	for (UINT i = 0; i < d->ntry; i++) {
		const struct try_block *t = &tries[i];
		const struct catch_block *cb =
			(const void *)(uintptr_t)(base + t->catches);

		if (state < t->try_low || state > t->try_high)
			continue;
		for (int j = 0; j < t->ncatch; j++) {
			const struct catch_block *c = &cb[j];
			const struct type_desc *want = c->type ?
				(const void *)(uintptr_t)(base + c->type) : NULL;

			if (ti) {
				const struct cxx_type *ct =
					caught_type(ti, ibase, want, c->adjectives);

				if (!ct)
					continue;
				if (want && c->obj_offset)
					copy_object(object,
						    (void **)((BYTE *)frame + c->obj_offset),
						    c->adjectives, ct, ibase);
			} else if (want) {
				continue;	/* only catch (...) takes these */
			}

			/* The frames above, then this function down to the try. */
			CONTEXT parent;

			target.frame = (ULONGLONG)(uintptr_t)frame;
			target.last = t->try_low;
			if (!seh_unwind_run(frame, NULL, rec, ctx, &parent)) {
				fprintf(stderr, "seance: C++ unwind: the frame "
					"of the catching function is gone\n");
				crt_terminate();
			}

			struct catch_rec cr;

			memset(&cr, 0, sizeof(cr));
			cr.rec = rec;
			cr.object = object;
			cr.ti = ti;
			cr.ibase = ibase;
			cr.frame = (ULONGLONG)(uintptr_t)frame;
			cr.pc = parent.Rip;
			cr.state = t->catch_high;
			cr.link.parent = parent;
			cr.link.unwound = catch_unwound;
			cr.link.arg = &cr;
			cr.outer = catching;
			catching = &cr;
			seh_link_push(&cr.link);
			void *cont = ((catch_fn)(uintptr_t)(base + c->handler))(
				NULL, frame);
			seh_link_pop(&cr.link);
			catch_leave(&cr, NULL);

			parent.Rip = (ULONGLONG)(uintptr_t)cont;
			parent.Rax = 0;
			seh_resume(&parent);
		}
	}
}

EXCEPTION_DISPOSITION WINAPI
crt___CxxFrameHandler3(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx,
		       DISPATCHER_CONTEXT *dc)
{
	ULONGLONG base = dc->ImageBase;
	const struct func_info *d = (const void *)(uintptr_t)
		(base + *(const DWORD *)dc->HandlerData);
	int state;

	if (d->magic < CXX_FRAME_MAGIC_VC6 || d->magic > CXX_FRAME_MAGIC_VC8)
		return ExceptionContinueSearch;
	if ((d->flags & FUNC_DESCR_SYNCHRONOUS) && !is_cxx(rec) &&
	    rec->ExceptionCode != CXX_EXCEPTION)
		return ExceptionContinueSearch;

	state = state_at(d, base, (ULONGLONG)(uintptr_t)frame, dc->ControlPc);

	if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
		int last = -1;

		if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) {
			if (target.frame == (ULONGLONG)(uintptr_t)frame)
				last = target.last;
			else if (dc->TargetIp >= base + dc->FunctionEntry->BeginAddress &&
				 dc->TargetIp < base + dc->FunctionEntry->EndAddress)
				last = ip_state(d, base,
						(DWORD)(dc->TargetIp - base));
		}
		local_unwind(d, base, frame, state, last);
		return ExceptionContinueSearch;
	}

	if (d->ntry)
		find_catch(rec, ctx, frame, dc, d, state);

	if (d->flags & FUNC_DESCR_NOEXCEPT)
		crt_terminate();	/* an exception leaving a noexcept function */
	return ExceptionContinueSearch;
}

/*
 * _CxxThrowException(object, ThrowInfo): raised from the caller's own
 * context, so the first frame the dispatcher meets is the thrower's. With
 * neither argument it is throw; the exception being handled, again.
 */
__asm__(
	".text\n"
	".globl crt__CxxThrowException\n"
	".type crt__CxxThrowException, @function\n"
	"crt__CxxThrowException:\n"
	"	subq $0x4d8, %rsp\n"
	"	movq %rcx, %r10\n"
	"	movq %rdx, %r11\n"
	"	movq %rsp, %rcx\n"
	"	call seh_RtlCaptureContext\n"
	"	leaq 0x4e0(%rsp), %rax\n"
	"	movq %rax, 0x98(%rsp)\n"
	"	movq 0x4d8(%rsp), %rax\n"
	"	movq %rax, 0xf8(%rsp)\n"
	"	movq %r10, %rdi\n"
	"	movq %r11, %rsi\n"
	"	movq %rsp, %rdx\n"
	"	call vfw_cxx_throw\n"
	"	ud2\n"
	".size crt__CxxThrowException, .-crt__CxxThrowException\n");

extern void crt__CxxThrowException(void);

void __attribute__((used, noreturn))
vfw_cxx_throw(void *object, const struct throw_info *ti, CONTEXT *ctx)
{
	EXCEPTION_RECORD rec;

	memset(&rec, 0, sizeof(rec));
	if (!object && !ti) {
		if (!catching)
			crt_terminate();	/* nothing to rethrow */
		rec = *catching->rec;
		rec.ExceptionRecord = NULL;
	} else {
		struct module *m = mod_from_address(ti);

		rec.ExceptionCode = CXX_EXCEPTION;
		rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
		rec.NumberParameters = 4;
		rec.ExceptionInformation[0] = CXX_FRAME_MAGIC_VC6;
		rec.ExceptionInformation[1] = (DWORD_PTR)object;
		rec.ExceptionInformation[2] = (DWORD_PTR)ti;
		rec.ExceptionInformation[3] = m ? (DWORD_PTR)m->base : 0;
	}
	rec.ExceptionAddress = (void *)(uintptr_t)ctx->Rip;
	seh_dispatch(&rec, ctx);
}

/* No _set_new_mode on x86-64 yet, so malloc never retries. */
int crt_malloc_retry(size_t size)
{
	return 0;
}

CRTFN(void *) crt_operator_new(size_t size)
{
	/* Zeroed unless VFW_ZERO_MEMORY=0, as for the i386 one. */
	return win_alloc(1, size ? size : 1);
}

CRTFN(void) crt_operator_delete(void *p) { free(p); }
CRTFN(void) crt_operator_delete_sized(void *p, size_t size) { free(p); }

CRTFN(void) crt__purecall(void)
{
	fprintf(stderr, "seance: a DLL called a pure virtual function\n");
	fflush(stderr);
	crash_exit(SIGABRT);
}

#define M(mangled, fn) { mangled, 0, (void *)fn }

const struct builtin_export crt_cxx_exports[] = {
	M("??2@YAPEAX_K@Z", crt_operator_new),
	M("??_U@YAPEAX_K@Z", crt_operator_new),
	M("??3@YAXPEAX@Z", crt_operator_delete),
	M("??_V@YAXPEAX@Z", crt_operator_delete),
	M("??3@YAXPEAX_K@Z", crt_operator_delete_sized),
	M("??_V@YAXPEAX_K@Z", crt_operator_delete_sized),
	M("_purecall", crt__purecall),
	M("_CxxThrowException", crt__CxxThrowException),
	M("__CxxFrameHandler3", crt___CxxFrameHandler3),
	M("?terminate@@YAXXZ", crt_terminate),
	M("terminate", crt_terminate),
	M("?set_terminate@@YAP6AXXZP6AXXZ@Z", crt_set_terminate),
	M("set_terminate", crt_set_terminate),
	BUILTIN_END
};

#endif /* __x86_64__ */
