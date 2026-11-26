/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's user marshal routines: what MIDL-generated proxies and stubs
 * call to put a BSTR, a VARIANT or a SAFEARRAY into an NDR buffer and
 * take it out again (BSTR_UserSize/Marshal/Unmarshal/Free and the same
 * for VARIANT and LPSAFEARRAY).
 *
 * The wire forms are the ones the automation IDL declares (MS-OAUT):
 *
 *   BSTR       a FLAGGED_WORD_BLOB, aligned to 4: the count of WCHARs
 *              (conformance), the byte length (0xffffffff for NULL), the
 *              count again, then the characters (an odd byte length is
 *              padded to a whole WCHAR).
 *   VARIANT    aligned to 8: its size in 8-byte units, a reserved DWORD,
 *              vt and the three reserved WORDs (written as zero, not
 *              read), the union's DWORD discriminant (vt, or for an
 *              array VT_ARRAY with any VT_BYREF), then the arm: a value,
 *              or for a pointer a 4-byte referent ID (0 for NULL) with
 *              what it points to after it. Interfaces go as an
 *              MInterfacePointer (a DWORD count twice, then what
 *              CoMarshalInterface wrote).
 *   SAFEARRAY  a referent ID, then the descriptor: cDims (conformance),
 *              cDims, fFeatures, cbElements as NDR20 has it (4 for
 *              BSTRs and interfaces, 16 for VARIANTs, whatever this
 *              build's sizes), cLocks with the element's VARTYPE in its
 *              high WORD if FADF_HAVEVARTYPE is set, the SF_ type (the
 *              union's discriminant), the element count, a referent ID
 *              for the data, the IID (SF_HAVEIID), the bounds as they
 *              are in memory; then the count again and the elements
 *              (BSTRs, interfaces and VARIANTs as pointers first, then
 *              what each points to).
 *
 * Referent IDs are any nonzero value. Data that can't be marshalled (a
 * VT_RECORD, an element type with no SF_ form) or a buffer that doesn't
 * read back raises RPC_X_BAD_STUB_DATA, as the NDR engine does. Reads
 * stop at the end of the RPC buffer, which the USER_MARSHAL_CB passed as
 * the flags gives; what was read before a failure is let go of, and the
 * destination is left as it was. The marshal context in the flags is
 * not looked at: interfaces always go through CoMarshalInterface, which
 * in seance is always in process.
 *
 * Windows' x64 oleaut32 also has the NDR64 forms (BSTR_UserSize64 and
 * so on); seance's x64 build has these NDR20 ones only.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "runtime/seh.h"
#include "win32/internal.h"
#include "win32/ole.h"
#include "win32/oleaut.h"

#define RPC_X_BAD_STUB_DATA 1783

/* The SAFEARRAY wire's element kinds. */
#define SF_I1      VT_I1
#define SF_I2      VT_I2
#define SF_I4      VT_I4
#define SF_I8      VT_I8
#define SF_BSTR    VT_BSTR
#define SF_UNKNOWN VT_UNKNOWN
#define SF_DISPATCH VT_DISPATCH
#define SF_VARIANT VT_VARIANT
#define SF_RECORD  VT_RECORD
#define SF_HAVEIID (VT_UNKNOWN | 0x8000)

#define REFERENT 0x00020000u

/* Too many dimensions, elements or nested VARIANTs to be real. */
#define DEPTH_MAX 64

static void bad_data(void)
{
	TRACE("oleaut32: can't marshal or unmarshal this data");
	seh_RaiseException(RPC_X_BAD_STUB_DATA, EXCEPTION_NONCONTINUABLE, 0,
			   NULL);
	abort();	/* (not reached) */
}

/* ---- writing (or, with p NULL, sizing) ---- */

struct out {
	BYTE *p;	/* NULL: only count */
	size_t at;	/* the offset, or the address, being written */
};

static void out_align(struct out *o, size_t a)
{
	size_t pad = (a - (o->at & (a - 1))) & (a - 1);

	if (o->p) {
		memset(o->p, 0, pad);
		o->p += pad;
	}
	o->at += pad;
}

static void out_bytes(struct out *o, const void *d, size_t n)
{
	if (o->p) {
		if (d)
			memcpy(o->p, d, n);
		else
			memset(o->p, 0, n);
		o->p += n;
	}
	o->at += n;
}

