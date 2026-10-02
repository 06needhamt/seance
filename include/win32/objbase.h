/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * COM definitions used by ole32 (guiddef.h, winerror.h, wtypes.h,
 * unknwn.h, objidl.h, objbase.h). Interfaces are declared the way the
 * Windows headers declare them for C: a struct whose first member points
 * to a table of stdcall functions taking the object as their first
 * argument.
 */
#ifndef VFW_WIN32_OBJBASE_H
#define VFW_WIN32_OBJBASE_H

#include "types.h"

typedef struct _GUID {
	DWORD Data1;
	WORD Data2;
	WORD Data3;
	BYTE Data4[8];
} GUID;
typedef GUID IID, CLSID;
typedef GUID *LPGUID;
typedef IID *LPIID;
typedef CLSID *LPCLSID;
typedef const GUID *REFGUID;
typedef const IID *REFIID;
typedef const CLSID *REFCLSID;

typedef WCHAR OLECHAR;
typedef OLECHAR *LPOLESTR;
typedef const OLECHAR *LPCOLESTR;

#define FACILITY_WIN32 7
#define HRESULT_FROM_WIN32(x) \
	((HRESULT)(x) <= 0 ? (HRESULT)(x) : \
	 (HRESULT)(((x) & 0x0000FFFF) | (FACILITY_WIN32 << 16) | 0x80000000))

#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr)    ((HRESULT)(hr) < 0)

#define S_OK                      ((HRESULT)0x00000000)
#define S_FALSE                   ((HRESULT)0x00000001)
#define E_NOTIMPL                 ((HRESULT)0x80004001)
#define E_NOINTERFACE             ((HRESULT)0x80004002)
#define E_POINTER                 ((HRESULT)0x80004003)
#define E_FAIL                    ((HRESULT)0x80004005)
#define E_UNEXPECTED              ((HRESULT)0x8000FFFF)
#define E_OUTOFMEMORY             ((HRESULT)0x8007000E)
#define E_INVALIDARG              ((HRESULT)0x80070057)
#define RPC_E_CHANGED_MODE        ((HRESULT)0x80010106)
#define CLASS_E_NOAGGREGATION     ((HRESULT)0x80040110)
#define CLASS_E_CLASSNOTAVAILABLE ((HRESULT)0x80040111)
#define REGDB_E_READREGDB         ((HRESULT)0x80040150)
#define REGDB_E_CLASSNOTREG       ((HRESULT)0x80040154)
#define CO_E_NOTINITIALIZED       ((HRESULT)0x800401F0)
#define CO_E_CLASSSTRING          ((HRESULT)0x800401F3)
#define CO_E_DLLNOTFOUND          ((HRESULT)0x800401F8)
#define CO_E_ERRORINDLL           ((HRESULT)0x800401F9)
#define CO_E_OBJNOTREG            ((HRESULT)0x800401FB)

/* CLSCTX (an enum in the Windows headers). */
#define CLSCTX_INPROC_SERVER  0x1
#define CLSCTX_INPROC_HANDLER 0x2
#define CLSCTX_LOCAL_SERVER   0x4
#define CLSCTX_REMOTE_SERVER  0x10
#define CLSCTX_INPROC         (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER)
#define CLSCTX_SERVER \
	(CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER | CLSCTX_REMOTE_SERVER)
#define CLSCTX_ALL            (CLSCTX_INPROC_HANDLER | CLSCTX_SERVER)

/* COINIT and REGCLS (enums too). */
#define COINIT_MULTITHREADED     0x0
#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_DISABLE_OLE1DDE   0x4
#define COINIT_SPEED_OVER_MEMORY 0x8

#define REGCLS_SINGLEUSE      0
#define REGCLS_MULTIPLEUSE    1
#define REGCLS_MULTI_SEPARATE 2
#define REGCLS_SUSPENDED      4

typedef struct IUnknown IUnknown;
typedef IUnknown *LPUNKNOWN;
typedef struct IUnknownVtbl {
	HRESULT (WINAPI *QueryInterface)(IUnknown *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IUnknown *This);
	ULONG (WINAPI *Release)(IUnknown *This);
} IUnknownVtbl;
struct IUnknown {
	const IUnknownVtbl *lpVtbl;
};

typedef struct IClassFactory IClassFactory;
typedef IClassFactory *LPCLASSFACTORY;
typedef struct IClassFactoryVtbl {
	HRESULT (WINAPI *QueryInterface)(IClassFactory *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IClassFactory *This);
	ULONG (WINAPI *Release)(IClassFactory *This);
	HRESULT (WINAPI *CreateInstance)(IClassFactory *This,
					 IUnknown *outer, REFIID riid,
					 void **ppv);
	HRESULT (WINAPI *LockServer)(IClassFactory *This, BOOL lock);
} IClassFactoryVtbl;
struct IClassFactory {
	const IClassFactoryVtbl *lpVtbl;
};

typedef struct IMalloc IMalloc;
typedef IMalloc *LPMALLOC;
typedef struct IMallocVtbl {
	HRESULT (WINAPI *QueryInterface)(IMalloc *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMalloc *This);
	ULONG (WINAPI *Release)(IMalloc *This);
	void *(WINAPI *Alloc)(IMalloc *This, SIZE_T cb);
	void *(WINAPI *Realloc)(IMalloc *This, void *pv, SIZE_T cb);
	void (WINAPI *Free)(IMalloc *This, void *pv);
	SIZE_T (WINAPI *GetSize)(IMalloc *This, void *pv);
	int (WINAPI *DidAlloc)(IMalloc *This, void *pv);
	void (WINAPI *HeapMinimize)(IMalloc *This);
} IMallocVtbl;
struct IMalloc {
	const IMallocVtbl *lpVtbl;
};

/* An in-process server's DllGetClassObject. */
typedef HRESULT (WINAPI *LPFNGETCLASSOBJECT)(REFCLSID, REFIID, LPVOID *);

#endif /* VFW_WIN32_OBJBASE_H */
