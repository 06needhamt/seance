/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32: calling through type information.
 *
 * - DispCallFunc calls a function, or an object's vtable slot, with
 *   arguments given as VARIANTARGs: stdcall or cdecl on i386, the one
 *   calling convention on x86-64 (four arguments in registers, each in
 *   both its integer and its XMM register, so a float finds its value
 *   where the callee looks for it). A VARIANT or DECIMAL is pushed whole
 *   on i386 and passed as a pointer to a copy on x86-64, and a function
 *   returning one gets a hidden pointer to the result, after the object.
 * - oa_typeinfo_invoke is ITypeInfo::Invoke over any type info's public
 *   methods: a coclass calls its default interface; a dual interface its
 *   vtable half; a dispatch-only interface the object's own
 *   IDispatch::Invoke. A vtable function's parameters are matched from
 *   DISPPARAMS (positional ones in reverse order, then named ones by
 *   index, a property put's value as DISPID_PROPERTYPUT), converted with
 *   VariantChangeType, and an [out, retval] parameter becomes the result.
 *   A failed HRESULT is DISP_E_EXCEPTION with the error information in
 *   EXCEPINFO.
 * - DispGetIDsOfNames, DispInvoke and CreateStdDispatch (an IDispatch,
 *   aggregatable, over an object and its type info). CreateDispTypeInfo
 *   is in oleaut_typelib.c, which builds its type info.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/oleaut.h"

#define WINFN(ret) static ret STUB_WINAPI

/* ----------------------------------------------------------------------
 * DispCallFunc.
 */

/*
 * void vfw_dispcall(const void *fn, size_t n, const uintptr_t *words,
 *                   void *out, int fp)
 * calls fn with the n words as its arguments, and stores its result at
 * out: 8 bytes of integer (%edx:%eax, or %rax), or with fp 4 or 8 the
 * float or double it returned (%st0, or %xmm0).
 */
#if !defined(__x86_64__)
__asm__(
	".text\n"
	".globl vfw_dispcall\n"
	".hidden vfw_dispcall\n"
	".type vfw_dispcall, @function\n"
	"vfw_dispcall:\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"pushl %ebx\n\t"
	"pushl %esi\n\t"
	"pushl %edi\n\t"
	"movl 12(%ebp), %ecx\n\t"
	"movl 16(%ebp), %esi\n\t"
	/* %esp 16-byte aligned once the words are pushed. */
	"leal 0(,%ecx,4), %eax\n\t"
	"subl %eax, %esp\n\t"
	"andl $-16, %esp\n\t"
	"addl %eax, %esp\n"
	"1:\n\t"
	"testl %ecx, %ecx\n\t"
	"jz 2f\n\t"
	"pushl -4(%esi,%ecx,4)\n\t"
	"decl %ecx\n\t"
	"jmp 1b\n"
	"2:\n\t"
	"call *8(%ebp)\n\t"
	"movl 20(%ebp), %ecx\n\t"
	"cmpl $0, 24(%ebp)\n\t"
	"je 3f\n\t"
	"cmpl $4, 24(%ebp)\n\t"
	"jne 4f\n\t"
	"fstps (%ecx)\n\t"
	"jmp 5f\n"
	"4:\n\t"
	"fstpl (%ecx)\n\t"
	"jmp 5f\n"
	"3:\n\t"
	"movl %eax, (%ecx)\n\t"
	"movl %edx, 4(%ecx)\n"
	"5:\n\t"
	"leal -12(%ebp), %esp\n\t"
	"popl %edi\n\t"
	"popl %esi\n\t"
	"popl %ebx\n\t"
	"popl %ebp\n\t"
	"ret\n\t"
	".size vfw_dispcall, .-vfw_dispcall\n"
);
#else
/*
 * SysV entry: rdi = fn, rsi = n, rdx = words, rcx = out, r8d = fp. At
 * least four slots (the callee's shadow space), 16-byte aligned. %rbx and
 * %r12, which this keeps out and fp in, are callee-saved under both
 * conventions; Win64 also keeps %rsi and %rdi, which SysV doesn't need.
 */
__asm__(
	".text\n"
	".globl vfw_dispcall\n"
	".hidden vfw_dispcall\n"
	".type vfw_dispcall, @function\n"
	"vfw_dispcall:\n\t"
	"pushq %rbp\n\t"
	"movq %rsp, %rbp\n\t"
	"pushq %rbx\n\t"
	"pushq %r12\n\t"
	"movq %rcx, %rbx\n\t"
	"movl %r8d, %r12d\n\t"
	"movq %rdi, %r11\n\t"
	"movq %rsi, %rax\n\t"
	"cmpq $4, %rax\n\t"
	"jae 1f\n\t"
	"movl $4, %eax\n"
	"1:\n\t"
	"leaq 15(,%rax,8), %rax\n\t"
	"andq $-16, %rax\n\t"
	"subq %rax, %rsp\n\t"
	"xorl %ecx, %ecx\n"
	"2:\n\t"
	"cmpq %rsi, %rcx\n\t"
	"jae 3f\n\t"
	"movq (%rdx,%rcx,8), %rax\n\t"
	"movq %rax, (%rsp,%rcx,8)\n\t"
	"incq %rcx\n\t"
	"jmp 2b\n"
	"3:\n\t"
	"movq 0(%rsp), %rcx\n\t"
	"movq 8(%rsp), %rdx\n\t"
	"movq 16(%rsp), %r8\n\t"
	"movq 24(%rsp), %r9\n\t"
	"movq %rcx, %xmm0\n\t"
	"movq %rdx, %xmm1\n\t"
	"movq %r8, %xmm2\n\t"
	"movq %r9, %xmm3\n\t"
	"callq *%r11\n\t"
	"movq %rax, (%rbx)\n\t"
	"testl %r12d, %r12d\n\t"
	"jz 4f\n\t"
	"movq %xmm0, (%rbx)\n"
	"4:\n\t"
	"leaq -16(%rbp), %rsp\n\t"
	"popq %r12\n\t"
	"popq %rbx\n\t"
	"popq %rbp\n\t"
	"ret\n\t"
	".size vfw_dispcall, .-vfw_dispcall\n"
);
#endif

void vfw_dispcall(const void *fn, size_t n, const uintptr_t *words,
		  void *out, int fp);

/* The bytes of a VARIANT's value a result of type vt fills. */
static size_t vt_result_size(VARTYPE vt)
{
	switch (vt) {
	case VT_EMPTY:
	case VT_VOID:
		return 0;
	case VT_I1:
	case VT_UI1:
		return 1;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		return 2;
	case VT_I4:
	case VT_UI4:
	case VT_INT:
	case VT_UINT:
	case VT_R4:
	case VT_ERROR:
	case VT_HRESULT:
		return 4;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_DATE:
	case VT_CY:
		return 8;
	default:
		return sizeof(void *);
	}
}

#define VARIANT_WORDS (sizeof(VARIANT) / sizeof(uintptr_t))

HRESULT STUB_WINAPI oa_DispCallFunc(void *instance, ULONG_PTR offset,
				    CALLCONV cc, VARTYPE ret, UINT count,
				    VARTYPE *types, VARIANTARG **args,
				    VARIANT *result)
{
	const void *fn;
	uintptr_t *words;
	VARIANT *copies = NULL;
	size_t n = 0;
	uint64_t out[2] = { 0, 0 };
	int fp = 0;
	HRESULT hr = S_OK;

	TRACE("DispCallFunc(%p, %#lx, %d, %#x, %u)", instance,
	      (unsigned long)offset, cc, ret, count);
	if (count && (!types || !args))
		return E_INVALIDARG;
#if defined(__x86_64__)
	if (cc >= CC_MAX)
		return E_INVALIDARG;
#else
	if (cc != CC_STDCALL && cc != CC_CDECL) {
		TRACE("DispCallFunc: calling convention %d isn't supported",
		      cc);
		return E_INVALIDARG;
	}
#endif
	if (instance) {
		if (offset % sizeof(void *))
			return E_INVALIDARG;
		fn = (*(void *const *const *)instance)[offset / sizeof(void *)];
	} else {
		fn = (const void *)offset;
	}
	if (!fn)
		return E_INVALIDARG;
	switch (ret) {
	case VT_VARIANT:
	case VT_DECIMAL:
		if (!result)
			return E_INVALIDARG;
		break;
	case VT_RECORD:
		return DISP_E_BADVARTYPE;
	case VT_R4:
		fp = 4;
		break;
	case VT_R8:
	case VT_DATE:
		fp = 8;
		break;
	}
	if (!(words = calloc(2 + (size_t)count * VARIANT_WORDS,
			     sizeof(*words))))
		return E_OUTOFMEMORY;
#if defined(__x86_64__)
	if (count && !(copies = calloc(count, sizeof(*copies)))) {
		free(words);
		return E_OUTOFMEMORY;
	}
#endif
	if (instance)
		words[n++] = (uintptr_t)instance;
	if (ret == VT_VARIANT || ret == VT_DECIMAL)
		words[n++] = (uintptr_t)result;
	for (UINT i = 0; i < count; i++) {
		VARIANTARG *a = args[i];
		VARTYPE vt = types[i];

		if (!a) {
			hr = E_INVALIDARG;
			goto done;
		}
		if (vt & (VT_BYREF | VT_ARRAY)) {
			words[n++] = (uintptr_t)a->byref;
			continue;
		}
		switch (vt) {
		case VT_I1:
			words[n++] = (uintptr_t)(intptr_t)(signed char)a->cVal;
			break;
		case VT_UI1:
			words[n++] = a->bVal;
			break;
		case VT_I2:
		case VT_BOOL:
			words[n++] = (uintptr_t)(intptr_t)a->iVal;
			break;
		case VT_UI2:
			words[n++] = a->uiVal;
			break;
		case VT_I4:
		case VT_INT:
		case VT_ERROR:
		case VT_HRESULT:
			words[n++] = (uintptr_t)(intptr_t)a->lVal;
			break;
		case VT_UI4:
		case VT_UINT:
			words[n++] = a->ulVal;
			break;
		case VT_R4: {
			uint32_t b;

			memcpy(&b, &a->fltVal, sizeof(b));
			words[n++] = b;
			break;
		}
		case VT_I8:
		case VT_UI8:
		case VT_R8:
		case VT_DATE:
		case VT_CY:
			memcpy(&words[n], &a->llVal, 8);
			n += 8 / sizeof(uintptr_t);
			break;
		case VT_VARIANT:
		case VT_DECIMAL:
#if defined(__x86_64__)
			copies[i] = *a;
			words[n++] = (uintptr_t)&copies[i];
#else
			memcpy(&words[n], a, sizeof(VARIANT));
			n += VARIANT_WORDS;
#endif
			break;
		case VT_RECORD:
			hr = DISP_E_BADVARTYPE;
			goto done;
		default:
			words[n++] = (uintptr_t)a->byref;
			break;
		}
	}
	vfw_dispcall(fn, n, words, out, fp);
	/* A DECIMAL's wReserved is the VARIANT's vt, which the callee left
	 * as 0. */
	if (ret == VT_DECIMAL)
		result->vt = VT_DECIMAL;
	if (result && ret != VT_VARIANT && ret != VT_DECIMAL) {
		memset(result, 0, sizeof(*result));
		result->vt = ret == VT_VOID ? VT_EMPTY : ret;
		memcpy(&result->llVal, out, vt_result_size(ret));
	}
done:
	free(copies);
	free(words);
	return hr;
}

/* ----------------------------------------------------------------------
 * ITypeInfo::Invoke.
 */

#define INVOKE_DEPTH 32

static HRESULT tdesc_vt(ITypeInfo *ti, const TYPEDESC *td, VARTYPE *out,
			int depth);

/* What a VARIANT holding a value of a user-defined type carries. */
static HRESULT userdefined_vt(ITypeInfo *ti, HREFTYPE href, VARTYPE *out,
			      int depth)
{
	ITypeInfo *r;
	TYPEATTR *a;
	HRESULT hr;

	if (depth > INVOKE_DEPTH)
		return DISP_E_BADVARTYPE;
	if (FAILED(hr = ti->lpVtbl->GetRefTypeInfo(ti, href, &r)))
		return hr;
	if (SUCCEEDED(hr = r->lpVtbl->GetTypeAttr(r, &a))) {
		switch (a->typekind) {
		case TKIND_ENUM:
			*out = VT_I4;
			break;
		case TKIND_ALIAS:
			hr = tdesc_vt(r, &a->tdescAlias, out, depth + 1);
			break;
		case TKIND_INTERFACE:
		case TKIND_COCLASS:
			*out = VT_UNKNOWN;
			break;
		case TKIND_DISPATCH:
			*out = VT_DISPATCH;
			break;
		case TKIND_RECORD:
			*out = VT_RECORD;
			break;
		default:
			hr = DISP_E_BADVARTYPE;
			break;
		}
		r->lpVtbl->ReleaseTypeAttr(r, a);
	}
	r->lpVtbl->Release(r);
	return hr;
}

/*
 * The VARTYPE a parameter of type td is passed as: VT_BYREF for a pointer,
 * except that a pointer to an interface is the interface's VT_UNKNOWN or
 * VT_DISPATCH.
 */
static HRESULT tdesc_vt(ITypeInfo *ti, const TYPEDESC *td, VARTYPE *out,
			int depth)
{
	VARTYPE vt;
	HRESULT hr;

	if (depth > INVOKE_DEPTH)
		return DISP_E_BADVARTYPE;
	switch (td->vt) {
	case VT_PTR:
		if (!td->lptdesc)
			return DISP_E_BADVARTYPE;
		if (FAILED(hr = tdesc_vt(ti, td->lptdesc, &vt, depth + 1)))
			return hr;
		if (td->lptdesc->vt == VT_USERDEFINED &&
		    (vt == VT_UNKNOWN || vt == VT_DISPATCH)) {
			*out = vt;
			return S_OK;
		}
		if (vt & VT_BYREF)
			return DISP_E_BADVARTYPE;
		*out = vt | VT_BYREF;
		return S_OK;
	case VT_SAFEARRAY:
		if (!td->lptdesc)
			return DISP_E_BADVARTYPE;
		if (FAILED(hr = tdesc_vt(ti, td->lptdesc, &vt, depth + 1)))
			return hr;
		if (vt & (VT_BYREF | VT_ARRAY))
			return DISP_E_BADVARTYPE;
		*out = VT_ARRAY | vt;
		return S_OK;
	case VT_USERDEFINED:
		return userdefined_vt(ti, td->hreftype, out, depth + 1);
	case VT_HRESULT:
		*out = VT_ERROR;
		return S_OK;
	case VT_INT_PTR:
		*out = sizeof(void *) == 8 ? VT_I8 : VT_I4;
		return S_OK;
	case VT_UINT_PTR:
		*out = sizeof(void *) == 8 ? VT_UI8 : VT_UI4;
		return S_OK;
	case VT_I2: case VT_I4: case VT_R4: case VT_R8: case VT_CY:
	case VT_DATE: case VT_BSTR: case VT_DISPATCH: case VT_ERROR:
	case VT_BOOL: case VT_VARIANT: case VT_UNKNOWN: case VT_DECIMAL:
	case VT_I1: case VT_UI1: case VT_UI2: case VT_UI4: case VT_I8:
	case VT_UI8: case VT_INT: case VT_UINT:
		*out = td->vt;
		return S_OK;
	default:
		TRACE("ITypeInfo::Invoke: a parameter of type %#x", td->vt);
		return DISP_E_BADVARTYPE;
	}
}

/*
 * The type info to call through: a coclass's default interface, a dual
 * interface's vtable half, or ti itself. *out holds a reference.
 */
static HRESULT invoke_target(ITypeInfo *ti, ITypeInfo **out, int depth)
{
	TYPEATTR *a;
	TYPEKIND kind;
	WORD tflags, nimpl;
	HREFTYPE h;
	ITypeInfo *r;
	HRESULT hr;

	if (depth > INVOKE_DEPTH)
		return DISP_E_MEMBERNOTFOUND;
	if (FAILED(hr = ti->lpVtbl->GetTypeAttr(ti, &a)))
		return hr;
	kind = a->typekind;
	tflags = a->wTypeFlags;
	nimpl = a->cImplTypes;
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	if (kind == TKIND_COCLASS) {
		UINT pick = 0;

		if (!nimpl)
			return DISP_E_MEMBERNOTFOUND;
		for (UINT i = 0; i < nimpl; i++) {
			INT f;

			if (SUCCEEDED(ti->lpVtbl->GetImplTypeFlags(ti, i, &f)) &&
			    (f & IMPLTYPEFLAG_FDEFAULT) &&
			    !(f & IMPLTYPEFLAG_FSOURCE)) {
				pick = i;
				break;
			}
		}
		if (FAILED(hr = ti->lpVtbl->GetRefTypeOfImplType(ti, pick, &h)) ||
		    FAILED(hr = ti->lpVtbl->GetRefTypeInfo(ti, h, &r)))
			return hr;
		hr = invoke_target(r, out, depth + 1);
		r->lpVtbl->Release(r);
		return hr;
	}
	if (kind == TKIND_DISPATCH && (tflags & TYPEFLAG_FDUAL) &&
	    SUCCEEDED(ti->lpVtbl->GetRefTypeOfImplType(ti, (UINT)-1, &h)) &&
	    SUCCEEDED(ti->lpVtbl->GetRefTypeInfo(ti, h, out)))
		return S_OK;
	ti->lpVtbl->AddRef(ti);
	*out = ti;
	return S_OK;
}

/*
 * The function with memid whose invoke kind is among flags, in ti or (an
 * interface's) its bases. *owner is the type info holding it, with a
 * reference; *out is its FUNCDESC, to release with owner.
 */
static HRESULT find_func(ITypeInfo *ti, MEMBERID memid, WORD flags,
			 ITypeInfo **owner, FUNCDESC **out, int depth)
{
	TYPEATTR *a;
	HRESULT hr;

	if (depth > INVOKE_DEPTH)
		return DISP_E_MEMBERNOTFOUND;
	if (FAILED(hr = ti->lpVtbl->GetTypeAttr(ti, &a)))
		return hr;
	for (UINT i = 0; i < a->cFuncs; i++) {
		FUNCDESC *d;

		if (FAILED(ti->lpVtbl->GetFuncDesc(ti, i, &d)))
			continue;
		if (d->memid == memid && (d->invkind & flags)) {
			ti->lpVtbl->ReleaseTypeAttr(ti, a);
			ti->lpVtbl->AddRef(ti);
			*owner = ti;
			*out = d;
			return S_OK;
		}
		ti->lpVtbl->ReleaseFuncDesc(ti, d);
	}
	hr = DISP_E_MEMBERNOTFOUND;
	if (a->typekind == TKIND_INTERFACE && a->cImplTypes) {
		HREFTYPE h;
		ITypeInfo *b, *v;

		if (SUCCEEDED(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &h)) &&
		    SUCCEEDED(ti->lpVtbl->GetRefTypeInfo(ti, h, &b))) {
			/* A dual base: its vtable half. */
			if (SUCCEEDED(invoke_target(b, &v, depth + 1))) {
				hr = find_func(v, memid, flags, owner, out,
					       depth + 1);
				v->lpVtbl->Release(v);
			}
			b->lpVtbl->Release(b);
		}
	}
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	return hr;
}