static void out16(struct out *o, WORD v)
{
	out_align(o, 2);
	out_bytes(o, &v, 2);
}

static void out32(struct out *o, DWORD v)
{
	out_align(o, 4);
	out_bytes(o, &v, 4);
}

/* A FLAGGED_WORD_BLOB. */
static void out_bstr(struct out *o, BSTR b)
{
	DWORD bytes = b ? oa_SysStringByteLen(b) : 0;
	DWORD len = (bytes + 1) / 2;

	out32(o, len);
	out32(o, b ? bytes : 0xffffffff);
	out32(o, len);
	out_bytes(o, b, bytes);
	if (bytes & 1)
		out_bytes(o, NULL, 1);
}

/* An MInterfacePointer, after its referent ID. */
static void out_iface(struct out *o, IUnknown *obj, REFIID riid)
{
	DWORD n = (DWORD)ole_marshal_size();

	out32(o, n);
	out32(o, n);
	if (o->p) {
		HRESULT hr = ole_marshal_write(o->p, riid, obj);
		if (FAILED(hr)) {
			TRACE("oleaut32: CoMarshalInterface: %#x",
			      (unsigned)hr);
			bad_data();
		}
		o->p += n;
	}
	o->at += n;
}

static void out_ptr(struct out *o, const void *p)
{
	out32(o, p ? REFERENT : 0);
}

/* A scalar's size, 0 if vt isn't one. */
static size_t scalar_size(VARTYPE vt)
{
	switch (vt) {
	case VT_I1: case VT_UI1:
		return 1;
	case VT_I2: case VT_UI2: case VT_BOOL:
		return 2;
	case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4:
	case VT_ERROR:
		return 4;
	case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
		return 8;
	}
	return 0;
}

static void out_variant(struct out *o, VARIANT *v, int depth);
static void out_array(struct out *o, SAFEARRAY *psa, int depth);

/* The union arm for vt: v's value, or what it points to. */
static void out_arm(struct out *o, VARTYPE vt, VARIANT *v, int depth)
{
	VARTYPE base = vt & VT_TYPEMASK;
	size_t n;

	if (vt & VT_ARRAY) {
		if (vt & VT_BYREF) {
			out_ptr(o, v->pparray);
			if (v->pparray) {
				out_ptr(o, *v->pparray);
				if (*v->pparray)
					out_array(o, *v->pparray, depth);
			}
		} else {
			out_ptr(o, v->parray);
			if (v->parray)
				out_array(o, v->parray, depth);
		}
		return;
	}
	if (vt & VT_BYREF) {
		void *p = v->byref;
		out_ptr(o, p);
		if (!p)
			return;
		switch (base) {
		case VT_BSTR:
			out_ptr(o, p);
			out_bstr(o, *(BSTR *)p);
			return;
		case VT_UNKNOWN:
		case VT_DISPATCH: {
			IUnknown *obj = *(IUnknown **)p;
			out_ptr(o, obj);
			if (obj)
				out_iface(o, obj, base == VT_UNKNOWN ?
					  &oa_IID_IUnknown :
					  &oa_IID_IDispatch);
			return;
		}
		case VT_VARIANT:
			out_variant(o, p, depth + 1);
			return;
		case VT_DECIMAL:
			out_align(o, 8);
			out_bytes(o, p, sizeof(DECIMAL));
			return;
		}
		if (!(n = scalar_size(base)))
			bad_data();
		out_align(o, n);
		out_bytes(o, p, n);
		return;
	}
	switch (base) {
	case VT_EMPTY:
	case VT_NULL:
		return;
	case VT_BSTR:
		out_ptr(o, v);		/* never NULL: the blob says */
		out_bstr(o, v->bstrVal);
		return;
	case VT_UNKNOWN:
	case VT_DISPATCH:
		out_ptr(o, v->punkVal);
		if (v->punkVal)
			out_iface(o, v->punkVal, base == VT_UNKNOWN ?
				  &oa_IID_IUnknown : &oa_IID_IDispatch);
		return;
	case VT_DECIMAL: {
		DECIMAL d = v->decVal;
		d.wReserved = 0;
		out_align(o, 8);
		out_bytes(o, &d, sizeof(d));
		return;
	}
	}
	if (!(n = scalar_size(base)))
		bad_data();
	out_align(o, n);
	out_bytes(o, &v->llVal, n);
}

