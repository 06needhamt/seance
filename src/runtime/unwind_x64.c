/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The x86-64 stack unwinder: finding the RUNTIME_FUNCTION for a code
 * address (a module's .pdata, or a table a program registered with
 * RtlAddFunctionTable) and RtlVirtualUnwind, which steps a CONTEXT back
 * out of one frame by interpreting the function's UNWIND_INFO, as the
 * PE/COFF specification's "x64 exception handling" section describes.
 *
 * A frame in code this harness itself runs (a built-in DLL's function, a
 * callback trampoline) has no unwind data: the walk stops at one, so an
 * exception cannot be caught by a __try that lies beyond host code.
 */
#if defined(__x86_64__)

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/seh.h"

struct dyn_table {
	struct dyn_table *next;
	PRUNTIME_FUNCTION entries;
	DWORD count;
	ULONGLONG base;
};

static struct dyn_table *dyn_tables;
static pthread_mutex_t dyn_lock = PTHREAD_MUTEX_INITIALIZER;

/* The entry covering rva in a sorted table, or NULL. */
static PRUNTIME_FUNCTION search(PRUNTIME_FUNCTION t, DWORD n, DWORD rva)
{
	DWORD lo = 0, hi = n;

	while (lo < hi) {
		DWORD mid = lo + (hi - lo) / 2;

		if (rva < t[mid].BeginAddress)
			hi = mid;
		else if (rva >= t[mid].EndAddress)
			lo = mid + 1;
		else
			return &t[mid];
	}
	return NULL;
}

PRUNTIME_FUNCTION WINAPI seh_RtlLookupFunctionEntry(ULONGLONG pc,
						    ULONGLONG *image_base,
						    void *history)
{
	PRUNTIME_FUNCTION fe = NULL;
	struct module *m = mod_from_address((const void *)(uintptr_t)pc);

	if (m && m->img && m->base) {
		const IMAGE_DATA_DIRECTORY *d =
			pe_dir(m->img, IMAGE_DIRECTORY_ENTRY_EXCEPTION);

		*image_base = (ULONGLONG)(uintptr_t)m->base;
		if (d) {
			fe = search((PRUNTIME_FUNCTION)(m->base + d->VirtualAddress),
				    d->Size / sizeof(RUNTIME_FUNCTION),
				    (DWORD)(pc - *image_base));
			if (fe)
				return fe;
		}
	}

	pthread_mutex_lock(&dyn_lock);
	for (struct dyn_table *t = dyn_tables; t && !fe; t = t->next) {
		if (pc < t->base)
			continue;
		fe = search(t->entries, t->count, (DWORD)(pc - t->base));
		if (fe)
			*image_base = t->base;
	}
	pthread_mutex_unlock(&dyn_lock);
	return fe;
}

BOOLEAN WINAPI seh_RtlAddFunctionTable(PRUNTIME_FUNCTION entries, DWORD count,
				       ULONGLONG base)
{
	struct dyn_table *t = malloc(sizeof(*t));

	if (!t)
		return 0;
	t->entries = entries;
	t->count = count;
	t->base = base;
	pthread_mutex_lock(&dyn_lock);
	t->next = dyn_tables;
	dyn_tables = t;
	pthread_mutex_unlock(&dyn_lock);
	return 1;
}

BOOLEAN WINAPI seh_RtlDeleteFunctionTable(PRUNTIME_FUNCTION entries)
{
	BOOLEAN found = 0;

	pthread_mutex_lock(&dyn_lock);
	for (struct dyn_table **p = &dyn_tables; *p; p = &(*p)->next)
		if ((*p)->entries == entries) {
			struct dyn_table *t = *p;

			*p = t->next;
			free(t);
			found = 1;
			break;
		}
	pthread_mutex_unlock(&dyn_lock);
	return found;
}

/*
 * Is pc at the start of a function epilogue: optionally an `add rsp, imm`
 * or `lea rsp, [fp+disp]`, any number of pops, then a ret or a jmp (a tail
 * call)? Returns the length of the part before the pops, with *pops set to
 * their number, or -1 if not.
 */