/* Whether a dispatch-only interface has memid as a function or variable. */
static int dispatch_has(ITypeInfo *ti, MEMBERID memid, WORD flags)
{
	ITypeInfo *owner;
	FUNCDESC *fd;
	TYPEATTR *a;
	int found = 0;

	if (SUCCEEDED(find_func(ti, memid, flags, &owner, &fd, 0))) {
		owner->lpVtbl->ReleaseFuncDesc(owner, fd);
		owner->lpVtbl->Release(owner);
		return 1;
	}
	if (FAILED(ti->lpVtbl->GetTypeAttr(ti, &a)))
		return 0;
	for (UINT i = 0; i < a->cVars && !found; i++) {
		VARDESC *v;

		if (SUCCEEDED(ti->lpVtbl->GetVarDesc(ti, i, &v))) {
			found = v->memid == memid;
			ti->lpVtbl->ReleaseVarDesc(ti, v);
		}
	}
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	return found;
}

/* One parameter's argument, as DispCallFunc is given it. */
struct slot {
	VARIANT holder;		/* a VT_BYREF argument's pointer */
	VARIANT temp;		/* a converted value, if used */
	VARIANT missing;	/* an optional VARIANT not given */
	VARIANT *writeback;	/* the caller's VARIANT temp goes back to */
	VARIANT *in_place;	/* the caller's VARIANT, passed by reference */
	int temp_used;
	VARIANTARG *arg;
	VARTYPE vt;
};

