/* SPDX-License-Identifier: GPL-2.0-only */
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

/* VarParseNumFromStr and VarNumFromParseNum. */
typedef struct tagNUMPARSE {
	INT cDig;
	ULONG dwInFlags;
	ULONG dwOutFlags;
	INT cchUsed;
	INT nBaseShift;
	INT nPwr10;
} NUMPARSE;

#define NUMPRS_LEADING_WHITE  0x00001
#define NUMPRS_TRAILING_WHITE 0x00002
#define NUMPRS_LEADING_PLUS   0x00004
#define NUMPRS_TRAILING_PLUS  0x00008
#define NUMPRS_LEADING_MINUS  0x00010
#define NUMPRS_TRAILING_MINUS 0x00020
#define NUMPRS_HEX_OCT        0x00040
#define NUMPRS_PARENS         0x00080
#define NUMPRS_DECIMAL        0x00100
#define NUMPRS_THOUSANDS      0x00200
#define NUMPRS_CURRENCY       0x00400
#define NUMPRS_EXPONENT       0x00800
#define NUMPRS_USE_ALL        0x01000
#define NUMPRS_STD            0x01fff
#define NUMPRS_NEG            0x10000
#define NUMPRS_INEXACT        0x20000

#define VTBIT_I1      (1 << VT_I1)
#define VTBIT_UI1     (1 << VT_UI1)
#define VTBIT_I2      (1 << VT_I2)
#define VTBIT_UI2     (1 << VT_UI2)
#define VTBIT_I4      (1 << VT_I4)
#define VTBIT_UI4     (1 << VT_UI4)
#define VTBIT_I8      (1 << VT_I8)
#define VTBIT_UI8     (1 << VT_UI8)
#define VTBIT_R4      (1 << VT_R4)
#define VTBIT_R8      (1 << VT_R8)
#define VTBIT_CY      (1 << VT_CY)
#define VTBIT_DECIMAL (1 << VT_DECIMAL)

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

/* Type libraries (oaidl.h): their descriptions and interfaces. */
typedef DWORD HREFTYPE;
/*
 * ITypeInfo is ITypeInfo2 and ITypeLib is ITypeLib2 here: the first is
 * the start of the second, and seance's objects implement both.
 */
typedef struct ITypeInfo2 ITypeInfo;
typedef struct ITypeInfo2 ITypeInfo2;
typedef struct ITypeLib2 ITypeLib;
typedef struct ITypeLib2 ITypeLib2;
typedef struct ITypeComp ITypeComp;

typedef enum tagTYPEKIND {
	TKIND_ENUM, TKIND_RECORD, TKIND_MODULE, TKIND_INTERFACE,
	TKIND_DISPATCH, TKIND_COCLASS, TKIND_ALIAS, TKIND_UNION, TKIND_MAX
} TYPEKIND;

typedef enum tagSYSKIND {
	SYS_WIN16, SYS_WIN32, SYS_MAC, SYS_WIN64
} SYSKIND;

typedef enum tagFUNCKIND {
	FUNC_VIRTUAL, FUNC_PUREVIRTUAL, FUNC_NONVIRTUAL, FUNC_STATIC,
	FUNC_DISPATCH
} FUNCKIND;

typedef enum tagINVOKEKIND {
	INVOKE_FUNC = 1, INVOKE_PROPERTYGET = 2, INVOKE_PROPERTYPUT = 4,
	INVOKE_PROPERTYPUTREF = 8
} INVOKEKIND;

typedef enum tagCALLCONV {
	CC_FASTCALL, CC_CDECL, CC_MSCPASCAL, CC_PASCAL = CC_MSCPASCAL,
	CC_MACPASCAL, CC_STDCALL, CC_FPFASTCALL, CC_SYSCALL, CC_MPWCDECL,
	CC_MPWPASCAL, CC_MAX
} CALLCONV;

typedef enum tagVARKIND {
	VAR_PERINSTANCE, VAR_STATIC, VAR_CONST, VAR_DISPATCH
} VARKIND;

typedef enum tagDESCKIND {
	DESCKIND_NONE, DESCKIND_FUNCDESC, DESCKIND_VARDESC,
	DESCKIND_TYPECOMP, DESCKIND_IMPLICITAPPOBJ, DESCKIND_MAX
} DESCKIND;

