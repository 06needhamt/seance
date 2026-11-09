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
 * __CxxFrameHandler4 (cl 2019 and later) decodes its compressed FuncInfo4
 * into the same tables. Not done yet (docs/X64_PLAN.md, phase 5): try
 * blocks inside catch funclets (cl gives these funclets their own handler
 * data), exception specifications, the SE translator, and RTTI.
 */
#if defined(__x86_64__)

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

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
#define ARENA_SIZE 0x10000
static BYTE *arena;

static void (STUB_CDECL *terminate_handler)(void);

CRTFN(void) crt_terminate(void)
{
	if (terminate_handler)
		terminate_handler();
	fprintf(stderr, "seance: terminate called\n");
	fflush(stderr);
	crash_exit(SIGABRT);
}

/*
 * _set_se_translator: a function (per thread) that gets a structured
 * exception, as /EHa code with a try block meets it, and may throw a C++
 * exception for it; if it returns, the search goes on.
 */
typedef void (STUB_CDECL *translator_fn)(unsigned code, EXCEPTION_POINTERS *ep);

static _Thread_local translator_fn se_translator;

CRTFN(translator_fn) crt__set_se_translator(translator_fn f)
{
	translator_fn old = se_translator;

	se_translator = f;
	return old;
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

/*
 * A function's tables in one form whichever handler it has: __CxxFrameHandler3
 * has them as structures in the image, __CxxFrameHandler4 as a byte stream of
 * compressed integers. Both are decoded once, on the first exception to meet
 * the function, and kept.
 */
enum uw_kind { UW_NONE, UW_FUNCLET, UW_DTOR_OBJ, UW_DTOR_PTR };

struct n_uw {
	int to_state;
	enum uw_kind kind;
	DWORD action;		/* RVA */
	int object;		/* frame offset, for the dtor kinds */
};

struct n_catch {
	UINT adjectives;
	DWORD type;		/* RVA, 0 for catch (...) */
	int obj_offset;
	DWORD handler;		/* RVA of the funclet */
	int ncont;		/* continuations given in the tables (FH4) */
	DWORD cont[2];		/* RVAs */
};

struct n_try {
	int try_low, try_high, catch_high;
	int ncatch;
	struct n_catch *catches;
};

struct n_info {
	struct n_info *next;
	const void *key;
	DWORD begin;		/* of the function, for FH4 */
	int max_state;
	struct n_uw *uw;
	UINT nip;
	struct ip_state *ip;	/* absolute RVAs */
	int ip_owned;
	UINT ntry;
	struct n_try *tries;
	UINT flags;		/* FUNC_DESCR_* */
	int is_catch;		/* a catch funclet (FH4) */
	int frame_disp;		/* ... where its parent's frame is kept, from its own */
	int bad;
};

static struct n_info *infos;
static pthread_mutex_t infos_lock = PTHREAD_MUTEX_INITIALIZER;

static struct n_info *n_build3(const struct func_info *d, ULONGLONG base)
{
	struct n_info *n = calloc(1, sizeof(*n));
	const struct unwind_entry *u =
		(const void *)(uintptr_t)(base + d->unwind_map);
	const struct try_block *t = (const void *)(uintptr_t)(base + d->try_map);

	n->max_state = d->max_state;
	n->flags = d->flags;
	n->uw = calloc(d->max_state ? d->max_state : 1, sizeof(*n->uw));
	for (int i = 0; i < d->max_state; i++) {
		n->uw[i].to_state = u[i].to_state;
		n->uw[i].action = u[i].action;
		n->uw[i].kind = u[i].action ? UW_FUNCLET : UW_NONE;
	}
	n->nip = d->nip;
	n->ip = (void *)(uintptr_t)(base + d->ip_map);	/* the same layout */
	n->ntry = d->ntry;
	n->tries = calloc(d->ntry ? d->ntry : 1, sizeof(*n->tries));
	for (UINT i = 0; i < d->ntry; i++) {
		const struct catch_block *c =
			(const void *)(uintptr_t)(base + t[i].catches);

		n->tries[i].try_low = t[i].try_low;
		n->tries[i].try_high = t[i].try_high;
		n->tries[i].catch_high = t[i].catch_high;
		n->tries[i].ncatch = t[i].ncatch;
		n->tries[i].catches = calloc(t[i].ncatch ? t[i].ncatch : 1,
					     sizeof(struct n_catch));
		for (int j = 0; j < t[i].ncatch; j++) {
			n->tries[i].catches[j].adjectives = c[j].adjectives;
			n->tries[i].catches[j].type = c[j].type;
			n->tries[i].catches[j].obj_offset = c[j].obj_offset;
			n->tries[i].catches[j].handler = c[j].handler;
		}
	}
	return n;
}

/*
 * The compressed integers of FuncInfo4: the low bits of the first byte give
 * the length (xxx0 one byte, xx01 two, x011 three, 0111 four, 1111 five:
 * the value being the bytes, little-endian, without those bits).
 */
static uint32_t rd_unsigned(const BYTE **p)
{
	const BYTE *b = *p;
	uint32_t v;

	if (!(b[0] & 1)) {
		v = b[0] >> 1;
		*p += 1;
	} else if (!(b[0] & 2)) {
		v = (b[0] | b[1] << 8) >> 2;
		*p += 2;
	} else if (!(b[0] & 4)) {
		v = (b[0] | b[1] << 8 | b[2] << 16) >> 3;
		*p += 3;
	} else if (!(b[0] & 8)) {
		v = (b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24) >> 4;
		*p += 4;
	} else {
		v = b[1] | b[2] << 8 | b[3] << 16 | (uint32_t)b[4] << 24;
		*p += 5;
	}
	return v;
}

static int32_t rd_int(const BYTE **p)
{
	int32_t v;

	memcpy(&v, *p, 4);
	*p += 4;
	return v;
}

static struct n_info *n_build4(const BYTE *p, ULONGLONG base, DWORD begin)
{
	struct n_info *n = calloc(1, sizeof(*n));
	BYTE hdr = *p++;
	int is_sep = hdr >> 1 & 1, has_bbt = hdr >> 2 & 1;
	DWORD uwmap = 0, trymap = 0, ipmap = 0;

	n->is_catch = hdr & 1;
	n->flags = (hdr >> 5 & 1 ? FUNC_DESCR_SYNCHRONOUS : 0) |
		   (hdr >> 6 & 1 ? FUNC_DESCR_NOEXCEPT : 0);
	if (has_bbt)
		rd_unsigned(&p);
	if (hdr >> 3 & 1)
		uwmap = rd_int(&p);
	if (hdr >> 4 & 1)
		trymap = rd_int(&p);
	if (is_sep) {
		/* Code split into pieces (profile-guided layout): one IP map each. */
		const BYTE *seg = (const BYTE *)(uintptr_t)(base + rd_int(&p));
		uint32_t cnt = rd_unsigned(&seg);

		for (uint32_t i = 0; i < cnt; i++) {
			DWORD start = rd_int(&seg);
			DWORD map = rd_int(&seg);

			if (start == begin) {
				ipmap = map;
				break;
			}
		}
	} else {
		ipmap = rd_int(&p);
	}
	if (n->is_catch)
		n->frame_disp = rd_unsigned(&p);
	n->begin = begin;

	if (uwmap) {
		const BYTE *u = (const BYTE *)(uintptr_t)(base + uwmap);
		uint32_t cnt = rd_unsigned(&u);
		const BYTE *start = u;
		const BYTE **offs = calloc(cnt ? cnt : 1, sizeof(*offs));

		n->max_state = cnt;
		n->uw = calloc(cnt ? cnt : 1, sizeof(*n->uw));
		for (uint32_t i = 0; i < cnt; i++) {
			const BYTE *at = u;
			uint32_t v = rd_unsigned(&u);
			uint32_t next = v >> 2;

			offs[i] = at;
			n->uw[i].kind = (enum uw_kind[]){ UW_NONE, UW_DTOR_OBJ,
							  UW_DTOR_PTR, UW_FUNCLET }[v & 3];
			if ((v & 3) == 1 || (v & 3) == 2) {
				n->uw[i].action = rd_int(&u);
				n->uw[i].object = rd_unsigned(&u);
			} else if ((v & 3) == 3) {
				n->uw[i].action = rd_int(&u);
			}
			/* nextOffset: back from this entry to the one it unwinds to. */
			const BYTE *to = at - next;
			int ts = -1;

			if (to >= start)
				for (uint32_t j = 0; j < i; j++)
					if (offs[j] == to)
						ts = j;
			n->uw[i].to_state = ts;
		}
		free(offs);
	}

	if (ipmap) {
		const BYTE *m = (const BYTE *)(uintptr_t)(base + ipmap);
		uint32_t cnt = rd_unsigned(&m), rel = 0;

		n->nip = cnt;
		n->ip = calloc(cnt ? cnt : 1, sizeof(*n->ip));
		n->ip_owned = 1;
		for (uint32_t i = 0; i < cnt; i++) {
			rel += rd_unsigned(&m);
			n->ip[i].ip = begin + rel;
			n->ip[i].state = (int)rd_unsigned(&m) - 1;
		}
	}

	if (trymap) {
		const BYTE *t = (const BYTE *)(uintptr_t)(base + trymap);
		uint32_t cnt = rd_unsigned(&t);

		n->ntry = cnt;
		n->tries = calloc(cnt ? cnt : 1, sizeof(*n->tries));
		for (uint32_t i = 0; i < cnt; i++) {
			n->tries[i].try_low = rd_unsigned(&t);
			n->tries[i].try_high = rd_unsigned(&t);
			n->tries[i].catch_high = rd_unsigned(&t);
			DWORD harr = rd_int(&t);
			const BYTE *h = (const BYTE *)(uintptr_t)(base + harr);
			uint32_t hc = rd_unsigned(&h);

			n->tries[i].ncatch = hc;
			n->tries[i].catches = calloc(hc ? hc : 1, sizeof(struct n_catch));
			for (uint32_t j = 0; j < hc; j++) {
				struct n_catch *c = &n->tries[i].catches[j];
				BYTE ch = *h++;
				int cont_rva = ch >> 3 & 1, nc = ch >> 4 & 3;

				if (ch & 1)
					c->adjectives = rd_unsigned(&h);
				if (ch & 2)
					c->type = rd_int(&h);
				if (ch & 4)
					c->obj_offset = rd_unsigned(&h);
				c->handler = rd_int(&h);
				if (nc == 3)
					nc = 0;	/* reserved */
				c->ncont = nc;
				for (int k = 0; k < nc; k++)
					c->cont[k] = cont_rva ? (DWORD)rd_int(&h) :
						begin + rd_unsigned(&h);
			}
		}
	}
	return n;
}

static const struct n_info *n_get(const void *key, ULONGLONG base, DWORD begin,
				  int v4)
{
	struct n_info *n;

	pthread_mutex_lock(&infos_lock);
	for (n = infos; n; n = n->next)
		if (n->key == key && n->begin == (v4 ? begin : 0))
			goto out;
	n = v4 ? n_build4(key, base, begin) : n_build3(key, base);
	n->key = key;
	if (!v4)
		n->begin = 0;
	n->next = infos;
	infos = n;
out:
	pthread_mutex_unlock(&infos_lock);
	return n;
}

/*
 * A module unloaded: its tables are gone, and another image may be mapped
 * at the same place with others. (Nothing runs from it by then.)
 */
void cxx_forget_image(const void *base, size_t len)
{
	const BYTE *lo = base, *hi = lo + len;

	pthread_mutex_lock(&infos_lock);
	for (struct n_info **p = &infos; *p;) {
		struct n_info *n = *p;
		const BYTE *k = n->key;

		if (k >= lo && k < hi) {
			*p = n->next;
			free(n->uw);
			for (UINT i = 0; i < n->ntry; i++)
				free(n->tries[i].catches);
			free(n->tries);
			if (n->ip_owned)	/* FH3's points into the image */
				free(n->ip);
			free(n);
			continue;
		}
		p = &n->next;
	}
	pthread_mutex_unlock(&infos_lock);
}

static int ip_state(const struct n_info *d, DWORD rva)
{
	int state = -1;

	for (UINT i = 0; i < d->nip && d->ip[i].ip <= rva; i++)
		state = d->ip[i].state;
	return state;
}

/* The state the function is in at pc: its catch block's, if one is running. */
static int state_at(const struct n_info *d, ULONGLONG base,
		    ULONGLONG frame, ULONGLONG pc)
{
	for (struct catch_rec *c = catching; c; c = c->outer)
		if (c->frame == frame && c->pc == pc)
			return c->state;
	return ip_state(d, (DWORD)(pc - base));
}

/* Runs the destroying actions from state cur down to last. */
static void local_unwind(const struct n_info *d, ULONGLONG base,
			 void *frame, int cur, int last)
{
	while (cur != last) {
		if (cur < 0 || cur >= d->max_state)
			return;
		const struct n_uw *u = &d->uw[cur];
		int next = u->to_state;

		switch (u->kind) {
		case UW_FUNCLET:
			((funclet_fn)(uintptr_t)(base + u->action))(NULL, frame);
			break;
		case UW_DTOR_OBJ:
			((dtor_fn)(uintptr_t)(base + u->action))(
				(BYTE *)frame + u->object);
			break;
		case UW_DTOR_PTR:
			((dtor_fn)(uintptr_t)(base + u->action))(
				*(void **)((BYTE *)frame + u->object));
			break;
		default:
			break;
		}
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
find_catch(EXCEPTION_RECORD *rec, CONTEXT *ctx, void *frame, void *objframe,
		       DISPATCHER_CONTEXT *dc, const struct n_info *d,
		       int state)
{
	ULONGLONG base = dc->ImageBase;
	const struct throw_info *ti = is_cxx(rec) ?
		(const void *)(uintptr_t)rec->ExceptionInformation[2] : NULL;
	ULONGLONG ibase = ti ? rec->ExceptionInformation[3] : 0;
	void *object = ti ? (void *)rec->ExceptionInformation[1] : NULL;
	for (UINT i = 0; i < d->ntry; i++) {
		const struct n_try *t = &d->tries[i];
		const struct n_catch *cb = t->catches;

		if (state < t->try_low || state > t->try_high)
			continue;
		for (int j = 0; j < t->ncatch; j++) {
			const struct n_catch *c = &cb[j];
			const struct type_desc *want = c->type ?
				(const void *)(uintptr_t)(base + c->type) : NULL;

			if (ti) {
				const struct cxx_type *ct =
					caught_type(ti, ibase, want, c->adjectives);

				if (!ct)
					continue;
				if (want && c->obj_offset)
					copy_object(object,
						    (void **)((BYTE *)objframe + c->obj_offset),
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
				NULL, objframe);
			seh_link_pop(&cr.link);
			catch_leave(&cr, NULL);

			if (c->ncont) {
				/* The funclet says which continuation, by number. */
				uintptr_t r = (uintptr_t)cont;

				cont = (void *)(uintptr_t)(base +
					c->cont[c->ncont > 1 && r == 2 ? 1 : 0]);
			}
			parent.Rip = (ULONGLONG)(uintptr_t)cont;
			parent.Rax = 0;
			seh_resume(&parent);
		}
	}
}

static EXCEPTION_DISPOSITION
frame_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx,
	      DISPATCHER_CONTEXT *dc, const struct n_info *d)
{
	ULONGLONG base = dc->ImageBase;
	int state;
	/*
	 * A catch funclet keeps the frame of the function it belongs to, which
	 * is where the catch variables and the states' objects are, at a fixed
	 * place in its own frame; a function's is its own.
	 */
	void *objframe = frame;

	if (d->is_catch)
		objframe = *(void **)((BYTE *)frame + d->frame_disp);
	if ((d->flags & FUNC_DESCR_SYNCHRONOUS) && !is_cxx(rec) &&
	    rec->ExceptionCode != CXX_EXCEPTION)
		return ExceptionContinueSearch;
	if (se_translator && d->ntry && !is_cxx(rec) &&
	    rec->ExceptionCode != CXX_EXCEPTION &&
	    !(rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))) {
		/* A throw from the translator carries on from this frame. */
		EXCEPTION_POINTERS ep = { rec, ctx };

		se_translator(rec->ExceptionCode, &ep);
	}

	state = state_at(d, base, (ULONGLONG)(uintptr_t)frame, dc->ControlPc);

	if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
		/* A funclet starts in its catch's state: not the parent's to undo. */
		int last = d->is_catch && d->nip ? d->ip[0].state : -1;

		if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) {
			if (target.frame == (ULONGLONG)(uintptr_t)frame)
				last = target.last;
			else if (dc->TargetIp >= base + dc->FunctionEntry->BeginAddress &&
				 dc->TargetIp < base + dc->FunctionEntry->EndAddress)
				last = ip_state(d, (DWORD)(dc->TargetIp - base));
		}
		local_unwind(d, base, objframe, state, last);
		return ExceptionContinueSearch;
	}

	if (d->ntry)
		find_catch(rec, ctx, frame, objframe, dc, d, state);

	if (d->flags & FUNC_DESCR_NOEXCEPT)
		crt_terminate();	/* an exception leaving a noexcept function */
	return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION WINAPI
crt___CxxFrameHandler3(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx,
		       DISPATCHER_CONTEXT *dc)
{
	ULONGLONG base = dc->ImageBase;
	const struct func_info *f = (const void *)(uintptr_t)
		(base + *(const DWORD *)dc->HandlerData);

	if (f->magic < CXX_FRAME_MAGIC_VC6 || f->magic > CXX_FRAME_MAGIC_VC8)
		return ExceptionContinueSearch;
	return frame_handler(rec, frame, ctx, dc, n_get(f, base, 0, 0));
}

/* cl 2019 and later: the FuncInfo is the compressed FuncInfo4. */
EXCEPTION_DISPOSITION WINAPI
crt___CxxFrameHandler4(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx,
		       DISPATCHER_CONTEXT *dc)
{
	ULONGLONG base = dc->ImageBase;
	const BYTE *f = (const BYTE *)(uintptr_t)
		(base + *(const DWORD *)dc->HandlerData);

	return frame_handler(rec, frame, ctx, dc,
			     n_get(f, base, dc->FunctionEntry->BeginAddress, 1));
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
		const BYTE *a = (const BYTE *)ti;

		rec.ExceptionCode = CXX_EXCEPTION;
		rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
		rec.NumberParameters = 4;
		rec.ExceptionInformation[0] = CXX_FRAME_MAGIC_VC6;
		rec.ExceptionInformation[1] = (DWORD_PTR)object;
		rec.ExceptionInformation[2] = (DWORD_PTR)ti;
		rec.ExceptionInformation[3] = m ? (DWORD_PTR)m->base :
			a >= arena && a < arena + ARENA_SIZE ? (DWORD_PTR)arena : 0;
	}
	rec.ExceptionAddress = (void *)(uintptr_t)ctx->Rip;
	seh_dispatch(&rec, ctx);
}


