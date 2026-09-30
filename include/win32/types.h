/*
 * Basic Win32 types for the VfW harness.
 *
 * These describe the ABI that 32-bit Windows DLLs expect. They are written
 * for a Linux i386 build: LONG is always 32-bit and WCHAR is always UTF-16,
 * whatever the host's long and wchar_t are.
 */
#ifndef VFW_WIN32_TYPES_H
#define VFW_WIN32_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if !defined(__i386__) && !defined(VFW_ALLOW_NON_I386)
#error "the VfW harness must be built for i386 (gcc -m32)"
#endif

/* Calling conventions. */
#define WINAPI   __attribute__((stdcall))
#define CALLBACK WINAPI
#define CDECL    __attribute__((cdecl))
#define WINAPIV  CDECL

/* Integer types. */
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef int16_t  SHORT;
typedef uint16_t USHORT;
typedef int32_t  INT;
typedef uint32_t UINT;
typedef int32_t  BOOL;
typedef char     CHAR;
typedef uint8_t  UCHAR;
typedef uint8_t  BOOLEAN;
typedef uint16_t WCHAR;

/*
 * 32-bit Windows aligns 64-bit integers to 8 bytes inside structures, but
 * the Linux i386 ABI only to 4, so ask for it explicitly. Win32-facing
 * structures must use these types (not int64_t or long long) for 64-bit
 * members.
 */
typedef int64_t  LONGLONG __attribute__((aligned(8)));
typedef uint64_t ULONGLONG __attribute__((aligned(8)));
typedef uint64_t DWORDLONG __attribute__((aligned(8)));

typedef intptr_t  INT_PTR, LONG_PTR;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR;
typedef UINT_PTR  WPARAM;
typedef LONG_PTR  LPARAM;
typedef LONG_PTR  LRESULT;
typedef LONG      HRESULT;
typedef ULONG_PTR SIZE_T;
typedef LONG_PTR  SSIZE_T;
typedef DWORD     LCID;
typedef WORD      LANGID;

/* Pointer types. */
typedef void        *LPVOID, *PVOID;
typedef const void  *LPCVOID;
typedef BYTE        *LPBYTE, *PBYTE;
typedef UCHAR       *PUCHAR;
typedef BOOLEAN     *PBOOLEAN;
typedef WORD        *LPWORD, *PWORD;
typedef DWORD       *LPDWORD, *PDWORD;
typedef LONG        *LPLONG, *PLONG;
typedef BOOL        *LPBOOL, *PBOOL;
typedef UINT        *LPUINT, *PUINT;
typedef CHAR        *LPSTR, *PSTR;
typedef const CHAR  *LPCSTR, *PCSTR;
typedef WCHAR       *LPWSTR, *PWSTR;
typedef const WCHAR *LPCWSTR, *PCWSTR;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Handles. */
#define DECLARE_HANDLE(name) \
	struct name##__ { int unused; }; \
	typedef struct name##__ *name

typedef void *HANDLE;
typedef HANDLE *PHANDLE, *LPHANDLE;
typedef SIZE_T *PSIZE_T;
typedef DWORD_PTR *PDWORD_PTR;
typedef HANDLE HGLOBAL, HLOCAL, HRSRC;
DECLARE_HANDLE(HINSTANCE);
typedef HINSTANCE HMODULE;
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HPALETTE);
DECLARE_HANDLE(HDRVR);
DECLARE_HANDLE(HKEY);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HPEN);
DECLARE_HANDLE(HRGN);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HWINSTA);
DECLARE_HANDLE(HMONITOR);
typedef HICON HCURSOR;
typedef HANDLE HGDIOBJ;

typedef WORD  ATOM;
typedef DWORD COLORREF;

typedef INT_PTR (WINAPI *FARPROC)(void);

/* Common structures. */
typedef struct tagPOINT {
	LONG x;
	LONG y;
} POINT, *PPOINT, *LPPOINT;

typedef struct tagRECT {
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
} RECT, *PRECT, *LPRECT;

typedef struct tagSIZE {
	LONG cx;
	LONG cy;
} SIZE, *PSIZE, *LPSIZE;

typedef struct _FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef union _LARGE_INTEGER {
	struct {
		DWORD LowPart;
		LONG HighPart;
	} u;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
	struct {
		DWORD LowPart;
		DWORD HighPart;
	} u;
	ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

#define MAKEWORD(lo, hi)  ((WORD)(((BYTE)(lo)) | ((WORD)((BYTE)(hi))) << 8))
#define MAKELONG(lo, hi)  ((LONG)(((WORD)(lo)) | ((DWORD)((WORD)(hi))) << 16))
#define MAKELPARAM(lo, hi) ((LPARAM)(DWORD)MAKELONG(lo, hi))
#define LOWORD(l)         ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l)         ((WORD)((DWORD_PTR)(l) >> 16))

#endif /* VFW_WIN32_TYPES_H */