typedef struct tagTYPEDESC {
	union {
		struct tagTYPEDESC *lptdesc;
		struct tagARRAYDESC *lpadesc;
		HREFTYPE hreftype;
	};
	VARTYPE vt;
} TYPEDESC;

typedef struct tagARRAYDESC {
	TYPEDESC tdescElem;
	USHORT cDims;
	SAFEARRAYBOUND rgbounds[1];
} ARRAYDESC;

typedef struct tagPARAMDESCEX {
	ULONG cBytes;
	VARIANTARG varDefaultValue;
} PARAMDESCEX, *LPPARAMDESCEX;

typedef struct tagPARAMDESC {
	LPPARAMDESCEX pparamdescex;
	USHORT wParamFlags;
} PARAMDESC;

typedef struct tagIDLDESC {
	ULONG_PTR dwReserved;
	USHORT wIDLFlags;
} IDLDESC;

typedef struct tagELEMDESC {
	TYPEDESC tdesc;
	union {
		IDLDESC idldesc;
		PARAMDESC paramdesc;
	};
} ELEMDESC;

typedef struct tagTYPEATTR {
	GUID guid;
	LCID lcid;
	DWORD dwReserved;
	MEMBERID memidConstructor;
	MEMBERID memidDestructor;
	LPOLESTR lpstrSchema;
	ULONG cbSizeInstance;
	TYPEKIND typekind;
	WORD cFuncs;
	WORD cVars;
	WORD cImplTypes;
	WORD cbSizeVft;
	WORD cbAlignment;
	WORD wTypeFlags;
	WORD wMajorVerNum;
	WORD wMinorVerNum;
	TYPEDESC tdescAlias;
	IDLDESC idldescType;
} TYPEATTR;

typedef struct tagFUNCDESC {
	MEMBERID memid;
	SCODE *lprgscode;
	ELEMDESC *lprgelemdescParam;
	FUNCKIND funckind;
	INVOKEKIND invkind;
	CALLCONV callconv;
	SHORT cParams;
	SHORT cParamsOpt;
	SHORT oVft;
	SHORT cScodes;
	ELEMDESC elemdescFunc;
	WORD wFuncFlags;
} FUNCDESC;

typedef struct tagVARDESC {
	MEMBERID memid;
	LPOLESTR lpstrSchema;
	union {
		ULONG oInst;
		VARIANT *lpvarValue;
	};
	ELEMDESC elemdescVar;
	WORD wVarFlags;
	VARKIND varkind;
} VARDESC;

typedef struct tagTLIBATTR {
	GUID guid;
	LCID lcid;
	SYSKIND syskind;
	WORD wMajorVerNum;
	WORD wMinorVerNum;
	WORD wLibFlags;
} TLIBATTR;

typedef union tagBINDPTR {
	FUNCDESC *lpfuncdesc;
	VARDESC *lpvardesc;
	ITypeComp *lptcomp;
} BINDPTR;

typedef struct tagCUSTDATAITEM {
	GUID guid;
	VARIANTARG varValue;
} CUSTDATAITEM;

typedef struct tagCUSTDATA {
	DWORD cCustData;
	CUSTDATAITEM *prgCustData;
} CUSTDATA;

/* TYPEATTR.wTypeFlags. */
#define TYPEFLAG_FAPPOBJECT     0x0001
#define TYPEFLAG_FCANCREATE     0x0002
#define TYPEFLAG_FLICENSED      0x0004
#define TYPEFLAG_FPREDECLID     0x0008
#define TYPEFLAG_FHIDDEN        0x0010
#define TYPEFLAG_FCONTROL       0x0020
#define TYPEFLAG_FDUAL          0x0040
#define TYPEFLAG_FNONEXTENSIBLE 0x0080
#define TYPEFLAG_FOLEAUTOMATION 0x0100
#define TYPEFLAG_FRESTRICTED    0x0200
#define TYPEFLAG_FAGGREGATABLE  0x0400
#define TYPEFLAG_FREPLACEABLE   0x0800
#define TYPEFLAG_FDISPATCHABLE  0x1000
#define TYPEFLAG_FREVERSEBIND   0x2000
#define TYPEFLAG_FPROXY         0x4000

