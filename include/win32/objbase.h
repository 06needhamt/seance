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
#include "wingdi.h"

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
	G(IID_IPersistFile, 0x0000010b, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IPersistStream, 0x00000109, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ISequentialStream, 0x0c733a30, 0x2a1c, 0x11ce, \
	  0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d) \
	G(IID_ILockBytes, 0x0000000a, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IStorage, 0x0000000b, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IEnumSTATSTG, 0x0000000d, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IPersistStorage, 0x0000010a, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IBindCtx, 0x0000000e, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IRunningObjectTable, 0x00000010, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IEnumString, 0x00000101, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IParseDisplayName, 0x0000011a, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IOleContainer, 0x0000011b, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IOleItemContainer, 0x0000011c, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IROTData, 0xf29f6bc0, 0x5021, 0x11ce, \
	  0xaa, 0x15, 0x00, 0x00, 0x69, 0x01, 0x29, 0x3f) \
	G(IID_IEnumFORMATETC, 0x00000103, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IEnumOLEVERB, 0x00000104, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IEnumSTATDATA, 0x00000105, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IDataObject, 0x0000010e, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IAdviseSink, 0x0000010f, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IDataAdviseHolder, 0x00000110, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IOleAdviseHolder, 0x00000111, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IOleObject, 0x00000112, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IDropSource, 0x00000121, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IDropTarget, 0x00000122, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IRunnableObject, 0x00000126, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IMessageFilter, 0x00000016, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IExternalConnection, 0x00000019, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IClientSecurity, 0x0000013d, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)

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

#define STGC_OVERWRITE                          1
#define STGC_ONLYIFCURRENT                      2
#define STGC_DANGEROUSLYCOMMITMERELYTODISKCACHE 4

#define STGM_DIRECT           0x00000000
#define STGM_READ             0x00000000
#define STGM_WRITE            0x00000001
#define STGM_READWRITE        0x00000002
#define STGM_SHARE_DENY_NONE  0x00000040
#define STGM_SHARE_DENY_READ  0x00000030
#define STGM_SHARE_DENY_WRITE 0x00000020
#define STGM_SHARE_EXCLUSIVE  0x00000010
#define STGM_PRIORITY         0x00040000
#define STGM_CREATE           0x00001000
#define STGM_CONVERT          0x00020000
#define STGM_FAILIFTHERE      0x00000000
#define STGM_TRANSACTED       0x00010000
#define STGM_NOSCRATCH        0x00100000
#define STGM_NOSNAPSHOT       0x00200000
#define STGM_DIRECT_SWMR      0x00400000
#define STGM_DELETEONRELEASE  0x04000000
#define STGM_SIMPLE           0x08000000

#define STGMOVE_MOVE 0
#define STGMOVE_COPY 1

#define STGFMT_STORAGE  0
#define STGFMT_FILE     3
#define STGFMT_ANY      4
#define STGFMT_DOCFILE  5

/* A clipboard format (wtypes.h). */
typedef WORD CLIPFORMAT;

#define STG_E_INVALIDFUNCTION ((HRESULT)0x80030001)
#define STG_E_INVALIDPOINTER  ((HRESULT)0x80030009)
#define STG_E_MEDIUMFULL      ((HRESULT)0x80030070)
#define STG_E_SEEKERROR       ((HRESULT)0x80030019)
#define STG_E_READFAULT       ((HRESULT)0x8003001E)
#define STG_E_WRITEFAULT      ((HRESULT)0x8003001D)
#define STG_E_FILENOTFOUND       ((HRESULT)0x80030002)
#define STG_E_PATHNOTFOUND       ((HRESULT)0x80030003)
#define STG_E_ACCESSDENIED       ((HRESULT)0x80030005)
#define STG_E_INVALIDHANDLE      ((HRESULT)0x80030006)
#define STG_E_INSUFFICIENTMEMORY ((HRESULT)0x80030008)
#define STG_E_NOMOREFILES        ((HRESULT)0x80030012)
#define STG_E_SHAREVIOLATION     ((HRESULT)0x80030020)
#define STG_E_LOCKVIOLATION      ((HRESULT)0x80030021)
#define STG_E_FILEALREADYEXISTS  ((HRESULT)0x80030050)
#define STG_E_INVALIDPARAMETER   ((HRESULT)0x80030057)
#define STG_E_INVALIDHEADER      ((HRESULT)0x800300FB)
#define STG_E_INVALIDNAME        ((HRESULT)0x800300FC)
#define STG_E_UNIMPLEMENTEDFUNCTION ((HRESULT)0x800300FE)
#define STG_E_INVALIDFLAG        ((HRESULT)0x800300FF)
#define STG_E_REVERTED           ((HRESULT)0x80030102)
#define STG_E_DOCFILECORRUPT     ((HRESULT)0x80030109)
#define STG_S_CONVERTED          ((HRESULT)0x00030200)

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

