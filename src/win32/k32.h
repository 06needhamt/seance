/* SPDX-License-Identifier: GPL-2.0-only */
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

/*
 * The clock DLLs see: set to date ("YYYY-MM-DD", optionally with
 * "THH:MM[:SS]", UTC) from now on, or the host's for NULL or "". -1 if
 * date isn't one. win_filetime_now() and win_unix_now() read it.
 */
int win_clock_set(const char *date);
ULONGLONG win_filetime_now(void);
long long win_unix_now(void);
void *heap_alloc(HANDLE heap, DWORD flags, SIZE_T size);
BOOL heap_free(HANDLE heap, DWORD flags, void *p);
SIZE_T heap_size(HANDLE heap, DWORD flags, const void *p);
void *heap_realloc(HANDLE heap, DWORD flags, void *p, SIZE_T size);

/*
 * GlobalAlloc, GlobalFree, GlobalLock, GlobalUnlock, GlobalSize and
 * GlobalReAlloc.
 */
HGLOBAL global_alloc(UINT flags, SIZE_T size);
HGLOBAL global_free(HGLOBAL h);
LPVOID global_lock(HGLOBAL h);
BOOL global_unlock(HGLOBAL h);
SIZE_T global_size(HGLOBAL h);
HGLOBAL global_realloc(HGLOBAL h, SIZE_T size, UINT flags);

/* The Unix fd behind a file handle, or -1. */
int file_handle_fd(HANDLE h);

struct kobject;
/* Associates a file with a completion port (a reference is taken); 0 if not. */
int file_set_port(HANDLE file, struct kobject *port, ULONG_PTR key);

/* Queues a completion packet on a port (KOBJ_IOCP). */
BOOL iocp_post(struct kobject *port, DWORD bytes, ULONG_PTR key,
	       LPOVERLAPPED ov);

/*
 * The same in two steps, for I/O that must not start without room for its
 * packet: iocp_packet_new allocates one (NULL with the error set), and
 * iocp_queue fills and queues it, or iocp_packet_free drops it.
 */
struct iocp_packet;
struct iocp_packet *iocp_packet_new(void);
void iocp_queue(struct kobject *port, struct iocp_packet *pk, DWORD bytes,
		ULONG_PTR key, LPOVERLAPPED ov);
void iocp_packet_free(struct iocp_packet *pk);

/* GetStdHandle. */
HANDLE file_std_handle(DWORD which);

/* Copies s (UTF-8) out as an ANSI or UTF-16 string with the usual Win32
 * contract: the length without the NUL if it fits in size characters,
 * otherwise the size needed including the NUL. */
DWORD copy_out_a(const char *s, LPSTR buf, DWORD size);
DWORD copy_out_w(const char *s, LPWSTR buf, DWORD size);

/* GetFullPathName's answer for a UTF-8 name (malloc()ed), or NULL. */
char *k32_full_path(const char *name);

/* CreateThread with the defaults, for threads the built-in DLLs run. */
HANDLE thread_create(LPTHREAD_START_ROUTINE start, LPVOID param);

/* CreateThread, for _beginthreadex: stack size, CREATE_SUSPENDED, thread id.
 * With THREAD_CLOSE_ON_EXIT (for _beginthread) the handle closes itself
 * when the thread ends. */
#define THREAD_CLOSE_ON_EXIT 0x80000000u
HANDLE thread_create_ex(SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param,
			DWORD flags, LPDWORD tid);

/* EnterCriticalSection and LeaveCriticalSection, for the other k32 files. */
void k32_cs_enter(CRITICAL_SECTION *cs);
void k32_cs_leave(CRITICAL_SECTION *cs);

/* SetEvent, or PulseEvent with pulse. */
BOOL event_signal(HANDLE h, int pulse);

/* Runs Fls callbacks and thread cleanup for the exiting thread. */
void thread_exit_cleanup(void);

/* Set by user32: ends the exiting thread's message queue, before the
 * thread is seen to have ended. */
extern void (*thread_end_hook)(void);

/* Set by user32: wakes threads asleep in its own waits, so a thread
 * TerminateThread ends sees the request (thread_terminated_check). */
extern void (*thread_wake_hook)(void);

/*
 * Ends the calling thread if TerminateThread has asked it to; called,
 * holding no locks, by a thread woken in a wait of its own.
 */
void thread_terminated_check(void);

/* Whether TerminateThread has asked the calling thread to end. */
int thread_terminate_pending(void);

/* ExitThread. */
void thread_exit(DWORD code) __attribute__((noreturn));

/*
 * Called with the wait lock held by a thread woken in a wait: if
 * TerminateThread has asked it to end, unlocks and ends it.
 */
void thread_wake_check(void);

/*
 * LoadLibrary from C: name in UTF-8, searched for as a caller at the
 * address caller (NULL: none) would have it.
 */
HMODULE k32_load_library(const char *name, const void *caller);

#endif /* VFW_WIN32_K32_H */