/*
 * The classes msvcrt itself throws and RTTI names (type_info and the
 * std::exception family) need the same tables a compiler would make, all
 * RVAs from one base. They are built at startup in one executable arena
 * (RVAs to host functions go through small jump thunks inside it).
 */
static size_t arena_used;

static void *arena_alloc(size_t n, size_t align)
{
	arena_used = (arena_used + align - 1) & ~(align - 1);
	void *p = arena + arena_used;

	arena_used += n;
	return p;
}

static DWORD rva_of(const void *p)
{
	return (DWORD)((const BYTE *)p - arena);
}

/* An RVA for a host function: a thunk in the arena that jumps to it. */
static DWORD thunk_for(void *fn)
{
	BYTE *t = arena_alloc(16, 16);

	t[0] = 0x48; t[1] = 0xb8;		/* movabs rax, fn */
	memcpy(t + 2, &fn, 8);
	t[10] = 0xff; t[11] = 0xe0;		/* jmp rax */
	return rva_of(t);
}

struct col {			/* the RTTI complete object locator */
	UINT signature;		/* 1 */
	int offset;		/* of this vftable's pointer in the complete object */
	UINT cd_offset;
	DWORD type, hierarchy, self;
};

struct chd {			/* class hierarchy descriptor */
	UINT signature, attributes;
	UINT nbases;
	DWORD bases;		/* RVA of DWORD[nbases], RVAs of bcd */
};