/* PARAMDESC.wParamFlags. */
#define PARAMFLAG_NONE         0x00
#define PARAMFLAG_FIN          0x01
#define PARAMFLAG_FOUT         0x02
#define PARAMFLAG_FLCID        0x04
#define PARAMFLAG_FRETVAL      0x08
#define PARAMFLAG_FOPT         0x10
#define PARAMFLAG_FHASDEFAULT  0x20
#define PARAMFLAG_FHASCUSTDATA 0x40

/* IMPLTYPEFLAGs. */
#define IMPLTYPEFLAG_FDEFAULT       0x1
#define IMPLTYPEFLAG_FSOURCE        0x2
#define IMPLTYPEFLAG_FRESTRICTED    0x4
#define IMPLTYPEFLAG_FDEFAULTVTABLE 0x8

/* FUNCDESC.wFuncFlags. */
#define FUNCFLAG_FRESTRICTED       0x0001
#define FUNCFLAG_FSOURCE           0x0002
#define FUNCFLAG_FBINDABLE         0x0004
#define FUNCFLAG_FREQUESTEDIT      0x0008
#define FUNCFLAG_FDISPLAYBIND      0x0010
#define FUNCFLAG_FDEFAULTBIND      0x0020
#define FUNCFLAG_FHIDDEN           0x0040
#define FUNCFLAG_FUSESGETLASTERROR 0x0080
#define FUNCFLAG_FDEFAULTCOLLELEM  0x0100
#define FUNCFLAG_FUIDEFAULT        0x0200
#define FUNCFLAG_FNONBROWSABLE     0x0400
#define FUNCFLAG_FREPLACEABLE      0x0800
#define FUNCFLAG_FIMMEDIATEBIND    0x1000

/* VARDESC.wVarFlags. */
#define VARFLAG_FREADONLY       0x0001
#define VARFLAG_FSOURCE         0x0002
#define VARFLAG_FBINDABLE       0x0004
#define VARFLAG_FREQUESTEDIT    0x0008
#define VARFLAG_FDISPLAYBIND    0x0010
#define VARFLAG_FDEFAULTBIND    0x0020
#define VARFLAG_FHIDDEN         0x0040
#define VARFLAG_FRESTRICTED     0x0080
#define VARFLAG_FDEFAULTCOLLELEM 0x0100
#define VARFLAG_FUIDEFAULT      0x0200
#define VARFLAG_FNONBROWSABLE   0x0400
#define VARFLAG_FREPLACEABLE    0x0800
#define VARFLAG_FIMMEDIATEBIND  0x1000

/* TLIBATTR.wLibFlags. */
#define LIBFLAG_FRESTRICTED   0x1
#define LIBFLAG_FCONTROL      0x2
#define LIBFLAG_FHIDDEN       0x4
#define LIBFLAG_FHASDISKIMAGE 0x8

/* LoadTypeLibEx. */
typedef enum tagREGKIND {
	REGKIND_DEFAULT, REGKIND_REGISTER, REGKIND_NONE
} REGKIND;

#define MEMBERID_NIL DISPID_UNKNOWN

/* CreateDispTypeInfo's description of an interface. */
typedef struct tagPARAMDATA {
	OLECHAR *szName;
	VARTYPE vt;
} PARAMDATA, *LPPARAMDATA;

typedef struct tagMETHODDATA {
	OLECHAR *szName;
	PARAMDATA *ppdata;
	DISPID dispid;
	UINT iMeth;
	CALLCONV cc;
	UINT cArgs;
	WORD wFlags;
	VARTYPE vtReturn;
} METHODDATA, *LPMETHODDATA;

typedef struct tagINTERFACEDATA {
	METHODDATA *pmethdata;
	UINT cMembers;
} INTERFACEDATA, *LPINTERFACEDATA;

typedef struct ITypeCompVtbl {
	HRESULT (WINAPI *QueryInterface)(ITypeComp *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ITypeComp *This);
	ULONG (WINAPI *Release)(ITypeComp *This);
	HRESULT (WINAPI *Bind)(ITypeComp *This, LPOLESTR name, ULONG hash,
			       WORD flags, ITypeInfo **info, DESCKIND *kind,
			       BINDPTR *ptr);
	HRESULT (WINAPI *BindType)(ITypeComp *This, LPOLESTR name, ULONG hash,
				   ITypeInfo **info, ITypeComp **comp);
} ITypeCompVtbl;
struct ITypeComp {
	const ITypeCompVtbl *lpVtbl;
};

