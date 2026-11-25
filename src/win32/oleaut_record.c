/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's IRecordInfo: a user-defined record type (TKIND_RECORD) as
 * GetRecordInfoFromTypeInfo and GetRecordInfoFromGuids give it, which is
 * what a VARIANT of VT_RECORD, a SAFEARRAY of records and Invoke use to
 * size, initialise, copy and free the record's data.
 *
 * Its fields are its type info's per-instance variables, each at its
 * oInst. A field that owns something (a BSTR, an interface, a VARIANT, a
 * SAFEARRAY, a nested record) is freed and copied as such; any other is
 * plain bytes. GetField and PutField convert through a VARIANT of the
 * field's type; fixed arrays and pointers are only bytes to them
 * (DISP_E_BADVARTYPE).
 */
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "win32/internal.h"
#include "win32/ole.h"
#include "win32/oleaut.h"

#define RECORD_DEPTH 16

struct field {
	BSTR name;
	ULONG off;
	VARTYPE vt;		/* VT_EMPTY: bytes only */
	ULONG size;
	IRecordInfo *rec;	/* VT_RECORD's */
};

struct record {
	IRecordInfo iface;
	LONG refs;
	ITypeInfo *ti;
	GUID guid;
	BSTR name;
	ULONG size;
	UINT nfields;
	struct field *fields;
};

static const IRecordInfoVtbl record_vtbl;

static struct record *rec_of(IRecordInfo *i)
{
	return (struct record *)i;
}

static HRESULT record_new(ITypeInfo *ti, int depth, IRecordInfo **out);
static ULONG vt_storage(const struct field *f);

/* The storage type of a field of type td, its size, and a record's info. */
static HRESULT field_type(ITypeInfo *ti, const TYPEDESC *td, struct field *f,
			  int depth)
{
	ITypeInfo *r;
	TYPEATTR *a;
	HRESULT hr;

	switch (td->vt) {
	case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_BOOL:
	case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4:
	case VT_ERROR: case VT_HRESULT: case VT_I8: case VT_UI8: case VT_R8:
	case VT_CY: case VT_DATE: case VT_BSTR: case VT_UNKNOWN:
	case VT_DISPATCH: case VT_VARIANT: case VT_DECIMAL:
		f->vt = td->vt == VT_HRESULT ? VT_ERROR : td->vt;
		return S_OK;
	case VT_INT_PTR:
		f->vt = sizeof(void *) == 8 ? VT_I8 : VT_I4;
		return S_OK;
	case VT_UINT_PTR:
		f->vt = sizeof(void *) == 8 ? VT_UI8 : VT_UI4;
		return S_OK;
	case VT_SAFEARRAY: {
		struct field e = { 0 };
		if (!td->lptdesc ||
		    FAILED(field_type(ti, td->lptdesc, &e, depth + 1)) ||
		    e.vt == VT_EMPTY || e.vt == VT_RECORD) {
			if (e.rec)
				e.rec->lpVtbl->Release(e.rec);
			f->vt = VT_EMPTY;	/* (only bytes, then) */
			return S_OK;
		}
		f->vt = VT_ARRAY | e.vt;
		return S_OK;
	}
	case VT_USERDEFINED:
		break;
	default:
		f->vt = VT_EMPTY;
		return S_OK;
	}
	if (depth > RECORD_DEPTH)
		return TYPE_E_CIRCULARTYPE;
	if (FAILED(hr = ti->lpVtbl->GetRefTypeInfo(ti, td->hreftype, &r)))
		return hr;
	if (SUCCEEDED(hr = r->lpVtbl->GetTypeAttr(r, &a))) {
		switch (a->typekind) {
		case TKIND_ENUM:
			f->vt = VT_I4;
			break;
		case TKIND_ALIAS:
			hr = field_type(r, &a->tdescAlias, f, depth + 1);
			break;
		case TKIND_INTERFACE:
		case TKIND_COCLASS:
			f->vt = VT_UNKNOWN;
			break;
		case TKIND_DISPATCH:
			f->vt = VT_DISPATCH;
			break;
		case TKIND_RECORD:
			f->vt = VT_RECORD;
			hr = record_new(r, depth + 1, &f->rec);
			break;
		default:
			f->vt = VT_EMPTY;
			break;
		}
		r->lpVtbl->ReleaseTypeAttr(r, a);
	}
	r->lpVtbl->Release(r);
	return hr;
}

