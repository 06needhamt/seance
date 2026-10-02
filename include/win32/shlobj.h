/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32 definitions from shlobj.h, shtypes.h and shobjidl.h: special
 * folders (CSIDL), item ID lists and folder attributes.
 */
#ifndef VFW_WIN32_SHLOBJ_H
#define VFW_WIN32_SHLOBJ_H

#include "types.h"

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

#endif /* VFW_WIN32_SHLOBJ_H */
