/*
 * SAFEARRAYs, laid out as Windows lays them out (after Wine's
 * safearray.c): the bounds are stored last dimension first
 * (rgsabound[0] is the rightmost), elements run with the leftmost index
 * varying fastest, and 16 hidden bytes before each descriptor hold its
 * element type (FADF_HAVEVARTYPE), interface IID (FADF_HAVEIID) or
 * IRecordInfo (FADF_RECORD). Elements that own something (BSTRs,
 * VARIANTs, interfaces, records) are copied and freed as such.
 */
#include <stddef.h>
#include <string.h>

#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/oleaut.h"

#define HIDDEN sizeof(GUID)

/* Features that don't carry over to a copy. */
#define NOT_COPIED (FADF_AUTO | FADF_STATIC | FADF_EMBEDDED | FADF_FIXEDSIZE)

static void *mem_alloc(size_t n)
{
	return heap_alloc(heap_process(), HEAP_ZERO_MEMORY, n ? n : 1);
}

static void mem_free(void *p)
{
	if (p)
		heap_free(heap_process(), 0, p);
}

static DWORD *hidden_dword(SAFEARRAY *psa)
{
	return (DWORD *)psa - 1;
}

static GUID *hidden_iid(SAFEARRAY *psa)
{
	return (GUID *)((BYTE *)psa - HIDDEN);
}

static IRecordInfo **hidden_record(SAFEARRAY *psa)
{
	return (IRecordInfo **)psa - 1;
}

/* An element's size for vt, or 0 if a SAFEARRAY can't hold it. */
static ULONG vt_size(VARTYPE vt)
{
	switch (vt) {
	case VT_I1: case VT_UI1:
		return 1;
	case VT_BOOL: case VT_I2: case VT_UI2:
		return 2;
	case VT_I4: case VT_UI4: case VT_R4: case VT_ERROR: case VT_INT:
	case VT_UINT:
		return 4;
	case VT_R8: case VT_CY: case VT_DATE: case VT_I8: case VT_UI8:
		return 8;
	case VT_BSTR: case VT_DISPATCH: case VT_UNKNOWN:
		return sizeof(void *);
	case VT_VARIANT:
		return sizeof(VARIANT);
	case VT_DECIMAL:
		return sizeof(DECIMAL);
	}
	return 0;
}

static ULONG cell_count(const SAFEARRAY *psa)
{
	ULONG n = 1;

	for (USHORT i = 0; i < psa->cDims; i++)
		n *= psa->rgsabound[i].cElements;
	return psa->cDims ? n : 0;
}

/* Frees what each of n elements at p owns, leaving them zeroed. */
static void clear_cells(SAFEARRAY *psa, BYTE *p, ULONG n)
{
	for (ULONG i = 0; i < n; i++, p += psa->cbElements) {
		if (psa->fFeatures & FADF_BSTR) {
			oa_SysFreeString(*(BSTR *)p);
		} else if (psa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
			IUnknown *u = *(IUnknown **)p;
			if (u)
				u->lpVtbl->Release(u);
		} else if (psa->fFeatures & FADF_VARIANT) {
			oa_VariantClear((VARIANT *)p);
		} else if (psa->fFeatures & FADF_RECORD) {
			IRecordInfo *ri = *hidden_record(psa);
			if (ri)
				ri->lpVtbl->RecordClear(ri, p);
		}
	}
}

