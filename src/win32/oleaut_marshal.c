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
 *              vt and the three reserved WORDs, vt again as the union's
 *              DWORD discriminant, then the arm: a value, or for a
 *              pointer a 4-byte referent ID (0 for NULL) with what it
 *              points to after it. Interfaces go as an MInterfacePointer
 *              (a DWORD count twice, then what CoMarshalInterface wrote).
 *   SAFEARRAY  a referent ID, then the descriptor: cDims (conformance),
 *              cDims, fFeatures, cbElements, cLocks with the element's
 *              VARTYPE in its high WORD, the SF_ type twice, the element
 *              count, a referent ID for the data, the IID (SF_HAVEIID),
 *              the bounds as they are in memory; then the count again and
 *              the elements (BSTRs, interfaces and VARIANTs as pointers
 *              first, then what each points to).
 *
 * Referent IDs are any nonzero value. Data that can't be marshalled (a
 * VT_RECORD, an element type with no SF_ form) or a buffer that doesn't
 * read back raises RPC_X_BAD_STUB_DATA, as the NDR engine does. The
 * marshal context in the flags is not looked at: interfaces always go
 * through CoMarshalInterface, which in seance is always in process.
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
	if (v->vt == VT_DECIMAL) {
		out16(o, 0);
		out16(o, 0);
		out16(o, 0);
	} else {
		out16(o, v->wReserved1);
		out16(o, v->wReserved2);
		out16(o, v->wReserved3);
	}
	out32(o, v->vt);
	out_arm(o, v->vt, v, depth);
	if (size_at) {
		DWORD units = (DWORD)((o->at - start + 7) / 8);
		memcpy(size_at, &units, 4);
	}
}

/* The SF_ kind an array's elements go as; bad_data() if none. */
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
		bad_data();
	}
	switch (psa->cbElements) {
	case 1: return SF_I1;
	case 2: return SF_I2;
	case 4: return SF_I4;
	case 8: return SF_I8;
	}
	bad_data();
	return 0;
}

/* Its element count; bad_data() if it overflows. */
static ULONG cells(USHORT dims, const SAFEARRAYBOUND *b)
{
	uint64_t n = 1;

	for (USHORT i = 0; i < dims; i++) {
		n *= b[i].cElements;
		if (n > 0x7fffffff)
			bad_data();
	}
	return (ULONG)n;
}

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
	ULONG n = cells(psa->cDims, psa->rgsabound);
	GUID iid;

	if (!psa->cDims || depth > DEPTH_MAX)
		bad_data();
	out32(o, psa->cDims);
	out16(o, psa->cDims);
	out16(o, psa->fFeatures);
	out32(o, psa->cbElements);
	out32(o, (psa->cLocks & 0xffff) | (DWORD)vt << 16);
	out32(o, sf);
	out32(o, sf);
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

/* ---- reading ---- */

static const BYTE *in_align(const BYTE *p, size_t a)
{
	return p + ((a - ((uintptr_t)p & (a - 1))) & (a - 1));
}

static DWORD in32(const BYTE **p)
{
	DWORD v;

	*p = in_align(*p, 4);
	memcpy(&v, *p, 4);
	*p += 4;
	return v;
}

static WORD in16(const BYTE **p)
{
	WORD v;

	*p = in_align(*p, 2);
	memcpy(&v, *p, 2);
	*p += 2;
	return v;
}

/* A FLAGGED_WORD_BLOB into a new BSTR (NULL for a NULL one). */
static BSTR in_bstr(const BYTE **p)
{
	DWORD len = in32(p), bytes = in32(p), len2 = in32(p);

	if (bytes == 0xffffffff) {
		if (len || len2)
			bad_data();
		return NULL;
	}
	if (len != len2 || len > 0x7ffffffe / 2 || bytes > len * 2 ||
	    len * 2 - bytes > 1)
		bad_data();
	BSTR b = oa_SysAllocStringByteLen((LPCSTR)*p, bytes);
	if (!b)
		bad_data();
	*p += len * 2;
	return b;
}