static void set_ref(struct slot *s, VARTYPE vt, void *p)
{
	s->holder.vt = vt;
	s->holder.byref = p;
	s->arg = &s->holder;
	s->vt = vt;
}

/* Binds src to a parameter passed as vt. */
static HRESULT bind_arg(struct slot *s, VARTYPE vt, VARIANTARG *src)
{
	VARTYPE base = vt & ~VT_BYREF;
	VARIANTARG *in = src;
	HRESULT hr;

	if (src->vt == (VT_BYREF | VT_VARIANT) && src->pvarVal)
		in = src->pvarVal;
	if (vt == VT_VARIANT) {
		s->arg = in;
		s->vt = vt;
		return S_OK;
	}
	if (vt == (VT_BYREF | VT_VARIANT)) {
		set_ref(s, vt, in);
		return S_OK;
	}
	if (vt & VT_BYREF) {
		if (src->vt == vt) {
			set_ref(s, vt, src->byref);
			return S_OK;
		}
		if (in->vt == vt) {
			set_ref(s, vt, in->byref);
			return S_OK;
		}
		if (in != src && in->vt == base) {
			/* A value in the caller's VARIANT: written in place. */
			set_ref(s, vt, base == VT_DECIMAL ? (void *)&in->decVal
							  : (void *)&in->llVal);
			s->in_place = in;
			return S_OK;
		}
		if (src->vt & VT_BYREF && in == src)
			return DISP_E_TYPEMISMATCH;
		/*
		 * Another type, by value or in the caller's VARIANT: converted
		 * (an empty one is a zero), and in the second case put back
		 * as the parameter's type after the call.
		 */
		oa_VariantInit(&s->temp);
		if (in->vt == VT_EMPTY) {
			s->temp.vt = base;
		} else if (FAILED(hr = oa_VariantChangeTypeEx(&s->temp, in,
							      LOCALE_USER_DEFAULT,
							      0, base))) {
			return hr == DISP_E_OVERFLOW ? hr : DISP_E_TYPEMISMATCH;
		}
		s->temp_used = 1;
		if (in != src)
			s->writeback = in;
		set_ref(s, vt, base == VT_DECIMAL ? (void *)&s->temp.decVal
						  : (void *)&s->temp.llVal);
		return S_OK;
	}
	if (in->vt == vt) {
		s->arg = in;
		s->vt = vt;
		return S_OK;
	}
	oa_VariantInit(&s->temp);
	if (FAILED(hr = oa_VariantChangeTypeEx(&s->temp, in,
					       LOCALE_USER_DEFAULT, 0, vt)))
		return hr == DISP_E_OVERFLOW ? hr : DISP_E_TYPEMISMATCH;
	s->temp_used = 1;
	s->arg = &s->temp;
	s->vt = vt;
	return S_OK;
}

