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

#endif /* VFW_WIN32_INTERNAL_H */