struct bcd {			/* base class descriptor */
	DWORD type;
	UINT nbases;		/* the entries after it that are its bases */
	struct this_offsets offsets;
	UINT attributes;
	DWORD hierarchy;
};

#define CHD_MULTINH    1
#define BCD_NOTVISIBLE 1
#define BCD_AMBIGUOUS  2

/* std::exception's layout (vcruntime140): vftable, what, do_free. */
struct exc {
	const void *const *vtable;
	const char *what;
	BYTE do_free;
};

enum { C_TYPEINFO, C_EXCEPTION, C_BAD_ALLOC, C_BAD_CAST, C_BAD_TYPEID,
       C_NON_RTTI, C_BAD_EXCEPTION, C_BAD_FUNCTION_CALL,
       C_LOGIC_ERROR, C_DOMAIN_ERROR, C_INVALID_ARGUMENT, C_LENGTH_ERROR,
       C_OUT_OF_RANGE, C_RUNTIME_ERROR, C_OVERFLOW_ERROR, C_UNDERFLOW_ERROR,
       C_RANGE_ERROR, NCLASS };

static const struct {
	const char *mangled;
	int parent;
} classes[NCLASS] = {
	[C_TYPEINFO]  = { ".?AVtype_info@@", -1 },
	[C_EXCEPTION] = { ".?AVexception@std@@", -1 },
	[C_BAD_ALLOC] = { ".?AVbad_alloc@std@@", C_EXCEPTION },
	[C_BAD_CAST]  = { ".?AVbad_cast@std@@", C_EXCEPTION },
	[C_BAD_TYPEID] = { ".?AVbad_typeid@std@@", C_EXCEPTION },
	[C_NON_RTTI]  = { ".?AV__non_rtti_object@std@@", C_BAD_TYPEID },
	/* msvcp's, all of exception's layout. */
	[C_BAD_EXCEPTION] = { ".?AVbad_exception@std@@", C_EXCEPTION },
	[C_BAD_FUNCTION_CALL] = { ".?AVbad_function_call@std@@", C_EXCEPTION },
	[C_LOGIC_ERROR] = { ".?AVlogic_error@std@@", C_EXCEPTION },
	[C_DOMAIN_ERROR] = { ".?AVdomain_error@std@@", C_LOGIC_ERROR },
	[C_INVALID_ARGUMENT] = { ".?AVinvalid_argument@std@@", C_LOGIC_ERROR },
	[C_LENGTH_ERROR] = { ".?AVlength_error@std@@", C_LOGIC_ERROR },
	[C_OUT_OF_RANGE] = { ".?AVout_of_range@std@@", C_LOGIC_ERROR },
	[C_RUNTIME_ERROR] = { ".?AVruntime_error@std@@", C_EXCEPTION },
	[C_OVERFLOW_ERROR] = { ".?AVoverflow_error@std@@", C_RUNTIME_ERROR },
	[C_UNDERFLOW_ERROR] = { ".?AVunderflow_error@std@@", C_RUNTIME_ERROR },
	[C_RANGE_ERROR] = { ".?AVrange_error@std@@", C_RUNTIME_ERROR },
};