static void record_free(struct record *rec)
{
	for (UINT i = 0; i < rec->nfields; i++) {
		oa_SysFreeString(rec->fields[i].name);
		if (rec->fields[i].rec)
			rec->fields[i].rec->lpVtbl->Release(rec->fields[i].rec);
	}
	free(rec->fields);
	oa_SysFreeString(rec->name);
	if (rec->ti)
		rec->ti->lpVtbl->Release(rec->ti);
	free(rec);
}

static HRESULT record_new(ITypeInfo *ti, int depth, IRecordInfo **out)
{
	TYPEATTR *a;
	HRESULT hr;

	*out = NULL;
	if (depth > RECORD_DEPTH)
		return TYPE_E_CIRCULARTYPE;
	if (FAILED(hr = ti->lpVtbl->GetTypeAttr(ti, &a)))
		return hr;
	if (a->typekind == TKIND_ALIAS &&
	    a->tdescAlias.vt == VT_USERDEFINED) {
		ITypeInfo *r;
		HREFTYPE h = a->tdescAlias.hreftype;
		ti->lpVtbl->ReleaseTypeAttr(ti, a);
		if (FAILED(hr = ti->lpVtbl->GetRefTypeInfo(ti, h, &r)))
			return hr;
		hr = record_new(r, depth + 1, out);
		r->lpVtbl->Release(r);
		return hr;
	}
	if (a->typekind != TKIND_RECORD) {
		ti->lpVtbl->ReleaseTypeAttr(ti, a);
		return E_INVALIDARG;
	}
	struct record *rec = calloc(1, sizeof(*rec));
	if (!rec || (a->cVars &&
		     !(rec->fields = calloc(a->cVars, sizeof(*rec->fields))))) {
		free(rec);
		ti->lpVtbl->ReleaseTypeAttr(ti, a);
		return E_OUTOFMEMORY;
	}
	rec->iface.lpVtbl = &record_vtbl;
	rec->refs = 1;
	rec->ti = ti;
	ti->lpVtbl->AddRef(ti);
	rec->guid = a->guid;
	rec->size = a->cbSizeInstance;
	UINT nvars = a->cVars;
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	ti->lpVtbl->GetDocumentation(ti, MEMBERID_NIL, &rec->name, NULL, NULL,
				     NULL);

	for (UINT i = 0; i < nvars && SUCCEEDED(hr); i++) {
		VARDESC *v;
		if (FAILED(hr = ti->lpVtbl->GetVarDesc(ti, i, &v)))
			break;
		struct field *f = &rec->fields[rec->nfields];
		if (v->varkind == VAR_PERINSTANCE) {
			UINT got = 0;
			f->off = v->oInst;
			ti->lpVtbl->GetNames(ti, v->memid, &f->name, 1, &got);
			hr = field_type(ti, &v->elemdescVar.tdesc, f,
					depth);
			rec->nfields++;
		}
		ti->lpVtbl->ReleaseVarDesc(ti, v);
	}
	/* Each field's size: up to the next field, or the end. */
	for (UINT i = 0; i < rec->nfields && SUCCEEDED(hr); i++) {
		ULONG end = rec->size;
		for (UINT k = 0; k < rec->nfields; k++)
			if (rec->fields[k].off > rec->fields[i].off &&
			    rec->fields[k].off < end)
				end = rec->fields[k].off;
		if (rec->fields[i].off > rec->size) {
			hr = TYPE_E_SIZETOOBIG;
			break;
		}
		rec->fields[i].size = end - rec->fields[i].off;
		/*
		 * A field too small for its type (a record laid out for
		 * other pointers than this process's) can't be used.
		 */
		if (rec->fields[i].vt != VT_EMPTY &&
		    rec->fields[i].vt != VT_RECORD &&
		    vt_storage(&rec->fields[i]) > rec->fields[i].size) {
			TRACE("GetRecordInfoFromTypeInfo: field %u is too "
			      "small for its type", i);
			hr = E_INVALIDARG;
			break;
		}
	}
	if (FAILED(hr)) {
		record_free(rec);
		return hr;
	}
	*out = &rec->iface;
	return S_OK;
}