/* IPersistFile (objidl.h): an object that loads and saves a file. */
typedef struct IPersistFile IPersistFile;
typedef struct IPersistFileVtbl {
	HRESULT (WINAPI *QueryInterface)(IPersistFile *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IPersistFile *This);
	ULONG (WINAPI *Release)(IPersistFile *This);
	HRESULT (WINAPI *GetClassID)(IPersistFile *This, CLSID *clsid);
	HRESULT (WINAPI *IsDirty)(IPersistFile *This);
	HRESULT (WINAPI *Load)(IPersistFile *This, LPCOLESTR name, DWORD mode);
	HRESULT (WINAPI *Save)(IPersistFile *This, LPCOLESTR name,
			       BOOL remember);
	HRESULT (WINAPI *SaveCompleted)(IPersistFile *This, LPCOLESTR name);
	HRESULT (WINAPI *GetCurFile)(IPersistFile *This, LPOLESTR *name);
} IPersistFileVtbl;
struct IPersistFile {
	const IPersistFileVtbl *lpVtbl;
};

/* Structured storage (objidl.h). */
typedef struct ILockBytes ILockBytes;
typedef struct ILockBytesVtbl {
	HRESULT (WINAPI *QueryInterface)(ILockBytes *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ILockBytes *This);
	ULONG (WINAPI *Release)(ILockBytes *This);
	HRESULT (WINAPI *ReadAt)(ILockBytes *This, ULARGE_INTEGER offset,
				 void *pv, ULONG cb, ULONG *read);
	HRESULT (WINAPI *WriteAt)(ILockBytes *This, ULARGE_INTEGER offset,
				  const void *pv, ULONG cb, ULONG *written);
	HRESULT (WINAPI *Flush)(ILockBytes *This);
	HRESULT (WINAPI *SetSize)(ILockBytes *This, ULARGE_INTEGER size);
	HRESULT (WINAPI *LockRegion)(ILockBytes *This, ULARGE_INTEGER offset,
				     ULARGE_INTEGER cb, DWORD type);
	HRESULT (WINAPI *UnlockRegion)(ILockBytes *This, ULARGE_INTEGER offset,
				       ULARGE_INTEGER cb, DWORD type);
	HRESULT (WINAPI *Stat)(ILockBytes *This, STATSTG *stat, DWORD flags);
} ILockBytesVtbl;
struct ILockBytes {
	const ILockBytesVtbl *lpVtbl;
};

typedef struct IEnumSTATSTG IEnumSTATSTG;
typedef struct IEnumSTATSTGVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumSTATSTG *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumSTATSTG *This);
	ULONG (WINAPI *Release)(IEnumSTATSTG *This);
	HRESULT (WINAPI *Next)(IEnumSTATSTG *This, ULONG count, STATSTG *out,
			       ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumSTATSTG *This, ULONG count);
	HRESULT (WINAPI *Reset)(IEnumSTATSTG *This);
	HRESULT (WINAPI *Clone)(IEnumSTATSTG *This, IEnumSTATSTG **clone);
} IEnumSTATSTGVtbl;
struct IEnumSTATSTG {
	const IEnumSTATSTGVtbl *lpVtbl;
};

typedef LPOLESTR *SNB;
typedef struct IStorage IStorage;
typedef IStorage *LPSTORAGE;
typedef struct IStorageVtbl {
	HRESULT (WINAPI *QueryInterface)(IStorage *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IStorage *This);
	ULONG (WINAPI *Release)(IStorage *This);
	HRESULT (WINAPI *CreateStream)(IStorage *This, LPCOLESTR name,
				       DWORD mode, DWORD reserved1,
				       DWORD reserved2, IStream **stream);
	HRESULT (WINAPI *OpenStream)(IStorage *This, LPCOLESTR name,
				     void *reserved1, DWORD mode,
				     DWORD reserved2, IStream **stream);
	HRESULT (WINAPI *CreateStorage)(IStorage *This, LPCOLESTR name,
					DWORD mode, DWORD reserved1,
					DWORD reserved2, IStorage **stg);
	HRESULT (WINAPI *OpenStorage)(IStorage *This, LPCOLESTR name,
				      IStorage *priority, DWORD mode,
				      SNB exclude, DWORD reserved,
				      IStorage **stg);
	HRESULT (WINAPI *CopyTo)(IStorage *This, DWORD n_iid_exclude,
				 const IID *iid_exclude, SNB exclude,
				 IStorage *dest);
	HRESULT (WINAPI *MoveElementTo)(IStorage *This, LPCOLESTR name,
					IStorage *dest, LPCOLESTR new_name,
					DWORD flags);
	HRESULT (WINAPI *Commit)(IStorage *This, DWORD flags);
	HRESULT (WINAPI *Revert)(IStorage *This);
	HRESULT (WINAPI *EnumElements)(IStorage *This, DWORD reserved1,
				       void *reserved2, DWORD reserved3,
				       IEnumSTATSTG **en);
	HRESULT (WINAPI *DestroyElement)(IStorage *This, LPCOLESTR name);
	HRESULT (WINAPI *RenameElement)(IStorage *This, LPCOLESTR old_name,
					LPCOLESTR new_name);
	HRESULT (WINAPI *SetElementTimes)(IStorage *This, LPCOLESTR name,
					  const FILETIME *ctime,
					  const FILETIME *atime,
					  const FILETIME *mtime);
	HRESULT (WINAPI *SetClass)(IStorage *This, REFCLSID clsid);
	HRESULT (WINAPI *SetStateBits)(IStorage *This, DWORD bits, DWORD mask);
	HRESULT (WINAPI *Stat)(IStorage *This, STATSTG *stat, DWORD flags);
} IStorageVtbl;
struct IStorage {
	const IStorageVtbl *lpVtbl;
};