/* An MInterfacePointer into an interface, or bad_data(). */
static void *in_iface(const BYTE **p, REFIID riid)
{
	DWORD n = in32(p), n2 = in32(p);
	void *obj = NULL;

	if (n != n2 || FAILED(ole_marshal_read(*p, n, riid, &obj)))
		bad_data();
	*p += n;
	return obj;
}

static void in_variant(const BYTE **p, VARIANT *v, int depth);
static SAFEARRAY *in_array(const BYTE **p, int depth);

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

static void in_arm(const BYTE **p, VARTYPE vt, VARIANT *v, int depth)
{
	VARTYPE base = vt & VT_TYPEMASK;
	size_t n;

	if (vt & VT_ARRAY) {
		if (vt & VT_BYREF) {
			if (!in32(p))
				return;
			SAFEARRAY **pp = ole_taskmem_alloc(sizeof(*pp));
			if (!pp)
				bad_data();
			*pp = in32(p) ? in_array(p, depth) : NULL;
			v->pparray = pp;
		} else {
			v->parray = in32(p) ? in_array(p, depth) : NULL;
		}
		return;
	}
	if (vt & VT_BYREF) {
		if (!in32(p))
			return;
		switch (base) {
		case VT_BSTR: n = sizeof(BSTR); break;
		case VT_UNKNOWN:
		case VT_DISPATCH: n = sizeof(IUnknown *); break;
		case VT_VARIANT: n = sizeof(VARIANT); break;
		case VT_DECIMAL: n = sizeof(DECIMAL); break;
		default:
			if (!(n = scalar_size(base)))
				bad_data();
		}
		void *ref = ole_taskmem_alloc(n);
		if (!ref)
			bad_data();
		memset(ref, 0, n);
		v->byref = ref;
		switch (base) {
		case VT_BSTR:
			if (!in32(p))
				bad_data();
			*(BSTR *)ref = in_bstr(p);
			return;
		case VT_UNKNOWN:
		case VT_DISPATCH:
			if (in32(p))
				*(void **)ref = in_iface(p, base == VT_UNKNOWN ?
							 &oa_IID_IUnknown :
							 &oa_IID_IDispatch);
			return;
		case VT_VARIANT:
			in_variant(p, ref, depth + 1);
			return;
		case VT_DECIMAL:
			*p = in_align(*p, 8);
			memcpy(ref, *p, n);
			*p += n;
			return;
		}
		*p = in_align(*p, n);
		memcpy(ref, *p, n);
		*p += n;
		return;
	}
	switch (base) {
	case VT_EMPTY:
	case VT_NULL:
		return;
	case VT_BSTR:
		if (!in32(p))
			bad_data();
		v->bstrVal = in_bstr(p);
		return;
	case VT_UNKNOWN:
	case VT_DISPATCH:
		v->punkVal = in32(p) ? in_iface(p, base == VT_UNKNOWN ?
						&oa_IID_IUnknown :
						&oa_IID_IDispatch) : NULL;
		return;
	case VT_DECIMAL:
		*p = in_align(*p, 8);
		memcpy(&v->decVal, *p, sizeof(DECIMAL));
		*p += sizeof(DECIMAL);
		return;
	}
	if (!(n = scalar_size(base)))
		bad_data();
	*p = in_align(*p, n);
	memcpy(&v->llVal, *p, n);
	*p += n;
}

static void in_variant(const BYTE **p, VARIANT *v, int depth)
{
	if (depth > DEPTH_MAX)
		bad_data();
	*p = in_align(*p, 8);
	in32(p);		/* its size */
	in32(p);
	VARTYPE vt = in16(p);
	WORD r1 = in16(p), r2 = in16(p), r3 = in16(p);
	if (in32(p) != vt || FAILED(oa_validate_vt(vt)) ||
	    (vt & VT_TYPEMASK) == VT_RECORD)
		bad_data();
	variant_free(v, depth);
	memset(v, 0, sizeof(*v));
	in_arm(p, vt, v, depth);
	/* (a DECIMAL's arm fills the whole VARIANT, vt aside) */
	v->vt = vt;
	if (vt != VT_DECIMAL) {
		v->wReserved1 = r1;
		v->wReserved2 = r2;
		v->wReserved3 = r3;
	}
}

