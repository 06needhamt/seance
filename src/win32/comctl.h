/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32 internals shared between its source files.
 */
#ifndef VFW_WIN32_COMCTL_H
#define VFW_WIN32_COMCTL_H

#include "runtime/builtin.h"
#include "win32.h"

extern const struct builtin_dll builtin_user32, builtin_gdi32;

/* A user32 or gdi32 export, typed at the call site. */
#define USER32(name) builtin_export(&builtin_user32, #name)
#define GDI32(name)  builtin_export(&builtin_gdi32, #name)

/* Alloc, ReAlloc, Free: zeroed memory from the process heap. */
void *comctl_alloc(size_t size);
void *comctl_realloc(void *p, size_t size);
void comctl_free(void *p);

/* The export tables of the other source files. */
extern const struct builtin_export comctl_dpa_exports[];
extern const struct builtin_export comctl_imagelist_exports[];
extern const struct builtin_export comctl_sheet_exports[];

#endif /* VFW_WIN32_COMCTL_H */