static struct type_desc *class_td[NCLASS];
static const struct throw_info *class_throw[NCLASS];
/* vftables: the locator, then the virtual functions; objects point at [1]. */
static const void *vftable[NCLASS][3];

static void WINAPI exc_dtor_free(struct exc *e);
static void *WINAPI exc_vdtor(struct exc *e, unsigned flags);
static const char *WINAPI exc_what(const struct exc *e);
static void *WINAPI ti_vdtor(void *ti, unsigned flags);

static void build_class(int c)
{
	int chain[8], n = 0;

	for (int k = c; k >= 0; k = classes[k].parent)
		chain[n++] = k;

	size_t len = strlen(classes[c].mangled) + 1;
	struct type_desc *td = arena_alloc(sizeof(*td) + len, 8);

	td->vtable = &vftable[C_TYPEINFO][1];
	memcpy(td->name, classes[c].mangled, len);
	class_td[c] = td;

	struct chd *h = arena_alloc(sizeof(*h), 4);
	DWORD *barr = arena_alloc(n * sizeof(DWORD), 4);

	h->nbases = n;
	h->bases = rva_of(barr);
	for (int i = 0; i < n; i++) {
		struct bcd *b = arena_alloc(sizeof(*b), 4);

		b->type = rva_of(class_td[chain[i]]);
		b->nbases = n - 1 - i;
		b->offsets.vbase_descr = -1;
		b->hierarchy = rva_of(h);
		barr[i] = rva_of(b);
	}

	struct col *l = arena_alloc(sizeof(*l), 4);

	l->signature = 1;
	l->type = rva_of(td);
	l->hierarchy = rva_of(h);
	l->self = rva_of(l);
	vftable[c][0] = l;
	if (c == C_TYPEINFO) {
		vftable[c][1] = (void *)ti_vdtor;
	} else {
		vftable[c][1] = (void *)exc_vdtor;
		vftable[c][2] = (void *)exc_what;

		/* ThrowInfo: the class, then each base it converts to. */
		struct cxx_type_table *t =
			arena_alloc(sizeof(*t) + n * sizeof(DWORD), 4);

		t->count = n;
		for (int i = 0; i < n; i++) {
			struct cxx_type *ct = arena_alloc(sizeof(*ct), 4);

			ct->type = rva_of(class_td[chain[i]]);
			ct->offsets.vbase_descr = -1;
			ct->size = sizeof(struct exc);
			t->types[i] = rva_of(ct);
		}
		struct throw_info *ti = arena_alloc(sizeof(*ti), 4);

		ti->destructor = thunk_for((void *)exc_dtor_free);
		ti->table = rva_of(t);
		class_throw[c] = ti;
	}
}