/* A failed HRESULT as EXCEPINFO, with the thread's error information. */
static void fill_excep(EXCEPINFO *e, HRESULT code)
{
	IErrorInfo *ei = NULL;

	memset(e, 0, sizeof(*e));
	e->scode = code;
	if (oa_GetErrorInfo(0, &ei) == S_OK && ei) {
		ei->lpVtbl->GetSource(ei, &e->bstrSource);
		ei->lpVtbl->GetDescription(ei, &e->bstrDescription);
		ei->lpVtbl->GetHelpFile(ei, &e->bstrHelpFile);
		ei->lpVtbl->GetHelpContext(ei, &e->dwHelpContext);
		ei->lpVtbl->Release(ei);
	}
}

/* Collects positional arguments k.. into a SAFEARRAY of VARIANT. */
static HRESULT bind_vararg(struct slot *s, VARTYPE vt, DISPPARAMS *dp,
			   UINT k, UINT npos, UINT *arg_err)
{
	UINT cnt = npos > k ? npos - k : 0;
	SAFEARRAY *sa;
	HRESULT hr;

	if ((vt & ~VT_BYREF) != (VT_ARRAY | VT_VARIANT))
		return DISP_E_BADVARTYPE;
	if (!(sa = oa_SafeArrayCreateVector(VT_VARIANT, 0, cnt)))
		return E_OUTOFMEMORY;
	for (UINT i = 0; i < cnt; i++) {
		UINT pos = dp->cArgs - 1 - (k + i);

		hr = oa_VariantCopyInd(&((VARIANT *)sa->pvData)[i],
				       &dp->rgvarg[pos]);
		if (FAILED(hr)) {
			*arg_err = pos;
			oa_SafeArrayDestroy(sa);
			return hr;
		}
	}
	oa_VariantInit(&s->temp);
	s->temp.vt = VT_ARRAY | VT_VARIANT;
	s->temp.parray = sa;
	s->temp_used = 1;
	if (vt & VT_BYREF) {
		set_ref(s, vt, &s->temp.parray);
	} else {
		s->arg = &s->temp;
		s->vt = vt;
	}
	return S_OK;
}

