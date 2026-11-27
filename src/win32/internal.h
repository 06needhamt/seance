/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared by the built-in DLL implementations in src/win32.
 */
#ifndef VFW_WIN32_INTERNAL_H
#define VFW_WIN32_INTERNAL_H

#include "common/log.h"
#include "runtime/builtin.h"
#include "runtime/teb.h"
#include "win32.h"

/* A stub with the Win32 (stdcall) calling convention. */
#define WINFN(ret) static ret STUB_WINAPI

/* The calling thread's last-error value (TEB+0x34). */
void win_set_error(DWORD err);
DWORD win_get_error(void);

/* The Win32 error closest to an errno value. */
DWORD win_error_from_errno(int err);

/* Sets the last error from errno and returns fail (for one-line returns). */
#define WIN_ERRNO(fail) (win_set_error(win_error_from_errno(errno)), (fail))

size_t wstrlen(const WCHAR *s);

/* The HMODULE of the module containing the caller of a stub, or NULL. */
struct module;
struct module *win_caller_module(const void *return_address);

/*
 * Memory for the guest's malloc, operator new and HeapAlloc. Zeroed unless
 * VFW_ZERO_MEMORY=0: memory fresh from a Windows heap usually is, and code
 * only ever run on one can depend on it. The setting is read once.
 */
int win_zero_memory(void);
void *win_alloc(size_t n, size_t size);
/* Always zeroed; free, realloc and the usable size of what these return. */
void *win_calloc(size_t n, size_t size);
void *win_realloc(void *p, size_t n);
void win_free(void *p);
size_t win_usable(void *p);

/*
 * Between user32 and the built-in controls: a list item's text as one
 * snapshot. user32 converts CB_GETLBTEXT and CB_GETLBTEXTLEN between
 * charsets with it, so the length and the copy come from the same text
 * (a callback item's text could change between a CB_GETLBTEXTLEN and a
 * CB_GETLBTEXT, and the copy would overrun a buffer sized by the first).
 * wParam: the item; lParam: a struct win_lbtext. A control that knows the
 * message sets done, result (the length, or CB_ERR) and text (a malloc'd
 * copy, or NULL); any other procedure leaves done at 0 and user32 falls
 * back to the two messages. The value is in the system range, which
 * Windows leaves unused here.
 */
#define WIN_LBTEXT_SNAPSHOT 0x03F7
/* Posted by a built-in control to itself for work it deferred (the rebar's
 * layout after its pass budget ran out). Same range, also unused. */
#define WIN_DEFERRED_LAYOUT 0x03F8
struct win_lbtext {
	int done;
	LRESULT result;
	WCHAR *text;
};

#endif /* VFW_WIN32_INTERNAL_H */