/* IPersistStorage (objidl.h): an object that saves itself to a storage. */
typedef struct IPersistStorage IPersistStorage;
typedef struct IPersistStorageVtbl {
	HRESULT (WINAPI *QueryInterface)(IPersistStorage *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IPersistStorage *This);
	ULONG (WINAPI *Release)(IPersistStorage *This);
	HRESULT (WINAPI *GetClassID)(IPersistStorage *This, CLSID *clsid);
	HRESULT (WINAPI *IsDirty)(IPersistStorage *This);
	HRESULT (WINAPI *InitNew)(IPersistStorage *This, IStorage *stg);
	HRESULT (WINAPI *Load)(IPersistStorage *This, IStorage *stg);
	HRESULT (WINAPI *Save)(IPersistStorage *This, IStorage *stg,
			       BOOL same_as_load);
	HRESULT (WINAPI *SaveCompleted)(IPersistStorage *This, IStorage *stg);
	HRESULT (WINAPI *HandsOffStorage)(IPersistStorage *This);
} IPersistStorageVtbl;
struct IPersistStorage {
	const IPersistStorageVtbl *lpVtbl;
};

/* Monikers, bind contexts and the running object table (objidl.h). */
typedef struct IMoniker IMoniker;
typedef struct IEnumMoniker IEnumMoniker;
typedef struct IBindCtx IBindCtx;
typedef struct IRunningObjectTable IRunningObjectTable;
typedef struct IEnumString IEnumString;

#define MK_E_CONNECTMANUALLY    ((HRESULT)0x800401e0)
#define MK_E_EXCEEDEDDEADLINE   ((HRESULT)0x800401e1)
#define MK_E_NEEDGENERIC        ((HRESULT)0x800401e2)
#define MK_E_UNAVAILABLE        ((HRESULT)0x800401e3)
#define MK_E_SYNTAX             ((HRESULT)0x800401e4)
#define MK_E_NOOBJECT           ((HRESULT)0x800401e5)
#define MK_E_INVALIDEXTENSION   ((HRESULT)0x800401e6)
#define MK_E_INTERMEDIATEINTERFACENOTSUPPORTED ((HRESULT)0x800401e7)
#define MK_E_NOTBINDABLE        ((HRESULT)0x800401e8)
#define MK_E_NOTBOUND           ((HRESULT)0x800401e9)
#define MK_E_CANTOPENFILE       ((HRESULT)0x800401ea)
#define MK_E_MUSTBOTHERUSER     ((HRESULT)0x800401eb)
#define MK_E_NOINVERSE          ((HRESULT)0x800401ec)
#define MK_E_NOSTORAGE          ((HRESULT)0x800401ed)
#define MK_E_NOPREFIX           ((HRESULT)0x800401ee)
#define MK_E_ENUMERATION_FAILED ((HRESULT)0x800401ef)
#define MK_S_REDUCED_TO_SELF    ((HRESULT)0x000401e2)
#define MK_S_ME                 ((HRESULT)0x000401e4)
#define MK_S_HIM                ((HRESULT)0x000401e5)
#define MK_S_US                 ((HRESULT)0x000401e6)
#define MK_S_MONIKERALREADYREGISTERED ((HRESULT)0x000401e7)

#define MKSYS_NONE             0
#define MKSYS_GENERICCOMPOSITE 1
#define MKSYS_FILEMONIKER      2
#define MKSYS_ANTIMONIKER      3
#define MKSYS_ITEMMONIKER      4
#define MKSYS_POINTERMONIKER   5
#define MKSYS_CLASSMONIKER     7
#define MKSYS_OBJREFMONIKER    8

#define MKRREDUCE_ONE         (3 << 16)
#define MKRREDUCE_TOUSER      (2 << 16)
#define MKRREDUCE_THROUGHUSER (1 << 16)
#define MKRREDUCE_ALL         0

#define BINDSPEED_INDEFINITE 1
#define BINDSPEED_MODERATE   2
#define BINDSPEED_IMMEDIATE  3

#define BIND_MAYBOTHERUSER     1
#define BIND_JUSTTESTEXISTENCE 2

#define ROTFLAGS_REGISTRATIONKEEPSALIVE 0x1
#define ROTFLAGS_ALLOWANYCLIENT         0x2

/* CoCreateInstanceEx and CoGetInstanceFrom*: one interface asked for. */
typedef struct tagMULTI_QI {
	const IID *pIID;
	IUnknown *pItf;
	HRESULT hr;
} MULTI_QI;
#define CO_S_NOTALLINTERFACES ((HRESULT)0x00080012)