/* Calls fd, a vtable function of owner, on instance. */
static HRESULT call_func(ITypeInfo *owner, FUNCDESC *fd, void *instance,
			 DISPPARAMS *dp, VARIANT *result, EXCEPINFO *excep,
			 UINT *arg_err)
{
	UINT cargs = dp->cArgs, cnamed = dp->cNamedArgs;
	UINT cpos = cargs - cnamed, npos = cpos, k = 0;
	int n = fd->cParams;
	int is_put = !!(fd->invkind & (INVOKE_PROPERTYPUT |
				       INVOKE_PROPERTYPUTREF));
	int retval = -1, putval = -1, vararg = -1, last;
	UINT put_src = 0;
	int named_put = -1;
	VARIANT ret, rv;
	struct slot *s;
	VARTYPE *types, rvt;
	VARIANTARG **args;
	HRESULT hr = S_OK;

	if (n < 0)
		return E_INVALIDARG;
	s = calloc(n ? n : 1, sizeof(*s));
	types = calloc(n ? n : 1, sizeof(*types));
	args = calloc(n ? n : 1, sizeof(*args));
	oa_VariantInit(&ret);
	oa_VariantInit(&rv);
	if (!s || !types || !args) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	if (n && (fd->lprgelemdescParam[n - 1].paramdesc.wParamFlags &
		  PARAMFLAG_FRETVAL))
		retval = n - 1;
	last = retval >= 0 ? n - 2 : n - 1;
	if (is_put)
		putval = last;
	else if (fd->cParamsOpt == -1 && last >= 0)
		vararg = last;

	for (UINT j = 0; j < cnamed; j++) {
		DISPID id = dp->rgdispidNamedArgs[j];

		if (id == DISPID_PROPERTYPUT && putval >= 0) {
			named_put = j;
			continue;
		}
		if (id < 0 || id >= n || id == retval || id == putval ||
		    id == vararg ||
		    (fd->lprgelemdescParam[id].paramdesc.wParamFlags &
		     PARAMFLAG_FLCID)) {
			*arg_err = j;
			hr = DISP_E_PARAMNOTFOUND;
			goto out;
		}
	}
	if (is_put) {
		if (putval < 0) {
			hr = DISP_E_BADPARAMCOUNT;
			goto out;
		}
		if (named_put >= 0) {
			put_src = named_put;
		} else if (npos) {
			put_src = cnamed;	/* the last positional one */
			npos--;
		} else {
			hr = DISP_E_PARAMNOTOPTIONAL;
			goto out;
		}
	}

	if (vararg < 0) {
		int ordinary = 0;

		for (int p = 0; p < n; p++)
			if (p != retval && p != putval &&
			    !(fd->lprgelemdescParam[p].paramdesc.wParamFlags &
			      PARAMFLAG_FLCID))
				ordinary++;
		if (npos > (UINT)ordinary) {
			hr = DISP_E_BADPARAMCOUNT;
			goto out;
		}
	}

	for (int p = 0; p < n; p++) {
		const ELEMDESC *e = &fd->lprgelemdescParam[p];
		USHORT pf = e->paramdesc.wParamFlags;
		struct slot *sl = &s[p];
		VARIANTARG *src = NULL;
		UINT src_pos = (UINT)-1;
		VARTYPE vt;

		if (FAILED(hr = tdesc_vt(owner, &e->tdesc, &vt, 0)))
			goto out;
		if (p == retval) {
			if (!(vt & VT_BYREF)) {
				hr = DISP_E_BADVARTYPE;
				goto out;
			}
			/* A DECIMAL is the whole VARIANT; the others are
			 * its value. */
			set_ref(sl, vt, vt == (VT_BYREF | VT_VARIANT)
					? (void *)&ret
					: vt == (VT_BYREF | VT_DECIMAL)
					? (void *)&ret.decVal
					: (void *)&ret.llVal);
			goto bound;
		}
		if (pf & PARAMFLAG_FLCID) {
			sl->temp.vt = VT_I4;
			sl->temp.lVal = LOCALE_USER_DEFAULT;
			if (vt == VT_I4 || vt == VT_UI4) {
				sl->arg = &sl->temp;
				sl->vt = vt;
			} else {
				hr = DISP_E_BADVARTYPE;
				goto out;
			}
			goto bound;
		}
		if (p == vararg) {
			if (FAILED(hr = bind_vararg(sl, vt, dp, k, npos,
						    arg_err)))
				goto out;
			k = npos;
			goto bound;
		}
		if (p == putval) {
			src_pos = put_src;
		} else if (k < npos) {
			src_pos = cargs - 1 - k++;
		} else {
			for (UINT j = 0; j < cnamed; j++)
				if (dp->rgdispidNamedArgs[j] == p)
					src_pos = j;
		}
		if (src_pos != (UINT)-1) {
			src = &dp->rgvarg[src_pos];
		} else if ((pf & PARAMFLAG_FHASDEFAULT) &&
			   e->paramdesc.pparamdescex) {
			src = &e->paramdesc.pparamdescex->varDefaultValue;
		} else if ((pf & PARAMFLAG_FOPT) &&
			   (vt & ~VT_BYREF) == VT_VARIANT) {
			sl->missing.vt = VT_ERROR;
			sl->missing.scode = DISP_E_PARAMNOTFOUND;
			src = &sl->missing;
		} else {
			hr = DISP_E_PARAMNOTOPTIONAL;
			goto out;
		}
		if (FAILED(hr = bind_arg(sl, vt, src))) {
			if (src_pos != (UINT)-1)
				*arg_err = src_pos;
			goto out;
		}
bound:
		types[p] = sl->vt;
		args[p] = sl->arg;
	}
	if (k < npos) {
		hr = DISP_E_BADPARAMCOUNT;
		goto out;
	}

	switch (fd->elemdescFunc.tdesc.vt) {
	case VT_VOID:
	case VT_EMPTY:
		rvt = VT_EMPTY;
		break;
	case VT_HRESULT:
		rvt = VT_HRESULT;
		break;
	default:
		if (FAILED(hr = tdesc_vt(owner, &fd->elemdescFunc.tdesc, &rvt,
					 0)))
			goto out;
		break;
	}
	hr = oa_DispCallFunc(instance, fd->oVft, fd->callconv, rvt, n,
			     types, args, &rv);
	if (FAILED(hr))
		goto out;
	/* A DECIMAL written by reference over a VARIANT also wrote its vt. */
	for (int p = 0; p < n; p++)
		if (s[p].vt == (VT_BYREF | VT_DECIMAL)) {
			if (s[p].in_place)
				s[p].in_place->vt = VT_DECIMAL;
			if (s[p].temp_used)
				s[p].temp.vt = VT_DECIMAL;
		}
	if (rvt == VT_HRESULT && FAILED(rv.scode)) {
		if (excep)
			fill_excep(excep, rv.scode);
		oa_VariantClear(&ret);
		hr = DISP_E_EXCEPTION;
		goto out;
	}
	for (int p = 0; p < n; p++)
		if (s[p].writeback) {
			oa_VariantClear(s[p].writeback);
			*s[p].writeback = s[p].temp;
			s[p].temp_used = 0;
		}
	if (retval >= 0 &&
	    fd->lprgelemdescParam[retval].tdesc.vt == VT_PTR &&
	    types[retval] != (VT_BYREF | VT_VARIANT))
		ret.vt = types[retval] & ~VT_BYREF;
	if (result) {
		oa_VariantInit(result);
		if (retval >= 0)
			*result = ret;
		else if (rvt != VT_HRESULT)
			*result = rv;
	} else {
		oa_VariantClear(&ret);
		if (rvt != VT_HRESULT)
			oa_VariantClear(&rv);
	}
	hr = S_OK;
out:
	if (s)
		for (int p = 0; p < n; p++)
			if (s[p].temp_used)
				oa_VariantClear(&s[p].temp);
	free(s);
	free(types);
	free(args);
	return hr;
}