/*
 * ITypeInfo2, of which ITypeInfo is the first part: an ITypeInfo has the
 * same table, cut short after ReleaseVarDesc.
 */
typedef struct ITypeInfo2Vtbl {
	HRESULT (WINAPI *QueryInterface)(ITypeInfo2 *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ITypeInfo2 *This);
	ULONG (WINAPI *Release)(ITypeInfo2 *This);
	HRESULT (WINAPI *GetTypeAttr)(ITypeInfo2 *This, TYPEATTR **attr);
	HRESULT (WINAPI *GetTypeComp)(ITypeInfo2 *This, ITypeComp **comp);
	HRESULT (WINAPI *GetFuncDesc)(ITypeInfo2 *This, UINT index,
				      FUNCDESC **desc);
	HRESULT (WINAPI *GetVarDesc)(ITypeInfo2 *This, UINT index,
				     VARDESC **desc);
	HRESULT (WINAPI *GetNames)(ITypeInfo2 *This, MEMBERID memid,
				   BSTR *names, UINT max, UINT *count);
	HRESULT (WINAPI *GetRefTypeOfImplType)(ITypeInfo2 *This, UINT index,
					       HREFTYPE *href);
	HRESULT (WINAPI *GetImplTypeFlags)(ITypeInfo2 *This, UINT index,
					   INT *flags);
	HRESULT (WINAPI *GetIDsOfNames)(ITypeInfo2 *This, LPOLESTR *names,
					UINT count, MEMBERID *ids);
	HRESULT (WINAPI *Invoke)(ITypeInfo2 *This, PVOID instance,
				 MEMBERID memid, WORD flags,
				 DISPPARAMS *params, VARIANT *result,
				 EXCEPINFO *excep, UINT *arg_err);
	HRESULT (WINAPI *GetDocumentation)(ITypeInfo2 *This, MEMBERID memid,
					   BSTR *name, BSTR *doc,
					   DWORD *help_context,
					   BSTR *help_file);
	HRESULT (WINAPI *GetDllEntry)(ITypeInfo2 *This, MEMBERID memid,
				      INVOKEKIND kind, BSTR *dll, BSTR *name,
				      WORD *ordinal);
	HRESULT (WINAPI *GetRefTypeInfo)(ITypeInfo2 *This, HREFTYPE href,
					 ITypeInfo **info);
	HRESULT (WINAPI *AddressOfMember)(ITypeInfo2 *This, MEMBERID memid,
					  INVOKEKIND kind, PVOID *addr);
	HRESULT (WINAPI *CreateInstance)(ITypeInfo2 *This, IUnknown *outer,
					 REFIID riid, PVOID *obj);
	HRESULT (WINAPI *GetMops)(ITypeInfo2 *This, MEMBERID memid,
				  BSTR *mops);
	HRESULT (WINAPI *GetContainingTypeLib)(ITypeInfo2 *This,
					       ITypeLib **lib, UINT *index);
	void (WINAPI *ReleaseTypeAttr)(ITypeInfo2 *This, TYPEATTR *attr);
	void (WINAPI *ReleaseFuncDesc)(ITypeInfo2 *This, FUNCDESC *desc);
	void (WINAPI *ReleaseVarDesc)(ITypeInfo2 *This, VARDESC *desc);
	/* ITypeInfo2 */
	HRESULT (WINAPI *GetTypeKind)(ITypeInfo2 *This, TYPEKIND *kind);
	HRESULT (WINAPI *GetTypeFlags)(ITypeInfo2 *This, ULONG *flags);
	HRESULT (WINAPI *GetFuncIndexOfMemId)(ITypeInfo2 *This,
					      MEMBERID memid,
					      INVOKEKIND kind, UINT *index);
	HRESULT (WINAPI *GetVarIndexOfMemId)(ITypeInfo2 *This,
					     MEMBERID memid, UINT *index);
	HRESULT (WINAPI *GetCustData)(ITypeInfo2 *This, REFGUID guid,
				      VARIANT *value);
	HRESULT (WINAPI *GetFuncCustData)(ITypeInfo2 *This, UINT index,
					  REFGUID guid, VARIANT *value);
	HRESULT (WINAPI *GetParamCustData)(ITypeInfo2 *This, UINT func,
					   UINT param, REFGUID guid,
					   VARIANT *value);
	HRESULT (WINAPI *GetVarCustData)(ITypeInfo2 *This, UINT index,
					 REFGUID guid, VARIANT *value);
	HRESULT (WINAPI *GetImplTypeCustData)(ITypeInfo2 *This, UINT index,
					      REFGUID guid, VARIANT *value);
	HRESULT (WINAPI *GetDocumentation2)(ITypeInfo2 *This, MEMBERID memid,
					    LCID lcid, BSTR *help_string,
					    DWORD *help_context,
					    BSTR *help_dll);
	HRESULT (WINAPI *GetAllCustData)(ITypeInfo2 *This, CUSTDATA *data);
	HRESULT (WINAPI *GetAllFuncCustData)(ITypeInfo2 *This, UINT index,
					     CUSTDATA *data);
	HRESULT (WINAPI *GetAllParamCustData)(ITypeInfo2 *This, UINT func,
					      UINT param, CUSTDATA *data);
	HRESULT (WINAPI *GetAllVarCustData)(ITypeInfo2 *This, UINT index,
					    CUSTDATA *data);
	HRESULT (WINAPI *GetAllImplTypeCustData)(ITypeInfo2 *This,
						 UINT index, CUSTDATA *data);
} ITypeInfo2Vtbl;
struct ITypeInfo2 {
	const ITypeInfo2Vtbl *lpVtbl;
};

