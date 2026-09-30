/*
 * OLE Automation (oleaut32): BSTRs, VARIANTs, SAFEARRAYs, the date and
 * currency types, and the IDispatch and error-info interfaces they
 * involve (wtypes.h, oaidl.h, oleauto.h, olectl.h).
 */
#ifndef VFW_WIN32_OLEAUTO_H
#define VFW_WIN32_OLEAUTO_H

#include "objbase.h"
#include "types.h"
#include "winbase.h"

/* Windows aligns doubles to 8 bytes in structures, i386 Linux to 4. */
typedef double DOUBLE __attribute__((aligned(8)));
typedef float FLOAT;

typedef OLECHAR *BSTR;
typedef BSTR *LPBSTR;
typedef unsigned short VARTYPE;
typedef short VARIANT_BOOL;
typedef LONG SCODE;
typedef DOUBLE DATE;
typedef LONG DISPID;
typedef DISPID MEMBERID;

#define VARIANT_TRUE  ((VARIANT_BOOL)0xffff)
#define VARIANT_FALSE ((VARIANT_BOOL)0)

/* Currency: a 64-bit integer, scaled by 10000. */
typedef union tagCY {
	struct {
		ULONG Lo;
		LONG Hi;
	};
	LONGLONG int64;
} CY, *LPCY;

typedef struct tagDEC {
	USHORT wReserved;
	union {
		struct {
			BYTE scale;
			BYTE sign;
		};
		USHORT signscale;
	};
	ULONG Hi32;
	union {
		struct {
			ULONG Lo32;
			ULONG Mid32;
		};
		ULONGLONG Lo64;
	};
} DECIMAL, *LPDECIMAL;

/* Variant types. */
#define VT_EMPTY           0
#define VT_NULL            1
#define VT_I2              2
#define VT_I4              3
#define VT_R4              4
#define VT_R8              5
#define VT_CY              6
#define VT_DATE            7
#define VT_BSTR            8
#define VT_DISPATCH        9
#define VT_ERROR           10
#define VT_BOOL            11
#define VT_VARIANT         12
#define VT_UNKNOWN         13
#define VT_DECIMAL         14
#define VT_I1              16
#define VT_UI1             17
#define VT_UI2             18
#define VT_UI4             19
#define VT_I8              20
#define VT_UI8             21
#define VT_INT             22
#define VT_UINT            23
#define VT_VOID            24
#define VT_HRESULT         25
#define VT_PTR             26
#define VT_SAFEARRAY       27
#define VT_CARRAY          28
#define VT_USERDEFINED     29
#define VT_LPSTR           30
#define VT_LPWSTR          31
#define VT_RECORD          36
#define VT_INT_PTR         37
#define VT_UINT_PTR        38
#define VT_FILETIME        64
#define VT_BLOB            65
#define VT_CLSID           72
#define VT_VECTOR          0x1000
#define VT_ARRAY           0x2000
#define VT_BYREF           0x4000
#define VT_RESERVED        0x8000
#define VT_ILLEGAL         0xffff
#define VT_ILLEGALMASKED   0x0fff
#define VT_TYPEMASK        0x0fff

typedef struct tagSAFEARRAYBOUND {
	ULONG cElements;
	LONG lLbound;
} SAFEARRAYBOUND, *LPSAFEARRAYBOUND;

typedef struct tagSAFEARRAY {
	USHORT cDims;
	USHORT fFeatures;
	ULONG cbElements;
	ULONG cLocks;
	PVOID pvData;
	SAFEARRAYBOUND rgsabound[1];
} SAFEARRAY, *LPSAFEARRAY;

/* SAFEARRAY.fFeatures. */
#define FADF_AUTO        0x0001
#define FADF_STATIC      0x0002
#define FADF_EMBEDDED    0x0004
#define FADF_FIXEDSIZE   0x0010
#define FADF_RECORD      0x0020
#define FADF_HAVEIID     0x0040
#define FADF_HAVEVARTYPE 0x0080
#define FADF_BSTR        0x0100
#define FADF_UNKNOWN     0x0200
#define FADF_DISPATCH    0x0400
#define FADF_VARIANT     0x0800
#define FADF_RESERVED    0xf008

typedef struct IDispatch IDispatch;
typedef struct IRecordInfo IRecordInfo;
typedef struct tagVARIANT VARIANT, VARIANTARG, *LPVARIANT, *LPVARIANTARG;