typedef struct tagBIND_OPTS {
	DWORD cbStruct;
	DWORD grfFlags;
	DWORD grfMode;
	DWORD dwTickCountDeadline;
} BIND_OPTS, *LPBIND_OPTS;

typedef struct tagBIND_OPTS2 {
	DWORD cbStruct;
	DWORD grfFlags;
	DWORD grfMode;
	DWORD dwTickCountDeadline;
	DWORD dwTrackFlags;
	DWORD dwClassContext;
	LCID locale;
	void *pServerInfo;	/* COSERVERINFO * */
} BIND_OPTS2, *LPBIND_OPTS2;

typedef struct IMonikerVtbl {
	HRESULT (WINAPI *QueryInterface)(IMoniker *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMoniker *This);
	ULONG (WINAPI *Release)(IMoniker *This);
	HRESULT (WINAPI *GetClassID)(IMoniker *This, CLSID *clsid);
	HRESULT (WINAPI *IsDirty)(IMoniker *This);
	HRESULT (WINAPI *Load)(IMoniker *This, IStream *stm);
	HRESULT (WINAPI *Save)(IMoniker *This, IStream *stm, BOOL clear_dirty);
	HRESULT (WINAPI *GetSizeMax)(IMoniker *This, ULARGE_INTEGER *size);
	HRESULT (WINAPI *BindToObject)(IMoniker *This, IBindCtx *bc,
				       IMoniker *left, REFIID riid, void **ppv);
	HRESULT (WINAPI *BindToStorage)(IMoniker *This, IBindCtx *bc,
					IMoniker *left, REFIID riid, void **ppv);
	HRESULT (WINAPI *Reduce)(IMoniker *This, IBindCtx *bc, DWORD how_far,
				 IMoniker **left_reduced, IMoniker **reduced);
	HRESULT (WINAPI *ComposeWith)(IMoniker *This, IMoniker *right,
				      BOOL only_if_not_generic,
				      IMoniker **composite);
	HRESULT (WINAPI *Enum)(IMoniker *This, BOOL forward,
			       IEnumMoniker **e);
	HRESULT (WINAPI *IsEqual)(IMoniker *This, IMoniker *other);
	HRESULT (WINAPI *Hash)(IMoniker *This, DWORD *hash);
	HRESULT (WINAPI *IsRunning)(IMoniker *This, IBindCtx *bc,
				    IMoniker *left, IMoniker *newly_running);
	HRESULT (WINAPI *GetTimeOfLastChange)(IMoniker *This, IBindCtx *bc,
					      IMoniker *left, FILETIME *time);
	HRESULT (WINAPI *Inverse)(IMoniker *This, IMoniker **inverse);
	HRESULT (WINAPI *CommonPrefixWith)(IMoniker *This, IMoniker *other,
					   IMoniker **prefix);
	HRESULT (WINAPI *RelativePathTo)(IMoniker *This, IMoniker *other,
					 IMoniker **rel);
	HRESULT (WINAPI *GetDisplayName)(IMoniker *This, IBindCtx *bc,
					 IMoniker *left, LPOLESTR *name);
	HRESULT (WINAPI *ParseDisplayName)(IMoniker *This, IBindCtx *bc,
					   IMoniker *left, LPOLESTR name,
					   ULONG *eaten, IMoniker **out);
	HRESULT (WINAPI *IsSystemMoniker)(IMoniker *This, DWORD *mksys);
} IMonikerVtbl;
struct IMoniker {
	const IMonikerVtbl *lpVtbl;
};

typedef struct IBindCtxVtbl {
	HRESULT (WINAPI *QueryInterface)(IBindCtx *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IBindCtx *This);
	ULONG (WINAPI *Release)(IBindCtx *This);
	HRESULT (WINAPI *RegisterObjectBound)(IBindCtx *This, IUnknown *obj);
	HRESULT (WINAPI *RevokeObjectBound)(IBindCtx *This, IUnknown *obj);
	HRESULT (WINAPI *ReleaseBoundObjects)(IBindCtx *This);
	HRESULT (WINAPI *SetBindOptions)(IBindCtx *This, BIND_OPTS *opts);
	HRESULT (WINAPI *GetBindOptions)(IBindCtx *This, BIND_OPTS *opts);
	HRESULT (WINAPI *GetRunningObjectTable)(IBindCtx *This,
						IRunningObjectTable **rot);
	HRESULT (WINAPI *RegisterObjectParam)(IBindCtx *This, LPOLESTR key,
					      IUnknown *obj);
	HRESULT (WINAPI *GetObjectParam)(IBindCtx *This, LPOLESTR key,
					 IUnknown **obj);
	HRESULT (WINAPI *EnumObjectParam)(IBindCtx *This, IEnumString **e);
	HRESULT (WINAPI *RevokeObjectParam)(IBindCtx *This, LPOLESTR key);
} IBindCtxVtbl;
struct IBindCtx {
	const IBindCtxVtbl *lpVtbl;
};

