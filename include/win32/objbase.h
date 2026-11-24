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
