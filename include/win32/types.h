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
typedef uint16_t WCHAR;
typedef int64_t  LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t DWORDLONG;

typedef intptr_t  INT_PTR, LONG_PTR;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR;
typedef UINT_PTR  WPARAM;
typedef LONG_PTR  LPARAM;
typedef LONG_PTR  LRESULT;
typedef LONG      HRESULT;

/* Pointer types. */
typedef void        *LPVOID, *PVOID;
typedef const void  *LPCVOID;
typedef BYTE        *LPBYTE, *PBYTE;
typedef WORD        *LPWORD, *PWORD;
typedef DWORD       *LPDWORD, *PDWORD;
typedef LONG        *LPLONG, *PLONG;
typedef BOOL        *LPBOOL, *PBOOL;
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
DECLARE_HANDLE(HINSTANCE);
typedef HINSTANCE HMODULE;
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HPALETTE);
DECLARE_HANDLE(HDRVR);

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
#define LOWORD(l)         ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l)         ((WORD)((DWORD_PTR)(l) >> 16))

#endif /* VFW_WIN32_TYPES_H */