static int epilogue_at(const BYTE *p, int *pops, int *is_ret)
{
	int len = 0;

	*pops = 0;
	if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xc4)
		len = 4;		/* add rsp, imm8 */
	else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xc4)
		len = 7;		/* add rsp, imm32 */
	else if ((p[0] == 0x48 || p[0] == 0x49) && p[1] == 0x8d &&
		 (p[2] & 0x38) == 0x20 && (p[2] & 0xc0) == 0x40)
		len = 4;		/* lea rsp, [reg+disp8] */
	else if ((p[0] == 0x48 || p[0] == 0x49) && p[1] == 0x8d &&
		 (p[2] & 0x38) == 0x20 && (p[2] & 0xc0) == 0x80)
		len = 7;		/* lea rsp, [reg+disp32] */

	const BYTE *q = p + len;

	for (;;) {
		if ((q[0] & 0xf8) == 0x58)
			q += 1;
		else if (q[0] == 0x41 && (q[1] & 0xf8) == 0x58)
			q += 2;
		else
			break;
		(*pops)++;
	}
	if (q[0] == 0xc3 || (q[0] == 0xf3 && q[1] == 0xc3) ||
	    (q[0] == 0xf2 && q[1] == 0xc3)) {
		*is_ret = 1;
		return len;
	}
	/* A tail call: jmp rel32/rel8, jmp [mem] or jmp reg. */
	if (q[0] == 0xe9 || q[0] == 0xeb ||
	    (q[0] == 0xff && (q[1] & 0x38) == 0x20) ||
	    (q[0] == 0x48 && q[1] == 0xff && (q[2] & 0x38) == 0x20) ||
	    (q[0] == 0x41 && q[1] == 0xff && (q[2] & 0x38) == 0x20)) {
		*is_ret = 0;
		return len;
	}
	return -1;
}

/* Steps ctx out of an epilogue, as executing it would. */
static void unwind_epilogue(CONTEXT *ctx, const BYTE *p, int len, int pops,
			    KNONVOLATILE_CONTEXT_POINTERS *ptrs)
{
	ULONGLONG *regs = &ctx->Rax;

	if (len == 4 && p[1] == 0x83)
		ctx->Rsp += (int8_t)p[3];
	else if (len == 7 && p[1] == 0x81)
		ctx->Rsp += *(const int32_t *)(p + 3);
	else if (len == 4)
		ctx->Rsp = regs[((p[0] & 1) ? 8 : 0) + (p[2] & 7)] +
			   (int8_t)p[3];
	else if (len == 7)
		ctx->Rsp = regs[((p[0] & 1) ? 8 : 0) + (p[2] & 7)] +
			   *(const int32_t *)(p + 3);

	p += len;
	for (int i = 0; i < pops; i++) {
		int rex = 0;

		if (*p == 0x41) {
			rex = 8;
			p++;
		}
		int r = rex + (*p++ & 7);

		regs[r] = *(ULONGLONG *)(uintptr_t)ctx->Rsp;
		if (ptrs)
			ptrs->Gpr[r] = (ULONGLONG *)(uintptr_t)ctx->Rsp;
		ctx->Rsp += 8;
	}
	ctx->Rip = *(ULONGLONG *)(uintptr_t)ctx->Rsp;
	ctx->Rsp += 8;
}

