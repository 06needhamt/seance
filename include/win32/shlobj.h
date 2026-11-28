/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32 definitions from shlobj.h, shtypes.h and shobjidl.h: special
 * folders (CSIDL), item ID lists and folder attributes.
 */
#ifndef VFW_WIN32_SHLOBJ_H
#define VFW_WIN32_SHLOBJ_H

#include "objbase.h"
#include "types.h"
#include "winbase.h"

#pragma pack(push, 1)
typedef struct _SHITEMID {
	USHORT cb;		/* including cb itself; 0 ends the list */
	BYTE abID[1];
} SHITEMID;

typedef struct _ITEMIDLIST {
	SHITEMID mkid;
} ITEMIDLIST, *LPITEMIDLIST;
#pragma pack(pop)
typedef const ITEMIDLIST *LPCITEMIDLIST;

typedef int (CALLBACK *BFFCALLBACK)(HWND, UINT, LPARAM, LPARAM);

typedef struct _browseinfoA {
	HWND hwndOwner;
	LPCITEMIDLIST pidlRoot;
	LPSTR pszDisplayName;
	LPCSTR lpszTitle;
	UINT ulFlags;
	BFFCALLBACK lpfn;
	LPARAM lParam;
	int iImage;
} BROWSEINFOA;

typedef struct _browseinfoW {
	HWND hwndOwner;
	LPCITEMIDLIST pidlRoot;
	LPWSTR pszDisplayName;
	LPCWSTR lpszTitle;
	UINT ulFlags;
	BFFCALLBACK lpfn;
	LPARAM lParam;
	int iImage;
} BROWSEINFOW;

#define CSIDL_DESKTOP                 0x0000
#define CSIDL_INTERNET                0x0001
#define CSIDL_PROGRAMS                0x0002
#define CSIDL_CONTROLS                0x0003
#define CSIDL_PRINTERS                0x0004
#define CSIDL_PERSONAL                0x0005
#define CSIDL_FAVORITES               0x0006
#define CSIDL_STARTUP                 0x0007
#define CSIDL_RECENT                  0x0008
#define CSIDL_SENDTO                  0x0009
#define CSIDL_BITBUCKET               0x000a
#define CSIDL_STARTMENU               0x000b
#define CSIDL_MYDOCUMENTS             0x0005
#define CSIDL_MYMUSIC                 0x000d
#define CSIDL_MYVIDEO                 0x000e
#define CSIDL_DESKTOPDIRECTORY        0x0010
#define CSIDL_DRIVES                  0x0011
#define CSIDL_NETWORK                 0x0012
#define CSIDL_NETHOOD                 0x0013
#define CSIDL_FONTS                   0x0014
#define CSIDL_TEMPLATES               0x0015
#define CSIDL_COMMON_STARTMENU        0x0016
#define CSIDL_COMMON_PROGRAMS         0x0017
#define CSIDL_COMMON_STARTUP          0x0018
#define CSIDL_COMMON_DESKTOPDIRECTORY 0x0019
#define CSIDL_APPDATA                 0x001a
#define CSIDL_PRINTHOOD               0x001b
#define CSIDL_LOCAL_APPDATA           0x001c
#define CSIDL_ALTSTARTUP              0x001d
#define CSIDL_COMMON_ALTSTARTUP       0x001e
#define CSIDL_COMMON_FAVORITES        0x001f
#define CSIDL_INTERNET_CACHE          0x0020
#define CSIDL_COOKIES                 0x0021
#define CSIDL_HISTORY                 0x0022
#define CSIDL_COMMON_APPDATA          0x0023
#define CSIDL_WINDOWS                 0x0024
#define CSIDL_SYSTEM                  0x0025
#define CSIDL_PROGRAM_FILES           0x0026
#define CSIDL_MYPICTURES              0x0027
#define CSIDL_PROFILE                 0x0028
#define CSIDL_SYSTEMX86               0x0029
#define CSIDL_PROGRAM_FILESX86        0x002a
#define CSIDL_PROGRAM_FILES_COMMON    0x002b
#define CSIDL_PROGRAM_FILES_COMMONX86 0x002c
#define CSIDL_COMMON_TEMPLATES        0x002d
#define CSIDL_COMMON_DOCUMENTS        0x002e
#define CSIDL_COMMON_ADMINTOOLS       0x002f
#define CSIDL_ADMINTOOLS              0x0030
#define CSIDL_CONNECTIONS             0x0031
#define CSIDL_COMMON_MUSIC            0x0035
#define CSIDL_COMMON_PICTURES         0x0036
#define CSIDL_COMMON_VIDEO            0x0037
#define CSIDL_RESOURCES               0x0038
#define CSIDL_RESOURCES_LOCALIZED     0x0039
#define CSIDL_COMMON_OEM_LINKS        0x003a
#define CSIDL_CDBURN_AREA             0x003b
#define CSIDL_COMPUTERSNEARME         0x003d
#define CSIDL_FLAG_DONT_VERIFY        0x4000
#define CSIDL_FLAG_CREATE             0x8000
#define CSIDL_FLAG_MASK               0xff00

