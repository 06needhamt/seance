/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The device moniker IFilterMapper2::RegisterFilter hands back: it names
 * a filter's registry key, and BindToStorage gives an IPropertyBag over
 * that key's values, as Windows' CDeviceMoniker does. Filters use it to
 * add values of their own; video compressors write FccHandler.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/oleaut.h"
#include "win32/quartz.h"
#include "win32/reg.h"

/* {4315D437-5B8C-11D0-BD3B-00A0C911CE86} */
static const CLSID CLSID_CDeviceMoniker = {
	0x4315d437, 0x5b8c, 0x11d0,
	{ 0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86 }
};

#ifndef MK_S_REDUCED_TO_SELF
#define MK_S_REDUCED_TO_SELF ((HRESULT)0x000401e2)
#endif

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(*a));
}

/* objidl.h's IMoniker, from IUnknown through IMoniker. */
typedef struct qz_moniker_vtbl {
	HRESULT (WINAPI *QueryInterface)(IMoniker *, REFIID, void **);
	ULONG (WINAPI *AddRef)(IMoniker *);
	ULONG (WINAPI *Release)(IMoniker *);
	HRESULT (WINAPI *GetClassID)(IMoniker *, CLSID *);
	HRESULT (WINAPI *IsDirty)(IMoniker *);
	HRESULT (WINAPI *Load)(IMoniker *, void *);
	HRESULT (WINAPI *Save)(IMoniker *, void *, BOOL);
	HRESULT (WINAPI *GetSizeMax)(IMoniker *, void *);
	HRESULT (WINAPI *BindToObject)(IMoniker *, void *, IMoniker *, REFIID,
				       void **);
	HRESULT (WINAPI *BindToStorage)(IMoniker *, void *, IMoniker *, REFIID,
					void **);
	HRESULT (WINAPI *Reduce)(IMoniker *, void *, DWORD, IMoniker **,
				 IMoniker **);
	HRESULT (WINAPI *ComposeWith)(IMoniker *, IMoniker *, BOOL, IMoniker **);
	HRESULT (WINAPI *Enum)(IMoniker *, BOOL, IEnumMoniker **);
	HRESULT (WINAPI *IsEqual)(IMoniker *, IMoniker *);
	HRESULT (WINAPI *Hash)(IMoniker *, DWORD *);
	HRESULT (WINAPI *IsRunning)(IMoniker *, void *, IMoniker *, IMoniker *);
	HRESULT (WINAPI *GetTimeOfLastChange)(IMoniker *, void *, IMoniker *,
					      FILETIME *);
	HRESULT (WINAPI *Inverse)(IMoniker *, IMoniker **);
	HRESULT (WINAPI *CommonPrefixWith)(IMoniker *, IMoniker *, IMoniker **);
	HRESULT (WINAPI *RelativePathTo)(IMoniker *, IMoniker *, IMoniker **);
	HRESULT (WINAPI *GetDisplayName)(IMoniker *, void *, IMoniker *,
					 LPOLESTR *);
	HRESULT (WINAPI *ParseDisplayName)(IMoniker *, void *, IMoniker *,
					   LPOLESTR, ULONG *, IMoniker **);
	HRESULT (WINAPI *IsSystemMoniker)(IMoniker *, DWORD *);
} qz_moniker_vtbl;

struct moniker {
	const qz_moniker_vtbl *vtbl;
	LONG refs;
	char *path;
};

struct bag {
	IPropertyBag iface;
	LONG refs;
	char *path;
};

static const qz_moniker_vtbl moniker_vtbl;
static HRESULT bag_new(const char *path, IPropertyBag **out);

static struct moniker *from_moniker(IMoniker *i)
{
	return (struct moniker *)i;
}

WINFN(HRESULT) mk_QueryInterface(IMoniker *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_IMoniker) &&
	    !guid_eq(riid, &IID_IPersist) && !guid_eq(riid, &IID_IPersistStream)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	__atomic_add_fetch(&from_moniker(self)->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) mk_AddRef(IMoniker *self)
{
	return __atomic_add_fetch(&from_moniker(self)->refs, 1, __ATOMIC_SEQ_CST);
}

WINFN(ULONG) mk_Release(IMoniker *self)
{
	struct moniker *m = from_moniker(self);
	ULONG r = __atomic_sub_fetch(&m->refs, 1, __ATOMIC_SEQ_CST);

	if (!r) {
		free(m->path);
		free(m);
	}
	return r;
}

WINFN(HRESULT) mk_GetClassID(IMoniker *self, CLSID *clsid)
{
	if (!clsid)
		return E_POINTER;
	*clsid = CLSID_CDeviceMoniker;
	return S_OK;
}

WINFN(HRESULT) mk_IsDirty(IMoniker *self)
{
	return S_FALSE;
}