/* The union discriminant a VARIANT's vt goes with: an array's arm is
 * VT_ARRAY (by reference or not), whatever its element type. */
static DWORD wire_disc(VARTYPE vt)
{
	return vt & VT_ARRAY ? (DWORD)(vt & (VT_ARRAY | VT_BYREF)) : vt;
}

static void out_variant(struct out *o, VARIANT *v, int depth)
{
	if (depth > DEPTH_MAX)
		bad_data();
	out_align(o, 8);
	size_t start = o->at;
	BYTE *size_at = o->p;
	out32(o, 0);		/* its size, below */
	out32(o, 0);
	out16(o, v->vt);
	out16(o, 0);		/* the reserved words: zero, always */
	out16(o, 0);
	out16(o, 0);
	out32(o, wire_disc(v->vt));
	out_arm(o, v->vt, v, depth);
	if (size_at) {
		DWORD units = (DWORD)((o->at - start + 7) / 8);
		memcpy(size_at, &units, 4);
	}
}

/* The SF_ kind an array's elements go as; 0 if none. */
static DWORD sf_kind(SAFEARRAY *psa, VARTYPE *vt)
{
	if (FAILED(oa_SafeArrayGetVartype(psa, vt))) {
		if (psa->fFeatures & FADF_BSTR)
			*vt = VT_BSTR;
		else if (psa->fFeatures & FADF_UNKNOWN)
			*vt = VT_UNKNOWN;
		else if (psa->fFeatures & FADF_DISPATCH)
			*vt = VT_DISPATCH;
		else if (psa->fFeatures & FADF_VARIANT)
			*vt = VT_VARIANT;
		else
			*vt = VT_EMPTY;
	}
	switch (*vt) {
	case VT_BSTR:
		return SF_BSTR;
	case VT_VARIANT:
		return SF_VARIANT;
	case VT_UNKNOWN:
	case VT_DISPATCH:
		if (psa->fFeatures & FADF_HAVEIID)
			return SF_HAVEIID;
		return *vt == VT_UNKNOWN ? SF_UNKNOWN : SF_DISPATCH;
	case VT_RECORD:
		return 0;
	}
	switch (psa->cbElements) {
	case 1: return SF_I1;
	case 2: return SF_I2;
	case 4: return SF_I4;
	case 8: return SF_I8;
	}
	return 0;
}

/* The element type an SF_ kind stands for, when the wire has none. */
static VARTYPE sf_vt(DWORD sf, USHORT features)
{
	switch (sf) {
	case SF_I1: case SF_I2: case SF_I4: case SF_I8:
	case SF_BSTR: case SF_VARIANT: case SF_UNKNOWN: case SF_DISPATCH:
		return (VARTYPE)sf;
	case SF_HAVEIID:
		return features & FADF_DISPATCH ? VT_DISPATCH : VT_UNKNOWN;
	}
	return VT_EMPTY;
}

/* The element size NDR20 gives for an SF_ kind: 32-bit pointers and
 * VARIANTs, whatever this build's are. */
static DWORD wire_elem(DWORD sf, ULONG local)
{
	switch (sf) {
	case SF_BSTR: case SF_UNKNOWN: case SF_DISPATCH: case SF_HAVEIID:
		return 4;
	case SF_VARIANT:
		return 16;
	}
	return local;
}

/* Its element count, or more than 0x7fffffff if that's too many. */
static uint64_t cells(USHORT dims, const SAFEARRAYBOUND *b)
{
	uint64_t n = 1;

	for (USHORT i = 0; i < dims; i++) {
		n *= b[i].cElements;
		if (n > 0x7fffffff)
			return n;
	}
	return n;
}

#define CELLS_MAX 0x7fffffffu

static size_t sf_size(DWORD sf)
{
	switch (sf) {
	case SF_I1: return 1;
	case SF_I2: return 2;
	case SF_I4: return 4;
	case SF_I8: return 8;
	}
	return 0;
}

