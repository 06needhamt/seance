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
 * Win64 code keeps it 16-byte aligned, as the SysV ABI does.
 */
#if defined(__x86_64__)
#define STUB_WINAPI __attribute__((ms_abi))
#define STUB_CDECL  __attribute__((ms_abi))
#else
#define STUB_WINAPI __attribute__((stdcall, force_align_arg_pointer))
#define STUB_CDECL  __attribute__((cdecl, force_align_arg_pointer))
#endif

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
	 * copy placed on the search path wins: BUILTIN_FALLBACK for every name
	 * it answers to, BUILTIN_FALLBACK_ALIASES for its aliases alone. The
	 * CRT is the latter: codecs ship their own msvcr71/80/90.dll, which
	 * win, but msvcrt.dll is one of Windows' KnownDLLs, always loaded from
	 * system32 even when an older copy sits beside the codec.
	 */
	int fallback;
};

#define BUILTIN_FALLBACK         1
#define BUILTIN_FALLBACK_ALIASES 2

#define BUILTIN_END { NULL, 0, NULL }

/* Whether a DLL called name on the search path is used instead of b. */
int builtin_shadowable(const struct builtin_dll *b, const char *name);

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
BYTE *builtin_fake_image(const struct builtin_dll *dll);
void builtin_free_image(BYTE *base);

#endif /* VFW_RUNTIME_BUILTIN_H */
