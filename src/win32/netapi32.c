/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * netapi32.dll for a machine that shares nothing: siaviref.dll lists the
 * network shares, to tell local paths from shared ones, and finds none.
 * Buffers it gives are freed with NetApiBufferFree, so they come from the
 * same heap as NetApiBufferAllocate's.
 */
#include <stdlib.h>

#include "win32/internal.h"

#define NERR_Success 0

WINFN(DWORD) net_NetApiBufferAllocate(DWORD size, void **buf)
{
	if (!buf)
		return ERROR_INVALID_PARAMETER;
	*buf = calloc(1, size ? size : 1);
	return *buf ? NERR_Success : ERROR_NOT_ENOUGH_MEMORY;
}

WINFN(DWORD) net_NetApiBufferFree(void *buf)
{
	free(buf);
	return NERR_Success;
}

WINFN(DWORD) net_NetShareEnum(LPWSTR server, DWORD level, BYTE **buf,
			      DWORD max_len, DWORD *read, DWORD *total,
			      DWORD *resume)
{
	if (!buf || !read || !total)
		return ERROR_INVALID_PARAMETER;
	*buf = NULL;
	*read = *total = 0;
	if (resume)
		*resume = 0;
	return NERR_Success;
}

#define E(name) { #name, 0, (void *)net_##name }

static const struct builtin_export exports[] = {
	E(NetApiBufferAllocate),
	E(NetApiBufferFree),
	E(NetShareEnum),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_netapi32 = { "netapi32.dll", NULL, tables, 0 };