/* A SAFEARRAY after its referent ID. */
static void out_array(struct out *o, SAFEARRAY *psa, int depth)
{
	VARTYPE vt;
	DWORD sf = sf_kind(psa, &vt);
	uint64_t cn = cells(psa->cDims, psa->rgsabound);
	GUID iid;

	if (!sf || cn > CELLS_MAX || !psa->cDims || depth > DEPTH_MAX)
		bad_data();
	ULONG n = (ULONG)cn;
	DWORD locks = psa->cLocks & 0xffff;
	if (psa->fFeatures & FADF_HAVEVARTYPE)
		locks |= (DWORD)vt << 16;
	out32(o, psa->cDims);
	out16(o, psa->cDims);
	out16(o, psa->fFeatures);
	out32(o, wire_elem(sf, psa->cbElements));
	out32(o, locks);
	out32(o, sf);		/* the union's discriminant */
	out32(o, n);
	out_ptr(o, n ? psa->pvData : NULL);
	if (sf == SF_HAVEIID) {
		if (FAILED(oa_SafeArrayGetIID(psa, &iid)))
			iid = vt == VT_DISPATCH ? oa_IID_IDispatch :
						  oa_IID_IUnknown;
		out_align(o, 4);
		out_bytes(o, &iid, sizeof(iid));
	}
	for (USHORT i = 0; i < psa->cDims; i++) {
		out32(o, psa->rgsabound[i].cElements);
		out32(o, (DWORD)psa->rgsabound[i].lLbound);
	}
	if (!n || !psa->pvData)
		return;
	out32(o, n);
	BYTE *data = psa->pvData;
	switch (sf) {
	case SF_BSTR:
		for (ULONG i = 0; i < n; i++)
			out_ptr(o, data);
		for (ULONG i = 0; i < n; i++)
			out_bstr(o, ((BSTR *)data)[i]);
		return;
	case SF_UNKNOWN:
	case SF_DISPATCH:
	case SF_HAVEIID: {
		IUnknown **objs = (IUnknown **)data;
		if (sf != SF_HAVEIID)
			iid = sf == SF_UNKNOWN ? oa_IID_IUnknown :
						 oa_IID_IDispatch;
		for (ULONG i = 0; i < n; i++)
			out_ptr(o, objs[i]);
		for (ULONG i = 0; i < n; i++)
			if (objs[i])
				out_iface(o, objs[i], &iid);
		return;
	}
	case SF_VARIANT:
		for (ULONG i = 0; i < n; i++)
			out_ptr(o, data);
		for (ULONG i = 0; i < n; i++)
			out_variant(o, &((VARIANT *)data)[i], depth + 1);
		return;
	}
	size_t es = sf_size(sf);
	out_align(o, es);
	out_bytes(o, data, (size_t)n * es);
}

/* ---- reading ----
 *
 * Every read is bounded by the end of the RPC buffer. A read that
 * doesn't fit, or data that doesn't make sense, sets bad and reads
 * nothing more; each reader then lets go of what it took, and only the
 * exports raise. A destination is only replaced once its value has
 * read in whole. */

struct in {
	const BYTE *p;
	uintptr_t end;		/* where the buffer ends */
	int bad;
};

static void in_fail(struct in *r)
{
	r->bad = 1;
}

/* n bytes after aligning to a, or NULL (and bad) if they aren't there. */
static const BYTE *in_take(struct in *r, size_t a, size_t n)
{
	if (r->bad)
		return NULL;
	uintptr_t at = (uintptr_t)r->p;
	uintptr_t pad = (a - (at & (a - 1))) & (a - 1);
	if (at > r->end || r->end - at < pad || r->end - at - pad < n) {
		in_fail(r);
		return NULL;
	}
	const BYTE *q = r->p + pad;
	r->p = q + n;
	return q;
}

static DWORD in32(struct in *r)
{
	const BYTE *q = in_take(r, 4, 4);
	DWORD v = 0;

	if (q)
		memcpy(&v, q, 4);
	return v;
}

static WORD in16(struct in *r)
{
	const BYTE *q = in_take(r, 2, 2);
	WORD v = 0;

	if (q)
		memcpy(&v, q, 2);
	return v;
}

