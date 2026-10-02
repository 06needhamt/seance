/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Built-in DLLs: the harness's own implementations of the Win32 system
 * DLLs (kernel32, msvcrt, user32, ...). A bare DLL name that matches one of
 * these is served from here rather than searched for on disk, the way
 * Windows treats its KnownDLLs.
 */
#ifndef VFW_RUNTIME_BUILTIN_H
#define VFW_RUNTIME_BUILTIN_H

#include <stddef.h>

#include "win32.h"

/*
 * Stubs are called from Win32 code, which only keeps the stack 4-byte
 * aligned, so they realign it before running (glibc and gcc assume 16).
 */
#define STUB_WINAPI __attribute__((stdcall, force_align_arg_pointer))
#define STUB_CDECL  __attribute__((cdecl, force_align_arg_pointer))

struct builtin_export {
	const char *name;	/* NULL for an ordinal-only export */
	WORD ordinal;		/* 0 if it has none */
	void *addr;		/* a function, or data for data exports */
};

/*
 * A DLL's exports come from one or more tables (a big DLL spreads them over
 * several source files), each ended by an entry with a NULL addr. They are
 * merged and sorted on first use; names must be unique across them.
 */
struct builtin_dll {
	const char *name;	/* lower case, with extension */
	/* Other names it answers to, as fnmatch() patterns; NULL-terminated. */
	const char *const *aliases;
	const struct builtin_export *const *tables;	/* NULL-terminated */
	/*
	 * Used only when no DLL of that name is found on disk, so a native
	 * copy placed on the search path wins. Set for the CRT (msvcrt.dll
	 * and its versioned names), which codecs sometimes ship themselves.
	 */
	int fallback;
};

#define BUILTIN_END { NULL, 0, NULL }

/* The DLL's named exports in sorted order: count, and the i-th one. */
size_t builtin_count(const struct builtin_dll *dll);
const struct builtin_export *builtin_entry(const struct builtin_dll *dll,
					   size_t i);

/* The built-in DLL serving name (e.g. "KERNEL32.dll"), or NULL. */
const struct builtin_dll *builtin_find(const char *name);

/* Every built-in DLL, NULL-terminated. */
extern const struct builtin_dll *const builtin_dlls[];

/* An export's address, or NULL if it is not implemented. */
void *builtin_export(const struct builtin_dll *dll, const char *name);
void *builtin_export_ordinal(const struct builtin_dll *dll, WORD ordinal);

/*
 * A page holding just a DOS and PE header, to serve as a built-in
 * module's HMODULE (some code inspects the header behind a handle).
 */
BYTE *builtin_fake_image(void);
void builtin_free_image(BYTE *base);

#endif /* VFW_RUNTIME_BUILTIN_H */