HRESULT oa_typeinfo_invoke(ITypeInfo *ti, PVOID instance, MEMBERID memid,
			   WORD flags, DISPPARAMS *params, VARIANT *result,
			   EXCEPINFO *excep, UINT *arg_err)
{
	ITypeInfo *t, *owner;
	FUNCDESC *fd;
	TYPEATTR *a;
	TYPEKIND kind;
	UINT dummy;
	HRESULT hr;

	TRACE("ITypeInfo::Invoke(%#x, %#x)", (unsigned)memid, flags);
	if (!arg_err)
		arg_err = &dummy;
	if (!ti || !instance || !params ||
	    params->cNamedArgs > params->cArgs ||
	    (params->cArgs && !params->rgvarg) ||
	    (params->cNamedArgs && !params->rgdispidNamedArgs))
		return E_INVALIDARG;
	if (!(flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET |
		       DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)))
		return E_INVALIDARG;
	if (FAILED(hr = invoke_target(ti, &t, 0)))
		return hr;
	if (FAILED(hr = t->lpVtbl->GetTypeAttr(t, &a)))
		goto done;
	kind = a->typekind;
	t->lpVtbl->ReleaseTypeAttr(t, a);
	if (kind == TKIND_DISPATCH) {
		IUnknown *u = instance;
		IDispatch *d;

		if (!dispatch_has(t, memid, flags)) {
			hr = DISP_E_MEMBERNOTFOUND;
			goto done;
		}
		if (FAILED(hr = u->lpVtbl->QueryInterface(u, &oa_IID_IDispatch,
							  (void **)&d)))
			goto done;
		hr = d->lpVtbl->Invoke(d, memid, &oa_IID_NULL,
				       LOCALE_USER_DEFAULT, flags, params,
				       result, excep, arg_err);
		d->lpVtbl->Release(d);
		goto done;
	}
	if (kind != TKIND_INTERFACE) {
		hr = DISP_E_MEMBERNOTFOUND;
		goto done;
	}
	if (FAILED(hr = find_func(t, memid, flags, &owner, &fd, 0))) {
		hr = DISP_E_MEMBERNOTFOUND;
		goto done;
	}
	if (fd->funckind == FUNC_VIRTUAL || fd->funckind == FUNC_PUREVIRTUAL) {
		hr = call_func(owner, fd, instance, params, result, excep,
			       arg_err);
	} else {
		TRACE("ITypeInfo::Invoke: function kind %d isn't supported",
		      fd->funckind);
		hr = DISP_E_MEMBERNOTFOUND;
	}
	owner->lpVtbl->ReleaseFuncDesc(owner, fd);
	owner->lpVtbl->Release(owner);