typedef struct IRunningObjectTableVtbl {
	HRESULT (WINAPI *QueryInterface)(IRunningObjectTable *This,
					 REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(IRunningObjectTable *This);
	ULONG (WINAPI *Release)(IRunningObjectTable *This);
	HRESULT (WINAPI *Register)(IRunningObjectTable *This, DWORD flags,
				   IUnknown *obj, IMoniker *name,
				   DWORD *cookie);
	HRESULT (WINAPI *Revoke)(IRunningObjectTable *This, DWORD cookie);
	HRESULT (WINAPI *IsRunning)(IRunningObjectTable *This,
				    IMoniker *name);
	HRESULT (WINAPI *GetObject)(IRunningObjectTable *This, IMoniker *name,
				    IUnknown **obj);
	HRESULT (WINAPI *NoteChangeTime)(IRunningObjectTable *This,
					 DWORD cookie, FILETIME *time);
	HRESULT (WINAPI *GetTimeOfLastChange)(IRunningObjectTable *This,
					      IMoniker *name, FILETIME *time);
	HRESULT (WINAPI *EnumRunning)(IRunningObjectTable *This,
				      IEnumMoniker **e);
} IRunningObjectTableVtbl;
struct IRunningObjectTable {
	const IRunningObjectTableVtbl *lpVtbl;
};

typedef struct IEnumStringVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumString *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumString *This);
	ULONG (WINAPI *Release)(IEnumString *This);
	HRESULT (WINAPI *Next)(IEnumString *This, ULONG count,
			       LPOLESTR *strings, ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumString *This, ULONG count);
	HRESULT (WINAPI *Reset)(IEnumString *This);
	HRESULT (WINAPI *Clone)(IEnumString *This, IEnumString **clone);
} IEnumStringVtbl;
struct IEnumString {
	const IEnumStringVtbl *lpVtbl;
};