/* SHGFP_TYPE, SHGetFolderPath's dwFlags. */
#define SHGFP_TYPE_CURRENT 0
#define SHGFP_TYPE_DEFAULT 1

/* SFGAO: what SHGetFileInfo reports in dwAttributes. */
#define SFGAO_CANCOPY           0x00000001
#define SFGAO_CANMOVE           0x00000002
#define SFGAO_CANLINK           0x00000004
#define SFGAO_CANRENAME         0x00000010
#define SFGAO_CANDELETE         0x00000020
#define SFGAO_READONLY          0x00040000
#define SFGAO_STREAM            0x00400000
#define SFGAO_FILESYSANCESTOR   0x10000000
#define SFGAO_FOLDER            0x20000000
#define SFGAO_FILESYSTEM        0x40000000
#define SFGAO_HASSUBFOLDER      0x80000000

#define SFGAO_LINK              0x00010000
#define SFGAO_HIDDEN            0x00080000
#define SFGAO_STORAGE           0x00000008
#define SFGAO_BROWSABLE         0x08000000

/* IShellFolder's display names and enumeration (shobjidl.h, shtypes.h). */
#define SHGDN_NORMAL        0x0000
#define SHGDN_INFOLDER      0x0001
#define SHGDN_FOREDITING    0x1000
#define SHGDN_FORADDRESSBAR 0x4000
#define SHGDN_FORPARSING    0x8000

#define SHCONTF_FOLDERS        0x0020
#define SHCONTF_NONFOLDERS     0x0040
#define SHCONTF_INCLUDEHIDDEN  0x0080

#define STRRET_WSTR   0
#define STRRET_OFFSET 1
#define STRRET_CSTR   2

typedef struct _STRRET {
	UINT uType;
	union {
		LPWSTR pOleStr;
		UINT uOffset;
		char cStr[MAX_PATH];
	} u;
} STRRET, *LPSTRRET;

typedef struct IBindCtx IBindCtx;

typedef struct IEnumIDList IEnumIDList;
typedef struct IEnumIDListVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumIDList *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumIDList *This);
	ULONG (WINAPI *Release)(IEnumIDList *This);
	HRESULT (WINAPI *Next)(IEnumIDList *This, ULONG n, LPITEMIDLIST *items,
			       ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumIDList *This, ULONG n);
	HRESULT (WINAPI *Reset)(IEnumIDList *This);
	HRESULT (WINAPI *Clone)(IEnumIDList *This, IEnumIDList **out);
} IEnumIDListVtbl;
struct IEnumIDList {
	const IEnumIDListVtbl *lpVtbl;
};

typedef struct IShellFolder IShellFolder;
typedef struct IShellFolderVtbl {
	HRESULT (WINAPI *QueryInterface)(IShellFolder *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IShellFolder *This);
	ULONG (WINAPI *Release)(IShellFolder *This);
	HRESULT (WINAPI *ParseDisplayName)(IShellFolder *This, HWND hwnd,
					   IBindCtx *bc, LPWSTR name,
					   ULONG *eaten, LPITEMIDLIST *pidl,
					   ULONG *attrs);
	HRESULT (WINAPI *EnumObjects)(IShellFolder *This, HWND hwnd,
				      DWORD flags, IEnumIDList **out);
	HRESULT (WINAPI *BindToObject)(IShellFolder *This, LPCITEMIDLIST pidl,
				       IBindCtx *bc, REFIID riid, void **ppv);
	HRESULT (WINAPI *BindToStorage)(IShellFolder *This, LPCITEMIDLIST pidl,
					IBindCtx *bc, REFIID riid, void **ppv);
	HRESULT (WINAPI *CompareIDs)(IShellFolder *This, LPARAM lparam,
				     LPCITEMIDLIST a, LPCITEMIDLIST b);
	HRESULT (WINAPI *CreateViewObject)(IShellFolder *This, HWND owner,
					   REFIID riid, void **ppv);
	HRESULT (WINAPI *GetAttributesOf)(IShellFolder *This, UINT n,
					  LPCITEMIDLIST *pidls, ULONG *attrs);
	HRESULT (WINAPI *GetUIObjectOf)(IShellFolder *This, HWND owner, UINT n,
					LPCITEMIDLIST *pidls, REFIID riid,
					UINT *reserved, void **ppv);
	HRESULT (WINAPI *GetDisplayNameOf)(IShellFolder *This,
					   LPCITEMIDLIST pidl, DWORD flags,
					   STRRET *name);
	HRESULT (WINAPI *SetNameOf)(IShellFolder *This, HWND hwnd,
				    LPCITEMIDLIST pidl, LPCWSTR name,
				    DWORD flags, LPITEMIDLIST *out);
} IShellFolderVtbl;
struct IShellFolder {
	const IShellFolderVtbl *lpVtbl;
};