done:
	t->lpVtbl->Release(t);
	return hr;
}

/* ----------------------------------------------------------------------
 * DispGetIDsOfNames, DispInvoke, CreateStdDispatch.
 */

WINFN(HRESULT) oa_DispGetIDsOfNames(ITypeInfo *ti, OLECHAR **names,
				    UINT count, DISPID *ids)
{
	if (!ti)
		return E_INVALIDARG;
	return ti->lpVtbl->GetIDsOfNames(ti, names, count, ids);
}

WINFN(HRESULT) oa_DispInvoke(void *obj, ITypeInfo *ti, DISPID id,
			     WORD flags, DISPPARAMS *params, VARIANT *result,
			     EXCEPINFO *excep, UINT *arg_err)
{
	if (!ti)
		return E_INVALIDARG;
	return ti->lpVtbl->Invoke(ti, obj, id, flags, params, result, excep,
				  arg_err);
}

struct std_dispatch {
	IDispatch disp;
	IUnknown inner;
	LONG refs;
	IUnknown *outer;	/* the controlling IUnknown */
	void *obj;
	ITypeInfo *ti;
};

static struct std_dispatch *sd_of_disp(IDispatch *d)
{
	return (struct std_dispatch *)((BYTE *)d -
				       offsetof(struct std_dispatch, disp));
}