/* ITypeLib2, of which ITypeLib is the first part (to ReleaseTLibAttr). */
typedef struct ITypeLib2Vtbl {
	HRESULT (WINAPI *QueryInterface)(ITypeLib2 *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ITypeLib2 *This);
	ULONG (WINAPI *Release)(ITypeLib2 *This);
	UINT (WINAPI *GetTypeInfoCount)(ITypeLib2 *This);
	HRESULT (WINAPI *GetTypeInfo)(ITypeLib2 *This, UINT index,
				      ITypeInfo **info);
	HRESULT (WINAPI *GetTypeInfoType)(ITypeLib2 *This, UINT index,
					  TYPEKIND *kind);
	HRESULT (WINAPI *GetTypeInfoOfGuid)(ITypeLib2 *This, REFGUID guid,
					    ITypeInfo **info);
	HRESULT (WINAPI *GetLibAttr)(ITypeLib2 *This, TLIBATTR **attr);
	HRESULT (WINAPI *GetTypeComp)(ITypeLib2 *This, ITypeComp **comp);
	HRESULT (WINAPI *GetDocumentation)(ITypeLib2 *This, INT index,
					   BSTR *name, BSTR *doc,
					   DWORD *help_context,
					   BSTR *help_file);
	HRESULT (WINAPI *IsName)(ITypeLib2 *This, LPOLESTR name, ULONG hash,
				 BOOL *found);
	HRESULT (WINAPI *FindName)(ITypeLib2 *This, LPOLESTR name,
				   ULONG hash, ITypeInfo **infos,
				   MEMBERID *memids, USHORT *found);
	void (WINAPI *ReleaseTLibAttr)(ITypeLib2 *This, TLIBATTR *attr);
	/* ITypeLib2 */
	HRESULT (WINAPI *GetCustData)(ITypeLib2 *This, REFGUID guid,
				      VARIANT *value);
	HRESULT (WINAPI *GetLibStatistics)(ITypeLib2 *This, ULONG *names,
					   ULONG *chars);
	HRESULT (WINAPI *GetDocumentation2)(ITypeLib2 *This, INT index,
					    LCID lcid, BSTR *help_string,
					    DWORD *help_context,
					    BSTR *help_dll);
	HRESULT (WINAPI *GetAllCustData)(ITypeLib2 *This, CUSTDATA *data);
} ITypeLib2Vtbl;
struct ITypeLib2 {
	const ITypeLib2Vtbl *lpVtbl;
};

/*
 * ICreateTypeInfo2 and ICreateTypeLib2, of which ICreateTypeInfo and
 * ICreateTypeLib are the first parts.
 */
typedef struct ICreateTypeInfo2 ICreateTypeInfo;
typedef struct ICreateTypeInfo2 ICreateTypeInfo2;
typedef struct ICreateTypeLib2 ICreateTypeLib;
typedef struct ICreateTypeLib2 ICreateTypeLib2;