/* Shortcuts (shobjidl.h). */
#define SLGP_SHORTPATH   0x1
#define SLGP_UNCPRIORITY 0x2
#define SLGP_RAWPATH     0x4

#define SLR_NO_UI     0x0001
#define SLR_UPDATE    0x0004
#define SLR_NOSEARCH  0x0010

typedef struct IShellLinkA IShellLinkA;
typedef struct IShellLinkAVtbl {
	HRESULT (WINAPI *QueryInterface)(IShellLinkA *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IShellLinkA *This);
	ULONG (WINAPI *Release)(IShellLinkA *This);
	HRESULT (WINAPI *GetPath)(IShellLinkA *This, LPSTR file, int cch,
				  WIN32_FIND_DATAA *fd, DWORD flags);
	HRESULT (WINAPI *GetIDList)(IShellLinkA *This, LPITEMIDLIST *pidl);
	HRESULT (WINAPI *SetIDList)(IShellLinkA *This, LPCITEMIDLIST pidl);
	HRESULT (WINAPI *GetDescription)(IShellLinkA *This, LPSTR name, int cch);
	HRESULT (WINAPI *SetDescription)(IShellLinkA *This, LPCSTR name);
	HRESULT (WINAPI *GetWorkingDirectory)(IShellLinkA *This, LPSTR dir,
					      int cch);
	HRESULT (WINAPI *SetWorkingDirectory)(IShellLinkA *This, LPCSTR dir);
	HRESULT (WINAPI *GetArguments)(IShellLinkA *This, LPSTR args, int cch);
	HRESULT (WINAPI *SetArguments)(IShellLinkA *This, LPCSTR args);
	HRESULT (WINAPI *GetHotkey)(IShellLinkA *This, WORD *hotkey);
	HRESULT (WINAPI *SetHotkey)(IShellLinkA *This, WORD hotkey);
	HRESULT (WINAPI *GetShowCmd)(IShellLinkA *This, int *show);
	HRESULT (WINAPI *SetShowCmd)(IShellLinkA *This, int show);
	HRESULT (WINAPI *GetIconLocation)(IShellLinkA *This, LPSTR path, int cch,
					  int *index);
	HRESULT (WINAPI *SetIconLocation)(IShellLinkA *This, LPCSTR path,
					  int index);
	HRESULT (WINAPI *SetRelativePath)(IShellLinkA *This, LPCSTR path,
					  DWORD reserved);
	HRESULT (WINAPI *Resolve)(IShellLinkA *This, HWND hwnd, DWORD flags);
	HRESULT (WINAPI *SetPath)(IShellLinkA *This, LPCSTR file);
} IShellLinkAVtbl;
struct IShellLinkA {
	const IShellLinkAVtbl *lpVtbl;
};

typedef struct IShellLinkW IShellLinkW;
typedef struct IShellLinkWVtbl {
	HRESULT (WINAPI *QueryInterface)(IShellLinkW *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IShellLinkW *This);
	ULONG (WINAPI *Release)(IShellLinkW *This);
	HRESULT (WINAPI *GetPath)(IShellLinkW *This, LPWSTR file, int cch,
				  WIN32_FIND_DATAW *fd, DWORD flags);
	HRESULT (WINAPI *GetIDList)(IShellLinkW *This, LPITEMIDLIST *pidl);
	HRESULT (WINAPI *SetIDList)(IShellLinkW *This, LPCITEMIDLIST pidl);
	HRESULT (WINAPI *GetDescription)(IShellLinkW *This, LPWSTR name,
					 int cch);
	HRESULT (WINAPI *SetDescription)(IShellLinkW *This, LPCWSTR name);
	HRESULT (WINAPI *GetWorkingDirectory)(IShellLinkW *This, LPWSTR dir,
					      int cch);
	HRESULT (WINAPI *SetWorkingDirectory)(IShellLinkW *This, LPCWSTR dir);
	HRESULT (WINAPI *GetArguments)(IShellLinkW *This, LPWSTR args, int cch);
	HRESULT (WINAPI *SetArguments)(IShellLinkW *This, LPCWSTR args);
	HRESULT (WINAPI *GetHotkey)(IShellLinkW *This, WORD *hotkey);
	HRESULT (WINAPI *SetHotkey)(IShellLinkW *This, WORD hotkey);
	HRESULT (WINAPI *GetShowCmd)(IShellLinkW *This, int *show);
	HRESULT (WINAPI *SetShowCmd)(IShellLinkW *This, int show);
	HRESULT (WINAPI *GetIconLocation)(IShellLinkW *This, LPWSTR path,
					  int cch, int *index);
	HRESULT (WINAPI *SetIconLocation)(IShellLinkW *This, LPCWSTR path,
					  int index);
	HRESULT (WINAPI *SetRelativePath)(IShellLinkW *This, LPCWSTR path,
					  DWORD reserved);
	HRESULT (WINAPI *Resolve)(IShellLinkW *This, HWND hwnd, DWORD flags);
	HRESULT (WINAPI *SetPath)(IShellLinkW *This, LPCWSTR file);
} IShellLinkWVtbl;
struct IShellLinkW {
	const IShellLinkWVtbl *lpVtbl;
};

