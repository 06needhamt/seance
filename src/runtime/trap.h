/*
 * Trap thunks: what an import of an unimplemented built-in function is
 * bound to. The DLL still loads; only calling the function stops the
 * harness, with a message naming it and the caller.
 */
#ifndef VFW_RUNTIME_TRAP_H
#define VFW_RUNTIME_TRAP_H

#include "win32.h"

/* A callable thunk for dll!name (or dll!#ordinal when name is NULL). */
void *trap_thunk(const char *dll, const char *name, WORD ordinal);

/*
 * The same for a function that can't be had for another reason, given as
 * why (e.g. "zz.dll could not be loaded"), which the report includes.
 */
void *trap_thunk_why(const char *dll, const char *name, WORD ordinal,
		     const char *why);

/* Non-zero if addr is one of the trap thunks. */
int trap_is_thunk(const void *addr);

#endif /* VFW_RUNTIME_TRAP_H */