typedef struct ICreateTypeInfo2Vtbl {
	HRESULT (WINAPI *QueryInterface)(ICreateTypeInfo2 *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ICreateTypeInfo2 *This);
	ULONG (WINAPI *Release)(ICreateTypeInfo2 *This);
	HRESULT (WINAPI *SetGuid)(ICreateTypeInfo2 *This, REFGUID guid);
	HRESULT (WINAPI *SetTypeFlags)(ICreateTypeInfo2 *This, UINT flags);
	HRESULT (WINAPI *SetDocString)(ICreateTypeInfo2 *This, LPOLESTR doc);
	HRESULT (WINAPI *SetHelpContext)(ICreateTypeInfo2 *This, DWORD ctx);
	HRESULT (WINAPI *SetVersion)(ICreateTypeInfo2 *This, WORD major,
				     WORD minor);
	HRESULT (WINAPI *AddRefTypeInfo)(ICreateTypeInfo2 *This,
					 ITypeInfo *info, HREFTYPE *href);
	HRESULT (WINAPI *AddFuncDesc)(ICreateTypeInfo2 *This, UINT index,
				      FUNCDESC *desc);
	HRESULT (WINAPI *AddImplType)(ICreateTypeInfo2 *This, UINT index,
				      HREFTYPE href);
	HRESULT (WINAPI *SetImplTypeFlags)(ICreateTypeInfo2 *This, UINT index,
					   INT flags);
	HRESULT (WINAPI *SetAlignment)(ICreateTypeInfo2 *This, WORD align);
	HRESULT (WINAPI *SetSchema)(ICreateTypeInfo2 *This, LPOLESTR schema);
	HRESULT (WINAPI *AddVarDesc)(ICreateTypeInfo2 *This, UINT index,
				     VARDESC *desc);
	HRESULT (WINAPI *SetFuncAndParamNames)(ICreateTypeInfo2 *This,
					       UINT index, LPOLESTR *names,
					       UINT count);
	HRESULT (WINAPI *SetVarName)(ICreateTypeInfo2 *This, UINT index,
				     LPOLESTR name);
	HRESULT (WINAPI *SetTypeDescAlias)(ICreateTypeInfo2 *This,
					   TYPEDESC *alias);
	HRESULT (WINAPI *DefineFuncAsDllEntry)(ICreateTypeInfo2 *This,
					       UINT index, LPOLESTR dll,
					       LPOLESTR proc);
	HRESULT (WINAPI *SetFuncDocString)(ICreateTypeInfo2 *This, UINT index,
					   LPOLESTR doc);
	HRESULT (WINAPI *SetVarDocString)(ICreateTypeInfo2 *This, UINT index,
					  LPOLESTR doc);
	HRESULT (WINAPI *SetFuncHelpContext)(ICreateTypeInfo2 *This,
					     UINT index, DWORD ctx);
	HRESULT (WINAPI *SetVarHelpContext)(ICreateTypeInfo2 *This,
					    UINT index, DWORD ctx);
	HRESULT (WINAPI *SetMops)(ICreateTypeInfo2 *This, UINT index,
				  BSTR mops);
	HRESULT (WINAPI *SetTypeIdldesc)(ICreateTypeInfo2 *This,
					 IDLDESC *desc);
	HRESULT (WINAPI *LayOut)(ICreateTypeInfo2 *This);
	/* ICreateTypeInfo2 */
	HRESULT (WINAPI *DeleteFuncDesc)(ICreateTypeInfo2 *This, UINT index);
	HRESULT (WINAPI *DeleteFuncDescByMemId)(ICreateTypeInfo2 *This,
						MEMBERID memid,
						INVOKEKIND kind);
	HRESULT (WINAPI *DeleteVarDesc)(ICreateTypeInfo2 *This, UINT index);
	HRESULT (WINAPI *DeleteVarDescByMemId)(ICreateTypeInfo2 *This,
					       MEMBERID memid);
	HRESULT (WINAPI *DeleteImplType)(ICreateTypeInfo2 *This, UINT index);
	HRESULT (WINAPI *SetCustData)(ICreateTypeInfo2 *This, REFGUID guid,
				      VARIANT *value);
	HRESULT (WINAPI *SetFuncCustData)(ICreateTypeInfo2 *This, UINT index,
					  REFGUID guid, VARIANT *value);
	HRESULT (WINAPI *SetParamCustData)(ICreateTypeInfo2 *This, UINT func,
					   UINT param, REFGUID guid,
					   VARIANT *value);
	HRESULT (WINAPI *SetVarCustData)(ICreateTypeInfo2 *This, UINT index,
					 REFGUID guid, VARIANT *value);
	HRESULT (WINAPI *SetImplTypeCustData)(ICreateTypeInfo2 *This,
					      UINT index, REFGUID guid,
					      VARIANT *value);
	HRESULT (WINAPI *SetHelpStringContext)(ICreateTypeInfo2 *This,
					       ULONG ctx);
	HRESULT (WINAPI *SetFuncHelpStringContext)(ICreateTypeInfo2 *This,
						   UINT index, ULONG ctx);
	HRESULT (WINAPI *SetVarHelpStringContext)(ICreateTypeInfo2 *This,
						  UINT index, ULONG ctx);
	HRESULT (WINAPI *Invalidate)(ICreateTypeInfo2 *This);
	HRESULT (WINAPI *SetName)(ICreateTypeInfo2 *This, LPOLESTR name);
} ICreateTypeInfo2Vtbl;
struct ICreateTypeInfo2 {
	const ICreateTypeInfo2Vtbl *lpVtbl;
};