static struct std_dispatch *sd_of_inner(IUnknown *u)
{
	return (struct std_dispatch *)((BYTE *)u -
				       offsetof(struct std_dispatch, inner));
}

static int iid_eq(REFIID a, const IID *b)
{
	return !memcmp(a, b, sizeof(IID));
}

WINFN(HRESULT) sdu_QueryInterface(IUnknown *self, REFIID riid, void **ppv)
{
	struct std_dispatch *sd = sd_of_inner(self);

	if (!ppv)
		return E_POINTER;
	if (iid_eq(riid, &oa_IID_IUnknown)) {
		*ppv = &sd->inner;
	} else if (iid_eq(riid, &oa_IID_IDispatch)) {
		*ppv = &sd->disp;
	} else {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	((IUnknown *)*ppv)->lpVtbl->AddRef(*ppv);
	return S_OK;
}

WINFN(ULONG) sdu_AddRef(IUnknown *self)
{
	return __atomic_add_fetch(&sd_of_inner(self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) sdu_Release(IUnknown *self)
{
	struct std_dispatch *sd = sd_of_inner(self);
	ULONG r = __atomic_sub_fetch(&sd->refs, 1, __ATOMIC_SEQ_CST);

	if (!r) {
		sd->ti->lpVtbl->Release(sd->ti);
		free(sd);
	}
	return r;
}

static const IUnknownVtbl std_inner_vtbl = {
	sdu_QueryInterface, sdu_AddRef, sdu_Release,
};

WINFN(HRESULT) sd_QueryInterface(IDispatch *self, REFIID riid, void **ppv)
{
	IUnknown *o = sd_of_disp(self)->outer;

	return o->lpVtbl->QueryInterface(o, riid, ppv);
}

WINFN(ULONG) sd_AddRef(IDispatch *self)
{
	IUnknown *o = sd_of_disp(self)->outer;

	return o->lpVtbl->AddRef(o);
}

WINFN(ULONG) sd_Release(IDispatch *self)
{
	IUnknown *o = sd_of_disp(self)->outer;

	return o->lpVtbl->Release(o);
}

WINFN(HRESULT) sd_GetTypeInfoCount(IDispatch *self, UINT *count)
{
	if (!count)
		return E_INVALIDARG;
	*count = 1;
	return S_OK;
}

WINFN(HRESULT) sd_GetTypeInfo(IDispatch *self, UINT index, LCID lcid,
			      void **info)
{
	struct std_dispatch *sd = sd_of_disp(self);

	if (!info)
		return E_INVALIDARG;
	*info = NULL;
	if (index)
		return DISP_E_BADINDEX;
	sd->ti->lpVtbl->AddRef(sd->ti);
	*info = sd->ti;
	return S_OK;
}

WINFN(HRESULT) sd_GetIDsOfNames(IDispatch *self, REFIID riid,
				LPOLESTR *names, UINT count, LCID lcid,
				DISPID *ids)
{
	struct std_dispatch *sd = sd_of_disp(self);

	if (!iid_eq(riid, &oa_IID_NULL))
		return DISP_E_UNKNOWNINTERFACE;
	return sd->ti->lpVtbl->GetIDsOfNames(sd->ti, names, count, ids);
}

WINFN(HRESULT) sd_Invoke(IDispatch *self, DISPID id, REFIID riid, LCID lcid,
			 WORD flags, DISPPARAMS *params, VARIANT *result,
			 EXCEPINFO *excep, UINT *arg_err)
{
	struct std_dispatch *sd = sd_of_disp(self);

	if (!iid_eq(riid, &oa_IID_NULL))
		return DISP_E_UNKNOWNINTERFACE;
	return sd->ti->lpVtbl->Invoke(sd->ti, sd->obj, id, flags, params,
				      result, excep, arg_err);
}

static const IDispatchVtbl std_dispatch_vtbl = {
	sd_QueryInterface, sd_AddRef, sd_Release, sd_GetTypeInfoCount,
	sd_GetTypeInfo, sd_GetIDsOfNames, sd_Invoke,
};

WINFN(HRESULT) oa_CreateStdDispatch(IUnknown *outer, void *obj,
				    ITypeInfo *ti, IUnknown **out)
{
	struct std_dispatch *sd;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!obj || !ti)
		return E_INVALIDARG;
	if (!(sd = calloc(1, sizeof(*sd))))
		return E_OUTOFMEMORY;
	sd->disp.lpVtbl = &std_dispatch_vtbl;
	sd->inner.lpVtbl = &std_inner_vtbl;
	sd->refs = 1;
	sd->outer = outer ? outer : &sd->inner;
	sd->obj = obj;
	sd->ti = ti;
	ti->lpVtbl->AddRef(ti);
	*out = &sd->inner;
	return S_OK;
}

#define E(name, ord) { #name, ord, (void *)oa_##name }

const struct builtin_export oa_dispatch_exports[] = {
	E(DispGetIDsOfNames, 29),
	E(DispInvoke, 30),
	E(CreateStdDispatch, 32),
	E(DispCallFunc, 146),
	BUILTIN_END
};
