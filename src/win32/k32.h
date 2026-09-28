/*
 * kernel32 internals shared between its source files, and with the other
 * built-in DLLs (the CRT allocates from the process heap, for example).
 */
#ifndef VFW_WIN32_K32_H
#define VFW_WIN32_K32_H

#include "win32.h"

/* Case mapping and CT_CTYPE1 classes (Latin-1; others unmapped). */
WCHAR k32_toupper_w(WCHAR c);
WCHAR k32_tolower_w(WCHAR c);
WORD k32_ctype1(WCHAR c);

/* GetCurrentThreadId. */
DWORD thread_current_id(void);

/* A new real handle to the calling thread (DuplicateHandle of -2). */
HANDLE thread_handle_current(void);

/* The process heap (GetProcessHeap) and allocation from it. */
HANDLE heap_process(void);
void *heap_alloc(HANDLE heap, DWORD flags, SIZE_T size);
BOOL heap_free(HANDLE heap, DWORD flags, void *p);
SIZE_T heap_size(HANDLE heap, DWORD flags, const void *p);
void *heap_realloc(HANDLE heap, DWORD flags, void *p, SIZE_T size);

/* GlobalAlloc, GlobalFree, GlobalLock, GlobalUnlock and GlobalSize. */
HGLOBAL global_alloc(UINT flags, SIZE_T size);
HGLOBAL global_free(HGLOBAL h);
LPVOID global_lock(HGLOBAL h);
BOOL global_unlock(HGLOBAL h);
SIZE_T global_size(HGLOBAL h);

/* The Unix fd behind a file handle, or -1. */
int file_handle_fd(HANDLE h);

/* GetStdHandle. */
HANDLE file_std_handle(DWORD which);

/* Copies s (UTF-8) out as an ANSI or UTF-16 string with the usual Win32
 * contract: the length without the NUL if it fits in size characters,
 * otherwise the size needed including the NUL. */
DWORD copy_out_a(const char *s, LPSTR buf, DWORD size);
DWORD copy_out_w(const char *s, LPWSTR buf, DWORD size);

/* Runs Fls callbacks and thread cleanup for the exiting thread. */
void thread_exit_cleanup(void);

/* ExitThread. */
void thread_exit(DWORD code) __attribute__((noreturn));

#endif /* VFW_WIN32_K32_H */