static SAFEARRAY *in_array(const BYTE **p, int depth)
{
	DWORD conf = in32(p);
	USHORT dims = in16(p), features = in16(p);
	DWORD elem = in32(p), locks = in32(p);
	DWORD sf = in32(p), sf2 = in32(p), n = in32(p), data = in32(p);
	VARTYPE vt = (VARTYPE)(locks >> 16);
	GUID iid = oa_IID_NULL;
	SAFEARRAY *psa = NULL;

	if (!dims || conf != dims || sf != sf2 || depth > DEPTH_MAX)
		bad_data();
	if (sf == SF_HAVEIID) {
		*p = in_align(*p, 4);
		memcpy(&iid, *p, sizeof(iid));
		*p += sizeof(iid);
	}
	if (FAILED(oa_SafeArrayAllocDescriptorEx(vt, dims, &psa)))
		bad_data();
	for (USHORT i = 0; i < dims; i++) {
		psa->rgsabound[i].cElements = in32(p);
		psa->rgsabound[i].lLbound = (LONG)in32(p);
	}
	VARTYPE want;
	if (psa->cbElements != elem || sf_kind(psa, &want) != sf ||
	    cells(dims, psa->rgsabound) != n ||
	    (features & FADF_HAVEIID) != (psa->fFeatures & FADF_HAVEIID)) {
		oa_SafeArrayDestroy(psa);
		bad_data();
	}
	if (sf == SF_HAVEIID)
		oa_SafeArraySetIID(psa, &iid);
	if (FAILED(oa_SafeArrayAllocData(psa))) {
		oa_SafeArrayDestroy(psa);
		bad_data();
	}
	if (!n || !data)
		return psa;
	if (in32(p) != n) {
		oa_SafeArrayDestroy(psa);
		bad_data();
	}
	BYTE *d = psa->pvData;
	switch (sf) {
	case SF_BSTR: {
		const BYTE *ids = *p;
		*p += 4 * (size_t)n;
		for (ULONG i = 0; i < n; i++) {
			if (!in32(&ids))
				bad_data();
			((BSTR *)d)[i] = in_bstr(p);
		}
		return psa;
	}
	case SF_UNKNOWN:
	case SF_DISPATCH:
	case SF_HAVEIID: {
		const BYTE *ids = *p;
		if (sf != SF_HAVEIID)
			iid = sf == SF_UNKNOWN ? oa_IID_IUnknown :
						 oa_IID_IDispatch;
		*p += 4 * (size_t)n;
		for (ULONG i = 0; i < n; i++)
			if (in32(&ids))
				((void **)d)[i] = in_iface(p, &iid);
		return psa;
	}
	case SF_VARIANT: {
		*p += 4 * (size_t)n;
		for (ULONG i = 0; i < n; i++)
			in_variant(p, &((VARIANT *)d)[i], depth + 1);
		return psa;
	}
	}
	size_t es = sf_size(sf);
	*p = in_align(*p, es);
	memcpy(d, *p, (size_t)n * es);
	*p += (size_t)n * es;
	return psa;
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
	const BYTE *p = in_align(buf, 4);
	BSTR b = in_bstr(&p);

	oa_SysFreeString(*pb);
	*pb = b;
	return (unsigned char *)p;
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
	const BYTE *p = buf;

	in_variant(&p, v, 0);
	return (unsigned char *)p;
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
	const BYTE *p = buf;
	SAFEARRAY *psa = in32(&p) ? in_array(&p, 0) : NULL;

	if (*ppsa)
		oa_SafeArrayDestroy(*ppsa);
	*ppsa = psa;
	return (unsigned char *)p;
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
