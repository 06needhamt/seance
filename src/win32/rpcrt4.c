/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rpcrt4.dll: the UUID functions codecs call (CineForm's cinevf2w.dll
 * imports UuidCreate), with the strings they make and take. There is no
 * RPC here: nothing else is exported.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "win32/internal.h"

#define RPC_S_OK                 0
#define RPC_S_INVALID_STRING_UUID 1705
#define RPC_S_OUT_OF_MEMORY      14
#define RPC_S_INVALID_ARG        87
#define RPC_S_UUID_LOCAL_ONLY    1824

static int random_bytes(void *p, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = getrandom((BYTE *)p + got, n - got, 0);

		if (r < 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

/* A version 4 (random) UUID. */
WINFN(LONG) rpc_UuidCreate(GUID *uuid)
{
	if (!uuid)
		return RPC_S_INVALID_ARG;
	if (random_bytes(uuid, sizeof(*uuid)))
		return RPC_S_OUT_OF_MEMORY;
	uuid->Data3 = (WORD)((uuid->Data3 & 0x0fff) | 0x4000);
	uuid->Data4[0] = (BYTE)((uuid->Data4[0] & 0x3f) | 0x80);
	return RPC_S_OK;
}

/* There is no network address to base one on, so as Windows says for that. */
WINFN(LONG) rpc_UuidCreateSequential(GUID *uuid)
{
	LONG r = rpc_UuidCreate(uuid);

	return r ? r : RPC_S_UUID_LOCAL_ONLY;
}

WINFN(LONG) rpc_UuidCreateNil(GUID *uuid)
{
	if (!uuid)
		return RPC_S_INVALID_ARG;
	memset(uuid, 0, sizeof(*uuid));
	return RPC_S_OK;
}

WINFN(int) rpc_UuidIsNil(const GUID *uuid, LONG *status)
{
	static const GUID nil;

	if (status)
		*status = RPC_S_OK;
	return !uuid || !memcmp(uuid, &nil, sizeof(nil));
}

WINFN(int) rpc_UuidEqual(const GUID *a, const GUID *b, LONG *status)
{
	static const GUID nil;

	if (status)
		*status = RPC_S_OK;
	if (!a)
		a = &nil;
	if (!b)
		b = &nil;
	return !memcmp(a, b, sizeof(*a));
}

WINFN(int) rpc_UuidCompare(const GUID *a, const GUID *b, LONG *status)
{
	static const GUID nil;

	if (status)
		*status = RPC_S_OK;
	if (!a)
		a = &nil;
	if (!b)
		b = &nil;
	if (a->Data1 != b->Data1)
		return a->Data1 < b->Data1 ? -1 : 1;
	if (a->Data2 != b->Data2)
		return a->Data2 < b->Data2 ? -1 : 1;
	if (a->Data3 != b->Data3)
		return a->Data3 < b->Data3 ? -1 : 1;
	int c = memcmp(a->Data4, b->Data4, 8);

	return c < 0 ? -1 : c > 0;
}

WINFN(WORD) rpc_UuidHash(const GUID *uuid, LONG *status)
{
	const WORD *w = (const WORD *)uuid;
	WORD h = 0;

	if (status)
		*status = RPC_S_OK;
	for (int i = 0; uuid && i < 8; i++)
		h ^= w[i];
	return h;
}

static void format(char *out, const GUID *u)
{
	snprintf(out, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 (unsigned)u->Data1, u->Data2, u->Data3, u->Data4[0],
		 u->Data4[1], u->Data4[2], u->Data4[3], u->Data4[4],
		 u->Data4[5], u->Data4[6], u->Data4[7]);
}

/* The string is malloc'd, to be freed with RpcStringFree. */
WINFN(LONG) rpc_UuidToStringA(const GUID *uuid, BYTE **str)
{
	static const GUID nil;
	char *s;

	if (!str)
		return RPC_S_INVALID_ARG;
	s = malloc(37);
	if (!s)
		return RPC_S_OUT_OF_MEMORY;
	format(s, uuid ? uuid : &nil);
	*str = (BYTE *)s;
	return RPC_S_OK;
}

WINFN(LONG) rpc_UuidToStringW(const GUID *uuid, WORD **str)
{
	char a[37];
	WORD *w;

	if (!str)
		return RPC_S_INVALID_ARG;
	w = malloc(37 * sizeof(WORD));
	if (!w)
		return RPC_S_OUT_OF_MEMORY;
	{
		static const GUID nil;

		format(a, uuid ? uuid : &nil);
	}
	for (int i = 0; i < 37; i++)
		w[i] = (BYTE)a[i];
	*str = w;
	return RPC_S_OK;
}

WINFN(LONG) rpc_RpcStringFreeA(BYTE **str)
{
	if (str) {
		free(*str);
		*str = NULL;
	}
	return RPC_S_OK;
}

WINFN(LONG) rpc_RpcStringFreeW(WORD **str)
{
	if (str) {
		free(*str);
		*str = NULL;
	}
	return RPC_S_OK;
}

/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", braces not allowed; NULL is nil. */
WINFN(LONG) rpc_UuidFromStringA(const BYTE *str, GUID *uuid)
{
	const char *s = (const char *)str;
	unsigned d1, d2, d3, b[8];
	int n = 0;

	if (!uuid)
		return RPC_S_INVALID_ARG;
	if (!s) {
		memset(uuid, 0, sizeof(*uuid));
		return RPC_S_OK;
	}
	if (strlen(s) != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' ||
	    s[23] != '-')
		return RPC_S_INVALID_STRING_UUID;
	for (int i = 0; i < 36; i++)
		if (i != 8 && i != 13 && i != 18 && i != 23 &&
		    !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
		      (s[i] >= 'A' && s[i] <= 'F')))
			return RPC_S_INVALID_STRING_UUID;
	if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x%n", &d1, &d2, &d3,
		   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7], &n) < 11)
		return RPC_S_INVALID_STRING_UUID;
	uuid->Data1 = d1;
	uuid->Data2 = (WORD)d2;
	uuid->Data3 = (WORD)d3;
	for (int i = 0; i < 8; i++)
		uuid->Data4[i] = (BYTE)b[i];
	return RPC_S_OK;
}

WINFN(LONG) rpc_UuidFromStringW(const WORD *str, GUID *uuid)
{
	char a[64];
	size_t i;

	if (!str)
		return rpc_UuidFromStringA(NULL, uuid);
	for (i = 0; str[i] && i < sizeof(a) - 1; i++)
		a[i] = str[i] < 0x80 ? (char)str[i] : '?';
	a[i] = 0;
	return rpc_UuidFromStringA((const BYTE *)a, uuid);
}

#define E(name) { #name, 0, (void *)rpc_##name }

static const struct builtin_export exports[] = {
	E(RpcStringFreeA),
	E(RpcStringFreeW),
	E(UuidCompare),
	E(UuidCreate),
	E(UuidCreateNil),
	E(UuidCreateSequential),
	E(UuidEqual),
	E(UuidFromStringA),
	E(UuidFromStringW),
	E(UuidHash),
	E(UuidIsNil),
	E(UuidToStringA),
	E(UuidToStringW),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_rpcrt4 = { "rpcrt4.dll", NULL, tables, 0 };
