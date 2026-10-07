/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * bcrypt.dll: the random number generator, which is what FFmpeg's
 * libraries use it for (the system RNG seeds their hash tables and tags).
 * Other algorithms are reported as not found, as Windows does for a name it
 * doesn't know, so a caller that wants one can say so.
 */
#include <stdlib.h>
#include <strings.h>
#include <sys/random.h>

#include "win32/codepage.h"
#include "win32/internal.h"

typedef LONG NTSTATUS;

#define STATUS_SUCCESS           ((NTSTATUS)0x00000000)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_HANDLE    ((NTSTATUS)0xC0000008)
#define STATUS_NOT_FOUND         ((NTSTATUS)0xC0000225)
#define STATUS_NO_MEMORY         ((NTSTATUS)0xC0000017)

#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002

/* A provider handle: a tag to tell ours from other pointers. */
#define PROVIDER_MAGIC 0x52474e42u	/* "BNGR" */

struct provider {
	DWORD magic;
};

static int is_provider(const void *h)
{
	return h && ((const struct provider *)h)->magic == PROVIDER_MAGIC;
}

WINFN(NTSTATUS) bcrypt_BCryptOpenAlgorithmProvider(void **out, LPCWSTR id,
						   LPCWSTR impl, ULONG flags)
{
	char *name;
	int rng;
	struct provider *p;

	if (!out || !id || flags)
		return STATUS_INVALID_PARAMETER;
	name = utf16_to_utf8_dup(id);
	rng = name && !strcasecmp(name, "RNG");
	free(name);
	if (!rng)
		return STATUS_NOT_FOUND;
	if (!(p = malloc(sizeof(*p))))
		return STATUS_NO_MEMORY;
	p->magic = PROVIDER_MAGIC;
	*out = p;
	return STATUS_SUCCESS;
}

WINFN(NTSTATUS) bcrypt_BCryptCloseAlgorithmProvider(void *h, ULONG flags)
{
	if (!is_provider(h))
		return STATUS_INVALID_HANDLE;
	((struct provider *)h)->magic = 0;
	free(h);
	return STATUS_SUCCESS;
}

WINFN(NTSTATUS) bcrypt_BCryptGenRandom(void *h, BYTE *buf, ULONG len,
				       ULONG flags)
{
	/* Without the system RNG flag, a provider handle is required. */
	if (!(flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG) && !is_provider(h))
		return STATUS_INVALID_HANDLE;
	if (!buf && len)
		return STATUS_INVALID_PARAMETER;
	while (len) {
		ssize_t n = getrandom(buf, len, 0);

		if (n <= 0)
			return STATUS_NO_MEMORY;
		buf += n;
		len -= n;
	}
	return STATUS_SUCCESS;
}

#define E(name) { #name, 0, (void *)bcrypt_##name }

static const struct builtin_export exports[] = {
	E(BCryptCloseAlgorithmProvider),
	E(BCryptGenRandom),
	E(BCryptOpenAlgorithmProvider),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_bcrypt = { "bcrypt.dll", NULL, tables, 0 };