/* oleidl.h: what an item moniker binds through. */
typedef struct IOleItemContainer IOleItemContainer;
typedef struct IOleItemContainerVtbl {
	HRESULT (WINAPI *QueryInterface)(IOleItemContainer *This,
					 REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(IOleItemContainer *This);
	ULONG (WINAPI *Release)(IOleItemContainer *This);
	HRESULT (WINAPI *ParseDisplayName)(IOleItemContainer *This,
					   IBindCtx *bc, LPOLESTR name,
					   ULONG *eaten, IMoniker **out);
	HRESULT (WINAPI *EnumObjects)(IOleItemContainer *This, DWORD flags,
				      void **e);	/* IEnumUnknown ** */
	HRESULT (WINAPI *LockContainer)(IOleItemContainer *This, BOOL lock);
	HRESULT (WINAPI *GetObject)(IOleItemContainer *This, LPOLESTR item,
				    DWORD speed, IBindCtx *bc, REFIID riid,
				    void **ppv);
	HRESULT (WINAPI *GetObjectStorage)(IOleItemContainer *This,
					   LPOLESTR item, IBindCtx *bc,
					   REFIID riid, void **ppv);
	HRESULT (WINAPI *IsRunning)(IOleItemContainer *This, LPOLESTR item);
} IOleItemContainerVtbl;
struct IOleItemContainer {
	const IOleItemContainerVtbl *lpVtbl;
};

/* IParseDisplayName is the first method of IOleItemContainer. */
typedef struct IParseDisplayName IParseDisplayName;
typedef struct IParseDisplayNameVtbl {
	HRESULT (WINAPI *QueryInterface)(IParseDisplayName *This,
					 REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(IParseDisplayName *This);
	ULONG (WINAPI *Release)(IParseDisplayName *This);
	HRESULT (WINAPI *ParseDisplayName)(IParseDisplayName *This,
					   IBindCtx *bc, LPOLESTR name,
					   ULONG *eaten, IMoniker **out);
} IParseDisplayNameVtbl;
struct IParseDisplayName {
	const IParseDisplayNameVtbl *lpVtbl;
};

typedef struct IROTData IROTData;
typedef struct IROTDataVtbl {
	HRESULT (WINAPI *QueryInterface)(IROTData *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IROTData *This);
	ULONG (WINAPI *Release)(IROTData *This);
	HRESULT (WINAPI *GetComparisonData)(IROTData *This, BYTE *data,
					    ULONG max, ULONG *size);
} IROTDataVtbl;
struct IROTData {
	const IROTDataVtbl *lpVtbl;
};
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

/* ---- uniform data transfer (objidl.h) ---- */

typedef HANDLE HMETAFILEPICT;

#define TYMED_HGLOBAL  1
#define TYMED_FILE     2
#define TYMED_ISTREAM  4
#define TYMED_ISTORAGE 8
#define TYMED_GDI      16
#define TYMED_MFPICT   32
#define TYMED_ENHMF    64
#define TYMED_NULL     0

#define DVASPECT_CONTENT   1
#define DVASPECT_THUMBNAIL 2
#define DVASPECT_ICON      4
#define DVASPECT_DOCPRINT  8

#define DATADIR_GET 1
#define DATADIR_SET 2

#define ADVF_NODATA            1
#define ADVF_PRIMEFIRST        2
#define ADVF_ONLYONCE          4
#define ADVF_DATAONSTOP        64
#define ADVFCACHE_NOHANDLER    8
#define ADVFCACHE_FORCEBUILTIN 16
#define ADVFCACHE_ONSAVE       32

#define USERCLASSTYPE_FULL    1
#define USERCLASSTYPE_SHORT   2
#define USERCLASSTYPE_APPNAME 3

#define DROPEFFECT_NONE   0
#define DROPEFFECT_COPY   1
#define DROPEFFECT_MOVE   2
#define DROPEFFECT_LINK   4

#define COWAIT_WAITALL   1
#define COWAIT_ALERTABLE 2

#define DV_E_FORMATETC          ((HRESULT)0x80040064)
#define DV_E_TYMED              ((HRESULT)0x80040069)
#define OLE_E_ADVISENOTSUPPORTED ((HRESULT)0x80040003)
#define OLE_E_NOCONNECTION      ((HRESULT)0x80040004)
#define OLE_E_NOTRUNNING        ((HRESULT)0x80040005)
#define OLE_S_USEREG            ((HRESULT)0x00040000)
#define OLEOBJ_E_NOVERBS        ((HRESULT)0x80040180)
#define REGDB_E_KEYMISSING      ((HRESULT)0x80040152)
#define REGDB_E_READREGDB       ((HRESULT)0x80040150)
#define REGDB_E_WRITEREGDB      ((HRESULT)0x80040151)
#define CLIPBRD_E_CANT_OPEN     ((HRESULT)0x800401d0)
#define CLIPBRD_E_CANT_EMPTY    ((HRESULT)0x800401d1)
#define CLIPBRD_E_CANT_SET      ((HRESULT)0x800401d2)
#define CLIPBRD_E_BAD_DATA      ((HRESULT)0x800401d3)
#define CLIPBRD_E_CANT_CLOSE    ((HRESULT)0x800401d4)
#define DRAGDROP_E_NOTREGISTERED     ((HRESULT)0x80040100)
#define DRAGDROP_E_ALREADYREGISTERED ((HRESULT)0x80040101)
#define DRAGDROP_E_INVALIDHWND       ((HRESULT)0x80040102)
#define DRAGDROP_S_DROP         ((HRESULT)0x00040100)
#define DRAGDROP_S_CANCEL       ((HRESULT)0x00040101)
#define DRAGDROP_S_USEDEFAULTCURSORS ((HRESULT)0x00040102)
#define RPC_E_TOO_LATE          ((HRESULT)0x80010119)
#define RPC_E_CALL_COMPLETE     ((HRESULT)0x80010117)
#define RPC_S_CALLPENDING       ((HRESULT)0x80010115)

typedef struct tagDVTARGETDEVICE {
	DWORD tdSize;
	WORD tdDriverNameOffset;
	WORD tdDeviceNameOffset;
	WORD tdPortNameOffset;
	WORD tdExtDevmodeOffset;
	BYTE tdData[1];
} DVTARGETDEVICE;

typedef struct tagFORMATETC {
	CLIPFORMAT cfFormat;
	DVTARGETDEVICE *ptd;
	DWORD dwAspect;
	LONG lindex;
	DWORD tymed;
} FORMATETC, *LPFORMATETC;

typedef struct tagSTGMEDIUM {
	DWORD tymed;
	union {
		HBITMAP hBitmap;
		HMETAFILEPICT hMetaFilePict;
		HANDLE hEnhMetaFile;
		HGLOBAL hGlobal;
		LPOLESTR lpszFileName;
		IStream *pstm;
		IStorage *pstg;
	};
	IUnknown *pUnkForRelease;
} STGMEDIUM, *LPSTGMEDIUM;

typedef struct IAdviseSink IAdviseSink;
typedef struct IDataObject IDataObject;
typedef struct IEnumFORMATETC IEnumFORMATETC;
typedef struct IEnumSTATDATA IEnumSTATDATA;

typedef struct tagSTATDATA {
	FORMATETC formatetc;
	DWORD advf;
	IAdviseSink *pAdvSink;
	DWORD dwConnection;
} STATDATA;

typedef struct IAdviseSinkVtbl {
	HRESULT (WINAPI *QueryInterface)(IAdviseSink *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IAdviseSink *This);
	ULONG (WINAPI *Release)(IAdviseSink *This);
	void (WINAPI *OnDataChange)(IAdviseSink *This, FORMATETC *fmt,
				    STGMEDIUM *med);
	void (WINAPI *OnViewChange)(IAdviseSink *This, DWORD aspect,
				    LONG index);
	void (WINAPI *OnRename)(IAdviseSink *This, IMoniker *mk);
	void (WINAPI *OnSave)(IAdviseSink *This);
	void (WINAPI *OnClose)(IAdviseSink *This);
} IAdviseSinkVtbl;
struct IAdviseSink {
	const IAdviseSinkVtbl *lpVtbl;
};

typedef struct IEnumFORMATETCVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumFORMATETC *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumFORMATETC *This);
	ULONG (WINAPI *Release)(IEnumFORMATETC *This);
	HRESULT (WINAPI *Next)(IEnumFORMATETC *This, ULONG count,
			       FORMATETC *out, ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumFORMATETC *This, ULONG count);
	HRESULT (WINAPI *Reset)(IEnumFORMATETC *This);
	HRESULT (WINAPI *Clone)(IEnumFORMATETC *This, IEnumFORMATETC **clone);
} IEnumFORMATETCVtbl;
struct IEnumFORMATETC {
	const IEnumFORMATETCVtbl *lpVtbl;
};

