/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Buffers a DLL writes into, each in its own mapping that ends at an
 * inaccessible page: a codec writing past its output buffer faults on the
 * spot, as it might well on Windows, instead of corrupting the heap for
 * someone else to trip over later. The crash report then says which
 * buffer it was and by how much it was overrun.
 */
#ifndef VFW_RUNTIME_GUARD_H
#define VFW_RUNTIME_GUARD_H

#include <stddef.h>

/*
 * n zeroed bytes (at least 1) at the start of a page, with the guard page
 * after the last page they touch: what is left of that page is slack, as
 * the Windows heap and glibc give big blocks, and some codecs need it
 * (aslcodec_vfw.dll's BGR24 output writes a little past the frame;
 * Sif1_vfw.dll refused BGR24 into a buffer flush against the guard). what
 * names the buffer in crash reports ("output buffer"). NULL when out of
 * memory.
 */
void *guard_alloc(size_t n, const char *what);
/* Frees what guard_alloc returned (NULL is fine). */
void guard_free(void *p);

/*
 * If addr is in or just past a guarded buffer, says so in out, e.g.
 * "4 bytes past the end of the 1024-byte output buffer"; returns 0 if it
 * isn't. Safe in a signal handler.
 */
int guard_describe(const void *addr, char *out, size_t size);

#endif /* VFW_RUNTIME_GUARD_H */
