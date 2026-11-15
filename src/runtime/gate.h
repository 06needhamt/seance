/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The call gate: how the harness calls into a DLL's entry points (DllMain,
 * a thread's start routine) as Windows' own kernel32 does.
 *
 * Two things set it apart from a plain C call through a function pointer.
 * It restores %esp itself afterwards, so a routine of the wrong calling
 * convention (a cdecl thread procedure handed to CreateThread, which leaves
 * its argument on the stack) can't shift the caller's frame; Windows calls
 * these from BaseThreadInitThunk, which never looks at its stack again. And
 * once the built-in kernel32's image exists, the gate runs from a copy
 * inside it, so the routine's return address lies in "kernel32": packed
 * DLLs (DivX 5.11, VSS H.264) find kernel32's base by scanning down from
 * that address, 64 KB at a time, for its MZ header.
 */
#ifndef VFW_RUNTIME_GATE_H
#define VFW_RUNTIME_GATE_H

#include <stddef.h>
#include <stdint.h>

#define GATE_MAX_ARGS 6

/* Calls fn(args[0], ..., args[n - 1]), stdcall or cdecl alike. */
uintptr_t gate_call(const void *fn, unsigned n, const uintptr_t *args);

/* The gate's machine code, position-independent, to copy into an image. */
size_t gate_code_size(void);
const void *gate_code(void);

/* Where the copy is; gate_call runs it from then on. */
void gate_set_copy(void *code);
const void *gate_copy(void);

/*
 * kernel32's DebugBreak, "int3; ret" as on Windows, gate_break_offset()
 * bytes after the start of the gate's copy, so a breakpoint is raised from
 * inside kernel32's image as a leaf the x64 unwinder can step out of into
 * the caller. NULL until the copy exists.
 */
size_t gate_break_offset(void);
const void *gate_break(void);

#endif