/* A FLAGGED_WORD_BLOB into a new BSTR (NULL for a NULL one). */
static BSTR in_bstr(struct in *r)
{
	DWORD len = in32(r), bytes = in32(r), len2 = in32(r);

	if (r->bad)
		return NULL;
	if (bytes == 0xffffffff) {
		if (len || len2)
			in_fail(r);
		return NULL;
	}
	if (len != len2 || len > 0x7ffffffe / 2 || bytes > len * 2 ||
	    len * 2 - bytes > 1) {
		in_fail(r);
		return NULL;
	}
	const BYTE *q = in_take(r, 1, (size_t)len * 2);
	if (!q)
		return NULL;
	BSTR b = oa_SysAllocStringByteLen((LPCSTR)q, bytes);
	if (!b)
		in_fail(r);
	return b;
}

/* An MInterfacePointer into an interface. */
static void *in_iface(struct in *r, REFIID riid)
{
	DWORD n = in32(r), n2 = in32(r);
	void *obj = NULL;

	if (r->bad)
		return NULL;
	if (n != n2) {
		in_fail(r);
		return NULL;
	}
	const BYTE *q = in_take(r, 1, n);
	if (q && FAILED(ole_marshal_read(q, n, riid, &obj))) {
		in_fail(r);
		return NULL;
	}
	return obj;
}

static void in_variant(struct in *r, VARIANT *v, int depth);
static SAFEARRAY *in_array(struct in *r, int depth);

/* Lets go of what a VARIANT holds, by reference too. */
static void variant_free(VARIANT *v, int depth)
{
	VARTYPE vt = v->vt;

	if (!(vt & VT_BYREF)) {
		oa_VariantClear(v);
		return;
	}
	void *ref = v->byref;
	if (ref && depth < DEPTH_MAX) {
		if (vt & VT_ARRAY) {
			if (*(SAFEARRAY **)ref)
				oa_SafeArrayDestroy(*(SAFEARRAY **)ref);
		} else {
			switch (vt & VT_TYPEMASK) {
			case VT_BSTR:
				oa_SysFreeString(*(BSTR *)ref);
				break;
			case VT_UNKNOWN:
			case VT_DISPATCH:
				if (*(IUnknown **)ref)
					(*(IUnknown **)ref)->lpVtbl->Release(
						*(IUnknown **)ref);
				break;
			case VT_VARIANT:
				variant_free(ref, depth + 1);
				break;
			}
		}
		ole_taskmem_free(ref);
	}
	v->vt = VT_EMPTY;
	v->byref = NULL;
}

static void release(void *obj)
{
	if (obj)
		((IUnknown *)obj)->lpVtbl->Release(obj);
}

/* A by-reference arm into *pref: the referent already there, which keeps
 * its address and has its old value let go of, or new storage. */
static void in_byref(struct in *r, VARTYPE vt, void **pref, int depth)
{
	VARTYPE base = vt & VT_TYPEMASK;
	DWORD id = in32(r);
	size_t n;

	if (r->bad)
		return;
	if (!id) {
		if (*pref) {
			VARIANT old;
			memset(&old, 0, sizeof(old));
			old.vt = vt;
			old.byref = *pref;
			variant_free(&old, depth);
			*pref = NULL;
		}
		return;
	}
	if (vt & VT_ARRAY)
		n = sizeof(SAFEARRAY *);
	else switch (base) {
	case VT_BSTR: n = sizeof(BSTR); break;
	case VT_UNKNOWN:
	case VT_DISPATCH: n = sizeof(IUnknown *); break;
	case VT_VARIANT: n = sizeof(VARIANT); break;
	case VT_DECIMAL: n = sizeof(DECIMAL); break;
	default:
		if (!(n = scalar_size(base))) {
			in_fail(r);
			return;
		}
	}
	void *ref = *pref;
	int fresh = !ref;
	if (fresh) {
		if (!(ref = ole_taskmem_alloc(n))) {
			in_fail(r);
			return;
		}
		memset(ref, 0, n);
	}
	const BYTE *q;
	if (vt & VT_ARRAY) {
		SAFEARRAY *psa = in32(r) ? in_array(r, depth) : NULL;
		if (!r->bad) {
			if (*(SAFEARRAY **)ref)
				oa_SafeArrayDestroy(*(SAFEARRAY **)ref);
			*(SAFEARRAY **)ref = psa;
		}
	} else switch (base) {
	case VT_BSTR: {
		if (!in32(r))
			in_fail(r);
		BSTR b = in_bstr(r);
		if (!r->bad) {
			oa_SysFreeString(*(BSTR *)ref);
			*(BSTR *)ref = b;
		}
		break;
	}
	case VT_UNKNOWN:
	case VT_DISPATCH: {
		void *obj = in32(r) ? in_iface(r, base == VT_UNKNOWN ?
					       &oa_IID_IUnknown :
					       &oa_IID_IDispatch) : NULL;
		if (!r->bad) {
			release(*(void **)ref);
			*(void **)ref = obj;
		}
		break;
	}
	case VT_VARIANT:
		in_variant(r, ref, depth + 1);
		break;
	case VT_DECIMAL:
		if ((q = in_take(r, 8, n)))
			memcpy(ref, q, n);
		break;
	default:
		if ((q = in_take(r, n, n)))
			memcpy(ref, q, n);
	}
	if (fresh) {
		if (r->bad)
			ole_taskmem_free(ref);	/* (it holds nothing) */
		else
			*pref = ref;
	}
}