static void __attribute__((constructor)) cxx_init(void)
{
	arena = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	if (arena == MAP_FAILED)
		arena = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (arena == MAP_FAILED) {
		arena = NULL;
		return;
	}
	for (int c = 0; c < NCLASS; c++)
		build_class(c);
}

/* std::exception. */

static struct exc *WINAPI exc_init(struct exc *e, int c, const char *what)
{
	e->vtable = &vftable[c][1];
	e->what = what;
	e->do_free = 0;
	return e;
}

/* MSVC's std::string: a 16-byte buffer or a pointer, then size and capacity. */
static const char *msvc_string_data(const void *str)
{
	const BYTE *b = str;

	return *(const size_t *)(b + 24) >= 16 ? *(const char *const *)b :
		(const char *)b;
}

#define EXC_CLASS(name, id) \
static __attribute__((unused)) struct exc *WINAPI name##_ctor(struct exc *e, const char *what) \
{ return exc_init(e, id, what); } \
static __attribute__((unused)) struct exc *WINAPI name##_ctor_ref(struct exc *e, const char *const *what) \
{ return exc_init(e, id, what ? *what : NULL); } \
static __attribute__((unused)) struct exc *WINAPI name##_default(struct exc *e) \
{ return exc_init(e, id, NULL); } \
static __attribute__((unused)) struct exc *WINAPI name##_ctor_str(struct exc *e, const void *str) \
{ exc_init(e, id, NULL); \
  if (str) { char *c = strdup(msvc_string_data(str)); \
	     if (c) { e->what = c; e->do_free = 1; } } \
  return e; } \
static __attribute__((unused)) struct exc *WINAPI name##_copy(struct exc *e, const struct exc *o) \
{ exc_init(e, id, o->what); \
  if (o->do_free && o->what) { e->what = strdup(o->what); e->do_free = 1; } \
  return e; }

EXC_CLASS(exc, C_EXCEPTION)
EXC_CLASS(bad_alloc, C_BAD_ALLOC)
EXC_CLASS(bad_cast, C_BAD_CAST)
EXC_CLASS(bad_typeid, C_BAD_TYPEID)
EXC_CLASS(non_rtti, C_NON_RTTI)
EXC_CLASS(bad_exception, C_BAD_EXCEPTION)
EXC_CLASS(bad_function_call, C_BAD_FUNCTION_CALL)
EXC_CLASS(logic_error, C_LOGIC_ERROR)
EXC_CLASS(domain_error, C_DOMAIN_ERROR)
EXC_CLASS(invalid_argument, C_INVALID_ARGUMENT)
EXC_CLASS(length_error, C_LENGTH_ERROR)
EXC_CLASS(out_of_range, C_OUT_OF_RANGE)
EXC_CLASS(runtime_error, C_RUNTIME_ERROR)
EXC_CLASS(overflow_error, C_OVERFLOW_ERROR)
EXC_CLASS(underflow_error, C_UNDERFLOW_ERROR)
EXC_CLASS(range_error, C_RANGE_ERROR)

static void WINAPI exc_dtor(struct exc *e)
{
	if (e->do_free)
		free((void *)e->what);
	e->what = NULL;
	e->do_free = 0;
}

static struct exc *WINAPI exc_assign(struct exc *e, const struct exc *o)
{
	if (e != o) {
		exc_dtor(e);
		e->what = o->what;
		if (o->do_free && o->what) {
			e->what = strdup(o->what);
			e->do_free = 1;
		}
	}
	return e;
}

static const char *WINAPI exc_what(const struct exc *e)
{
	return e->what ? e->what : "Unknown exception";
}

/* The scalar deleting destructor. */
static void *WINAPI exc_vdtor(struct exc *e, unsigned flags)
{
	exc_dtor(e);
	if (flags & 1)
		win_free(e);	/* the guest's operator new made it */
	return e;
}

/* A ThrowInfo's destructor for objects msvcrt allocated and throws. */
static void WINAPI exc_dtor_free(struct exc *e)
{
	exc_dtor(e);
	free(e);
}

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
		dst->do_free = 1;
	} else {
		dst->what = src->what;
		dst->do_free = 0;
	}
}