PEXCEPTION_ROUTINE WINAPI
seh_RtlVirtualUnwind(DWORD type, ULONGLONG base, ULONGLONG pc,
		     PRUNTIME_FUNCTION fe, CONTEXT *ctx, void **handler_data,
		     ULONGLONG *frame_ret, KNONVOLATILE_CONTEXT_POINTERS *ptrs)
{
	ULONGLONG *regs = &ctx->Rax;
	M128A *xmm = &ctx->Xmm0;
	const UNWIND_INFO *info;
	DWORD offset;
	int first = 1;
	DWORD prolog;
	ULONGLONG frame = ctx->Rsp;
	int have_frame = 0;
	PEXCEPTION_ROUTINE handler = NULL;

	if (handler_data)
		*handler_data = NULL;
	if (!fe) {
		/* A leaf: nothing was pushed, the return address is on top. */
		ctx->Rip = *(ULONGLONG *)(uintptr_t)ctx->Rsp;
		ctx->Rsp += 8;
		*frame_ret = frame;
		return NULL;
	}

	if (fe->UnwindData & 1)
		fe = (PRUNTIME_FUNCTION)(uintptr_t)(base + (fe->UnwindData & ~3u));
	offset = (DWORD)(pc - base - fe->BeginAddress);

	/* In an epilogue the registers are being given back by the code
	 * itself; the table describes only the prolog. */
	{
		int pops, is_ret;
		const BYTE *code = (const BYTE *)(uintptr_t)pc;
		int len = epilogue_at(code, &pops, &is_ret);

		info = (const UNWIND_INFO *)(uintptr_t)(base + fe->UnwindData);
		prolog = info->SizeOfProlog;
		if (len >= 0 && offset >= prolog) {
			unwind_epilogue(ctx, code, len, pops, ptrs);
			*frame_ret = frame;
			return NULL;
		}
	}

	for (;;) {
		DWORD rel = first ? offset : 0xffffffffu;
		DWORD n, i;
		const UNWIND_CODE *c;

		info = (const UNWIND_INFO *)(uintptr_t)(base + fe->UnwindData);
		n = info->CountOfCodes;
		c = info->UnwindCode;
		if (info->FrameRegister && !have_frame && first &&
		    offset >= prolog) {
			frame = regs[info->FrameRegister] -
				(ULONGLONG)info->FrameOffset * 16;
			have_frame = 1;
		}

		for (i = 0; i < n;) {
			const UNWIND_CODE *u = &c[i];
			DWORD slots = 1;
			int reg = u->OpInfo;

			switch (u->UnwindOp) {
			case UWOP_ALLOC_LARGE:
				slots = u->OpInfo ? 3 : 2;
				break;
			case UWOP_SAVE_NONVOL:
			case UWOP_SAVE_XMM128:
			case UWOP_EPILOG:
				slots = 2;
				break;
			case UWOP_SAVE_NONVOL_FAR:
			case UWOP_SAVE_XMM128_FAR:
				slots = 3;
				break;
			}
			if (u->CodeOffset > rel) {	/* not yet run */
				i += slots;
				continue;
			}
			switch (u->UnwindOp) {
			case UWOP_PUSH_NONVOL:
				regs[reg] = *(ULONGLONG *)(uintptr_t)ctx->Rsp;
				if (ptrs)
					ptrs->Gpr[reg] = (ULONGLONG *)(uintptr_t)ctx->Rsp;
				ctx->Rsp += 8;
				break;
			case UWOP_ALLOC_LARGE:
				if (u->OpInfo)
					ctx->Rsp += c[i + 1].FrameOffset |
						    ((ULONGLONG)c[i + 2].FrameOffset << 16);
				else
					ctx->Rsp += (ULONGLONG)c[i + 1].FrameOffset * 8;
				break;
			case UWOP_ALLOC_SMALL:
				ctx->Rsp += (ULONGLONG)u->OpInfo * 8 + 8;
				break;
			case UWOP_SET_FPREG:
				ctx->Rsp = regs[info->FrameRegister] -
					   (ULONGLONG)info->FrameOffset * 16;
				break;
			case UWOP_SAVE_NONVOL:
			case UWOP_SAVE_NONVOL_FAR: {
				ULONGLONG off = u->UnwindOp == UWOP_SAVE_NONVOL ?
					(ULONGLONG)c[i + 1].FrameOffset * 8 :
					(c[i + 1].FrameOffset |
					 ((ULONGLONG)c[i + 2].FrameOffset << 16));
				ULONGLONG *at = (ULONGLONG *)(uintptr_t)(ctx->Rsp + off);

				regs[reg] = *at;
				if (ptrs)
					ptrs->Gpr[reg] = at;
				break;
			}
			case UWOP_SAVE_XMM128:
			case UWOP_SAVE_XMM128_FAR: {
				ULONGLONG off = u->UnwindOp == UWOP_SAVE_XMM128 ?
					(ULONGLONG)c[i + 1].FrameOffset * 16 :
					(c[i + 1].FrameOffset |
					 ((ULONGLONG)c[i + 2].FrameOffset << 16));
				M128A *at = (M128A *)(uintptr_t)(ctx->Rsp + off);

				xmm[reg] = *at;
				if (ptrs)
					ptrs->Xmm[reg] = at;
				break;
			}
			case UWOP_PUSH_MACHFRAME:
				if (u->OpInfo)
					ctx->Rsp += 8;	/* the error code */
				ctx->Rip = ((ULONGLONG *)(uintptr_t)ctx->Rsp)[0];
				ctx->Rsp = ((ULONGLONG *)(uintptr_t)ctx->Rsp)[3];
				*frame_ret = frame;
				return NULL;
			default:
				break;
			}
			i += slots;
		}

		if (info->Flags & UNW_FLAG_CHAININFO) {
			/* The parent entry follows the (even-padded) codes. */
			fe = (PRUNTIME_FUNCTION)&info->UnwindCode[(n + 1) & ~1u];
			if (fe->UnwindData & 1)
				fe = (PRUNTIME_FUNCTION)(uintptr_t)
					(base + (fe->UnwindData & ~3u));
			first = 0;
			continue;
		}
		break;
	}

	/* The handler lives after the codes of the last (primary) info. */
	if ((info->Flags & type) && offset >= prolog) {
		const DWORD *h = (const DWORD *)&info->UnwindCode[(info->CountOfCodes + 1) & ~1u];

		handler = (PEXCEPTION_ROUTINE)(uintptr_t)(base + h[0]);
		if (handler_data)
			*handler_data = (void *)(h + 1);
	}

	ctx->Rip = *(ULONGLONG *)(uintptr_t)ctx->Rsp;
	ctx->Rsp += 8;
	*frame_ret = frame;
	return handler;
}

#endif /* __x86_64__ */
