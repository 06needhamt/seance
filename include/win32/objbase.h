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

/*
 * Lists of GUIDs: G(name, Data1, Data2, Data3, then the eight bytes of
 * Data4). VFW_GUID_DECLARE turns a list into extern declarations; the
 * objects are defined in src/win32/guids.c, and check_abi compares every
 * value with the Windows headers.
 */
#define VFW_GUID_DECLARE(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
	extern const GUID name;

#define VFW_OBJBASE_GUIDS(G) \
	G(IID_IUnknown, 0x00000000, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IClassFactory, 0x00000001, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IMalloc, 0x00000002, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IStream, 0x0000000c, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IMoniker, 0x0000000f, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IEnumMoniker, 0x00000102, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IPersist, 0x0000010c, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IPersistStream, 0x00000109, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ISequentialStream, 0x0c733a30, 0x2a1c, 0x11ce, \
	  0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d)

VFW_OBJBASE_GUIDS(VFW_GUID_DECLARE)

/* IPersist (objidl.h): an object that can name its class. */
typedef struct IPersist IPersist;
typedef struct IPersistVtbl {
	HRESULT (WINAPI *QueryInterface)(IPersist *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IPersist *This);
	ULONG (WINAPI *Release)(IPersist *This);
	HRESULT (WINAPI *GetClassID)(IPersist *This, CLSID *clsid);
} IPersistVtbl;
struct IPersist {
	const IPersistVtbl *lpVtbl;
};

/* Streams (objidlbase.h). */
#define STREAM_SEEK_SET 0
#define STREAM_SEEK_CUR 1
#define STREAM_SEEK_END 2

#define STGTY_STORAGE   1
#define STGTY_STREAM    2
#define STGTY_LOCKBYTES 3
#define STGTY_PROPERTY  4

#define STATFLAG_DEFAULT 0
#define STATFLAG_NONAME  1
#define STATFLAG_NOOPEN  2

#define STGC_DEFAULT 0

#define STG_E_INVALIDFUNCTION ((HRESULT)0x80030001)
#define STG_E_INVALIDPOINTER  ((HRESULT)0x80030009)
#define STG_E_MEDIUMFULL      ((HRESULT)0x80030070)
#define STG_E_READFAULT       ((HRESULT)0x8003001E)
#define STG_E_WRITEFAULT      ((HRESULT)0x8003001D)

typedef struct tagSTATSTG {
	LPOLESTR pwcsName;
	DWORD type;
	ULARGE_INTEGER cbSize;
	FILETIME mtime;
	FILETIME ctime;
	FILETIME atime;
	DWORD grfMode;
	DWORD grfLocksSupported;
	CLSID clsid;
	DWORD grfStateBits;
	DWORD reserved;
} STATSTG;

typedef struct ISequentialStream ISequentialStream;
typedef struct ISequentialStreamVtbl {
	HRESULT (WINAPI *QueryInterface)(ISequentialStream *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ISequentialStream *This);
	ULONG (WINAPI *Release)(ISequentialStream *This);
	HRESULT (WINAPI *Read)(ISequentialStream *This, void *pv, ULONG cb,
			       ULONG *read);
	HRESULT (WINAPI *Write)(ISequentialStream *This, const void *pv,
				ULONG cb, ULONG *written);
} ISequentialStreamVtbl;
struct ISequentialStream {
	const ISequentialStreamVtbl *lpVtbl;
};

/* 64-bit offsets and sizes are passed by value, as two stack words. */
typedef struct IStream IStream;
typedef IStream *LPSTREAM;
typedef struct IStreamVtbl {
	HRESULT (WINAPI *QueryInterface)(IStream *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IStream *This);
	ULONG (WINAPI *Release)(IStream *This);
	HRESULT (WINAPI *Read)(IStream *This, void *pv, ULONG cb,
			       ULONG *read);
	HRESULT (WINAPI *Write)(IStream *This, const void *pv, ULONG cb,
				ULONG *written);
	HRESULT (WINAPI *Seek)(IStream *This, LARGE_INTEGER move,
			       DWORD origin, ULARGE_INTEGER *pos);
	HRESULT (WINAPI *SetSize)(IStream *This, ULARGE_INTEGER size);
	HRESULT (WINAPI *CopyTo)(IStream *This, IStream *to, ULARGE_INTEGER cb,
				 ULARGE_INTEGER *read, ULARGE_INTEGER *written);
	HRESULT (WINAPI *Commit)(IStream *This, DWORD flags);
	HRESULT (WINAPI *Revert)(IStream *This);
	HRESULT (WINAPI *LockRegion)(IStream *This, ULARGE_INTEGER offset,
				     ULARGE_INTEGER cb, DWORD type);
	HRESULT (WINAPI *UnlockRegion)(IStream *This, ULARGE_INTEGER offset,
				       ULARGE_INTEGER cb, DWORD type);
	HRESULT (WINAPI *Stat)(IStream *This, STATSTG *stat, DWORD flags);
	HRESULT (WINAPI *Clone)(IStream *This, IStream **clone);
} IStreamVtbl;
struct IStream {
	const IStreamVtbl *lpVtbl;
};

/* IPersistStream (objidl.h): an object that saves itself to a stream. */
typedef struct IPersistStream IPersistStream;
typedef struct IPersistStreamVtbl {
	HRESULT (WINAPI *QueryInterface)(IPersistStream *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IPersistStream *This);
	ULONG (WINAPI *Release)(IPersistStream *This);
	HRESULT (WINAPI *GetClassID)(IPersistStream *This, CLSID *clsid);
	HRESULT (WINAPI *IsDirty)(IPersistStream *This);
	HRESULT (WINAPI *Load)(IPersistStream *This, IStream *stream);
	HRESULT (WINAPI *Save)(IPersistStream *This, IStream *stream,
			       BOOL clear_dirty);
	HRESULT (WINAPI *GetSizeMax)(IPersistStream *This,
				     ULARGE_INTEGER *size);
} IPersistStreamVtbl;
struct IPersistStream {
	const IPersistStreamVtbl *lpVtbl;
};

/*
 * Monikers (objidl.h) only appear as what an enumerator hands out, so
 * IMoniker is left opaque.
 */
typedef struct IMoniker IMoniker;
typedef struct IEnumMoniker IEnumMoniker;
typedef struct IEnumMonikerVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumMoniker *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumMoniker *This);
	ULONG (WINAPI *Release)(IEnumMoniker *This);
	HRESULT (WINAPI *Next)(IEnumMoniker *This, ULONG count,
			       IMoniker **monikers, ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumMoniker *This, ULONG count);
	HRESULT (WINAPI *Reset)(IEnumMoniker *This);
	HRESULT (WINAPI *Clone)(IEnumMoniker *This, IEnumMoniker **clone);
} IEnumMonikerVtbl;
struct IEnumMoniker {
	const IEnumMonikerVtbl *lpVtbl;
};

#endif /* VFW_WIN32_OBJBASE_H */
