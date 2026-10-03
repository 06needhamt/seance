/* SPDX-License-Identifier: GPL-2.0-only */
/* Compatibility entry points traditionally exported by kernel32. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "win32/internal.h"

typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;

WINFN(BOOL) k32_CreateProcessA(LPCSTR app, LPSTR cmd, void *pa, void *ta,
	BOOL inherit, DWORD flags, void *env, LPCSTR cwd, void *startup,
	PROCESS_INFORMATION *pi)
{
	if (pi) memset(pi, 0, sizeof(*pi));
	win_set_error(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

WINFN(DWORD) k32_GetLogicalDriveStringsA(DWORD size, LPSTR buf)
{
	static const char drives[] = "C:\\\0\0";
	if (!buf || size < sizeof(drives)) return sizeof(drives);
	memcpy(buf, drives, sizeof(drives));
	return sizeof(drives) - 1;
}

WINFN(DWORD) k32_GetProcessVersion(DWORD pid) { return 0x00050001; }
WINFN(UINT) k32_SetHandleCount(UINT count) { return count; }
WINFN(int) k32_MulDiv(int a, int b, int c)
{
	if (!c) return -1;
	long long v = (long long)a * b;
	v += (v < 0) == (c < 0) ? c / 2 : -(c / 2);
	v /= c;
	return v < INT32_MIN || v > INT32_MAX ? -1 : (int)v;
}
struct atom { char *name; unsigned refs; };
static struct atom atoms[0x4000];
static pthread_mutex_t atom_lock = PTHREAD_MUTEX_INITIALIZER;
WINFN(ATOM) k32_GlobalFindAtomA(LPCSTR name)
{
	if (!name) return 0;
	pthread_mutex_lock(&atom_lock);
	ATOM found = 0;
	for (unsigned i = 0; i < 0x4000; i++)
		if (atoms[i].name && !strcasecmp(atoms[i].name, name)) { found = 0xc000 + i; break; }
	pthread_mutex_unlock(&atom_lock);
	return found;
}
WINFN(ATOM) k32_GlobalAddAtomA(LPCSTR name)
{
	if (!name) { win_set_error(ERROR_INVALID_PARAMETER); return 0; }
	pthread_mutex_lock(&atom_lock);
	unsigned free_slot = 0x4000;
	for (unsigned i = 0; i < 0x4000; i++) {
		if (atoms[i].name && !strcasecmp(atoms[i].name, name)) {
			atoms[i].refs++; pthread_mutex_unlock(&atom_lock); return 0xc000 + i;
		}
		if (!atoms[i].name && free_slot == 0x4000) free_slot = i;
	}
	if (free_slot < 0x4000 && (atoms[free_slot].name = strdup(name))) atoms[free_slot].refs = 1;
	pthread_mutex_unlock(&atom_lock);
	return free_slot < 0x4000 && atoms[free_slot].name ? 0xc000 + free_slot : 0;
}
WINFN(ATOM) k32_GlobalDeleteAtom(ATOM atom)
{
	unsigned i = atom - 0xc000;
	if (i >= 0x4000) return atom;
	pthread_mutex_lock(&atom_lock);
	if (atoms[i].name && !--atoms[i].refs) { free(atoms[i].name); atoms[i].name = NULL; }
	pthread_mutex_unlock(&atom_lock); return 0;
}
WINFN(UINT) k32_GlobalGetAtomNameA(ATOM atom, LPSTR out, int size)
{
	unsigned i = atom - 0xc000; UINT n = 0;
	if (!out || size <= 0 || i >= 0x4000) return 0;
	pthread_mutex_lock(&atom_lock);
	if (atoms[i].name) { n = strlen(atoms[i].name); if (n >= (UINT)size) n = size - 1; memcpy(out, atoms[i].name, n); out[n] = 0; }
	pthread_mutex_unlock(&atom_lock); return n;
}
WINFN(void) k32_GlobalFix(HGLOBAL h) { (void)h; }
WINFN(void) k32_GlobalUnfix(HGLOBAL h) { (void)h; }

#define E(n) { #n, 0, (void *)k32_##n }
const struct builtin_export k32_legacy_exports[] = {
	E(CreateProcessA), E(GetLogicalDriveStringsA),
	E(GetProcessVersion), E(GlobalAddAtomA), E(GlobalDeleteAtom), E(GlobalFindAtomA),
	E(GlobalFix), E(GlobalGetAtomNameA), E(GlobalUnfix), E(MulDiv), E(SetHandleCount),
	BUILTIN_END
};