typedef struct ICreateTypeLib2Vtbl {
	HRESULT (WINAPI *QueryInterface)(ICreateTypeLib2 *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ICreateTypeLib2 *This);
	ULONG (WINAPI *Release)(ICreateTypeLib2 *This);
	HRESULT (WINAPI *CreateTypeInfo)(ICreateTypeLib2 *This, LPOLESTR name,
					 TYPEKIND kind,
					 ICreateTypeInfo **info);
	HRESULT (WINAPI *SetName)(ICreateTypeLib2 *This, LPOLESTR name);
	HRESULT (WINAPI *SetVersion)(ICreateTypeLib2 *This, WORD major,
				     WORD minor);
	HRESULT (WINAPI *SetGuid)(ICreateTypeLib2 *This, REFGUID guid);
	HRESULT (WINAPI *SetDocString)(ICreateTypeLib2 *This, LPOLESTR doc);
	HRESULT (WINAPI *SetHelpFileName)(ICreateTypeLib2 *This,
					  LPOLESTR file);
	HRESULT (WINAPI *SetHelpContext)(ICreateTypeLib2 *This, DWORD ctx);
	HRESULT (WINAPI *SetLcid)(ICreateTypeLib2 *This, LCID lcid);
	HRESULT (WINAPI *SetLibFlags)(ICreateTypeLib2 *This, UINT flags);
	HRESULT (WINAPI *SaveAllChanges)(ICreateTypeLib2 *This);
	/* ICreateTypeLib2 */
	HRESULT (WINAPI *DeleteTypeInfo)(ICreateTypeLib2 *This,
					 LPOLESTR name);
	HRESULT (WINAPI *SetCustData)(ICreateTypeLib2 *This, REFGUID guid,
				      VARIANT *value);
	HRESULT (WINAPI *SetHelpStringContext)(ICreateTypeLib2 *This,
					       ULONG ctx);
	HRESULT (WINAPI *SetHelpStringDll)(ICreateTypeLib2 *This,
					   LPOLESTR dll);
} ICreateTypeLib2Vtbl;
struct ICreateTypeLib2 {
	const ICreateTypeLib2Vtbl *lpVtbl;
};

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
#define TYPE_E_BUFFERTOOSMALL   ((HRESULT)0x80028016)
#define TYPE_E_FIELDNOTFOUND    ((HRESULT)0x80028017)
#define TYPE_E_INVDATAREAD      ((HRESULT)0x80028018)
#define TYPE_E_UNSUPFORMAT      ((HRESULT)0x80028019)
#define TYPE_E_REGISTRYACCESS   ((HRESULT)0x8002801C)
#define TYPE_E_LIBNOTREGISTERED ((HRESULT)0x8002801D)
#define TYPE_E_UNDEFINEDTYPE    ((HRESULT)0x80028027)
#define TYPE_E_QUALIFIEDNAMEDISALLOWED ((HRESULT)0x80028028)
#define TYPE_E_INVALIDSTATE     ((HRESULT)0x80028029)
#define TYPE_E_WRONGTYPEKIND    ((HRESULT)0x8002802A)
#define TYPE_E_ELEMENTNOTFOUND  ((HRESULT)0x8002802B)
#define TYPE_E_AMBIGUOUSNAME    ((HRESULT)0x8002802C)
#define TYPE_E_NAMECONFLICT     ((HRESULT)0x8002802D)
#define TYPE_E_UNKNOWNLCID      ((HRESULT)0x8002802E)
#define TYPE_E_DLLFUNCTIONNOTFOUND ((HRESULT)0x8002802F)
#define TYPE_E_BADMODULEKIND    ((HRESULT)0x800288BD)
#define TYPE_E_SIZETOOBIG       ((HRESULT)0x800288C5)
#define TYPE_E_DUPLICATEID      ((HRESULT)0x800288C6)
#define TYPE_E_INVALIDID        ((HRESULT)0x800288CF)
#define TYPE_E_TYPEMISMATCH     ((HRESULT)0x80028CA0)
#define TYPE_E_OUTOFBOUNDS      ((HRESULT)0x80028CA1)
#define TYPE_E_IOERROR          ((HRESULT)0x80028CA2)
#define TYPE_E_CANTCREATETMPFILE ((HRESULT)0x80028CA3)
#define TYPE_E_CANTLOADLIBRARY  ((HRESULT)0x80029C4A)
#define TYPE_E_INCONSISTENTPROPFUNCS ((HRESULT)0x80029C83)
#define TYPE_E_CIRCULARTYPE     ((HRESULT)0x80029C84)
#define CTL_E_OVERFLOW          ((HRESULT)0x800A0006)
#define CTL_E_OUTOFMEMORY       ((HRESULT)0x800A0007)
#define CTL_E_ILLEGALFUNCTIONCALL ((HRESULT)0x800A0005)