/* Shortcut extra data (shlobj.h, shlguid.h, MS-SHLLINK 2.5). */
#define SLDF_HAS_ID_LIST          0x00000001
#define SLDF_HAS_LINK_INFO        0x00000002
#define SLDF_HAS_NAME             0x00000004
#define SLDF_HAS_RELPATH          0x00000008
#define SLDF_HAS_WORKINGDIR       0x00000010
#define SLDF_HAS_ARGS             0x00000020
#define SLDF_HAS_ICONLOCATION     0x00000040
#define SLDF_UNICODE              0x00000080
#define SLDF_FORCE_NO_LINKINFO    0x00000100
#define SLDF_HAS_EXP_SZ           0x00000200
#define SLDF_RUN_IN_SEPARATE      0x00000400
#define SLDF_HAS_DARWINID         0x00001000
#define SLDF_RUNAS_USER           0x00002000
#define SLDF_HAS_EXP_ICON_SZ      0x00004000
#define SLDF_NO_PIDL_ALIAS        0x00008000
#define SLDF_FORCE_UNCNAME        0x00010000
#define SLDF_RUN_WITH_SHIMLAYER   0x00020000

#define EXP_SZ_LINK_SIG           0xA0000001
#define NT_CONSOLE_PROPS_SIG      0xA0000002
#define EXP_TRACKER_SIG           0xA0000003
#define NT_FE_CONSOLE_PROPS_SIG   0xA0000004
#define EXP_SPECIAL_FOLDER_SIG    0xA0000005
#define EXP_DARWIN_ID_SIG         0xA0000006
#define EXP_SZ_ICON_SIG           0xA0000007
#define EXP_SHIM_SIG              0xA0000008
#define EXP_PROPERTYSTORAGE_SIG   0xA0000009
#define EXP_KNOWN_FOLDER_SIG      0xA000000B
#define EXP_VISTA_ID_LIST_SIG     0xA000000C

typedef struct tagDATABLOCKHEADER {
	DWORD cbSize;
	DWORD dwSignature;
} DATABLOCK_HEADER, *LPDATABLOCK_HEADER, *LPDBLIST;

typedef struct {
	DATABLOCK_HEADER dbh;
	CHAR szTarget[MAX_PATH];
	WCHAR swzTarget[MAX_PATH];
} EXP_SZ_LINK, *LPEXP_SZ_LINK;

typedef struct {
	DATABLOCK_HEADER dbh;
	CHAR szDarwinID[MAX_PATH];
	WCHAR szwDarwinID[MAX_PATH];
} EXP_DARWIN_LINK, *LPEXP_DARWIN_LINK;

typedef struct {
	DWORD cbSize;
	DWORD dwSignature;
	DWORD idSpecialFolder;
	DWORD cbOffset;
} EXP_SPECIAL_FOLDER, *LPEXP_SPECIAL_FOLDER;

typedef struct {
	DATABLOCK_HEADER dbh;
	WORD wFillAttribute;
	WORD wPopupFillAttribute;
	COORD dwScreenBufferSize;
	COORD dwWindowSize;
	COORD dwWindowOrigin;
	DWORD nFont;
	DWORD nInputBufferSize;
	COORD dwFontSize;
	UINT uFontFamily;
	UINT uFontWeight;
	WCHAR FaceName[32];
	UINT uCursorSize;
	BOOL bFullScreen;
	BOOL bQuickEdit;
	BOOL bInsertMode;
	BOOL bAutoPosition;
	UINT uHistoryBufferSize;
	UINT uNumberOfHistoryBuffers;
	BOOL bHistoryNoDup;
	COLORREF ColorTable[16];
} NT_CONSOLE_PROPS, *LPNT_CONSOLE_PROPS;

typedef struct {
	DATABLOCK_HEADER dbh;
	UINT uCodePage;
} NT_FE_CONSOLE_PROPS, *LPNT_FE_CONSOLE_PROPS;