/* What the field at p owns, let go of. */
static void field_clear(const struct field *f, BYTE *p)
{
	if (f->vt & VT_ARRAY) {
		if (*(SAFEARRAY **)p)
			oa_SafeArrayDestroy(*(SAFEARRAY **)p);
		*(SAFEARRAY **)p = NULL;
		return;
	}
	switch (f->vt) {
	case VT_BSTR:
		oa_SysFreeString(*(BSTR *)p);
		*(BSTR *)p = NULL;
		break;
	case VT_UNKNOWN:
	case VT_DISPATCH:
		if (*(IUnknown **)p)
			(*(IUnknown **)p)->lpVtbl->Release(*(IUnknown **)p);
		*(IUnknown **)p = NULL;
		break;
	case VT_VARIANT:
		oa_VariantClear((VARIANT *)p);
		break;
	case VT_RECORD:
		f->rec->lpVtbl->RecordClear(f->rec, p);
		break;
	}
}

/* Copies the field from src to dst, whose old value is not looked at. */
static HRESULT field_copy(const struct field *f, BYTE *dst, const BYTE *src)
{
	if (f->vt & VT_ARRAY) {
		SAFEARRAY *s = *(SAFEARRAY *const *)src;
		*(SAFEARRAY **)dst = NULL;
		return s ? oa_SafeArrayCopy(s, (SAFEARRAY **)dst) : S_OK;
	}
	switch (f->vt) {
	case VT_BSTR: {
		BSTR s = *(const BSTR *)src;
		*(BSTR *)dst = NULL;
		if (s && !(*(BSTR *)dst = oa_SysAllocStringByteLen(
				   (LPCSTR)s, oa_SysStringByteLen(s))))
			return E_OUTOFMEMORY;
		return S_OK;
	}
	case VT_UNKNOWN:
	case VT_DISPATCH: {
		IUnknown *u = *(IUnknown *const *)src;
		if (u)
			u->lpVtbl->AddRef(u);
		*(IUnknown **)dst = u;
		return S_OK;
	}
	case VT_VARIANT:
		oa_VariantInit((VARIANT *)dst);
		return oa_VariantCopy((VARIANT *)dst, (const VARIANT *)src);
	case VT_RECORD:
		return f->rec->lpVtbl->RecordCopy(f->rec, (void *)src, dst);
	}
	memcpy(dst, src, vt_storage(f));
	return S_OK;
}

static const struct field *find_field(struct record *rec, LPCOLESTR name)
{
	if (!name)
		return NULL;
	for (UINT i = 0; i < rec->nfields; i++) {
		const WCHAR *a = rec->fields[i].name, *b = name;
		if (!a)
			continue;
		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == *b)
			return &rec->fields[i];
	}
	return NULL;
}

/* ---- IRecordInfo ---- */