/* Copies one element as its type says; dst holds nothing yet. */
static HRESULT copy_cell(SAFEARRAY *psa, const BYTE *src, BYTE *dst)
{
	if (psa->fFeatures & FADF_BSTR) {
		BSTR b = *(const BSTR *)src;
		*(BSTR *)dst = b ? oa_SysAllocStringByteLen(
			(LPCSTR)b, oa_SysStringByteLen(b)) : NULL;
		return b && !*(BSTR *)dst ? E_OUTOFMEMORY : S_OK;
	}
	if (psa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
		IUnknown *u = *(IUnknown *const *)src;
		if (u)
			u->lpVtbl->AddRef(u);
		*(IUnknown **)dst = u;
		return S_OK;
	}
	if (psa->fFeatures & FADF_VARIANT) {
		oa_VariantInit((VARIANT *)dst);
		return oa_VariantCopy((VARIANT *)dst, (const VARIANT *)src);
	}
	if (psa->fFeatures & FADF_RECORD) {
		IRecordInfo *ri = *hidden_record(psa);
		return ri ? ri->lpVtbl->RecordCopy(ri, (void *)src, dst) :
			    E_INVALIDARG;
	}
	memcpy(dst, src, psa->cbElements);
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayAllocDescriptor(UINT dims, SAFEARRAY **out)
{
	if (!out)
		return E_POINTER;
	if (!dims || dims > 0xffff)
		return E_INVALIDARG;
	BYTE *p = mem_alloc(HIDDEN + offsetof(SAFEARRAY, rgsabound) +
			    dims * sizeof(SAFEARRAYBOUND));
	if (!p)
		return E_UNEXPECTED;
	*out = (SAFEARRAY *)(p + HIDDEN);
	(*out)->cDims = (USHORT)dims;
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayAllocDescriptorEx(VARTYPE vt, UINT dims,
					     SAFEARRAY **out)
{
	ULONG size = vt == VT_RECORD ? 0 : vt_size(vt);

	if (!size && vt != VT_RECORD)
		return E_INVALIDARG;
	HRESULT hr = oa_SafeArrayAllocDescriptor(dims, out);
	if (FAILED(hr))
		return hr;
	SAFEARRAY *psa = *out;
	switch (vt) {
	case VT_DISPATCH:
		psa->fFeatures = FADF_HAVEIID;
		*hidden_iid(psa) = oa_IID_IDispatch;
		break;
	case VT_UNKNOWN:
		psa->fFeatures = FADF_HAVEIID;
		*hidden_iid(psa) = oa_IID_IUnknown;
		break;
	case VT_RECORD:
		psa->fFeatures = FADF_RECORD;
		break;
	default:
		psa->fFeatures = FADF_HAVEVARTYPE;
		*hidden_dword(psa) = vt;
		break;
	}
	psa->cbElements = size;
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayAllocData(SAFEARRAY *psa)
{
	if (!psa)
		return E_INVALIDARG;
	ULONG n = cell_count(psa);
	if (psa->cbElements && n > 0xffffffffu / psa->cbElements)
		return E_OUTOFMEMORY;
	if (!(psa->pvData = mem_alloc((size_t)n * psa->cbElements)))
		return E_OUTOFMEMORY;
	return S_OK;
}

/* The features an element type brings. */
static USHORT type_features(VARTYPE vt)
{
	switch (vt) {
	case VT_BSTR:     return FADF_BSTR;
	case VT_UNKNOWN:  return FADF_UNKNOWN;
	case VT_DISPATCH: return FADF_DISPATCH;
	case VT_VARIANT:  return FADF_VARIANT;
	}
	return 0;
}

static SAFEARRAY *create(VARTYPE vt, UINT dims, const SAFEARRAYBOUND *bounds,
			 IRecordInfo *ri)
{
	SAFEARRAY *psa;

	if (!bounds || (vt == VT_RECORD && !ri))
		return NULL;
	if (FAILED(oa_SafeArrayAllocDescriptorEx(vt, dims, &psa)))
		return NULL;
	psa->fFeatures |= type_features(vt);
	if (vt == VT_RECORD) {
		ULONG size = 0;
		ri->lpVtbl->GetSize(ri, &size);
		psa->cbElements = size;
		ri->lpVtbl->AddRef(ri);
		*hidden_record(psa) = ri;
	}
	for (UINT i = 0; i < dims; i++)
		psa->rgsabound[i] = bounds[dims - 1 - i];
	if (FAILED(oa_SafeArrayAllocData(psa))) {
		if (ri && vt == VT_RECORD)
			ri->lpVtbl->Release(ri);
		mem_free((BYTE *)psa - HIDDEN);
		return NULL;
	}
	return psa;
}

WINFN(SAFEARRAY *) oa_SafeArrayCreate(VARTYPE vt, UINT dims,
				      SAFEARRAYBOUND *bounds)
{
	return vt == VT_RECORD ? NULL : create(vt, dims, bounds, NULL);
}

/* pvExtra: the IRecordInfo for VT_RECORD, an IID for interfaces. */
WINFN(SAFEARRAY *) oa_SafeArrayCreateEx(VARTYPE vt, UINT dims,
					SAFEARRAYBOUND *bounds, void *extra)
{
	SAFEARRAY *psa = create(vt, dims, bounds,
				vt == VT_RECORD ? extra : NULL);

	if (psa && extra && (vt == VT_UNKNOWN || vt == VT_DISPATCH))
		*hidden_iid(psa) = *(const GUID *)extra;
	return psa;
}

WINFN(SAFEARRAY *) oa_SafeArrayCreateVector(VARTYPE vt, LONG lbound,
					    ULONG count)
{
	SAFEARRAYBOUND b = { count, lbound };

	return vt == VT_RECORD ? NULL : create(vt, 1, &b, NULL);
}

WINFN(SAFEARRAY *) oa_SafeArrayCreateVectorEx(VARTYPE vt, LONG lbound,
					      ULONG count, void *extra)
{
	SAFEARRAYBOUND b = { count, lbound };

	return oa_SafeArrayCreateEx(vt, 1, &b, extra);
}

WINFN(HRESULT) oa_SafeArrayDestroyData(SAFEARRAY *psa)
{
	if (!psa)
		return E_INVALIDARG;
	if (psa->cLocks)
		return DISP_E_ARRAYISLOCKED;
	if (!psa->pvData)
		return S_OK;
	ULONG n = cell_count(psa);
	clear_cells(psa, psa->pvData, n);
	if (psa->fFeatures & (FADF_STATIC | FADF_EMBEDDED)) {
		memset(psa->pvData, 0, (size_t)n * psa->cbElements);
		return S_OK;
	}
	mem_free(psa->pvData);
	psa->pvData = NULL;
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayDestroyDescriptor(SAFEARRAY *psa)
{
	if (!psa)
		return S_OK;
	if (psa->cLocks)
		return DISP_E_ARRAYISLOCKED;
	if (psa->fFeatures & FADF_RECORD) {
		IRecordInfo *ri = *hidden_record(psa);
		if (ri)
			ri->lpVtbl->Release(ri);
	}
	mem_free((BYTE *)psa - HIDDEN);
	return S_OK;
}

HRESULT STUB_WINAPI oa_SafeArrayDestroy(SAFEARRAY *psa)
{
	if (!psa)
		return S_OK;
	if (psa->cLocks)
		return DISP_E_ARRAYISLOCKED;
	oa_SafeArrayDestroyData(psa);
	return oa_SafeArrayDestroyDescriptor(psa);
}

WINFN(UINT) oa_SafeArrayGetDim(SAFEARRAY *psa)
{
	return psa ? psa->cDims : 0;
}

WINFN(UINT) oa_SafeArrayGetElemsize(SAFEARRAY *psa)
{
	return psa ? psa->cbElements : 0;
}

/* Dimension dim, counted from 1 on the left. */
static SAFEARRAYBOUND *bound(SAFEARRAY *psa, UINT dim)
{
	return &psa->rgsabound[psa->cDims - dim];
}

WINFN(HRESULT) oa_SafeArrayGetUBound(SAFEARRAY *psa, UINT dim, LONG *out)
{
	if (!psa || !out)
		return E_INVALIDARG;
	if (!dim || dim > psa->cDims)
		return DISP_E_BADINDEX;
	SAFEARRAYBOUND *b = bound(psa, dim);
	*out = b->lLbound + (LONG)b->cElements - 1;
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayGetLBound(SAFEARRAY *psa, UINT dim, LONG *out)
{
	if (!psa || !out)
		return E_INVALIDARG;
	if (!dim || dim > psa->cDims)
		return DISP_E_BADINDEX;
	*out = bound(psa, dim)->lLbound;
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayLock(SAFEARRAY *psa)
{
	if (!psa)
		return E_INVALIDARG;
	if (__atomic_add_fetch(&psa->cLocks, 1, __ATOMIC_ACQ_REL) > 0xffff) {
		__atomic_sub_fetch(&psa->cLocks, 1, __ATOMIC_ACQ_REL);
		return E_UNEXPECTED;
	}
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayUnlock(SAFEARRAY *psa)
{
	if (!psa)
		return E_INVALIDARG;
	ULONG n = psa->cLocks;
	do {
		if (!n)
			return E_UNEXPECTED;
	} while (!__atomic_compare_exchange_n(&psa->cLocks, &n, n - 1, 0,
					      __ATOMIC_ACQ_REL,
					      __ATOMIC_ACQUIRE));
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayAccessData(SAFEARRAY *psa, void **out)
{
	if (!psa || !out)
		return E_INVALIDARG;
	HRESULT hr = oa_SafeArrayLock(psa);
	*out = SUCCEEDED(hr) ? psa->pvData : NULL;
	return hr;
}

WINFN(HRESULT) oa_SafeArrayUnaccessData(SAFEARRAY *psa)
{
	return oa_SafeArrayUnlock(psa);
}

/* The element at indices (leftmost first), or NULL with *hr. */
static BYTE *cell(SAFEARRAY *psa, const LONG *idx, HRESULT *hr)
{
	ULONG offset = 0, stride = 1;

	for (USHORT d = 0; d < psa->cDims; d++) {
		const SAFEARRAYBOUND *b = &psa->rgsabound[psa->cDims - 1 - d];
		LONG i = idx[d] - b->lLbound;
		if (idx[d] < b->lLbound || (ULONG)i >= b->cElements) {
			*hr = DISP_E_BADINDEX;
			return NULL;
		}
		offset += (ULONG)i * stride;
		stride *= b->cElements;
	}
	*hr = S_OK;
	return (BYTE *)psa->pvData + (size_t)offset * psa->cbElements;
}

WINFN(HRESULT) oa_SafeArrayPtrOfIndex(SAFEARRAY *psa, LONG *idx, void **out)
{
	HRESULT hr;

	if (!psa || !idx || !out)
		return E_INVALIDARG;
	BYTE *p = cell(psa, idx, &hr);
	if (p)
		*out = p;
	return hr;
}

WINFN(HRESULT) oa_SafeArrayGetElement(SAFEARRAY *psa, LONG *idx, void *out)
{
	HRESULT hr;

	if (!psa || !idx || !out)
		return E_INVALIDARG;
	if (FAILED(hr = oa_SafeArrayLock(psa)))
		return hr;
	BYTE *p = cell(psa, idx, &hr);
	if (p)
		hr = copy_cell(psa, p, out);
	oa_SafeArrayUnlock(psa);
	return hr;
}

/*
 * The value to store: for BSTRs the BSTR itself, for interfaces the
 * pointer itself, otherwise a pointer to the value (a VARIANT, a record).
 */
WINFN(HRESULT) oa_SafeArrayPutElement(SAFEARRAY *psa, LONG *idx, void *value)
{
	HRESULT hr;

	if (!psa || !idx)
		return E_INVALIDARG;
	if (!value && !(psa->fFeatures & (FADF_BSTR | FADF_UNKNOWN |
					   FADF_DISPATCH)))
		return E_INVALIDARG;
	if (FAILED(hr = oa_SafeArrayLock(psa)))
		return hr;
	BYTE *p = cell(psa, idx, &hr);
	if (p) {
		if (psa->fFeatures & FADF_BSTR) {
			BSTR b = value ? oa_SysAllocStringByteLen(
				value, oa_SysStringByteLen(value)) : NULL;
			if (value && !b) {
				hr = E_OUTOFMEMORY;
			} else {
				oa_SysFreeString(*(BSTR *)p);
				*(BSTR *)p = b;
			}
		} else if (psa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
			IUnknown *u = value, *old = *(IUnknown **)p;
			if (u)
				u->lpVtbl->AddRef(u);
			*(IUnknown **)p = u;
			if (old)
				old->lpVtbl->Release(old);
		} else if (psa->fFeatures & FADF_VARIANT) {
			hr = oa_VariantCopy((VARIANT *)p, value);
		} else if (psa->fFeatures & FADF_RECORD) {
			IRecordInfo *ri = *hidden_record(psa);
			hr = ri ? ri->lpVtbl->RecordCopy(ri, value, p) :
				  E_INVALIDARG;
		} else {
			memcpy(p, value, psa->cbElements);
		}
	}
	oa_SafeArrayUnlock(psa);
	return hr;
}

static int same_shape(const SAFEARRAY *a, const SAFEARRAY *b)
{
	if (a->cDims != b->cDims || a->cbElements != b->cbElements)
		return 0;
	for (USHORT i = 0; i < a->cDims; i++)
		if (a->rgsabound[i].cElements != b->rgsabound[i].cElements)
			return 0;
	return 1;
}

WINFN(HRESULT) oa_SafeArrayCopyData(SAFEARRAY *src, SAFEARRAY *dst)
{
	if (!src || !dst || !same_shape(src, dst) || !src->pvData ||
	    !dst->pvData)
		return E_INVALIDARG;
	ULONG n = cell_count(src);
	clear_cells(dst, dst->pvData, n);
	for (ULONG i = 0; i < n; i++) {
		HRESULT hr = copy_cell(src,
				       (BYTE *)src->pvData + (size_t)i * src->cbElements,
				       (BYTE *)dst->pvData + (size_t)i * dst->cbElements);
		if (FAILED(hr))
			return hr;
	}
	return S_OK;
}

HRESULT STUB_WINAPI oa_SafeArrayCopy(SAFEARRAY *psa, SAFEARRAY **out)
{
	SAFEARRAY *c;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!psa)
		return S_OK;
	HRESULT hr = oa_SafeArrayAllocDescriptor(psa->cDims, &c);
	if (FAILED(hr))
		return hr;
	memcpy((BYTE *)c - HIDDEN, (BYTE *)psa - HIDDEN, HIDDEN);
	c->fFeatures = psa->fFeatures & ~NOT_COPIED;
	c->cbElements = psa->cbElements;
	memcpy(c->rgsabound, psa->rgsabound,
	       psa->cDims * sizeof(SAFEARRAYBOUND));
	if (c->fFeatures & FADF_RECORD) {
		IRecordInfo *ri = *hidden_record(c);
		if (ri)
			ri->lpVtbl->AddRef(ri);
	}
	if (psa->pvData) {
		if (SUCCEEDED(hr = oa_SafeArrayAllocData(c)))
			hr = oa_SafeArrayCopyData(psa, c);
		if (FAILED(hr)) {
			oa_SafeArrayDestroy(c);
			return hr;
		}
	}
	*out = c;
	return S_OK;
}

/* Only the rightmost dimension can change. */
WINFN(HRESULT) oa_SafeArrayRedim(SAFEARRAY *psa, SAFEARRAYBOUND *nb)
{
	if (!psa || !nb)
		return E_INVALIDARG;
	if (psa->cLocks || (psa->fFeatures & FADF_FIXEDSIZE))
		return DISP_E_ARRAYISLOCKED;
	/* The rightmost index varies slowest: its elements come last. */
	ULONG per = 1;
	for (USHORT i = 1; i < psa->cDims; i++)
		per *= psa->rgsabound[i].cElements;
	ULONG old_n = per * psa->rgsabound[0].cElements;
	if (nb->cElements && per > 0xffffffffu / nb->cElements)
		return E_OUTOFMEMORY;
	ULONG new_n = per * nb->cElements;
	if (psa->cbElements && new_n > 0xffffffffu / psa->cbElements)
		return E_OUTOFMEMORY;
	if (new_n < old_n && psa->pvData)
		clear_cells(psa, (BYTE *)psa->pvData +
			    (size_t)new_n * psa->cbElements, old_n - new_n);
	if (psa->pvData || new_n) {
		BYTE *p = mem_alloc((size_t)new_n * psa->cbElements);
		if (!p)
			return E_OUTOFMEMORY;
		if (psa->pvData) {
			memcpy(p, psa->pvData, (size_t)(new_n < old_n ? new_n :
							 old_n) * psa->cbElements);
			mem_free(psa->pvData);
		}
		psa->pvData = p;
	}
	psa->rgsabound[0] = *nb;
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayGetVartype(SAFEARRAY *psa, VARTYPE *out)
{
	if (!psa || !out)
		return E_INVALIDARG;
	if (psa->fFeatures & FADF_RECORD)
		*out = VT_RECORD;
	else if ((psa->fFeatures & (FADF_HAVEIID | FADF_DISPATCH)) ==
		 (FADF_HAVEIID | FADF_DISPATCH))
		*out = VT_DISPATCH;
	else if (psa->fFeatures & FADF_HAVEIID)
		*out = VT_UNKNOWN;
	else if (psa->fFeatures & FADF_HAVEVARTYPE)
		*out = (VARTYPE)*hidden_dword(psa);
	else
		return E_INVALIDARG;
	return S_OK;
}

WINFN(HRESULT) oa_SafeArraySetIID(SAFEARRAY *psa, REFGUID iid)
{
	if (!psa || !iid || !(psa->fFeatures & FADF_HAVEIID))
		return E_INVALIDARG;
	*hidden_iid(psa) = *iid;
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayGetIID(SAFEARRAY *psa, GUID *out)
{
	if (!psa || !out || !(psa->fFeatures & FADF_HAVEIID))
		return E_INVALIDARG;
	*out = *hidden_iid(psa);
	return S_OK;
}

WINFN(HRESULT) oa_SafeArraySetRecordInfo(SAFEARRAY *psa, IRecordInfo *ri)
{
	if (!psa || !(psa->fFeatures & FADF_RECORD))
		return E_INVALIDARG;
	IRecordInfo *old = *hidden_record(psa);
	if (ri)
		ri->lpVtbl->AddRef(ri);
	*hidden_record(psa) = ri;
	if (old)
		old->lpVtbl->Release(old);
	return S_OK;
}

WINFN(HRESULT) oa_SafeArrayGetRecordInfo(SAFEARRAY *psa, IRecordInfo **out)
{
	if (!psa || !out || !(psa->fFeatures & FADF_RECORD))
		return E_INVALIDARG;
	*out = *hidden_record(psa);
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return S_OK;
}

/* A BSTR's bytes as a vector of VT_UI1, and back. */
HRESULT STUB_WINAPI oa_VectorFromBstr(BSTR b, SAFEARRAY **out)
{
	if (!out)
		return E_INVALIDARG;
	UINT n = oa_SysStringByteLen(b);
	SAFEARRAY *psa = oa_SafeArrayCreateVector(VT_UI1, 0, n);
	if (!(*out = psa))
		return E_OUTOFMEMORY;
	if (n)
		memcpy(psa->pvData, b, n);
	return S_OK;
}

HRESULT STUB_WINAPI oa_BstrFromVector(SAFEARRAY *psa, BSTR *out)
{
	VARTYPE vt;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!psa || psa->cDims != 1 || FAILED(oa_SafeArrayGetVartype(psa, &vt)) ||
	    vt != VT_UI1)
		return E_INVALIDARG;
	if (!(*out = oa_SysAllocStringByteLen(psa->pvData,
					      psa->rgsabound[0].cElements)))
		return E_OUTOFMEMORY;
	return S_OK;
}

#define E(name, ord) { #name, ord, (void *)oa_##name }

const struct builtin_export oa_array_exports[] = {
	E(SafeArrayCreate, 15),
	E(SafeArrayDestroy, 16),
	E(SafeArrayGetDim, 17),
	E(SafeArrayGetElemsize, 18),
	E(SafeArrayGetUBound, 19),
	E(SafeArrayGetLBound, 20),
	E(SafeArrayLock, 21),
	E(SafeArrayUnlock, 22),
	E(SafeArrayAccessData, 23),
	E(SafeArrayUnaccessData, 24),
	E(SafeArrayGetElement, 25),
	E(SafeArrayPutElement, 26),
	E(SafeArrayCopy, 27),
	E(SafeArrayAllocDescriptor, 36),
	E(SafeArrayAllocData, 37),
	E(SafeArrayDestroyDescriptor, 38),
	E(SafeArrayDestroyData, 39),
	E(SafeArrayRedim, 40),
	E(SafeArrayAllocDescriptorEx, 41),
	E(SafeArrayCreateEx, 42),
	E(SafeArrayCreateVectorEx, 43),
	E(SafeArraySetRecordInfo, 44),
	E(SafeArrayGetRecordInfo, 45),
	E(SafeArraySetIID, 57),
	E(SafeArrayGetIID, 67),
	E(SafeArrayGetVartype, 77),
	E(SafeArrayPtrOfIndex, 148),
	E(SafeArrayCreateVector, 411),
	E(SafeArrayCopyData, 412),
	E(VectorFromBstr, 413),
	E(BstrFromVector, 414),
	BUILTIN_END
};