/* A by-value arm into v, which is empty. */
static void in_arm(struct in *r, VARTYPE vt, VARIANT *v, int depth)
{
	VARTYPE base = vt & VT_TYPEMASK;
	const BYTE *q;
	size_t n;

	if (vt & VT_ARRAY) {
		v->parray = in32(r) ? in_array(r, depth) : NULL;
		return;
	}
	switch (base) {
	case VT_EMPTY:
	case VT_NULL:
		return;
	case VT_BSTR:
		if (!in32(r))
			in_fail(r);
		v->bstrVal = in_bstr(r);
		return;
	case VT_UNKNOWN:
	case VT_DISPATCH:
		v->punkVal = in32(r) ? in_iface(r, base == VT_UNKNOWN ?
						&oa_IID_IUnknown :
						&oa_IID_IDispatch) : NULL;
		return;
	case VT_DECIMAL:
		if ((q = in_take(r, 8, sizeof(DECIMAL))))
			memcpy(&v->decVal, q, sizeof(DECIMAL));
		return;
	}
	if (!(n = scalar_size(base))) {
		in_fail(r);
		return;
	}
	if ((q = in_take(r, n, n)))
		memcpy(&v->llVal, q, n);
}

/* A VARIANT into v, which keeps its old value if this one doesn't read.
 * A by-reference one of the same type is read into the referent v
 * already has. The reserved words aren't looked at. */
static void in_variant(struct in *r, VARIANT *v, int depth)
{
	if (depth > DEPTH_MAX) {
		in_fail(r);
		return;
	}
	in_take(r, 8, 0);
	in32(r);		/* its size */
	in32(r);
	VARTYPE vt = in16(r);
	in16(r);
	in16(r);
	in16(r);
	DWORD disc = in32(r);
	if (r->bad)
		return;
	if (disc != wire_disc(vt) || FAILED(oa_validate_vt(vt)) ||
	    (vt & VT_TYPEMASK) == VT_RECORD) {
		in_fail(r);
		return;
	}
	if ((vt & VT_BYREF) && v->vt == vt && v->byref) {
		in_byref(r, vt, &v->byref, depth);
		return;
	}
	VARIANT tmp;
	memset(&tmp, 0, sizeof(tmp));
	if (vt & VT_BYREF)
		in_byref(r, vt, &tmp.byref, depth);
	else
		in_arm(r, vt, &tmp, depth);
	/* (a DECIMAL's arm fills the whole VARIANT, vt aside) */
	tmp.vt = vt;
	if (r->bad) {
		variant_free(&tmp, depth);
		return;
	}
	variant_free(v, depth);
	*v = tmp;
}

