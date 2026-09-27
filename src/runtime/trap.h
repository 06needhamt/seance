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

/* Non-zero if addr is one of the trap thunks. */
int trap_is_thunk(const void *addr);

#endif /* VFW_RUNTIME_TRAP_H */