WINFN(HRESULT) mk_Load(IMoniker *self, void *stream)
{
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_Save(IMoniker *self, void *stream, BOOL clear)
{
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_GetSizeMax(IMoniker *self, void *size)
{
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_BindToObject(IMoniker *self, void *bc, IMoniker *left,
			       REFIID riid, void **ppv)
{
	if (ppv)
		*ppv = NULL;
	vfw_warn("quartz: IMoniker::BindToObject on a device moniker is not "
		 "implemented");
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_BindToStorage(IMoniker *self, void *bc, IMoniker *left,
				REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (!guid_eq(riid, &IID_IPropertyBag))
		return E_NOINTERFACE;
	return bag_new(from_moniker(self)->path, (IPropertyBag **)ppv);
}

WINFN(HRESULT) mk_Reduce(IMoniker *self, void *bc, DWORD how,
			 IMoniker **left, IMoniker **out)
{
	if (!out)
		return E_POINTER;
	*out = self;
	mk_AddRef(self);
	return MK_S_REDUCED_TO_SELF;
}

WINFN(HRESULT) mk_ComposeWith(IMoniker *self, IMoniker *right, BOOL only,
			      IMoniker **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_Enum(IMoniker *self, BOOL forward, IEnumMoniker **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;		/* no sub-monikers */
	return S_OK;
}

/* Another device moniker for the same key. */
WINFN(HRESULT) mk_IsEqual(IMoniker *self, IMoniker *other)
{
	if (!other || *(const void **)other != &moniker_vtbl)
		return S_FALSE;
	return strcasecmp(from_moniker(self)->path,
			  from_moniker(other)->path) ? S_FALSE : S_OK;
}

WINFN(HRESULT) mk_Hash(IMoniker *self, DWORD *hash)
{
	DWORD h = 0;

	if (!hash)
		return E_POINTER;
	for (const char *p = from_moniker(self)->path; *p; p++)
		h = h * 31 + (unsigned char)(*p | 0x20);
	*hash = h;
	return S_OK;
}

WINFN(HRESULT) mk_IsRunning(IMoniker *self, void *bc, IMoniker *left,
			    IMoniker *newly)
{
	return S_FALSE;
}

WINFN(HRESULT) mk_GetTimeOfLastChange(IMoniker *self, void *bc,
				      IMoniker *left, FILETIME *t)
{
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_Inverse(IMoniker *self, IMoniker **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_CommonPrefixWith(IMoniker *self, IMoniker *other,
				   IMoniker **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_RelativePathTo(IMoniker *self, IMoniker *other,
				 IMoniker **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_GetDisplayName(IMoniker *self, void *bc, IMoniker *left,
				 LPOLESTR *name)
{
	if (name)
		*name = NULL;
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_ParseDisplayName(IMoniker *self, void *bc, IMoniker *left,
				   LPOLESTR name, ULONG *eaten, IMoniker **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_IsSystemMoniker(IMoniker *self, DWORD *sys)
{
	if (!sys)
		return E_POINTER;
	*sys = 0;		/* MKSYS_NONE */
	return S_FALSE;
}

static const qz_moniker_vtbl moniker_vtbl = {
	mk_QueryInterface, mk_AddRef, mk_Release, mk_GetClassID, mk_IsDirty,
	mk_Load, mk_Save, mk_GetSizeMax, mk_BindToObject, mk_BindToStorage,
	mk_Reduce, mk_ComposeWith, mk_Enum, mk_IsEqual, mk_Hash, mk_IsRunning,
	mk_GetTimeOfLastChange, mk_Inverse, mk_CommonPrefixWith,
	mk_RelativePathTo, mk_GetDisplayName, mk_ParseDisplayName,
	mk_IsSystemMoniker,
};

/* The property bag over the key's values. */

static struct bag *from_bag(IPropertyBag *i)
{
	return (struct bag *)i;
}

WINFN(HRESULT) bag_QueryInterface(IPropertyBag *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_IPropertyBag)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	__atomic_add_fetch(&from_bag(self)->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) bag_AddRef(IPropertyBag *self)
{
	return __atomic_add_fetch(&from_bag(self)->refs, 1, __ATOMIC_SEQ_CST);
}

WINFN(ULONG) bag_Release(IPropertyBag *self)
{
	struct bag *b = from_bag(self);
	ULONG r = __atomic_sub_fetch(&b->refs, 1, __ATOMIC_SEQ_CST);

	if (!r) {
		free(b->path);
		free(b);
	}
	return r;
}

/*
 * A value as Windows' device moniker reads it: REG_SZ as VT_BSTR, REG_DWORD
 * as VT_I4 and REG_BINARY as VT_ARRAY | VT_UI1; then, if the caller asked
 * for another type (vt not VT_EMPTY), changed to it.
 */
WINFN(HRESULT) bag_Read(IPropertyBag *self, LPCOLESTR name, VARIANT *value,
			IErrorLog *log)
{
	VARIANT v;
	HRESULT hr = S_OK;
	char *n;

	if (!name || !value)
		return E_POINTER;
	if (!(n = utf16_to_utf8_dup(name)))
		return E_OUTOFMEMORY;
	memset(&v, 0, sizeof(v));
	reg_lock();
	struct reg_key *k = reg_open(from_bag(self)->path);
	struct reg_value *rv = k ? reg_get(k, n) : NULL;
	if (!rv) {
		hr = E_FAIL;
	} else if (rv->type == REG_SZ || rv->type == REG_EXPAND_SZ) {
		UINT len = rv->size / sizeof(WCHAR);
		const WCHAR *w = (const WCHAR *)rv->data;
		while (len && !w[len - 1])
			len--;
		v.vt = VT_BSTR;
		if (!(v.bstrVal = oa_SysAllocStringLen(w, len)))
			hr = E_OUTOFMEMORY;
	} else if (rv->type == REG_DWORD && rv->size >= sizeof(DWORD)) {
		v.vt = VT_I4;
		memcpy(&v.lVal, rv->data, sizeof(DWORD));
	} else if (rv->type == REG_BINARY) {
		SAFEARRAY *a = oa_SafeArrayCreateVector(VT_UI1, 0, rv->size);
		if (!a) {
			hr = E_OUTOFMEMORY;
		} else {
			memcpy(a->pvData, rv->data, rv->size);
			v.vt = VT_ARRAY | VT_UI1;
			v.parray = a;
		}
	} else {
		hr = E_FAIL;
	}
	reg_unlock();
	free(n);
	if (FAILED(hr))
		return hr;
	if (value->vt != VT_EMPTY && value->vt != v.vt) {
		/*
		 * Only value's vt is the caller's: its union is unset, so the
		 * conversion goes to an empty VARIANT of our own.
		 */
		VARTYPE want = value->vt;
		VARIANT out;
		oa_VariantInit(&out);
		hr = oa_VariantChangeTypeEx(&out, &v, 0, 0, want);
		oa_VariantClear(&v);
		if (SUCCEEDED(hr))
			*value = out;
		return hr;
	}
	*value = v;
	return S_OK;
}

/* VT_BSTR as REG_SZ, the integer types as REG_DWORD, VT_ARRAY | VT_UI1 as
 * REG_BINARY. */
WINFN(HRESULT) bag_Write(IPropertyBag *self, LPCOLESTR name, VARIANT *value)
{
	const void *data;
	DWORD type, size, dw;
	LONG err;
	char *n;

	if (!name || !value)
		return E_POINTER;
	switch (value->vt) {
	case VT_BSTR: {
		/* Callers pass plain strings as well as BSTRs. */
		static const WCHAR empty[1];
		const WCHAR *w = value->bstrVal ? value->bstrVal : empty;
		type = REG_SZ;
		data = w;
		size = (DWORD)((wstrlen(w) + 1) * sizeof(WCHAR));
		break;
	}
	case VT_I4: case VT_UI4: case VT_INT: case VT_UINT:
		type = REG_DWORD;
		dw = (DWORD)value->lVal;
		data = &dw;
		size = sizeof(dw);
		break;
	case VT_ARRAY | VT_UI1: {
		const SAFEARRAY *a = value->parray;
		if (!a || a->cDims != 1 || a->cbElements != 1)
			return E_INVALIDARG;
		type = REG_BINARY;
		data = a->pvData;
		size = a->rgsabound[0].cElements;
		break;
	}
	default:
		return E_INVALIDARG;
	}
	if (!(n = utf16_to_utf8_dup(name)))
		return E_OUTOFMEMORY;
	reg_lock();
	struct reg_key *k = reg_create(from_bag(self)->path, 0, NULL, &err);
	if (k && !(err = reg_set(k, n, type, data, size)))
		err = reg_save();
	reg_unlock();
	TRACE("IPropertyBag::Write(%s\\%s, type %lu) = %ld", from_bag(self)->path,
	      n, (unsigned long)type, (long)err);
	free(n);
	return err ? HRESULT_FROM_WIN32(err) : S_OK;
}

static const IPropertyBagVtbl bag_vtbl = {
	bag_QueryInterface, bag_AddRef, bag_Release, bag_Read, bag_Write,
};

static HRESULT bag_new(const char *path, IPropertyBag **out)
{
	struct bag *b = calloc(1, sizeof(*b));

	if (!b || !(b->path = strdup(path))) {
		free(b);
		return E_OUTOFMEMORY;
	}
	b->iface.lpVtbl = &bag_vtbl;
	b->refs = 1;
	*out = &b->iface;
	return S_OK;
}

HRESULT qz_moniker_new(const char *path, IMoniker **out)
{
	struct moniker *m = calloc(1, sizeof(*m));

	*out = NULL;
	if (!m || !(m->path = strdup(path))) {
		free(m);
		return E_OUTOFMEMORY;
	}
	m->vtbl = &moniker_vtbl;
	m->refs = 1;
	*out = (IMoniker *)m;
	return S_OK;
}

const char *qz_moniker_path(IMoniker *moniker)
{
	if (!moniker || *(const void **)moniker != &moniker_vtbl)
		return NULL;
	return from_moniker(moniker)->path;
}
