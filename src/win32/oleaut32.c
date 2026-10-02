/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32.dll: OLE Automation's data types, after Wine's oleaut32.
 *
 * - BSTRs: a DWORD byte count, the characters, then a NUL WCHAR, from the
 *   process heap (so below 2 GB, where Win32 code can use them).
 * - VARIANTs: VariantInit/Clear/Copy/CopyInd; conversions are in
 *   oleaut_conv.c and SAFEARRAYs in oleaut_array.c.
 * - Error information (errinfo.c), DispGetParam, VarBstrCat/Cmp.
 * - Headless: OleCreatePropertyFrame(Indirect) prints its caption and
 *   answers as if the frame were closed with Cancel, as MessageBox answers
 *   with its default button. There are no type libraries (LoadTypeLib
 *   fails with TYPE_E_CANTLOADLIBRARY) and no OLE pictures or fonts.
 *
 * Most of oleaut32 is imported by ordinal, so the exports carry Windows'
 * ordinals, as Wine's oleaut32.spec lists them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/oleaut.h"

extern const struct builtin_dll builtin_user32, builtin_gdi32;

const IID oa_IID_IUnknown = { 0x00000000, 0x0000, 0x0000,
			      { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
const IID oa_IID_IDispatch = { 0x00020400, 0x0000, 0x0000,
			       { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
const IID oa_IID_NULL;

/* BSTRs. */

#define BSTR_MAX 0x7ffffff0u

/* A BSTR of len bytes, copied from s if s isn't NULL (else zeroed). */
static BSTR bstr_alloc(const void *s, UINT len)
{
	if (len > BSTR_MAX)
		return NULL;
	/* The count, the bytes, and a NUL WCHAR (after an odd byte too). */
	BYTE *p = heap_alloc(heap_process(), HEAP_ZERO_MEMORY,
			     sizeof(DWORD) + len + sizeof(WCHAR) + 1);
	if (!p)
		return NULL;
	*(DWORD *)p = len;
	if (s)
		memcpy(p + sizeof(DWORD), s, len);
	return (BSTR)(p + sizeof(DWORD));
}

BSTR STUB_WINAPI oa_SysAllocStringLen(const OLECHAR *s, UINT len)
{
	return len > BSTR_MAX / sizeof(WCHAR) ? NULL :
	       bstr_alloc(s, len * sizeof(WCHAR));
}

BSTR STUB_WINAPI oa_SysAllocString(LPCOLESTR s)
{
	return s ? oa_SysAllocStringLen(s, (UINT)wstrlen(s)) : NULL;
}

BSTR STUB_WINAPI oa_SysAllocStringByteLen(LPCSTR s, UINT len)
{
	return bstr_alloc(s, len);
}

void STUB_WINAPI oa_SysFreeString(BSTR s)
{
	if (s)
		heap_free(heap_process(), 0, (BYTE *)s - sizeof(DWORD));
}

UINT STUB_WINAPI oa_SysStringByteLen(BSTR s)
{
	return s ? *(const DWORD *)((BYTE *)s - sizeof(DWORD)) : 0;
}

UINT STUB_WINAPI oa_SysStringLen(BSTR s)
{
	return oa_SysStringByteLen(s) / sizeof(WCHAR);
}

/* s may point into *old, so the copy is made before *old is freed. */
WINFN(INT) oa_SysReAllocStringLen(BSTR *old, const OLECHAR *s, UINT len)
{
	if (!old)
		return FALSE;
	BSTR b = oa_SysAllocStringLen(s, len);
	if (!b)
		return FALSE;
	oa_SysFreeString(*old);
	*old = b;
	return TRUE;
}

WINFN(INT) oa_SysReAllocString(BSTR *old, LPCOLESTR s)
{
	if (!old)
		return FALSE;
	if (*old && *old == s)
		return TRUE;
	return oa_SysReAllocStringLen(old, s, s ? (UINT)wstrlen(s) : 0);
}

BSTR oa_bstr_utf8(const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	BSTR b = w ? oa_SysAllocString(w) : NULL;

	free(w);
	return b;
}

/* BSTRs held in no cache: nothing to turn off. */
WINFN(void) oa_SetOaNoCache(void)
{
}

/* Concatenates two BSTRs (NULL is empty). */
WINFN(HRESULT) oa_VarBstrCat(BSTR a, BSTR b, BSTR *out)
{
	UINT na = oa_SysStringByteLen(a), nb = oa_SysStringByteLen(b);

	if (!out)
		return E_INVALIDARG;
	if (na > BSTR_MAX - nb)
		return E_OUTOFMEMORY;
	BSTR r = bstr_alloc(NULL, na + nb);
	if (!r)
		return E_OUTOFMEMORY;
	if (na)
		memcpy(r, a, na);
	if (nb)
		memcpy((BYTE *)r + na, b, nb);
	*out = r;
	return S_OK;
}

/*
 * Compares two BSTRs (NULL is empty) by UTF-16 code unit, ignoring case
 * with NORM_IGNORECASE; there is no locale-aware collation.
 */
WINFN(HRESULT) oa_VarBstrCmp(BSTR a, BSTR b, LCID lcid, ULONG flags)
{
	UINT na = oa_SysStringLen(a), nb = oa_SysStringLen(b);
	const ULONG ignore_case = 0x1;	/* NORM_IGNORECASE */

	for (UINT i = 0; i < na && i < nb; i++) {
		WCHAR x = a[i], y = b[i];
		if (flags & ignore_case) {
			x = k32_toupper_w(x);
			y = k32_toupper_w(y);
		}
		if (x != y)
			return x < y ? VARCMP_LT : VARCMP_GT;
	}
	return na == nb ? VARCMP_EQ : na < nb ? VARCMP_LT : VARCMP_GT;
}

/* VARIANTs. */

HRESULT oa_validate_vt(VARTYPE vt)
{
	VARTYPE extra = vt & (VT_VECTOR | VT_ARRAY | VT_BYREF | VT_RESERVED);
	VARTYPE base = vt & VT_TYPEMASK;

	if (extra & (VT_VECTOR | VT_RESERVED))
		return DISP_E_BADVARTYPE;
	if (base < VT_VOID || base == VT_RECORD || base == VT_CLSID) {
		if ((extra & (VT_BYREF | VT_ARRAY)) && base <= VT_NULL)
			return DISP_E_BADVARTYPE;
		if (base != 15)
			return S_OK;
	}
	return DISP_E_BADVARTYPE;
}

void STUB_WINAPI oa_VariantInit(VARIANTARG *v)
{
	v->vt = VT_EMPTY;
}

static ULONG unk_addref(IUnknown *u)
{
	return u ? u->lpVtbl->AddRef(u) : 0;
}

static ULONG unk_release(IUnknown *u)
{
	return u ? u->lpVtbl->Release(u) : 0;
}

HRESULT STUB_WINAPI oa_VariantClear(VARIANTARG *v)
{
	HRESULT hr = oa_validate_vt(v->vt);

	if (FAILED(hr))
		return hr;
	if (!(v->vt & VT_BYREF)) {
		if ((v->vt & VT_ARRAY) || v->vt == VT_SAFEARRAY) {
			/* A locked array stays, and so does the variant. */
			if (FAILED(hr = oa_SafeArrayDestroy(v->parray)))
				return hr;
		} else if (v->vt == VT_BSTR) {
			oa_SysFreeString(v->bstrVal);
		} else if (v->vt == VT_RECORD) {
			if (v->pRecInfo) {
				v->pRecInfo->lpVtbl->RecordClear(v->pRecInfo,
								 v->pvRecord);
				v->pRecInfo->lpVtbl->Release(v->pRecInfo);
			}
		} else if (v->vt == VT_DISPATCH || v->vt == VT_UNKNOWN) {
			unk_release(v->punkVal);
		}
	}
	v->vt = VT_EMPTY;
	return hr;
}

/* A record's copy, in memory of the record's own size. */
static HRESULT copy_record(const VARIANT *src, VARIANT *dst)
{
	IRecordInfo *ri = src->pRecInfo;
	ULONG size = 0;

	dst->pRecInfo = ri;
	dst->pvRecord = NULL;
	if (!ri)
		return src->pvRecord ? E_INVALIDARG : S_OK;
	HRESULT hr = ri->lpVtbl->GetSize(ri, &size);
	if (FAILED(hr))
		return hr;
	if (!(dst->pvRecord = heap_alloc(heap_process(), HEAP_ZERO_MEMORY,
					 size ? size : 1)))
		return E_OUTOFMEMORY;
	hr = ri->lpVtbl->RecordCopy(ri, src->pvRecord, dst->pvRecord);
	if (FAILED(hr)) {
		heap_free(heap_process(), 0, dst->pvRecord);
		return hr;
	}
	ri->lpVtbl->AddRef(ri);
	return S_OK;
}

HRESULT STUB_WINAPI oa_VariantCopy(VARIANTARG *dst, const VARIANTARG *src)
{
	HRESULT hr;

	if (dst == src)
		return S_OK;
	if (FAILED(hr = oa_validate_vt(src->vt)))
		return hr;
	if (FAILED(hr = oa_VariantClear(dst)))
		return hr;
	VARIANT v = *src;
	if (!(src->vt & VT_BYREF)) {
		if (src->vt & VT_ARRAY) {
			hr = oa_SafeArrayCopy(src->parray, &v.parray);
		} else if (src->vt == VT_BSTR) {
			if (src->bstrVal &&
			    !(v.bstrVal = oa_SysAllocStringByteLen(
				      (LPCSTR)src->bstrVal,
				      oa_SysStringByteLen(src->bstrVal))))
				hr = E_OUTOFMEMORY;
		} else if (src->vt == VT_RECORD) {
			hr = copy_record(src, &v);
		} else if (src->vt == VT_DISPATCH || src->vt == VT_UNKNOWN) {
			unk_addref(src->punkVal);
		}
	}
	if (SUCCEEDED(hr))
		*dst = v;
	return hr;
}

/* The size of what a VT_BYREF of type vt points to, or 0. */
static size_t byref_size(VARTYPE vt)
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
	case VT_BSTR: case VT_UNKNOWN: case VT_DISPATCH:
		return sizeof(void *);
	}
	return 0;
}

HRESULT STUB_WINAPI oa_VariantCopyInd(VARIANTARG *dst, const VARIANTARG *src)
{
	HRESULT hr;
	VARIANT v;

	if (!(src->vt & VT_BYREF))
		return oa_VariantCopy(dst, src);
	if (FAILED(hr = oa_validate_vt(src->vt)))
		return hr;
	if (!src->byref)
		return E_INVALIDARG;

	VARTYPE vt = src->vt & ~VT_BYREF;
	oa_VariantInit(&v);
	if (vt & VT_ARRAY) {
		hr = oa_SafeArrayCopy(*src->pparray, &v.parray);
	} else if (vt == VT_VARIANT) {
		/* A VARIANT pointing to another by reference is too deep. */
		if (src->pvarVal->vt & VT_BYREF)
			return E_INVALIDARG;
		return oa_VariantCopy(dst, src->pvarVal);
	} else if (vt == VT_DECIMAL) {
		v.decVal = *src->pdecVal;
	} else if (vt == VT_BSTR) {
		BSTR b = *src->pbstrVal;
		if (b && !(v.bstrVal = oa_SysAllocStringByteLen(
				   (LPCSTR)b, oa_SysStringByteLen(b))))
			hr = E_OUTOFMEMORY;
		else if (!b)
			v.bstrVal = NULL;
	} else if (vt == VT_RECORD) {
		hr = copy_record(src, &v);
	} else if (byref_size(vt)) {
		memcpy(&v.llVal, src->byref, byref_size(vt));
		if (vt == VT_UNKNOWN || vt == VT_DISPATCH)
			unk_addref(v.punkVal);
	} else {
		return DISP_E_BADVARTYPE;
	}
	if (FAILED(hr))
		return hr;
	v.vt = vt;
	/* In place too: src is only read above. */
	if (dst == src) {
		*dst = v;
		return S_OK;
	}
	if (FAILED(hr = oa_VariantClear(dst))) {
		VARIANT tmp = v;
		oa_VariantClear(&tmp);
		return hr;
	}
	*dst = v;
	return S_OK;
}

WINFN(HRESULT) oa_VariantChangeType(VARIANTARG *dst, const VARIANTARG *src,
				    USHORT flags, VARTYPE vt)
{
	return oa_VariantChangeTypeEx(dst, src, 0x400 /* LOCALE_USER_DEFAULT */,
				      flags, vt);
}

/*
 * DispGetParam: argument position (positional ones are in reverse order
 * in rgvarg; past them, a named argument's DISPID) as type vt.
 */
WINFN(HRESULT) oa_DispGetParam(DISPPARAMS *params, UINT position, VARTYPE vt,
			       VARIANT *result, UINT *arg_err)
{
	UINT pos;

	if (!params || !result)
		return E_INVALIDARG;
	if (position < params->cArgs - params->cNamedArgs) {
		pos = params->cArgs - 1 - position;
	} else {
		for (pos = 0; pos < params->cNamedArgs; pos++)
			if (params->rgdispidNamedArgs[pos] == (DISPID)position)
				break;
		if (pos == params->cNamedArgs)
			return DISP_E_PARAMNOTFOUND;
	}
	oa_VariantInit(result);
	HRESULT hr = oa_VariantChangeTypeEx(result, &params->rgvarg[pos], 0, 0,
					    vt);
	if (FAILED(hr) && arg_err)
		*arg_err = pos;
	return hr;
}

/* Headless: property frames and pictures. */

static char *caption_utf8(LPCOLESTR s)
{
	return s ? utf16_to_utf8_dup(s) : NULL;
}

/* The frame a codec's property pages would show: closed with Cancel. */
WINFN(HRESULT) oa_OleCreatePropertyFrame(HWND owner, UINT x, UINT y,
					 LPCOLESTR caption, ULONG nobjects,
					 IUnknown **objects, ULONG npages,
					 CLSID *pages, LCID lcid,
					 DWORD reserved, void *reserved2)
{
	char *c = caption_utf8(caption);

	fprintf(stderr, "OleCreatePropertyFrame [%s]: %lu page(s)\n"
		"  (no GUI: closed, as with Cancel)\n", c ? c : "",
		(unsigned long)npages);
	free(c);
	return S_OK;
}

WINFN(HRESULT) oa_OleCreatePropertyFrameIndirect(OCPFIPARAMS *p)
{
	if (!p)
		return E_POINTER;
	return oa_OleCreatePropertyFrame(p->hWndOwner, p->x, p->y,
					 p->lpszCaption, p->cObjects,
					 p->lplpUnk, p->cPages, p->lpPages,
					 p->lcid, 0, NULL);
}

/*
 * OLE_COLOR to COLORREF: RGB and PALETTERGB as they are, a palette index
 * checked against hpal, 0x80000000 | n system colour n.
 */
WINFN(HRESULT) oa_OleTranslateColor(OLE_COLOR c, HPALETTE hpal,
				    COLORREF *out)
{
	COLORREF r = c;

	switch (c >> 24) {
	case 0x00:
	case 0x02:
		break;
	case 0x01:
		if (hpal) {
			UINT (WINAPI *entries)(HPALETTE, UINT, UINT,
					       PALETTEENTRY *) =
				builtin_export(&builtin_gdi32,
					       "GetPaletteEntries");
			if (!entries || (c & 0xffff) >= entries(hpal, 0, 0, NULL))
				return E_INVALIDARG;
		}
		break;
	case 0x80: {
		DWORD (WINAPI *sys)(int) =
			builtin_export(&builtin_user32, "GetSysColor");
		if ((c & 0xffff) > 30 /* COLOR_MENUBAR */ || !sys)
			return E_INVALIDARG;
		r = sys((int)(c & 0xffff));
		break;
	}
	default:
		return E_INVALIDARG;
	}
	if (out)
		*out = r;
	return S_OK;
}

static HRESULT no_picture(void **out, const char *what)
{
	if (out)
		*out = NULL;
	TRACE("%s: no OLE pictures or fonts here", what);
	return E_NOTIMPL;
}

WINFN(HRESULT) oa_OleCreatePictureIndirect(void *desc, REFIID riid, BOOL own,
					   void **out)
{
	return no_picture(out, "OleCreatePictureIndirect");
}

WINFN(HRESULT) oa_OleCreateFontIndirect(void *desc, REFIID riid, void **out)
{
	return no_picture(out, "OleCreateFontIndirect");
}

WINFN(HRESULT) oa_OleLoadPicture(IUnknown *stream, LONG size, BOOL runmode,
				 REFIID riid, void **out)
{
	return no_picture(out, "OleLoadPicture");
}

WINFN(HRESULT) oa_OleLoadPictureEx(IUnknown *stream, LONG size, BOOL runmode,
				   REFIID riid, DWORD x, DWORD y, DWORD flags,
				   void **out)
{
	return no_picture(out, "OleLoadPictureEx");
}

WINFN(HRESULT) oa_OleLoadPicturePath(LPOLESTR path, IUnknown *outer,
				     DWORD reserved, OLE_COLOR clear,
				     REFIID riid, void **out)
{
	return no_picture(out, "OleLoadPicturePath");
}

/* Type libraries: none can be loaded or registered. */

WINFN(HRESULT) oa_LoadTypeLibEx(LPCOLESTR file, int kind, void **out)
{
	char *f = caption_utf8(file);

	if (out)
		*out = NULL;
	TRACE("LoadTypeLib(%s): no type libraries", f ? f : "");
	free(f);
	return TYPE_E_CANTLOADLIBRARY;
}

WINFN(HRESULT) oa_LoadTypeLib(LPCOLESTR file, void **out)
{
	return oa_LoadTypeLibEx(file, 0, out);
}

WINFN(HRESULT) oa_LoadRegTypeLib(REFGUID guid, WORD major, WORD minor,
				 LCID lcid, void **out)
{
	if (out)
		*out = NULL;
	return TYPE_E_LIBNOTREGISTERED;
}

WINFN(HRESULT) oa_QueryPathOfRegTypeLib(REFGUID guid, WORD major, WORD minor,
					LCID lcid, BSTR *path)
{
	return TYPE_E_LIBNOTREGISTERED;
}

/* A type library can only come from LoadTypeLib, which gives none. */
WINFN(HRESULT) oa_RegisterTypeLib(void *lib, LPCOLESTR path, LPCOLESTR help)
{
	return lib ? E_NOTIMPL : E_INVALIDARG;
}

WINFN(HRESULT) oa_RegisterTypeLibForUser(void *lib, LPCOLESTR path,
					 LPCOLESTR help)
{
	return oa_RegisterTypeLib(lib, path, help);
}

WINFN(HRESULT) oa_UnRegisterTypeLib(REFGUID guid, WORD major, WORD minor,
				    LCID lcid, int kind)
{
	return TYPE_E_LIBNOTREGISTERED;
}

WINFN(HRESULT) oa_UnRegisterTypeLibForUser(REFGUID guid, WORD major,
					   WORD minor, LCID lcid, int kind)
{
	return TYPE_E_LIBNOTREGISTERED;
}

WINFN(HRESULT) oa_CreateTypeLib2(int kind, LPCOLESTR file, void **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

/* oleaut32 as a COM server: none of its classes are here. */

WINFN(HRESULT) oa_DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	if (out)
		*out = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

WINFN(HRESULT) oa_DllCanUnloadNow(void)
{
	return S_FALSE;
}

WINFN(HRESULT) oa_DllRegisterServer(void)
{
	return S_OK;
}

WINFN(HRESULT) oa_DllUnregisterServer(void)
{
	return S_OK;
}

/* The exports, with Windows' ordinals. */
#define E(name, ord) { #name, ord, (void *)oa_##name }

static const struct builtin_export exports[] = {
	E(SysAllocString, 2),
	E(SysReAllocString, 3),
	E(SysAllocStringLen, 4),
	E(SysReAllocStringLen, 5),
	E(SysFreeString, 6),
	E(SysStringLen, 7),
	E(VariantInit, 8),
	E(VariantClear, 9),
	E(VariantCopy, 10),
	E(VariantCopyInd, 11),
	E(VariantChangeType, 12),
	E(DispGetParam, 28),
	E(VariantChangeTypeEx, 147),
	E(SysStringByteLen, 149),
	E(SysAllocStringByteLen, 150),
	E(CreateTypeLib2, 180),
	E(LoadTypeLib, 161),
	E(LoadRegTypeLib, 162),
	E(RegisterTypeLib, 163),
	E(QueryPathOfRegTypeLib, 164),
	E(LoadTypeLibEx, 183),
	E(UnRegisterTypeLib, 186),
	E(GetErrorInfo, 200),
	E(SetErrorInfo, 201),
	E(CreateErrorInfo, 202),
	E(VarBstrCat, 313),
	E(VarBstrCmp, 314),
	E(SetOaNoCache, 327),
	E(OleLoadPictureEx, 401),
	E(OleCreatePropertyFrameIndirect, 416),
	E(OleCreatePropertyFrame, 417),
	E(OleLoadPicture, 418),
	E(OleCreatePictureIndirect, 419),
	E(OleCreateFontIndirect, 420),
	E(OleTranslateColor, 421),
	E(OleLoadPicturePath, 424),
	E(RegisterTypeLibForUser, 442),
	E(UnRegisterTypeLibForUser, 443),
	E(DllCanUnloadNow, 0),
	E(DllGetClassObject, 0),
	E(DllRegisterServer, 0),
	E(DllUnregisterServer, 0),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = {
	exports, oa_conv_exports, oa_array_exports, NULL
};

const struct builtin_dll builtin_oleaut32 = { "oleaut32.dll", NULL, tables, 0 };