typedef struct IShellLinkDataList IShellLinkDataList;
typedef struct IShellLinkDataListVtbl {
	HRESULT (WINAPI *QueryInterface)(IShellLinkDataList *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IShellLinkDataList *This);
	ULONG (WINAPI *Release)(IShellLinkDataList *This);
	HRESULT (WINAPI *AddDataBlock)(IShellLinkDataList *This, void *block);
	HRESULT (WINAPI *CopyDataBlock)(IShellLinkDataList *This, DWORD sig,
					void **block);
	HRESULT (WINAPI *RemoveDataBlock)(IShellLinkDataList *This, DWORD sig);
	HRESULT (WINAPI *GetFlags)(IShellLinkDataList *This, DWORD *flags);
	HRESULT (WINAPI *SetFlags)(IShellLinkDataList *This, DWORD flags);
} IShellLinkDataListVtbl;
struct IShellLinkDataList {
	const IShellLinkDataListVtbl *lpVtbl;
};

/* Dropped files and data objects (shlobj.h). */
typedef struct _DROPFILES {
	DWORD pFiles;		/* offset of the file list */
	POINT pt;
	BOOL fNC;
	BOOL fWide;
} DROPFILES, *LPDROPFILES;

/* "Shell IDList Array": the parent's ID list, then each item's. */
typedef struct _IDA {
	UINT cidl;
	UINT aoffset[1];
} CIDA, *LPIDA;

/* Context menus (shobjidl.h). */
#define CMF_NORMAL         0x00000000
#define CMF_DEFAULTONLY    0x00000001
#define CMF_VERBSONLY      0x00000002
#define CMF_EXPLORE        0x00000004
#define CMF_NOVERBS        0x00000008
#define CMF_CANRENAME      0x00000010
#define CMF_NODEFAULT      0x00000020
#define CMF_INCLUDESTATIC  0x00000040
#define CMF_EXTENDEDVERBS  0x00000100

#define GCS_VERBA          0x00000000
#define GCS_HELPTEXTA      0x00000001
#define GCS_VALIDATEA      0x00000002
#define GCS_VERBW          0x00000004
#define GCS_HELPTEXTW      0x00000005
#define GCS_VALIDATEW      0x00000006
#define GCS_UNICODE        0x00000004

#define CMIC_MASK_HOTKEY        0x00000020
#define CMIC_MASK_ICON          0x00000010
#define CMIC_MASK_FLAG_NO_UI    0x00000400
#define CMIC_MASK_UNICODE       0x00004000
#define CMIC_MASK_NO_CONSOLE    0x00008000
#define CMIC_MASK_SHIFT_DOWN    0x10000000
#define CMIC_MASK_PTINVOKE      0x20000000
#define CMIC_MASK_CONTROL_DOWN  0x40000000

typedef struct _CMINVOKECOMMANDINFO {
	DWORD cbSize;
	DWORD fMask;
	HWND hwnd;
	LPCSTR lpVerb;
	LPCSTR lpParameters;
	LPCSTR lpDirectory;
	int nShow;
	DWORD dwHotKey;
	HANDLE hIcon;
} CMINVOKECOMMANDINFO, *LPCMINVOKECOMMANDINFO;

typedef struct _CMInvokeCommandInfoEx {
	DWORD cbSize;
	DWORD fMask;
	HWND hwnd;
	LPCSTR lpVerb;
	LPCSTR lpParameters;
	LPCSTR lpDirectory;
	int nShow;
	DWORD dwHotKey;
	HANDLE hIcon;
	LPCSTR lpTitle;
	LPCWSTR lpVerbW;
	LPCWSTR lpParametersW;
	LPCWSTR lpDirectoryW;
	LPCWSTR lpTitleW;
	POINT ptInvoke;
} CMINVOKECOMMANDINFOEX, *LPCMINVOKECOMMANDINFOEX;

typedef struct IContextMenu IContextMenu;
typedef struct IContextMenuVtbl {
	HRESULT (WINAPI *QueryInterface)(IContextMenu *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IContextMenu *This);
	ULONG (WINAPI *Release)(IContextMenu *This);
	HRESULT (WINAPI *QueryContextMenu)(IContextMenu *This, HMENU menu,
					   UINT index, UINT first, UINT last,
					   UINT flags);
	HRESULT (WINAPI *InvokeCommand)(IContextMenu *This,
					LPCMINVOKECOMMANDINFO info);
	HRESULT (WINAPI *GetCommandString)(IContextMenu *This, UINT_PTR cmd,
					   UINT type, UINT *reserved,
					   LPSTR name, UINT cch);
} IContextMenuVtbl;
struct IContextMenu {
	const IContextMenuVtbl *lpVtbl;
};

