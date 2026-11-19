/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * 3DNow! in software. Codecs from the K6 and Athlon years (ARVI's
 * _arvic.dll among them) run 3DNow! code without asking the CPU whether
 * it has it, and no CPU made since 2010 or so does: each such instruction
 * raises #UD, which Linux delivers as SIGILL. The fault handler hands the
 * instruction here first; this works out what it would have done, on
 * the MMX registers in the signal context, and steps past it.
 *
 * Covered: the 3DNow! (0F 0F modrm imm8) and Extended 3DNow! operations,
 * FEMMS (0F 0E) and PREFETCH/PREFETCHW (0F 0D). IsProcessorFeaturePresent
 * still says what the CPU says, so a codec that asks takes its MMX or SSE
 * path; this is for the ones that don't ask.
 *
 * It is slow - a signal per instruction - but exact where AMD defines the
 * result. PFRCP and PFRSQRT give the full single-precision answer rather
 * than the 14- and 15-bit estimates, and the Newton-Raphson steps that
 * refine them (PFRCPIT1, PFRSQIT1, PFRCPIT2) pass their second operand
 * through, so the sequences AMD documents end with that answer, as QEMU
 * does it.
 */
#ifndef VFW_RUNTIME_AMD3DNOW_H
#define VFW_RUNTIME_AMD3DNOW_H

#include <signal.h>

/*
 * If the instruction at the context's program counter is one of the
 * above, carries it out on the context and moves the program counter
 * past it: returns 1, and the signal handler just returns. 0 for any
 * other instruction, or one whose memory operand can't be read (which
 * the caller then reports as the illegal instruction it is). For the
 * fault handler: it takes no locks and allocates nothing.
 */
int amd3dnow_emulate(void *ucontext);

/* The number of instructions emulated so far, for tests and traces. */
unsigned long amd3dnow_count(void);

#endif