CRTFN(void) crt___std_exception_destroy(struct std_exception_data *d)
{
	if (d->do_free)
		free((void *)d->what);
	d->what = NULL;
	d->do_free = 0;
}

/* type_info. */

static pthread_mutex_t name_lock = PTHREAD_MUTEX_INITIALIZER;
char *crt_undecorate_type(const char *mangled);

static const char *WINAPI ti_name(struct type_desc *ti)
{
	pthread_mutex_lock(&name_lock);
	if (!ti->spare)
		ti->spare = crt_undecorate_type(ti->name);
	pthread_mutex_unlock(&name_lock);
	return ti->spare;
}

static const char *WINAPI ti_raw_name(struct type_desc *ti) { return ti->name; }

static int same_type(const struct type_desc *a, const struct type_desc *b)
{
	return a == b || !strcmp(a->name + 1, b->name + 1);
}

static BOOLEAN WINAPI ti_equal(const struct type_desc *a, const struct type_desc *b)
{
	return same_type(a, b);
}

static BOOLEAN WINAPI ti_not_equal(const struct type_desc *a, const struct type_desc *b)
{
	return !same_type(a, b);
}

static BOOLEAN WINAPI ti_before(const struct type_desc *a, const struct type_desc *b)
{
	return strcmp(a->name + 1, b->name + 1) < 0;
}

static void WINAPI ti_dtor(struct type_desc *ti) { }

static void *WINAPI ti_vdtor(void *ti, unsigned flags)
{
	return ti;
}

/* The caller's context is the one to throw from: see throw_std. */
void vfw_cxx_throw(void *object, const struct throw_info *ti, CONTEXT *ctx)
	__attribute__((noreturn));

static void __attribute__((noreturn))
throw_std(int c, const char *what, CONTEXT *ctx)
{
	struct exc *e = malloc(sizeof(*e));

	if (!e)
		crt_terminate();
	exc_init(e, c, what);
	vfw_cxx_throw(e, class_throw[c], ctx);
}

/* RTTI: __RTtypeid, __RTDynamicCast, __RTCastToVoid. */

struct rtti_query {
	void *object;		/* where its vftable pointer is */
	int vfdelta;		/* that pointer's offset in the source type */
	const struct type_desc *src, *dst;
	const void *result;
};

static const struct col *locator_of(void *object)
{
	return (*(const struct col *const *const *)object)[-1];
}

static BYTE *col_base(const struct col *l)
{
	return (BYTE *)l - l->self;
}

static void rtti_typeid(void *arg)
{
	struct rtti_query *q = arg;
	const struct col *l = locator_of(q->object);

	q->result = col_base(l) + l->type;
}

/*
 * The complete object, as MSVC's runtime finds it: less the vftable
 * pointer's offset in it, plus a construction displacement (stored just
 * before that pointer, while a virtual base's constructor or destructor
 * runs) if the locator says there is one.
 */
static BYTE *complete_of(void *object, const struct col *loc)
{
	BYTE *complete = (BYTE *)object - loc->offset;

	if (loc->cd_offset)
		complete += *(const int *)((BYTE *)object - loc->cd_offset);
	return complete;
}

static void rtti_complete(void *arg)
{
	struct rtti_query *q = arg;

	q->result = complete_of(q->object, locator_of(q->object));
}

/*
 * The base descriptor a dynamic_cast lands on, as MSVC's runtime picks
 * it. The source is found first, by its type (and with multiple
 * inheritance its offset): none is allowed from a base that isn't public.
 */
static const struct bcd *find_target(const struct rtti_query *q, BYTE *base,
				     const struct chd *h, BYTE *complete)
{
	const DWORD *b = (const void *)(base + h->bases);
#define BASE(i) ((const struct bcd *)(base + b[i]))
#define BTYPE(i) ((const struct type_desc *)(base + BASE(i)->type))
	int multi = h->attributes & CHD_MULTINH;
	int n = (int)h->nbases;
	ptrdiff_t src_offset = (BYTE *)q->object - q->vfdelta - complete;
	int src = -1;

	for (int i = 0; i < n && src < 0 && q->src; i++)
		if (same_type(BTYPE(i), q->src) &&
		    (!multi || (BYTE *)this_pointer(&BASE(i)->offsets, complete) -
			       complete == src_offset))
			src = i;
	if (src >= 0 && (BASE(src)->attributes & BCD_NOTVISIBLE))
		return NULL;

	if (!multi) {
		/* Single inheritance: the one instance, if it is public. */
		for (int i = 0; i < n; i++)
			if (same_type(BTYPE(i), q->dst))
				return BASE(i)->attributes & BCD_NOTVISIBLE ?
				       NULL : BASE(i);
		return NULL;
	}

	/* A down-cast: the instance whose subobjects hold the source. */
	for (int i = 0; i < src; i++)
		if (same_type(BTYPE(i), q->dst) &&
		    src <= i + (int)BASE(i)->nbases)
			return BASE(i);
	/* Else a cross-cast: an instance that is public and the only one. */
	for (int i = 0; i < n; i++)
		if (same_type(BTYPE(i), q->dst) &&
		    !(BASE(i)->attributes & (BCD_NOTVISIBLE | BCD_AMBIGUOUS)))
			return BASE(i);
	return NULL;
#undef BASE
#undef BTYPE
}

static void rtti_cast(void *arg)
{
	struct rtti_query *q = arg;
	const struct col *loc = locator_of(q->object);
	BYTE *base = col_base(loc);
	BYTE *complete = complete_of(q->object, loc);
	const struct bcd *b = find_target(q, base,
					  (const void *)(base + loc->hierarchy),
					  complete);

	q->result = b ? this_pointer(&b->offsets, complete) : NULL;
}

/*
 * Called from the entry stubs below with the caller's context, so that
 * what they throw (bad_typeid, bad_cast, __non_rtti_object) starts in the
 * caller's frame.
 */
const void * WINAPI vfw_rttypeid_c(void *object, CONTEXT *ctx)
{
	struct rtti_query q = { .object = object };

	if (!object)
		throw_std(C_BAD_TYPEID, "Attempted a typeid of NULL pointer!", ctx);
	if (seh_guarded(rtti_typeid, &q))
		throw_std(C_NON_RTTI, "Bad read pointer - no RTTI data!", ctx);
	return q.result;
}