/* Icons (shlobj.h). */
#define GIL_OPENICON     0x0001
#define GIL_FORSHELL     0x0002
#define GIL_ASYNC        0x0020
#define GIL_DEFAULTICON  0x0040
#define GIL_FORSHORTCUT  0x0080
#define GIL_SIMULATEDOC  0x0001
#define GIL_PERINSTANCE  0x0002
#define GIL_PERCLASS     0x0004
#define GIL_NOTFILENAME  0x0008
#define GIL_DONTCACHE    0x0010

typedef struct IExtractIconA IExtractIconA;
typedef struct IExtractIconAVtbl {
	HRESULT (WINAPI *QueryInterface)(IExtractIconA *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IExtractIconA *This);
	ULONG (WINAPI *Release)(IExtractIconA *This);
	HRESULT (WINAPI *GetIconLocation)(IExtractIconA *This, UINT flags,
					  LPSTR file, UINT cch, int *index,
					  UINT *out_flags);
	HRESULT (WINAPI *Extract)(IExtractIconA *This, LPCSTR file,
				  UINT index, HICON *large, HICON *small,
				  UINT size);
} IExtractIconAVtbl;
struct IExtractIconA {
	const IExtractIconAVtbl *lpVtbl;
};

typedef struct IExtractIconW IExtractIconW;
typedef struct IExtractIconWVtbl {
	HRESULT (WINAPI *QueryInterface)(IExtractIconW *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IExtractIconW *This);
	ULONG (WINAPI *Release)(IExtractIconW *This);
	HRESULT (WINAPI *GetIconLocation)(IExtractIconW *This, UINT flags,
					  LPWSTR file, UINT cch, int *index,
					  UINT *out_flags);
	HRESULT (WINAPI *Extract)(IExtractIconW *This, LPCWSTR file,
				  UINT index, HICON *large, HICON *small,
				  UINT size);
} IExtractIconWVtbl;
struct IExtractIconW {
	const IExtractIconWVtbl *lpVtbl;
};

/* Shell views (shobjidl.h). */
#define FVM_ICON         1
#define FVM_SMALLICON    2
#define FVM_LIST         3
#define FVM_DETAILS      4

#define SVSI_DESELECT       0x00000000
#define SVSI_SELECT         0x00000001
#define SVSI_EDIT           0x00000003
#define SVSI_DESELECTOTHERS 0x00000004
#define SVSI_ENSUREVISIBLE  0x00000008
#define SVSI_FOCUSED        0x00000010

#define SVGIO_BACKGROUND    0x00000000
#define SVGIO_SELECTION     0x00000001
#define SVGIO_ALLVIEW       0x00000002
#define SVGIO_TYPE_MASK     0x0000000f

#define SVUIA_DEACTIVATE        0
#define SVUIA_ACTIVATE_NOFOCUS  1
#define SVUIA_ACTIVATE_FOCUS    2

typedef struct {
	UINT ViewMode;
	UINT fFlags;
} FOLDERSETTINGS, *LPFOLDERSETTINGS;
typedef const FOLDERSETTINGS *LPCFOLDERSETTINGS;

typedef struct IShellBrowser IShellBrowser;
typedef BOOL (CALLBACK *LPFNADDPROPSHEETPAGE)(HANDLE page, LPARAM lp);

typedef struct IShellView IShellView;
typedef struct IShellViewVtbl {
	HRESULT (WINAPI *QueryInterface)(IShellView *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IShellView *This);
	ULONG (WINAPI *Release)(IShellView *This);
	HRESULT (WINAPI *GetWindow)(IShellView *This, HWND *hwnd);
	HRESULT (WINAPI *ContextSensitiveHelp)(IShellView *This, BOOL enter);
	HRESULT (WINAPI *TranslateAccelerator)(IShellView *This, MSG *msg);
	HRESULT (WINAPI *EnableModeless)(IShellView *This, BOOL enable);
	HRESULT (WINAPI *UIActivate)(IShellView *This, UINT state);
	HRESULT (WINAPI *Refresh)(IShellView *This);
	HRESULT (WINAPI *CreateViewWindow)(IShellView *This, IShellView *prev,
					   LPCFOLDERSETTINGS fs,
					   IShellBrowser *browser, RECT *rc,
					   HWND *hwnd);
	HRESULT (WINAPI *DestroyViewWindow)(IShellView *This);
	HRESULT (WINAPI *GetCurrentInfo)(IShellView *This,
					 LPFOLDERSETTINGS fs);
	HRESULT (WINAPI *AddPropertySheetPages)(IShellView *This, DWORD res,
						LPFNADDPROPSHEETPAGE fn,
						LPARAM lp);
	HRESULT (WINAPI *SaveViewState)(IShellView *This);
	HRESULT (WINAPI *SelectItem)(IShellView *This, LPCITEMIDLIST pidl,
				     UINT flags);
	HRESULT (WINAPI *GetItemObject)(IShellView *This, UINT item,
					REFIID riid, void **ppv);
} IShellViewVtbl;
struct IShellView {
	const IShellViewVtbl *lpVtbl;
};