typedef struct IEnumSTATDATAVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumSTATDATA *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumSTATDATA *This);
	ULONG (WINAPI *Release)(IEnumSTATDATA *This);
	HRESULT (WINAPI *Next)(IEnumSTATDATA *This, ULONG count,
			       STATDATA *out, ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumSTATDATA *This, ULONG count);
	HRESULT (WINAPI *Reset)(IEnumSTATDATA *This);
	HRESULT (WINAPI *Clone)(IEnumSTATDATA *This, IEnumSTATDATA **clone);
} IEnumSTATDATAVtbl;
struct IEnumSTATDATA {
	const IEnumSTATDATAVtbl *lpVtbl;
};

typedef struct IDataObjectVtbl {
	HRESULT (WINAPI *QueryInterface)(IDataObject *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IDataObject *This);
	ULONG (WINAPI *Release)(IDataObject *This);
	HRESULT (WINAPI *GetData)(IDataObject *This, FORMATETC *fmt,
				  STGMEDIUM *med);
	HRESULT (WINAPI *GetDataHere)(IDataObject *This, FORMATETC *fmt,
				      STGMEDIUM *med);
	HRESULT (WINAPI *QueryGetData)(IDataObject *This, FORMATETC *fmt);
	HRESULT (WINAPI *GetCanonicalFormatEtc)(IDataObject *This,
						FORMATETC *in, FORMATETC *out);
	HRESULT (WINAPI *SetData)(IDataObject *This, FORMATETC *fmt,
				  STGMEDIUM *med, BOOL release);
	HRESULT (WINAPI *EnumFormatEtc)(IDataObject *This, DWORD dir,
					IEnumFORMATETC **e);
	HRESULT (WINAPI *DAdvise)(IDataObject *This, FORMATETC *fmt,
				  DWORD advf, IAdviseSink *sink, DWORD *conn);
	HRESULT (WINAPI *DUnadvise)(IDataObject *This, DWORD conn);
	HRESULT (WINAPI *EnumDAdvise)(IDataObject *This, IEnumSTATDATA **e);
} IDataObjectVtbl;
struct IDataObject {
	const IDataObjectVtbl *lpVtbl;
};

/* ---- OLE documents (oleidl.h), as far as ole32 itself calls them ---- */

typedef struct IOleAdviseHolder IOleAdviseHolder;
typedef struct IOleAdviseHolderVtbl {
	HRESULT (WINAPI *QueryInterface)(IOleAdviseHolder *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IOleAdviseHolder *This);
	ULONG (WINAPI *Release)(IOleAdviseHolder *This);
	HRESULT (WINAPI *Advise)(IOleAdviseHolder *This, IAdviseSink *sink,
				 DWORD *conn);
	HRESULT (WINAPI *Unadvise)(IOleAdviseHolder *This, DWORD conn);
	HRESULT (WINAPI *EnumAdvise)(IOleAdviseHolder *This,
				     IEnumSTATDATA **e);
	HRESULT (WINAPI *SendOnRename)(IOleAdviseHolder *This, IMoniker *mk);
	HRESULT (WINAPI *SendOnSave)(IOleAdviseHolder *This);
	HRESULT (WINAPI *SendOnClose)(IOleAdviseHolder *This);
} IOleAdviseHolderVtbl;
struct IOleAdviseHolder {
	const IOleAdviseHolderVtbl *lpVtbl;
};

typedef struct IDataAdviseHolder IDataAdviseHolder;
typedef struct IDataAdviseHolderVtbl {
	HRESULT (WINAPI *QueryInterface)(IDataAdviseHolder *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IDataAdviseHolder *This);
	ULONG (WINAPI *Release)(IDataAdviseHolder *This);
	HRESULT (WINAPI *Advise)(IDataAdviseHolder *This, IDataObject *data,
				 FORMATETC *fmt, DWORD advf,
				 IAdviseSink *sink, DWORD *conn);
	HRESULT (WINAPI *Unadvise)(IDataAdviseHolder *This, DWORD conn);
	HRESULT (WINAPI *EnumAdvise)(IDataAdviseHolder *This,
				     IEnumSTATDATA **e);
	HRESULT (WINAPI *SendOnDataChange)(IDataAdviseHolder *This,
					   IDataObject *data, DWORD reserved,
					   DWORD advf);
} IDataAdviseHolderVtbl;
struct IDataAdviseHolder {
	const IDataAdviseHolderVtbl *lpVtbl;
};

