/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A record of names noted as missing, such as DLLs a DLL looked for and
 * didn't find, kept as a ring of the last few. A message about a codec
 * that gave up can then name what it looked for since a mark.
 *
 * Callers keep one per thread (static __thread), so a message names only
 * what that thread's open looked for: a DllMain or DRV_LOAD runs on the
 * thread that opens the driver.
 */
#ifndef VFW_COMMON_NOTED_H
#define VFW_COMMON_NOTED_H

#include <stddef.h>

#define NOTED_MAX 16

struct noted {
	char *names[NOTED_MAX];	/* a ring of the last ones */
	unsigned count;		/* how many ever noted */
};

void noted_add(struct noted *n, const char *name);

/* Where the record stands, for noted_since(). */
unsigned noted_mark(const struct noted *n);

/*
 * Writes the names noted since mark, each once (whatever its case),
 * ", "-separated; "" if none. Returns how many it wrote.
 */
int noted_since(const struct noted *n, unsigned mark, char *buf, size_t size);

#endif /* VFW_COMMON_NOTED_H */
