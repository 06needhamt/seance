/* SPDX-License-Identifier: GPL-2.0-only */
/* Compatibility entry points traditionally exported by kernel32. */
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

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
	static const char drives[] = "C:\\\0";
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
static struct atom local_atoms[0x4000];
static pthread_mutex_t atom_lock = PTHREAD_MUTEX_INITIALIZER;
static ATOM find_atom(struct atom *table, LPCSTR name)
{
	if (!name) return 0;
	if ((ULONG_PTR)name <= 0xffff) return (ATOM)(ULONG_PTR)name;
	pthread_mutex_lock(&atom_lock);
	ATOM found = 0;
	for (unsigned i = 0; i < 0x4000; i++)
		if (table[i].name && !strcasecmp(table[i].name, name)) { found = 0xc000 + i; break; }
	pthread_mutex_unlock(&atom_lock);
	return found;
}
static ATOM add_atom(struct atom *table, LPCSTR name)
{
	if (!name) { win_set_error(ERROR_INVALID_PARAMETER); return 0; }
	if ((ULONG_PTR)name <= 0xffff) return (ATOM)(ULONG_PTR)name;
	pthread_mutex_lock(&atom_lock);
	unsigned free_slot = 0x4000;
	for (unsigned i = 0; i < 0x4000; i++) {
		if (table[i].name && !strcasecmp(table[i].name, name)) {
			table[i].refs++; pthread_mutex_unlock(&atom_lock); return 0xc000 + i;
		}
		if (!table[i].name && free_slot == 0x4000) free_slot = i;
	}
	if (free_slot < 0x4000 && (table[free_slot].name = strdup(name))) table[free_slot].refs = 1;
	pthread_mutex_unlock(&atom_lock);
	return free_slot < 0x4000 && table[free_slot].name ? 0xc000 + free_slot : 0;
}
static ATOM delete_atom(struct atom *table, ATOM atom)
{
	if (atom < 0xc000) return 0;
	unsigned i = atom - 0xc000;
	if (i >= 0x4000) return atom;
	pthread_mutex_lock(&atom_lock);
	if (table[i].name && !--table[i].refs) { free(table[i].name); table[i].name = NULL; }
	pthread_mutex_unlock(&atom_lock); return 0;
}
static UINT get_atom_name(struct atom *table, ATOM atom, LPSTR out, int size)
{
	UINT n = 0;
	if (atom < 0xc000) {
		if (!out || size <= 0) return 0;
		n = (UINT)snprintf(out, size, "#%u", atom);
		return n < (UINT)size ? n : (UINT)size - 1;
	}
	unsigned i = atom - 0xc000;
	if (!out || size <= 0 || i >= 0x4000) return 0;
	pthread_mutex_lock(&atom_lock);
	if (table[i].name) { n = strlen(table[i].name); if (n >= (UINT)size) n = size - 1; memcpy(out, table[i].name, n); out[n] = 0; }
	pthread_mutex_unlock(&atom_lock); return n;
}
WINFN(ATOM) k32_GlobalFindAtomA(LPCSTR n) { return find_atom(atoms, n); }
WINFN(ATOM) k32_GlobalAddAtomA(LPCSTR n) { return add_atom(atoms, n); }
WINFN(ATOM) k32_GlobalDeleteAtom(ATOM a) { return delete_atom(atoms, a); }
WINFN(UINT) k32_GlobalGetAtomNameA(ATOM a, LPSTR o, int n) { return get_atom_name(atoms, a, o, n); }
WINFN(ATOM) k32_FindAtomA(LPCSTR n) { return find_atom(local_atoms, n); }
WINFN(ATOM) k32_AddAtomA(LPCSTR n) { return add_atom(local_atoms, n); }
WINFN(ATOM) k32_DeleteAtom(ATOM a) { return delete_atom(local_atoms, a); }
WINFN(UINT) k32_GetAtomNameA(ATOM a, LPSTR o, int n) { return get_atom_name(local_atoms, a, o, n); }
WINFN(void) k32_GlobalFix(HGLOBAL h) { (void)h; }
WINFN(void) k32_GlobalUnfix(HGLOBAL h) { (void)h; }

WINFN(BOOL) k32_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_size,
	LPVOID out, DWORD out_size, LPDWORD returned, LPVOID overlapped)
{
	(void)code; (void)in; (void)in_size; (void)out; (void)out_size; (void)overlapped;
	if (returned) *returned = 0;
	if (file_handle_fd(h) < 0) return FALSE;
	win_set_error(ERROR_INVALID_FUNCTION);
	return FALSE;
}
WINFN(LONG) k32__hread(int h, LPVOID buffer, LONG count)
{
	ssize_t n = read((int)h, buffer, (size_t)count);
	return n < 0 ? WIN_ERRNO(-1) : (LONG)n;
}
WINFN(LONG) k32__hwrite(int h, LPCSTR buffer, LONG count)
{
	ssize_t n = write((int)h, buffer, (size_t)count);
	return n < 0 ? WIN_ERRNO(-1) : (LONG)n;
}
WINFN(int) k32__lcreat(LPCSTR path, int attr)
{
	(void)attr;
	char *p = path_to_unix(path);
	if (!p) return -1;
	int fd = open(p, O_CREAT | O_TRUNC | O_RDWR, 0666);
	free(p);
	return fd < 0 ? WIN_ERRNO(-1) : fd;
}
WINFN(int) k32__lclose(int h) { return close(h) ? WIN_ERRNO(-1) : 0; }
WINFN(LONG) k32__llseek(int h, LONG off, int origin)
{
	off_t n = lseek((int)h, off, origin);
	return n < 0 ? WIN_ERRNO(-1) : (LONG)n;
}

#define E(n) { #n, 0, (void *)k32_##n }
const struct builtin_export k32_legacy_exports[] = {
	E(AddAtomA), E(CreateProcessA), E(DeleteAtom), E(DeviceIoControl), E(FindAtomA), E(GetAtomNameA), E(GetLogicalDriveStringsA),
	E(GetProcessVersion), E(GlobalAddAtomA), E(GlobalDeleteAtom), E(GlobalFindAtomA),
	E(GlobalFix), E(GlobalGetAtomNameA), E(GlobalUnfix), E(MulDiv), E(SetHandleCount),
	E(_hread), E(_hwrite), E(_lclose), E(_lcreat), E(_llseek),
	BUILTIN_END
};
