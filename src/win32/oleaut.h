/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32 internals, shared between oleaut32.c (BSTRs, VARIANTs and the
 * rest), oleaut_conv.c (conversions and dates), oleaut_array.c
 * (SAFEARRAYs) and errinfo.c (error information, also in ole32).
 */
#ifndef VFW_WIN32_OLEAUT_H
#define VFW_WIN32_OLEAUT_H

#include "runtime/builtin.h"
#include "win32.h"

extern const IID oa_IID_IUnknown, oa_IID_IDispatch, oa_IID_NULL;

/* BSTRs (the exports, callable from C). */
BSTR STUB_WINAPI oa_SysAllocString(LPCOLESTR s);
BSTR STUB_WINAPI oa_SysAllocStringLen(const OLECHAR *s, UINT len);
BSTR STUB_WINAPI oa_SysAllocStringByteLen(LPCSTR s, UINT len);
void STUB_WINAPI oa_SysFreeString(BSTR s);
UINT STUB_WINAPI oa_SysStringLen(BSTR s);
UINT STUB_WINAPI oa_SysStringByteLen(BSTR s);
/* A BSTR from UTF-8, or NULL. */
BSTR oa_bstr_utf8(const char *s);

/* VARIANTs. */
void STUB_WINAPI oa_VariantInit(VARIANTARG *v);
HRESULT STUB_WINAPI oa_VariantClear(VARIANTARG *v);
HRESULT STUB_WINAPI oa_VariantCopy(VARIANTARG *dst, const VARIANTARG *src);
HRESULT STUB_WINAPI oa_VariantCopyInd(VARIANTARG *dst, const VARIANTARG *src);
HRESULT STUB_WINAPI oa_VariantChangeTypeEx(VARIANTARG *dst,
					   const VARIANTARG *src, LCID lcid,
					   USHORT flags, VARTYPE vt);
/* S_OK, or DISP_E_BADVARTYPE for a type no VARIANT may have. */
HRESULT oa_validate_vt(VARTYPE vt);

/* SAFEARRAYs. */
HRESULT STUB_WINAPI oa_SafeArrayDestroy(SAFEARRAY *psa);
HRESULT STUB_WINAPI oa_SafeArrayCopy(SAFEARRAY *psa, SAFEARRAY **out);
HRESULT STUB_WINAPI oa_VectorFromBstr(BSTR b, SAFEARRAY **out);
HRESULT STUB_WINAPI oa_BstrFromVector(SAFEARRAY *psa, BSTR *out);

/* Error information. */
HRESULT STUB_WINAPI oa_CreateErrorInfo(ICreateErrorInfo **out);
HRESULT STUB_WINAPI oa_SetErrorInfo(ULONG reserved, IErrorInfo *info);
HRESULT STUB_WINAPI oa_GetErrorInfo(ULONG reserved, IErrorInfo **out);

/* The export tables, merged into oleaut32's (and errinfo's into ole32's). */
extern const struct builtin_export oa_conv_exports[];
extern const struct builtin_export oa_array_exports[];
extern const struct builtin_export oa_errinfo_ole32_exports[];

#endif /* VFW_WIN32_OLEAUT_H */