/* The Recycle Bin. */
#define SHERB_NOCONFIRMATION 0x1
#define SHERB_NOPROGRESSUI   0x2
#define SHERB_NOSOUND        0x4

/* shellapi.h byte-packs this on 32-bit Windows: 20 bytes there. */
#if !defined(__x86_64__)
#pragma pack(push, 1)
#endif
typedef struct _SHQUERYRBINFO {
	DWORD cbSize;
	LONGLONG i64Size;
	LONGLONG i64NumItems;
} SHQUERYRBINFO, *LPSHQUERYRBINFO;
#if !defined(__x86_64__)
#pragma pack(pop)
#endif

/* Known folders (shlobj.h, knownfolders.h). */
typedef GUID KNOWNFOLDERID;
typedef const KNOWNFOLDERID *REFKNOWNFOLDERID;

#define KF_FLAG_DEFAULT     0x00000000
#define KF_FLAG_NO_ALIAS    0x00001000
#define KF_FLAG_DONT_VERIFY 0x00004000
#define KF_FLAG_CREATE      0x00008000

#define VFW_SHLOBJ_GUIDS(G) \
	G(IID_IContextMenu, 0x000214e4, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IExtractIconA, 0x000214eb, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IExtractIconW, 0x000214fa, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IShellView, 0x000214e3, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IOleWindow, 0x00000114, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IShellLinkDataList, 0x45e2b4ae, 0xb1c3, 0x11d0, \
	  0xb9, 0x2f, 0x00, 0xa0, 0xc9, 0x03, 0x12, 0xe1) \
	G(IID_IShellFolder, 0x000214e6, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IEnumIDList, 0x000214f2, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IShellLinkA, 0x000214ee, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(IID_IShellLinkW, 0x000214f9, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(CLSID_ShellLink, 0x00021401, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(CLSID_ShellDesktop, 0x00021400, 0x0000, 0x0000, \
	  0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46) \
	G(FOLDERID_Desktop, 0xb4bfcc3a, 0xdb2c, 0x424c, \
	  0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41) \
	G(FOLDERID_Documents, 0xfdd39ad0, 0x238f, 0x46af, \
	  0xad, 0xb4, 0x6c, 0x85, 0x48, 0x03, 0x69, 0xc7) \
	G(FOLDERID_RoamingAppData, 0x3eb685db, 0x65f9, 0x4cf6, \
	  0xa0, 0x3a, 0xe3, 0xef, 0x65, 0x72, 0x9f, 0x3d) \
	G(FOLDERID_LocalAppData, 0xf1b32785, 0x6fba, 0x4fcf, \
	  0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91) \
	G(FOLDERID_ProgramData, 0x62ab5d82, 0xfdc1, 0x4dc3, \
	  0xa9, 0xdd, 0x07, 0x0d, 0x1d, 0x49, 0x5d, 0x97) \
	G(FOLDERID_ProgramFiles, 0x905e63b6, 0xc1bf, 0x494e, \
	  0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a) \
	G(FOLDERID_ProgramFilesX86, 0x7c5a40ef, 0xa0fb, 0x4bfc, \
	  0x87, 0x4a, 0xc0, 0xf2, 0xe0, 0xb9, 0xfa, 0x8e) \
	G(FOLDERID_ProgramFilesCommon, 0xf7f1ed05, 0x9f6d, 0x47a2, \
	  0xaa, 0xae, 0x29, 0xd3, 0x17, 0xc6, 0xf0, 0x66) \
	G(FOLDERID_System, 0x1ac14e77, 0x02e7, 0x4e5d, \
	  0xb7, 0x44, 0x2e, 0xb1, 0xae, 0x51, 0x98, 0xb7) \
	G(FOLDERID_SystemX86, 0xd65231b0, 0xb2f1, 0x4857, \
	  0xa4, 0xce, 0xa8, 0xe7, 0xc6, 0xea, 0x7d, 0x27) \
	G(FOLDERID_Windows, 0xf38bf404, 0x1d43, 0x42f2, \
	  0x93, 0x05, 0x67, 0xde, 0x0b, 0x28, 0xfc, 0x23) \
	G(FOLDERID_Profile, 0x5e6c858f, 0x0e22, 0x4760, \
	  0x9a, 0xfe, 0xea, 0x33, 0x17, 0xb6, 0x71, 0x73) \
	G(FOLDERID_Music, 0x4bd8d571, 0x6d19, 0x48d3, \
	  0xbe, 0x97, 0x42, 0x22, 0x20, 0x08, 0x0e, 0x43) \
	G(FOLDERID_Pictures, 0x33e28130, 0x4e1e, 0x4676, \
	  0x83, 0x5a, 0x98, 0x39, 0x5c, 0x3b, 0xc3, 0xbb) \
	G(FOLDERID_Videos, 0x18989b1d, 0x99b5, 0x455b, \
	  0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc) \
	G(FOLDERID_Fonts, 0xfd228cb7, 0xae11, 0x4ae3, \
	  0x86, 0x4c, 0x16, 0xf3, 0x91, 0x0a, 0xb8, 0xfe) \
	G(FOLDERID_Startup, 0xb97d20bb, 0xf46a, 0x4c97, \
	  0xba, 0x10, 0x5e, 0x36, 0x08, 0x43, 0x08, 0x54) \
	G(FOLDERID_Programs, 0xa77f5d77, 0x2e2b, 0x44c3, \
	  0xa6, 0xa2, 0xab, 0xa6, 0x01, 0x05, 0x4a, 0x51) \
	G(FOLDERID_StartMenu, 0x625b53c3, 0xab48, 0x4ec1, \
	  0xba, 0x1f, 0xa1, 0xef, 0x41, 0x46, 0xfc, 0x19) \
	G(FOLDERID_Favorites, 0x1777f761, 0x68ad, 0x4d8a, \
	  0x87, 0xbd, 0x30, 0xb7, 0x59, 0xfa, 0x33, 0xdd) \
	G(FOLDERID_Recent, 0xae50c081, 0xebd2, 0x438a, \
	  0x86, 0x55, 0x8a, 0x09, 0x2e, 0x34, 0x98, 0x7a) \
	G(FOLDERID_SendTo, 0x8983036c, 0x27c0, 0x404b, \
	  0x8f, 0x08, 0x10, 0x2d, 0x10, 0xdc, 0xfd, 0x74) \
	G(FOLDERID_Templates, 0xa63293e8, 0x664e, 0x48db, \
	  0xa0, 0x79, 0xdf, 0x75, 0x9e, 0x05, 0x09, 0xf7) \
	G(FOLDERID_PublicDesktop, 0xc4aa340d, 0xf20f, 0x4863, \
	  0xaf, 0xef, 0xf8, 0x7e, 0xf2, 0xe6, 0xba, 0x25) \
	G(FOLDERID_PublicDocuments, 0xed4824af, 0xdce4, 0x45a8, \
	  0x81, 0xe2, 0xfc, 0x79, 0x65, 0x08, 0x36, 0x34) \
	G(FOLDERID_InternetCache, 0x352481e8, 0x33be, 0x4251, \
	  0xba, 0x85, 0x60, 0x07, 0xca, 0xed, 0xcf, 0x9d) \
	G(FOLDERID_Cookies, 0x2b0f765d, 0xc0e9, 0x4171, \
	  0x90, 0x8e, 0x08, 0xa6, 0x11, 0xb8, 0x4f, 0xf6) \
	G(FOLDERID_History, 0xd9dc8a3b, 0xb784, 0x432e, \
	  0xa7, 0x81, 0x5a, 0x11, 0x30, 0xa7, 0x59, 0x63) \
	G(FOLDERID_CommonStartMenu, 0xa4115719, 0xd62e, 0x491d, \
	  0xaa, 0x7c, 0xe7, 0x4b, 0x8b, 0xe3, 0xb0, 0x67) \
	G(FOLDERID_CommonPrograms, 0x0139d44e, 0x6afe, 0x49f2, \
	  0x86, 0x90, 0x3d, 0xaf, 0xca, 0xe6, 0xff, 0xb8) \
	G(FOLDERID_CommonStartup, 0x82a5ea35, 0xd9cd, 0x47c5, \
	  0x96, 0x29, 0xe1, 0x5d, 0x2f, 0x71, 0x4e, 0x6e) \
	G(FOLDERID_AdminTools, 0x724ef170, 0xa42d, 0x4fef, \
	  0x9f, 0x26, 0xb6, 0x0e, 0x84, 0x6f, 0xba, 0x4f) \
	G(FOLDERID_ComputerFolder, 0x0ac0837c, 0xbbf8, 0x452a, \
	  0x85, 0x0d, 0x79, 0xd0, 0x8e, 0x66, 0x7c, 0xa7) \
	G(FOLDERID_RecycleBinFolder, 0xb7534046, 0x3ecb, 0x4c18, \
	  0xbe, 0x4e, 0x64, 0xcd, 0x4c, 0xb7, 0xd6, 0xac) \
	G(FOLDERID_Downloads, 0x374de290, 0x123f, 0x4565, \
	  0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b)

VFW_SHLOBJ_GUIDS(VFW_GUID_DECLARE)

#endif /* VFW_WIN32_SHLOBJ_H */