static SAFEARRAY *in_array(struct in *r, int depth)
{
	DWORD conf = in32(r);
	USHORT dims = in16(r), features = in16(r);
	DWORD elem = in32(r), locks = in32(r);
	DWORD sf = in32(r), n = in32(r), data = in32(r);
	GUID iid = oa_IID_NULL;
	SAFEARRAY *psa = NULL;
	const BYTE *q;

	if (r->bad)
		return NULL;
	if (!dims || conf != dims || depth > DEPTH_MAX) {
		in_fail(r);
		return NULL;
	}
	if (sf == SF_HAVEIID) {
		if (!(q = in_take(r, 4, sizeof(iid))))
			return NULL;
		memcpy(&iid, q, sizeof(iid));
	}
	/* The element type is in cLocks' high word only with
	 * FADF_HAVEVARTYPE; otherwise the SF_ kind says. */
	VARTYPE vt = features & FADF_HAVEVARTYPE ? (VARTYPE)(locks >> 16) :
						   sf_vt(sf, features);
	if (FAILED(oa_SafeArrayAllocDescriptorEx(vt, dims, &psa))) {
		in_fail(r);
		return NULL;
	}
	if (!(features & FADF_HAVEVARTYPE))
		psa->fFeatures &= ~FADF_HAVEVARTYPE;
	for (USHORT i = 0; i < dims; i++) {
		psa->rgsabound[i].cElements = in32(r);
		psa->rgsabound[i].lLbound = (LONG)in32(r);
	}
	VARTYPE want;
	DWORD kind = r->bad ? 0 : sf_kind(psa, &want);
	if (r->bad || !kind || kind != sf ||
	    wire_elem(sf, psa->cbElements) != elem ||
	    cells(dims, psa->rgsabound) != n ||
	    (features & FADF_HAVEIID) != (psa->fFeatures & FADF_HAVEIID))
		goto bad;
	if (sf == SF_HAVEIID)
		oa_SafeArraySetIID(psa, &iid);
	if (FAILED(oa_SafeArrayAllocData(psa)))
		goto bad;
	if (n)
		memset(psa->pvData, 0, (size_t)n * psa->cbElements);
	if (!n || !data)
		return psa;
	if (in32(r) != n)
		goto bad;
	BYTE *d = psa->pvData;
	switch (sf) {
	case SF_BSTR: {
		const BYTE *ids = in_take(r, 4, 4 * (size_t)n);
		for (ULONG i = 0; ids && i < n && !r->bad; i++) {
			DWORD id;
			memcpy(&id, ids + 4 * (size_t)i, 4);
			if (!id)
				in_fail(r);
			else
				((BSTR *)d)[i] = in_bstr(r);
		}
		break;
	}
	case SF_UNKNOWN:
	case SF_DISPATCH:
	case SF_HAVEIID: {
		if (sf != SF_HAVEIID)
			iid = sf == SF_UNKNOWN ? oa_IID_IUnknown :
						 oa_IID_IDispatch;
		const BYTE *ids = in_take(r, 4, 4 * (size_t)n);
		for (ULONG i = 0; ids && i < n && !r->bad; i++) {
			DWORD id;
			memcpy(&id, ids + 4 * (size_t)i, 4);
			if (id)
				((void **)d)[i] = in_iface(r, &iid);
		}
		break;
	}
	case SF_VARIANT:
		in_take(r, 4, 4 * (size_t)n);
		for (ULONG i = 0; i < n && !r->bad; i++)
			in_variant(r, &((VARIANT *)d)[i], depth + 1);
		break;
	default: {
		size_t es = sf_size(sf);
		if ((q = in_take(r, es, (size_t)n * es)))
			memcpy(d, q, (size_t)n * es);
	}
	}
	if (!r->bad)
		return psa;
bad:
	in_fail(r);
	oa_SafeArrayDestroy(psa);
	return NULL;
}

/* Where the RPC buffer ends, from the USER_MARSHAL_CB the NDR engine
 * passes as the flags (its Flags come first). A caller that passes the
 * bare flags gets no bound. */
#define USER_MARSHAL_CB_SIGNATURE 0x55535243u	/* 'USRC' */

struct stub_message_head {
	void *RpcMsg;
	BYTE *Buffer;
	BYTE *BufferStart;
	BYTE *BufferEnd;
};

struct user_marshal_cb_head {
	ULONG Flags;
	struct stub_message_head *pStubMsg;
	void *pReserve;
	ULONG Signature;
};

static struct in in_start(ULONG *flags, const BYTE *buf)
{
	struct in r = { buf, UINTPTR_MAX, 0 };
	const struct user_marshal_cb_head *cb = (const void *)flags;

	if (cb && cb->Signature == USER_MARSHAL_CB_SIGNATURE &&
	    cb->pStubMsg && cb->pStubMsg->BufferEnd) {
		r.end = (uintptr_t)cb->pStubMsg->BufferEnd;
		if ((uintptr_t)buf > r.end)
			r.bad = 1;
	}
	return r;
}

/* ---- the exports ---- */