void * WINAPI vfw_rtdyncast_c(void *object, int vfdelta,
			      const struct type_desc *src,
			      const struct type_desc *dst, int is_reference,
			      CONTEXT *ctx)
{
	struct rtti_query q = { object, vfdelta, src, dst, NULL };

	if (!object)
		return NULL;
	if (seh_guarded(rtti_cast, &q))
		throw_std(C_NON_RTTI, "Access violation - no RTTI data!", ctx);
	if (!q.result && is_reference)
		throw_std(C_BAD_CAST, "Bad dynamic_cast!", ctx);
	return (void *)q.result;
}

void * WINAPI vfw_rtvoid_c(void *object, CONTEXT *ctx)
{
	struct rtti_query q = { .object = object };

	if (!object)
		return NULL;
	if (seh_guarded(rtti_complete, &q))
		throw_std(C_NON_RTTI, "Access violation - no RTTI data!", ctx);
	return (void *)q.result;
}

/*
 * Each entry point captures its caller's context (as _CxxThrowException
 * does) in a frame of its own, spills the arguments, and calls the C
 * function above with the context as its last argument.
 */
#define RTTI_STUB_HEAD \
	"	subq $0x508, %rsp\n" \
	"	movq %rcx, 0x510(%rsp)\n" \
	"	movq %rdx, 0x518(%rsp)\n" \
	"	movq %r8, 0x520(%rsp)\n" \
	"	movq %r9, 0x528(%rsp)\n" \
	"	leaq 0x30(%rsp), %rcx\n" \
	"	call seh_RtlCaptureContext\n" \
	"	leaq 0x510(%rsp), %rax\n" \
	"	movq %rax, 0xc8(%rsp)\n" \
	"	movq 0x508(%rsp), %rax\n" \
	"	movq %rax, 0x128(%rsp)\n" \
	"	movq 0x510(%rsp), %rcx\n"

__asm__(
	".text\n"
	".globl crt___RTtypeid\n"
	"crt___RTtypeid:\n"
	RTTI_STUB_HEAD
	"	leaq 0x30(%rsp), %rdx\n"
	"	call vfw_rttypeid_c\n"
	"	addq $0x508, %rsp\n"
	"	ret\n"

	".globl crt___RTCastToVoid\n"
	"crt___RTCastToVoid:\n"
	RTTI_STUB_HEAD
	"	leaq 0x30(%rsp), %rdx\n"
	"	call vfw_rtvoid_c\n"
	"	addq $0x508, %rsp\n"
	"	ret\n"

	".globl crt___RTDynamicCast\n"
	"crt___RTDynamicCast:\n"
	RTTI_STUB_HEAD
	"	movq 0x518(%rsp), %rdx\n"
	"	movq 0x520(%rsp), %r8\n"
	"	movq 0x528(%rsp), %r9\n"
	"	movl 0x530(%rsp), %eax\n"
	"	movl %eax, 0x20(%rsp)\n"
	"	leaq 0x30(%rsp), %rax\n"
	"	movq %rax, 0x28(%rsp)\n"
	"	call vfw_rtdyncast_c\n"
	"	addq $0x508, %rsp\n"
	"	ret\n");

extern void crt___RTtypeid(void), crt___RTCastToVoid(void),
	crt___RTDynamicCast(void);

/*
 * msvcp's throwing helpers (std::_Xlength_error and the like, which its
 * containers call): the class thrown, with the message they are given.
 */
#define XTHROW(name, id, msg) \
void WINAPI vfw_##name##_c(const char *what, CONTEXT *ctx) \
{ throw_std(id, (msg) ? (msg) : what, ctx); }
XTHROW(Xlength_error, C_LENGTH_ERROR, NULL)
XTHROW(Xout_of_range, C_OUT_OF_RANGE, NULL)
XTHROW(Xinvalid_argument, C_INVALID_ARGUMENT, NULL)
XTHROW(Xruntime_error, C_RUNTIME_ERROR, NULL)
XTHROW(Xoverflow_error, C_OVERFLOW_ERROR, NULL)
XTHROW(Xbad_alloc, C_BAD_ALLOC, "bad allocation")
XTHROW(Xbad_function_call, C_BAD_FUNCTION_CALL, "bad function call")

#define XSTUB(name) \
	".globl crt__" #name "\n" \
	"crt__" #name ":\n" \
	RTTI_STUB_HEAD \
	"	leaq 0x30(%rsp), %rdx\n" \
	"	call vfw_" #name "_c\n" \
	"	ud2\n"

__asm__(
	".text\n"
	XSTUB(Xlength_error)
	XSTUB(Xout_of_range)
	XSTUB(Xinvalid_argument)
	XSTUB(Xruntime_error)
	XSTUB(Xoverflow_error)
	XSTUB(Xbad_alloc)
	XSTUB(Xbad_function_call));

extern void crt__Xlength_error(void), crt__Xout_of_range(void),
	crt__Xinvalid_argument(void), crt__Xruntime_error(void),
	crt__Xoverflow_error(void), crt__Xbad_alloc(void),
	crt__Xbad_function_call(void);

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

CRTFN(void) crt_operator_delete(void *p) { win_free(p); }
CRTFN(void) crt_operator_delete_sized(void *p, size_t size) { win_free(p); }

