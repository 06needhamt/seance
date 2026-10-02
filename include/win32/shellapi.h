/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32 definitions (shellapi.h). On 32-bit Windows the structures in
 * shellapi.h are byte-packed (pshpack1.h), which moves some fields off
 * their natural alignment; the ABI check holds these to the same.
 */
#ifndef VFW_WIN32_SHELLAPI_H
#define VFW_WIN32_SHELLAPI_H

#include "types.h"

DECLARE_HANDLE(HDROP);

/* SHFileOperation */
#define FO_MOVE   0x0001
#define FO_COPY   0x0002
#define FO_DELETE 0x0003
#define FO_RENAME 0x0004

#define FOF_MULTIDESTFILES        0x0001
#define FOF_CONFIRMMOUSE          0x0002
#define FOF_SILENT                0x0004
#define FOF_RENAMEONCOLLISION     0x0008
#define FOF_NOCONFIRMATION        0x0010
#define FOF_WANTMAPPINGHANDLE     0x0020
#define FOF_ALLOWUNDO             0x0040
#define FOF_FILESONLY             0x0080
#define FOF_SIMPLEPROGRESS        0x0100
#define FOF_NOCONFIRMMKDIR        0x0200
#define FOF_NOERRORUI             0x0400
#define FOF_NOCOPYSECURITYATTRIBS 0x0800
#define FOF_NORECURSION           0x1000

typedef WORD FILEOP_FLAGS;

#pragma pack(push, 1)

typedef struct _SHFILEOPSTRUCTA {
	HWND hwnd;
	UINT wFunc;
	LPCSTR pFrom;
	LPCSTR pTo;
	FILEOP_FLAGS fFlags;
	BOOL fAnyOperationsAborted;
	LPVOID hNameMappings;
	LPCSTR lpszProgressTitle;
} SHFILEOPSTRUCTA, *LPSHFILEOPSTRUCTA;

typedef struct _SHFILEOPSTRUCTW {
	HWND hwnd;
	UINT wFunc;
	LPCWSTR pFrom;
	LPCWSTR pTo;
	FILEOP_FLAGS fFlags;
	BOOL fAnyOperationsAborted;
	LPVOID hNameMappings;
	LPCWSTR lpszProgressTitle;
} SHFILEOPSTRUCTW, *LPSHFILEOPSTRUCTW;

/* ShellExecuteEx */
typedef struct _SHELLEXECUTEINFOA {
	DWORD cbSize;
	ULONG fMask;
	HWND hwnd;
	LPCSTR lpVerb;
	LPCSTR lpFile;
	LPCSTR lpParameters;
	LPCSTR lpDirectory;
	int nShow;
	HINSTANCE hInstApp;
	void *lpIDList;
	LPCSTR lpClass;
	HKEY hkeyClass;
	DWORD dwHotKey;
	HANDLE hIcon;		/* a union with hMonitor */
	HANDLE hProcess;
} SHELLEXECUTEINFOA, *LPSHELLEXECUTEINFOA;

typedef struct _SHELLEXECUTEINFOW {
	DWORD cbSize;
	ULONG fMask;
	HWND hwnd;
	LPCWSTR lpVerb;
	LPCWSTR lpFile;
	LPCWSTR lpParameters;
	LPCWSTR lpDirectory;
	int nShow;
	HINSTANCE hInstApp;
	void *lpIDList;
	LPCWSTR lpClass;
	HKEY hkeyClass;
	DWORD dwHotKey;
	HANDLE hIcon;
	HANDLE hProcess;
} SHELLEXECUTEINFOW, *LPSHELLEXECUTEINFOW;

/* SHGetFileInfo */
typedef struct _SHFILEINFOA {
	HICON hIcon;
	int iIcon;
	DWORD dwAttributes;
	CHAR szDisplayName[MAX_PATH];
	CHAR szTypeName[80];
} SHFILEINFOA;

typedef struct _SHFILEINFOW {
	HICON hIcon;
	int iIcon;
	DWORD dwAttributes;
	WCHAR szDisplayName[MAX_PATH];
	WCHAR szTypeName[80];
} SHFILEINFOW;

#pragma pack(pop)

#define SEE_MASK_IDLIST         0x00000004
#define SEE_MASK_NOCLOSEPROCESS 0x00000040
#define SEE_MASK_FLAG_NO_UI     0x00000400

/* What ShellExecute returns for failure (at most 32). */
#define SE_ERR_FNF             2
#define SE_ERR_PNF             3
#define SE_ERR_ACCESSDENIED    5
#define SE_ERR_OOM             8
#define SE_ERR_SHARE           26
#define SE_ERR_ASSOCINCOMPLETE 27
#define SE_ERR_DDETIMEOUT      28
#define SE_ERR_DDEFAIL         29
#define SE_ERR_DDEBUSY         30
#define SE_ERR_NOASSOC         31
#define SE_ERR_DLLNOTFOUND     32

#define SHGFI_LARGEICON         0x00000000
#define SHGFI_SMALLICON         0x00000001
#define SHGFI_OPENICON          0x00000002
#define SHGFI_SHELLICONSIZE     0x00000004
#define SHGFI_PIDL              0x00000008
#define SHGFI_USEFILEATTRIBUTES 0x00000010
#define SHGFI_ICON              0x00000100
#define SHGFI_DISPLAYNAME       0x00000200
#define SHGFI_TYPENAME          0x00000400
#define SHGFI_ATTRIBUTES        0x00000800
#define SHGFI_ICONLOCATION      0x00001000
#define SHGFI_EXETYPE           0x00002000
#define SHGFI_SYSICONINDEX      0x00004000

#endif /* VFW_WIN32_SHELLAPI_H */
