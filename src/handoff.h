/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SEANCE_HANDOFF_H
#define SEANCE_HANDOFF_H

#include "win32/types.h"

/*
 * seance runs i386 codecs and seance64 x86-64 ones. A command given a
 * codec of the other architecture runs the same command in the other
 * binary: $VFW_SEANCE64 (or $VFW_SEANCE32), else seance64 (or seance)
 * beside this one, through $VFW_EMULATOR64 (or $VFW_EMULATOR32) if set.
 */

/* Remembers the command line, for a hand-off later. */
void handoff_init(int argc, char **argv);

/*
 * The other binary's path, in out; 0 if there is one to run, else -1
 * with an error set.
 */
int handoff_sibling(char *out, size_t size);

/*
 * Runs the command line given to handoff_init() in the other binary, for
 * a codec of the given machine type. Returns only if it couldn't (with
 * an error set, and a message printed); not at all once handed off.
 */
void handoff_exec(WORD machine);

/* Hands off now if path is a PE file of the other machine type. */
void handoff_if_foreign(const char *path);

/* Arms pe_map() to hand off when the first image it maps is foreign. */
void handoff_arm(void);

#endif