CRTFN(void) crt_abort(void)
{
	fprintf(stderr, "seance: a DLL called abort()\n");
	fflush(stderr);
	crash_exit(SIGABRT);
}

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
	M("abort", crt_abort),
	M("_CxxThrowException", crt__CxxThrowException),
	M("__RTtypeid", crt___RTtypeid),
	M("__RTDynamicCast", crt___RTDynamicCast),
	M("__RTCastToVoid", crt___RTCastToVoid),
	M("__std_exception_copy", crt___std_exception_copy),
	M("__std_exception_destroy", crt___std_exception_destroy),
	M("??_7type_info@@6B@", &vftable[C_TYPEINFO][1]),
	M("?name@type_info@@QEBAPEBDXZ", ti_name),
	M("?raw_name@type_info@@QEBAPEBDXZ", ti_raw_name),
	M("??8type_info@@QEBA_NAEBV0@@Z", ti_equal),
	M("??9type_info@@QEBA_NAEBV0@@Z", ti_not_equal),
	M("?before@type_info@@QEBA_NAEBV1@@Z", ti_before),
	M("??1type_info@@UEAA@XZ", ti_dtor),
	M("?what@exception@std@@UEBAPEBDXZ", exc_what),
	M("??_7exception@std@@6B@", &vftable[C_EXCEPTION][1]),
	M("??0exception@std@@QEAA@XZ", exc_default),
	M("??0exception@std@@QEAA@PEBD@Z", exc_ctor),
	M("??0exception@std@@QEAA@AEBQEBD@Z", exc_ctor_ref),
	M("??0exception@std@@QEAA@AEBV01@@Z", exc_copy),
	M("??1exception@std@@UEAA@XZ", exc_dtor),
	M("??4exception@std@@QEAAAEAV01@AEBV01@@Z", exc_assign),
	M("??_7bad_alloc@std@@6B@", &vftable[C_BAD_ALLOC][1]),
	M("??0bad_alloc@std@@QEAA@XZ", bad_alloc_default),
	M("??0bad_alloc@std@@QEAA@PEBD@Z", bad_alloc_ctor),
	M("??0bad_alloc@std@@QEAA@AEBV01@@Z", bad_alloc_copy),
	M("??1bad_alloc@std@@UEAA@XZ", exc_dtor),
	M("??_7bad_cast@std@@6B@", &vftable[C_BAD_CAST][1]),
	M("??0bad_cast@std@@QEAA@XZ", bad_cast_default),
	M("??0bad_cast@std@@QEAA@PEBD@Z", bad_cast_ctor),
	M("??0bad_cast@std@@QEAA@AEBV01@@Z", bad_cast_copy),
	M("??1bad_cast@std@@UEAA@XZ", exc_dtor),
	M("??_7bad_typeid@std@@6B@", &vftable[C_BAD_TYPEID][1]),
	M("??0bad_typeid@std@@QEAA@XZ", bad_typeid_default),
	M("??0bad_typeid@std@@QEAA@PEBD@Z", bad_typeid_ctor),
	M("??0bad_typeid@std@@QEAA@AEBV01@@Z", bad_typeid_copy),
	M("??1bad_typeid@std@@UEAA@XZ", exc_dtor),
	M("??_7__non_rtti_object@std@@6B@", &vftable[C_NON_RTTI][1]),
	M("??0__non_rtti_object@std@@QEAA@PEBD@Z", non_rtti_ctor),
	M("??0__non_rtti_object@std@@QEAA@AEBV01@@Z", non_rtti_copy),
	M("??1__non_rtti_object@std@@UEAA@XZ", exc_dtor),
#define STD_EXPORTS(cls, id) \
	M("??_7" #cls "@std@@6B@", &vftable[id][1]), \
	M("??0" #cls "@std@@QEAA@PEBD@Z", cls##_ctor), \
	M("??0" #cls "@std@@QEAA@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@1@@Z", cls##_ctor_str), \
	M("??0" #cls "@std@@QEAA@AEBV01@@Z", cls##_copy), \
	M("??1" #cls "@std@@UEAA@XZ", exc_dtor), \
	M("??4" #cls "@std@@QEAAAEAV01@AEBV01@@Z", exc_assign), \
	M("??_E" #cls "@std@@UEAAPEAXI@Z", exc_vdtor), \
	M("??_G" #cls "@std@@UEAAPEAXI@Z", exc_vdtor)
	STD_EXPORTS(logic_error, C_LOGIC_ERROR),
	STD_EXPORTS(domain_error, C_DOMAIN_ERROR),
	STD_EXPORTS(invalid_argument, C_INVALID_ARGUMENT),
	STD_EXPORTS(length_error, C_LENGTH_ERROR),
	STD_EXPORTS(out_of_range, C_OUT_OF_RANGE),
	STD_EXPORTS(runtime_error, C_RUNTIME_ERROR),
	STD_EXPORTS(overflow_error, C_OVERFLOW_ERROR),
	STD_EXPORTS(underflow_error, C_UNDERFLOW_ERROR),
	STD_EXPORTS(range_error, C_RANGE_ERROR),
	STD_EXPORTS(bad_function_call, C_BAD_FUNCTION_CALL),
	STD_EXPORTS(bad_exception, C_BAD_EXCEPTION),
	M("?what@logic_error@std@@UEBAPEBDXZ", exc_what),
	M("?what@runtime_error@std@@UEBAPEBDXZ", exc_what),
	M("?_Xlength_error@std@@YAXPEBD@Z", crt__Xlength_error),
	M("?_Xout_of_range@std@@YAXPEBD@Z", crt__Xout_of_range),
	M("?_Xinvalid_argument@std@@YAXPEBD@Z", crt__Xinvalid_argument),
	M("?_Xruntime_error@std@@YAXPEBD@Z", crt__Xruntime_error),
	M("?_Xoverflow_error@std@@YAXPEBD@Z", crt__Xoverflow_error),
	M("?_Xbad_alloc@std@@YAXXZ", crt__Xbad_alloc),
	M("?_Xbad_function_call@std@@YAXXZ", crt__Xbad_function_call),

	M("__CxxFrameHandler3", crt___CxxFrameHandler3),
	M("__CxxFrameHandler4", crt___CxxFrameHandler4),
	M("?_set_se_translator@@YAP6AXIPEAU_EXCEPTION_POINTERS@@@ZP6AXI0@Z@Z",
	  crt__set_se_translator),
	M("_set_se_translator", crt__set_se_translator),
	M("?terminate@@YAXXZ", crt_terminate),
	M("terminate", crt_terminate),
	M("?set_terminate@@YAP6AXXZP6AXXZ@Z", crt_set_terminate),
	M("set_terminate", crt_set_terminate),
	BUILTIN_END
};

#endif /* __x86_64__ */