/*
 * IErrorLog and IPropertyBag (ocidl.h): settings read and written by name,
 * as some codecs (DMO encoders among them) take them.
 */
typedef struct IErrorLog IErrorLog;
typedef struct IErrorLogVtbl {
	HRESULT (WINAPI *QueryInterface)(IErrorLog *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IErrorLog *This);
	ULONG (WINAPI *Release)(IErrorLog *This);
	HRESULT (WINAPI *AddError)(IErrorLog *This, LPCOLESTR name,
				   EXCEPINFO *info);
} IErrorLogVtbl;
struct IErrorLog {
	const IErrorLogVtbl *lpVtbl;
};

typedef struct IPropertyBag IPropertyBag;
typedef struct IPropertyBagVtbl {
	HRESULT (WINAPI *QueryInterface)(IPropertyBag *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IPropertyBag *This);
	ULONG (WINAPI *Release)(IPropertyBag *This);
	HRESULT (WINAPI *Read)(IPropertyBag *This, LPCOLESTR name,
			       VARIANT *value, IErrorLog *log);
	HRESULT (WINAPI *Write)(IPropertyBag *This, LPCOLESTR name,
				VARIANT *value);
} IPropertyBagVtbl;
struct IPropertyBag {
	const IPropertyBagVtbl *lpVtbl;
};

/* Their GUIDs, in VFW_GUID_DECLARE's form (see objbase.h). */
#define VFW_OLEAUTO_GUIDS(G) \
	G(IID_IErrorLog, 0x3127ca40, 0x446e, 0x11ce, \
	  0x81, 0x35, 0x00, 0xaa, 0x00, 0x4b, 0xb8, 0x51) \
	G(IID_IPropertyBag, 0x55272a00, 0x42cb, 0x11ce, \
	  0x81, 0x35, 0x00, 0xaa, 0x00, 0x4b, 0xb8, 0x51) \
	G(IID_ITypeInfo, 0x00020401, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ITypeLib, 0x00020402, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ITypeComp, 0x00020403, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ITypeLib2, 0x00020411, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ITypeInfo2, 0x00020412, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ICreateTypeInfo, 0x00020405, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ICreateTypeLib, 0x00020406, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ICreateTypeInfo2, 0x0002040e, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_ICreateTypeLib2, 0x0002040f, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)

VFW_OLEAUTO_GUIDS(VFW_GUID_DECLARE)

#endif /* VFW_WIN32_OLEAUTO_H */