typedef struct IRunnableObject IRunnableObject;
typedef struct IRunnableObjectVtbl {
	HRESULT (WINAPI *QueryInterface)(IRunnableObject *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IRunnableObject *This);
	ULONG (WINAPI *Release)(IRunnableObject *This);
	HRESULT (WINAPI *GetRunningClass)(IRunnableObject *This, CLSID *clsid);
	HRESULT (WINAPI *Run)(IRunnableObject *This, IBindCtx *bc);
	BOOL (WINAPI *IsRunning)(IRunnableObject *This);
	HRESULT (WINAPI *LockRunning)(IRunnableObject *This, BOOL lock,
				      BOOL last_unlock_closes);
	HRESULT (WINAPI *SetContainedObject)(IRunnableObject *This,
					     BOOL contained);
} IRunnableObjectVtbl;
struct IRunnableObject {
	const IRunnableObjectVtbl *lpVtbl;
};

typedef struct tagINTERFACEINFO {
	IUnknown *pUnk;
	IID iid;
	WORD wMethod;
} INTERFACEINFO, *LPINTERFACEINFO;

typedef struct IMessageFilter IMessageFilter;
typedef struct IMessageFilterVtbl {
	HRESULT (WINAPI *QueryInterface)(IMessageFilter *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMessageFilter *This);
	ULONG (WINAPI *Release)(IMessageFilter *This);
	DWORD (WINAPI *HandleInComingCall)(IMessageFilter *This, DWORD type,
					   HANDLE caller, DWORD tick,
					   LPINTERFACEINFO info);
	DWORD (WINAPI *RetryRejectedCall)(IMessageFilter *This, HANDLE callee,
					  DWORD elapsed, DWORD reject);
	DWORD (WINAPI *MessagePending)(IMessageFilter *This, HANDLE callee,
				       DWORD elapsed, DWORD pending);
} IMessageFilterVtbl;
struct IMessageFilter {
	const IMessageFilterVtbl *lpVtbl;
};

#define EXTCONN_STRONG 1

typedef struct IExternalConnection IExternalConnection;
typedef struct IExternalConnectionVtbl {
	HRESULT (WINAPI *QueryInterface)(IExternalConnection *This,
					 REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(IExternalConnection *This);
	ULONG (WINAPI *Release)(IExternalConnection *This);
	DWORD (WINAPI *AddConnection)(IExternalConnection *This, DWORD type,
				      DWORD reserved);
	DWORD (WINAPI *ReleaseConnection)(IExternalConnection *This,
					  DWORD type, DWORD reserved,
					  BOOL last_release_closes);
} IExternalConnectionVtbl;
struct IExternalConnection {
	const IExternalConnectionVtbl *lpVtbl;
};

typedef struct IDropTarget IDropTarget;
typedef struct IDropTargetVtbl {
	HRESULT (WINAPI *QueryInterface)(IDropTarget *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IDropTarget *This);
	ULONG (WINAPI *Release)(IDropTarget *This);
	HRESULT (WINAPI *DragEnter)(IDropTarget *This, IDataObject *data,
				    DWORD keys, POINTL pt, DWORD *effect);
	HRESULT (WINAPI *DragOver)(IDropTarget *This, DWORD keys, POINTL pt,
				   DWORD *effect);
	HRESULT (WINAPI *DragLeave)(IDropTarget *This);
	HRESULT (WINAPI *Drop)(IDropTarget *This, IDataObject *data,
			       DWORD keys, POINTL pt, DWORD *effect);
} IDropTargetVtbl;
struct IDropTarget {
	const IDropTargetVtbl *lpVtbl;
};

typedef struct IDropSource IDropSource;
typedef struct IDropSourceVtbl {
	HRESULT (WINAPI *QueryInterface)(IDropSource *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IDropSource *This);
	ULONG (WINAPI *Release)(IDropSource *This);
	HRESULT (WINAPI *QueryContinueDrag)(IDropSource *This, BOOL escape,
					    DWORD keys);
	HRESULT (WINAPI *GiveFeedback)(IDropSource *This, DWORD effect);
} IDropSourceVtbl;
struct IDropSource {
	const IDropSourceVtbl *lpVtbl;
};

typedef struct tagOLEVERB {
	LONG lVerb;
	LPOLESTR lpszVerbName;
	DWORD fuFlags;
	DWORD grfAttribs;
} OLEVERB, *LPOLEVERB;

typedef struct IEnumOLEVERB IEnumOLEVERB;
typedef struct IEnumOLEVERBVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumOLEVERB *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumOLEVERB *This);
	ULONG (WINAPI *Release)(IEnumOLEVERB *This);
	HRESULT (WINAPI *Next)(IEnumOLEVERB *This, ULONG count, OLEVERB *out,
			       ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumOLEVERB *This, ULONG count);
	HRESULT (WINAPI *Reset)(IEnumOLEVERB *This);
	HRESULT (WINAPI *Clone)(IEnumOLEVERB *This, IEnumOLEVERB **clone);
} IEnumOLEVERBVtbl;
struct IEnumOLEVERB {
	const IEnumOLEVERBVtbl *lpVtbl;
};

typedef struct tagOleMenuGroupWidths {
	LONG width[6];
} OLEMENUGROUPWIDTHS, *LPOLEMENUGROUPWIDTHS;
typedef HGLOBAL HOLEMENU;

#endif /* VFW_WIN32_OBJBASE_H */