struct tagVARIANT {
	union {
		struct {
			VARTYPE vt;
			WORD wReserved1;
			WORD wReserved2;
			WORD wReserved3;
			union {
				LONGLONG llVal;
				LONG lVal;
				BYTE bVal;
				SHORT iVal;
				FLOAT fltVal;
				DOUBLE dblVal;
				VARIANT_BOOL boolVal;
				SCODE scode;
				CY cyVal;
				DATE date;
				BSTR bstrVal;
				IUnknown *punkVal;
				IDispatch *pdispVal;
				SAFEARRAY *parray;
				BYTE *pbVal;
				SHORT *piVal;
				LONG *plVal;
				LONGLONG *pllVal;
				FLOAT *pfltVal;
				DOUBLE *pdblVal;
				VARIANT_BOOL *pboolVal;
				SCODE *pscode;
				CY *pcyVal;
				DATE *pdate;
				BSTR *pbstrVal;
				IUnknown **ppunkVal;
				IDispatch **ppdispVal;
				SAFEARRAY **pparray;
				VARIANT *pvarVal;
				PVOID byref;
				CHAR cVal;
				USHORT uiVal;
				ULONG ulVal;
				ULONGLONG ullVal;
				INT intVal;
				UINT uintVal;
				DECIMAL *pdecVal;
				CHAR *pcVal;
				USHORT *puiVal;
				ULONG *pulVal;
				ULONGLONG *pullVal;
				INT *pintVal;
				UINT *puintVal;
				struct {
					PVOID pvRecord;
					IRecordInfo *pRecInfo;
				};
			};
		};
		DECIMAL decVal;
	};
};

/* VariantChangeType(Ex) flags, and the Var*From* conversions'. */
#define VARIANT_NOVALUEPROP     0x01
#define VARIANT_ALPHABOOL       0x02
#define VARIANT_NOUSEROVERRIDE  0x04
#define VARIANT_CALENDAR_HIJRI  0x08
#define VARIANT_LOCALBOOL       0x10
#define VAR_TIMEVALUEONLY       0x00000001
#define VAR_DATEVALUEONLY       0x00000002
#define VAR_VALIDDATE           0x00000004
#define VAR_CALENDAR_HIJRI      0x00000008
#define VAR_LOCALBOOL           0x00000010
#define VAR_FORMAT_NOSUBSTITUTE 0x00000020
#define VAR_FOURDIGITYEARS      0x00000040
#define LOCALE_NOUSEROVERRIDE   0x80000000

/* VarBstrCmp and VarCmp. */
#define VARCMP_LT   0
#define VARCMP_EQ   1
#define VARCMP_GT   2
#define VARCMP_NULL 3

/* IDispatch. */
#define DISPID_UNKNOWN     (-1)
#define DISPID_VALUE       0
#define DISPID_PROPERTYPUT (-3)

#define DISPATCH_METHOD         0x1
#define DISPATCH_PROPERTYGET    0x2
#define DISPATCH_PROPERTYPUT    0x4
#define DISPATCH_PROPERTYPUTREF 0x8

typedef struct tagDISPPARAMS {
	VARIANTARG *rgvarg;
	DISPID *rgdispidNamedArgs;
	UINT cArgs;
	UINT cNamedArgs;
} DISPPARAMS;

typedef struct tagEXCEPINFO {
	WORD wCode;
	WORD wReserved;
	BSTR bstrSource;
	BSTR bstrDescription;
	BSTR bstrHelpFile;
	DWORD dwHelpContext;
	PVOID pvReserved;
	HRESULT (WINAPI *pfnDeferredFillIn)(struct tagEXCEPINFO *);
	SCODE scode;
} EXCEPINFO, *LPEXCEPINFO;

typedef struct IDispatchVtbl {
	HRESULT (WINAPI *QueryInterface)(IDispatch *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IDispatch *This);
	ULONG (WINAPI *Release)(IDispatch *This);
	HRESULT (WINAPI *GetTypeInfoCount)(IDispatch *This, UINT *count);
	HRESULT (WINAPI *GetTypeInfo)(IDispatch *This, UINT index, LCID lcid,
				      void **info);
	HRESULT (WINAPI *GetIDsOfNames)(IDispatch *This, REFIID riid,
					LPOLESTR *names, UINT count, LCID lcid,
					DISPID *ids);
	HRESULT (WINAPI *Invoke)(IDispatch *This, DISPID id, REFIID riid,
				 LCID lcid, WORD flags, DISPPARAMS *params,
				 VARIANT *result, EXCEPINFO *excep,
				 UINT *arg_err);
} IDispatchVtbl;
struct IDispatch {
	const IDispatchVtbl *lpVtbl;
};

/* IRecordInfo, as far as copying and clearing records needs it. */
typedef struct IRecordInfoVtbl {
	HRESULT (WINAPI *QueryInterface)(IRecordInfo *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IRecordInfo *This);
	ULONG (WINAPI *Release)(IRecordInfo *This);
	HRESULT (WINAPI *RecordInit)(IRecordInfo *This, PVOID pv);
	HRESULT (WINAPI *RecordClear)(IRecordInfo *This, PVOID pv);
	HRESULT (WINAPI *RecordCopy)(IRecordInfo *This, PVOID from, PVOID to);
	HRESULT (WINAPI *GetGuid)(IRecordInfo *This, GUID *guid);
	HRESULT (WINAPI *GetName)(IRecordInfo *This, BSTR *name);
	HRESULT (WINAPI *GetSize)(IRecordInfo *This, ULONG *size);
} IRecordInfoVtbl;
struct IRecordInfo {
	const IRecordInfoVtbl *lpVtbl;
};