WINFN(HRESULT) ri_QueryInterface(IRecordInfo *self, REFIID riid, void **ppv)
{
	static const IID iid_record = { 0x0000002f, 0, 0,
		{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

	if (!ppv)
		return E_POINTER;
	if (!memcmp(riid, &oa_IID_IUnknown, sizeof(IID)) ||
	    !memcmp(riid, &iid_record, sizeof(IID))) {
		self->lpVtbl->AddRef(self);
		*ppv = self;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) ri_AddRef(IRecordInfo *self)
{
	return (ULONG)__atomic_add_fetch(&rec_of(self)->refs, 1,
					 __ATOMIC_SEQ_CST);
}

WINFN(ULONG) ri_Release(IRecordInfo *self)
{
	LONG r = __atomic_sub_fetch(&rec_of(self)->refs, 1, __ATOMIC_SEQ_CST);

	if (!r)
		record_free(rec_of(self));
	return (ULONG)r;
}

WINFN(HRESULT) ri_RecordInit(IRecordInfo *self, PVOID pv)
{
	if (!pv)
		return E_INVALIDARG;
	memset(pv, 0, rec_of(self)->size);
	return S_OK;
}

WINFN(HRESULT) ri_RecordClear(IRecordInfo *self, PVOID pv)
{
	struct record *rec = rec_of(self);

	if (!pv)
		return E_INVALIDARG;
	for (UINT i = 0; i < rec->nfields; i++)
		field_clear(&rec->fields[i], (BYTE *)pv + rec->fields[i].off);
	return S_OK;
}

WINFN(HRESULT) ri_RecordCopy(IRecordInfo *self, PVOID from, PVOID to)
{
	struct record *rec = rec_of(self);
	HRESULT hr = S_OK;

	if (!from || !to)
		return E_INVALIDARG;
	if (from == to)
		return S_OK;
	/* The bytes between fields (padding) too. */
	memcpy(to, from, rec->size);
	for (UINT i = 0; i < rec->nfields; i++) {
		const struct field *f = &rec->fields[i];
		HRESULT r = field_copy(f, (BYTE *)to + f->off,
				       (const BYTE *)from + f->off);
		if (FAILED(r) && SUCCEEDED(hr))
			hr = r;
	}
	return hr;
}

WINFN(HRESULT) ri_GetGuid(IRecordInfo *self, GUID *guid)
{
	if (!guid)
		return E_INVALIDARG;
	*guid = rec_of(self)->guid;
	return S_OK;
}

WINFN(HRESULT) ri_GetName(IRecordInfo *self, BSTR *name)
{
	BSTR n = rec_of(self)->name;

	if (!name)
		return E_INVALIDARG;
	*name = n ? oa_SysAllocString(n) : NULL;
	return n && !*name ? E_OUTOFMEMORY : S_OK;
}

WINFN(HRESULT) ri_GetSize(IRecordInfo *self, ULONG *size)
{
	if (!size)
		return E_INVALIDARG;
	*size = rec_of(self)->size;
	return S_OK;
}

WINFN(HRESULT) ri_GetTypeInfo(IRecordInfo *self, ITypeInfo **ti)
{
	if (!ti)
		return E_INVALIDARG;
	*ti = rec_of(self)->ti;
	(*ti)->lpVtbl->AddRef(*ti);
	return S_OK;
}

WINFN(HRESULT) ri_GetFieldNoCopy(IRecordInfo *self, PVOID data,
				 LPCOLESTR name, VARIANT *out, PVOID *carray)
{
	const struct field *f = find_field(rec_of(self), name);

	if (!data || !out)
		return E_INVALIDARG;
	if (!f)
		return TYPE_E_FIELDNOTFOUND;
	BYTE *p = (BYTE *)data + f->off;
	if (carray)
		*carray = f->vt == VT_EMPTY ? p : NULL;
	if (f->vt == VT_EMPTY)
		return DISP_E_BADVARTYPE;
	memset(out, 0, sizeof(*out));
	if (f->vt == VT_RECORD) {
		out->vt = VT_RECORD | VT_BYREF;
		out->pvRecord = p;
		out->pRecInfo = f->rec;
		f->rec->lpVtbl->AddRef(f->rec);
		return S_OK;
	}
	out->vt = f->vt | VT_BYREF;
	out->byref = p;
	return S_OK;
}

WINFN(HRESULT) ri_GetField(IRecordInfo *self, PVOID data, LPCOLESTR name,
			   VARIANT *out)
{
	const struct field *f = find_field(rec_of(self), name);

	if (!data || !out)
		return E_INVALIDARG;
	if (!f)
		return TYPE_E_FIELDNOTFOUND;
	if (f->vt == VT_EMPTY)
		return DISP_E_BADVARTYPE;
	BYTE *p = (BYTE *)data + f->off;
	memset(out, 0, sizeof(*out));
	if (f->vt == VT_RECORD) {
		void *copy = f->rec->lpVtbl->RecordCreate(f->rec);
		if (!copy)
			return E_OUTOFMEMORY;
		HRESULT hr = f->rec->lpVtbl->RecordCopy(f->rec, p, copy);
		if (FAILED(hr)) {
			f->rec->lpVtbl->RecordDestroy(f->rec, copy);
			return hr;
		}
		out->vt = VT_RECORD;
		out->pvRecord = copy;
		out->pRecInfo = f->rec;
		f->rec->lpVtbl->AddRef(f->rec);
		return S_OK;
	}
	if (f->vt == VT_VARIANT)
		return oa_VariantCopy(out, (VARIANT *)p);
	HRESULT hr = field_copy(f, f->vt == VT_DECIMAL ? (BYTE *)&out->decVal :
				   (BYTE *)&out->llVal, p);
	if (SUCCEEDED(hr))
		out->vt = f->vt;
	return hr;
}

/* The bytes a field of type vt takes (its value, not its padding). */
static ULONG vt_storage(const struct field *f)
{
	if (f->vt & VT_ARRAY)
		return sizeof(SAFEARRAY *);
	switch (f->vt) {
	case VT_I1: case VT_UI1:
		return 1;
	case VT_I2: case VT_UI2: case VT_BOOL:
		return 2;
	case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4:
	case VT_ERROR:
		return 4;
	case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
		return 8;
	case VT_DECIMAL:
		return sizeof(DECIMAL);
	case VT_VARIANT:
		return sizeof(VARIANT);
	case VT_BSTR: case VT_UNKNOWN: case VT_DISPATCH:
		return sizeof(void *);
	}
	return f->size;
}

static HRESULT put_field(IRecordInfo *self, PVOID data, LPCOLESTR name,
			 VARIANT *v, int nocopy)
{
	const struct field *f = find_field(rec_of(self), name);
	VARIANT tmp;
	HRESULT hr;

	if (!data || !v)
		return E_INVALIDARG;
	if (!f)
		return TYPE_E_FIELDNOTFOUND;
	if (f->vt == VT_EMPTY)
		return DISP_E_BADVARTYPE;
	BYTE *p = (BYTE *)data + f->off;
	if (f->vt == VT_RECORD) {
		VARIANT *in = v->vt == (VT_BYREF | VT_VARIANT) ? v->pvarVal : v;
		if (!in || (in->vt & ~VT_BYREF) != VT_RECORD ||
		    !in->pRecInfo || !in->pvRecord ||
		    !f->rec->lpVtbl->IsMatchingType(f->rec, in->pRecInfo))
			return DISP_E_TYPEMISMATCH;
		void *copy = f->rec->lpVtbl->RecordCreate(f->rec);
		if (!copy)
			return E_OUTOFMEMORY;
		if (FAILED(hr = f->rec->lpVtbl->RecordCopy(f->rec,
							   in->pvRecord,
							   copy))) {
			f->rec->lpVtbl->RecordDestroy(f->rec, copy);
			return hr;
		}
		ULONG size = 0;
		f->rec->lpVtbl->GetSize(f->rec, &size);
		f->rec->lpVtbl->RecordClear(f->rec, p);
		memcpy(p, copy, size);
		ole_taskmem_free(copy);	/* (its contents moved) */
		return S_OK;
	}
	if (nocopy && v->vt == f->vt) {
		/* The caller hands its value over, as it is. */
		field_clear(f, p);
		memcpy(p, f->vt == VT_VARIANT ? (void *)v :
		       f->vt == VT_DECIMAL ? (void *)&v->decVal :
		       (void *)&v->llVal, vt_storage(f));
		if (f->vt == VT_DECIMAL)
			((DECIMAL *)p)->wReserved = 0;
		return S_OK;
	}
	oa_VariantInit(&tmp);
	if (f->vt == VT_VARIANT)
		hr = oa_VariantCopyInd(&tmp, v);
	else
		hr = oa_VariantChangeTypeEx(&tmp, v, LOCALE_USER_DEFAULT, 0,
					    f->vt);
	if (FAILED(hr))
		return hr;
	/* tmp's value is a new one: it moves into the field. */
	field_clear(f, p);
	memcpy(p, f->vt == VT_VARIANT ? (void *)&tmp :
	       f->vt == VT_DECIMAL ? (void *)&tmp.decVal :
	       (void *)&tmp.llVal, vt_storage(f));
	if (f->vt == VT_DECIMAL)
		((DECIMAL *)p)->wReserved = 0;
	return S_OK;
}

WINFN(HRESULT) ri_PutField(IRecordInfo *self, ULONG flags, PVOID data,
			   LPCOLESTR name, VARIANT *v)
{
	return put_field(self, data, name, v, 0);
}

WINFN(HRESULT) ri_PutFieldNoCopy(IRecordInfo *self, ULONG flags, PVOID data,
				 LPCOLESTR name, VARIANT *v)
{
	return put_field(self, data, name, v, 1);
}

WINFN(HRESULT) ri_GetFieldNames(IRecordInfo *self, ULONG *count,
				BSTR *names)
{
	struct record *rec = rec_of(self);

	if (!count)
		return E_INVALIDARG;
	if (!names) {
		*count = rec->nfields;
		return S_OK;
	}
	ULONG n = *count < rec->nfields ? *count : rec->nfields;
	for (ULONG i = 0; i < n; i++)
		names[i] = rec->fields[i].name ?
			   oa_SysAllocString(rec->fields[i].name) : NULL;
	*count = n;
	return S_OK;
}

WINFN(BOOL) ri_IsMatchingType(IRecordInfo *self, IRecordInfo *other)
{
	GUID g;

	if (!other)
		return FALSE;
	if (other == self)
		return TRUE;
	if (FAILED(other->lpVtbl->GetGuid(other, &g)))
		return FALSE;
	return !memcmp(&g, &rec_of(self)->guid, sizeof(g)) &&
	       memcmp(&g, &oa_IID_NULL, sizeof(g));
}

WINFN(PVOID) ri_RecordCreate(IRecordInfo *self)
{
	struct record *rec = rec_of(self);
	void *p = ole_taskmem_alloc(rec->size ? rec->size : 1);

	if (p)
		memset(p, 0, rec->size);
	return p;
}

WINFN(HRESULT) ri_RecordCreateCopy(IRecordInfo *self, PVOID src, PVOID *dst)
{
	if (!src || !dst)
		return E_INVALIDARG;
	if (!(*dst = ri_RecordCreate(self)))
		return E_OUTOFMEMORY;
	HRESULT hr = ri_RecordCopy(self, src, *dst);
	if (FAILED(hr)) {
		ri_RecordClear(self, *dst);
		ole_taskmem_free(*dst);
		*dst = NULL;
	}
	return hr;
}

WINFN(HRESULT) ri_RecordDestroy(IRecordInfo *self, PVOID record)
{
	if (!record)
		return S_OK;
	ri_RecordClear(self, record);
	ole_taskmem_free(record);
	return S_OK;
}

static const IRecordInfoVtbl record_vtbl = {
	ri_QueryInterface, ri_AddRef, ri_Release, ri_RecordInit,
	ri_RecordClear, ri_RecordCopy, ri_GetGuid, ri_GetName, ri_GetSize,
	ri_GetTypeInfo, ri_GetField, ri_GetFieldNoCopy, ri_PutField,
	ri_PutFieldNoCopy, ri_GetFieldNames, ri_IsMatchingType,
	ri_RecordCreate, ri_RecordCreateCopy, ri_RecordDestroy,
};

/* ---- the exports ---- */

HRESULT STUB_WINAPI oa_GetRecordInfoFromTypeInfo(ITypeInfo *ti,
						 IRecordInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!ti)
		return E_INVALIDARG;
	return record_new(ti, 0, out);
}

WINFN(HRESULT) oa_GetRecordInfoFromGuids(REFGUID libid, ULONG major,
					 ULONG minor, LCID lcid,
					 REFGUID guid, IRecordInfo **out)
{
	ITypeLib *lib;
	ITypeInfo *ti;
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!libid || !guid)
		return E_INVALIDARG;
	if (FAILED(hr = oa_LoadRegTypeLib(libid, (WORD)major, (WORD)minor,
					  lcid, &lib)))
		return hr;
	hr = lib->lpVtbl->GetTypeInfoOfGuid(lib, guid, &ti);
	lib->lpVtbl->Release(lib);
	if (FAILED(hr))
		return hr;
	hr = record_new(ti, 0, out);
	ti->lpVtbl->Release(ti);
	return hr;
}

#define E(name, ord) { #name, ord, (void *)oa_##name }

const struct builtin_export oa_record_exports[] = {
	E(GetRecordInfoFromGuids, 322),
	E(GetRecordInfoFromTypeInfo, 323),
	BUILTIN_END
};
