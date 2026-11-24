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

/* DECIMAL arithmetic (oleaut_conv.c). */
HRESULT STUB_WINAPI oa_VarDecAbs(const DECIMAL *in, DECIMAL *out);
HRESULT STUB_WINAPI oa_VarDecNeg(const DECIMAL *in, DECIMAL *out);
HRESULT STUB_WINAPI oa_VarDecRound(const DECIMAL *in, int dp, DECIMAL *out);
HRESULT STUB_WINAPI oa_VarDecFix(const DECIMAL *in, DECIMAL *out);
HRESULT STUB_WINAPI oa_VarDecInt(const DECIMAL *in, DECIMAL *out);
HRESULT STUB_WINAPI oa_VarDecCmp(const DECIMAL *a, const DECIMAL *b);
HRESULT STUB_WINAPI oa_VarDecAdd(const DECIMAL *a, const DECIMAL *b,
				 DECIMAL *out);
HRESULT STUB_WINAPI oa_VarDecSub(const DECIMAL *a, const DECIMAL *b,
				 DECIMAL *out);
HRESULT STUB_WINAPI oa_VarDecMul(const DECIMAL *a, const DECIMAL *b,
				 DECIMAL *out);
HRESULT STUB_WINAPI oa_VarDecDiv(const DECIMAL *a, const DECIMAL *b,
				 DECIMAL *out);

/* SAFEARRAYs. */
SAFEARRAY *STUB_WINAPI oa_SafeArrayCreateVector(VARTYPE vt, LONG lbound,
					    ULONG count);
HRESULT STUB_WINAPI oa_SafeArrayDestroy(SAFEARRAY *psa);
HRESULT STUB_WINAPI oa_SafeArrayCopy(SAFEARRAY *psa, SAFEARRAY **out);
HRESULT STUB_WINAPI oa_VectorFromBstr(BSTR b, SAFEARRAY **out);
HRESULT STUB_WINAPI oa_BstrFromVector(SAFEARRAY *psa, BSTR *out);

/* Type libraries (oleaut_typelib.c). */
HRESULT STUB_WINAPI oa_LoadTypeLibEx(LPCOLESTR file, REGKIND kind,
				     ITypeLib **out);
HRESULT STUB_WINAPI oa_LoadRegTypeLib(REFGUID guid, WORD major, WORD minor,
				      LCID lcid, ITypeLib **out);

/*
 * Calling through type information (oleaut_dispatch.c): ITypeInfo::Invoke
 * for any type info, over its public methods, and DispCallFunc.
 */
HRESULT oa_typeinfo_invoke(ITypeInfo *ti, PVOID instance, MEMBERID memid,
			   WORD flags, DISPPARAMS *params, VARIANT *result,
			   EXCEPINFO *excep, UINT *arg_err);
HRESULT STUB_WINAPI oa_DispCallFunc(void *instance, ULONG_PTR offset,
				    CALLCONV cc, VARTYPE ret, UINT count,
				    VARTYPE *types, VARIANTARG **args,
				    VARIANT *result);

/* Type library registration (oleaut_typereg.c). */
HRESULT STUB_WINAPI oa_QueryPathOfRegTypeLib(REFGUID guid, WORD major,
					     WORD minor, LCID lcid,
					     BSTR *path);
/*
 * RegisterTypeLib, for the machine (HKLM's classes) or with for_user the
 * user (HKCU's): the library's key, with path for its own syskind and
 * help_dir (else path's directory), and its interfaces'.
 */
HRESULT oa_register_typelib(ITypeLib *lib, const WCHAR *path,
			    const WCHAR *help_dir, int for_user);

/* Error information. */
HRESULT STUB_WINAPI oa_CreateErrorInfo(ICreateErrorInfo **out);
HRESULT STUB_WINAPI oa_SetErrorInfo(ULONG reserved, IErrorInfo *info);
HRESULT STUB_WINAPI oa_GetErrorInfo(ULONG reserved, IErrorInfo **out);

/* The export tables, merged into oleaut32's (and errinfo's into ole32's). */
extern const struct builtin_export oa_conv_exports[];
extern const struct builtin_export oa_array_exports[];
extern const struct builtin_export oa_typelib_exports[];
extern const struct builtin_export oa_typereg_exports[];
extern const struct builtin_export oa_dispatch_exports[];
extern const struct builtin_export oa_var_exports[];
extern const struct builtin_export oa_format_exports[];
extern const struct builtin_export oa_errinfo_ole32_exports[];

#endif /* VFW_WIN32_OLEAUT_H */