/* Error information (IErrorInfo and ICreateErrorInfo). */
typedef struct IErrorInfo IErrorInfo;
typedef struct IErrorInfoVtbl {
	HRESULT (WINAPI *QueryInterface)(IErrorInfo *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IErrorInfo *This);
	ULONG (WINAPI *Release)(IErrorInfo *This);
	HRESULT (WINAPI *GetGUID)(IErrorInfo *This, GUID *guid);
	HRESULT (WINAPI *GetSource)(IErrorInfo *This, BSTR *source);
	HRESULT (WINAPI *GetDescription)(IErrorInfo *This, BSTR *desc);
	HRESULT (WINAPI *GetHelpFile)(IErrorInfo *This, BSTR *file);
	HRESULT (WINAPI *GetHelpContext)(IErrorInfo *This, DWORD *context);
} IErrorInfoVtbl;
struct IErrorInfo {
	const IErrorInfoVtbl *lpVtbl;
};

typedef struct ICreateErrorInfo ICreateErrorInfo;
typedef struct ICreateErrorInfoVtbl {
	HRESULT (WINAPI *QueryInterface)(ICreateErrorInfo *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ICreateErrorInfo *This);
	ULONG (WINAPI *Release)(ICreateErrorInfo *This);
	HRESULT (WINAPI *SetGUID)(ICreateErrorInfo *This, REFGUID guid);
	HRESULT (WINAPI *SetSource)(ICreateErrorInfo *This, LPOLESTR source);
	HRESULT (WINAPI *SetDescription)(ICreateErrorInfo *This, LPOLESTR desc);
	HRESULT (WINAPI *SetHelpFile)(ICreateErrorInfo *This, LPOLESTR file);
	HRESULT (WINAPI *SetHelpContext)(ICreateErrorInfo *This, DWORD context);
} ICreateErrorInfoVtbl;
struct ICreateErrorInfo {
	const ICreateErrorInfoVtbl *lpVtbl;
};

/* OleCreatePropertyFrameIndirect's parameters (olectl.h). */
typedef struct tagOCPFIPARAMS {
	ULONG cbStructSize;
	HWND hWndOwner;
	int x;
	int y;
	LPCOLESTR lpszCaption;
	ULONG cObjects;
	LPUNKNOWN *lplpUnk;
	ULONG cPages;
	CLSID *lpPages;
	LCID lcid;
	DISPID dispidInitialProperty;
} OCPFIPARAMS, *LPOCPFIPARAMS;

typedef DWORD OLE_COLOR;

/* Errors. */
#define DISP_E_UNKNOWNINTERFACE ((HRESULT)0x80020001)
#define DISP_E_MEMBERNOTFOUND   ((HRESULT)0x80020003)
#define DISP_E_PARAMNOTFOUND    ((HRESULT)0x80020004)
#define DISP_E_TYPEMISMATCH     ((HRESULT)0x80020005)
#define DISP_E_UNKNOWNNAME      ((HRESULT)0x80020006)
#define DISP_E_NONAMEDARGS      ((HRESULT)0x80020007)
#define DISP_E_BADVARTYPE       ((HRESULT)0x80020008)
#define DISP_E_EXCEPTION        ((HRESULT)0x80020009)
#define DISP_E_OVERFLOW         ((HRESULT)0x8002000A)
#define DISP_E_BADINDEX         ((HRESULT)0x8002000B)
#define DISP_E_UNKNOWNLCID      ((HRESULT)0x8002000C)
#define DISP_E_ARRAYISLOCKED    ((HRESULT)0x8002000D)
#define DISP_E_BADPARAMCOUNT    ((HRESULT)0x8002000E)
#define DISP_E_PARAMNOTOPTIONAL ((HRESULT)0x8002000F)
#define DISP_E_BADCALLEE        ((HRESULT)0x80020010)
#define DISP_E_NOTACOLLECTION   ((HRESULT)0x80020011)
#define DISP_E_DIVBYZERO        ((HRESULT)0x80020012)
#define DISP_E_BUFFERTOOSMALL   ((HRESULT)0x80020013)
#define TYPE_E_LIBNOTREGISTERED ((HRESULT)0x8002801D)
#define TYPE_E_CANTLOADLIBRARY  ((HRESULT)0x80029C4A)
#define CTL_E_OVERFLOW          ((HRESULT)0x800A0006)
#define CTL_E_OUTOFMEMORY       ((HRESULT)0x800A0007)
#define CTL_E_ILLEGALFUNCTIONCALL ((HRESULT)0x800A0005)

#endif /* VFW_WIN32_OLEAUTO_H */