WINFN(ULONG) oa_BSTR_UserSize(ULONG *flags, ULONG start, BSTR *pb)
{
	struct out o = { NULL, start };

	out_align(&o, 4);
	out_bstr(&o, *pb);
	return (ULONG)o.at;
}

WINFN(unsigned char *) oa_BSTR_UserMarshal(ULONG *flags, unsigned char *buf,
					   BSTR *pb)
{
	struct out o = { buf, (uintptr_t)buf };

	out_align(&o, 4);
	out_bstr(&o, *pb);
	return o.p;
}

WINFN(unsigned char *) oa_BSTR_UserUnmarshal(ULONG *flags,
					     unsigned char *buf, BSTR *pb)
{
	struct in r = in_start(flags, buf);

	in_take(&r, 4, 0);
	BSTR b = in_bstr(&r);
	if (r.bad)
		bad_data();
	oa_SysFreeString(*pb);
	*pb = b;
	return (unsigned char *)r.p;
}

WINFN(void) oa_BSTR_UserFree(ULONG *flags, BSTR *pb)
{
	oa_SysFreeString(*pb);
	*pb = NULL;
}

WINFN(ULONG) oa_VARIANT_UserSize(ULONG *flags, ULONG start, VARIANT *v)
{
	struct out o = { NULL, start };

	out_variant(&o, v, 0);
	return (ULONG)o.at;
}

WINFN(unsigned char *) oa_VARIANT_UserMarshal(ULONG *flags,
					      unsigned char *buf, VARIANT *v)
{
	struct out o = { buf, (uintptr_t)buf };

	out_variant(&o, v, 0);
	return o.p;
}

WINFN(unsigned char *) oa_VARIANT_UserUnmarshal(ULONG *flags,
						unsigned char *buf,
						VARIANT *v)
{
	struct in r = in_start(flags, buf);

	in_variant(&r, v, 0);
	if (r.bad)
		bad_data();
	return (unsigned char *)r.p;
}

WINFN(void) oa_VARIANT_UserFree(ULONG *flags, VARIANT *v)
{
	variant_free(v, 0);
}

WINFN(ULONG) oa_LPSAFEARRAY_UserSize(ULONG *flags, ULONG start,
				     SAFEARRAY **ppsa)
{
	struct out o = { NULL, start };

	out_ptr(&o, *ppsa);
	if (*ppsa)
		out_array(&o, *ppsa, 0);
	return (ULONG)o.at;
}

WINFN(unsigned char *) oa_LPSAFEARRAY_UserMarshal(ULONG *flags,
						  unsigned char *buf,
						  SAFEARRAY **ppsa)
{
	struct out o = { buf, (uintptr_t)buf };

	out_ptr(&o, *ppsa);
	if (*ppsa)
		out_array(&o, *ppsa, 0);
	return o.p;
}

WINFN(unsigned char *) oa_LPSAFEARRAY_UserUnmarshal(ULONG *flags,
						    unsigned char *buf,
						    SAFEARRAY **ppsa)
{
	struct in r = in_start(flags, buf);
	SAFEARRAY *psa = in32(&r) ? in_array(&r, 0) : NULL;

	if (r.bad)
		bad_data();
	if (*ppsa)
		oa_SafeArrayDestroy(*ppsa);
	*ppsa = psa;
	return (unsigned char *)r.p;
}

WINFN(void) oa_LPSAFEARRAY_UserFree(ULONG *flags, SAFEARRAY **ppsa)
{
	if (*ppsa)
		oa_SafeArrayDestroy(*ppsa);
	*ppsa = NULL;
}

#define E(name, ord) { #name, ord, (void *)oa_##name }

const struct builtin_export oa_marshal_exports[] = {
	E(BSTR_UserSize, 283),
	E(BSTR_UserMarshal, 284),
	E(BSTR_UserUnmarshal, 285),
	E(BSTR_UserFree, 286),
	E(VARIANT_UserSize, 287),
	E(VARIANT_UserMarshal, 288),
	E(VARIANT_UserUnmarshal, 289),
	E(VARIANT_UserFree, 290),
	E(LPSAFEARRAY_UserSize, 291),
	E(LPSAFEARRAY_UserMarshal, 292),
	E(LPSAFEARRAY_UserUnmarshal, 293),
	E(LPSAFEARRAY_UserFree, 294),
	BUILTIN_END
};
