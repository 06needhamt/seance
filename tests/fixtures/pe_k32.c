/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Exercises kernel32 from the Win32 side. Built with a 4-byte preferred
 * stack boundary, as old MSVC code is, so calls reach the stubs misaligned.
 * k32_selftest returns 0, or the number of the first check that failed.
 */
#include "fixture.h"

static HINSTANCE self;
static CRITICAL_SECTION lock;
static LONG shared_count;
static HANDLE ready;
static DWORD tls;

static DWORD WINAPI worker(LPVOID arg)
{
	int i;

	if (TlsGetValue(tls))
		return 100;
	TlsSetValue(tls, arg);
	for (i = 0; i < 1000; i++) {
		EnterCriticalSection(&lock);
		shared_count++;
		LeaveCriticalSection(&lock);
	}
	SetEvent(ready);
	return (DWORD)(ULONG_PTR)TlsGetValue(tls);
}

static int ends_with(const char *s, const char *suffix)
{
	int n = lstrlenA(s), m = lstrlenA(suffix);
	return n >= m && !lstrcmpiA(s + n - m, suffix);
}

/* TerminateThread's targets: one spinning in this DLL's code, one waiting,
 * one asleep, and one that never starts. */
static volatile LONG spins, never_ran = 1;
static HANDLE never_set;

static DWORD WINAPI spinner(LPVOID arg)
{
	for (;;)
		spins++;
	return 0;
}

static DWORD WINAPI waiter(LPVOID arg)
{
	WaitForSingleObject(never_set, INFINITE);
	return 1;
}

static DWORD WINAPI sleeper(LPVOID arg)
{
	Sleep(INFINITE);
	return 1;
}

static DWORD WINAPI unstarted(LPVOID arg)
{
	never_ran = 0;
	return 1;
}

static int terminated(HANDLE t, DWORD want)
{
	DWORD code = 0;
	int ok = TerminateThread(t, want) &&
		 WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 &&
		 GetExitCodeThread(t, &code) && code == want;
	CloseHandle(t);
	return ok;
}

/* Returns 0, or the number of the first check that failed. */
EXPORT int k32_terminate_test(void)
{
	never_set = CreateEventA(NULL, TRUE, FALSE, NULL);

	HANDLE t = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
	while (t && spins < 1000)
		Sleep(1);
	if (!t || !terminated(t, 5))
		return 1;
	LONG at = spins;
	Sleep(20);
	if (spins != at)
		return 2;

	t = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
	Sleep(20);
	if (!t || !terminated(t, 6))
		return 3;

	t = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
	Sleep(20);
	if (!t || !terminated(t, 7))
		return 4;

	t = CreateThread(NULL, 0, unstarted, NULL, CREATE_SUSPENDED, NULL);
	if (!t || !terminated(t, 8) || !never_ran)
		return 5;
	CloseHandle(never_set);
	return 0;
}

EXPORT int k32_selftest(void)
{
	char path[MAX_PATH];
	WCHAR wide[16];
	DWORD code;

	/* 1: heap */
	char *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64);
	if (!p || p[63] || HeapSize(GetProcessHeap(), 0, p) != 64)
		return 1;
	HeapFree(GetProcessHeap(), 0, p);

	/* 2-3: threads, a critical section, an event and TLS */
	InitializeCriticalSection(&lock);
	ready = CreateEventA(NULL, TRUE, FALSE, NULL);
	tls = TlsAlloc();
	TlsSetValue(tls, (LPVOID)7);
	HANDLE t = CreateThread(NULL, 0, worker, (LPVOID)77, 0, NULL);
	if (!t || WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0)
		return 2;
	WaitForSingleObject(t, INFINITE);
	if (!GetExitCodeThread(t, &code) || code != 77 || shared_count != 1000 ||
	    TlsGetValue(tls) != (LPVOID)7)
		return 3;
	CloseHandle(t);
	CloseHandle(ready);
	DeleteCriticalSection(&lock);
	TlsFree(tls);

	/* 4: our own file name, as a Windows path */
	if (!GetModuleFileNameA(self, path, sizeof(path)) ||
	    !ends_with(path, "\\pe_k32.dll") || path[1] != ':')
		return 4;

	/* 5: our own resource */
	HRSRC r = FindResourceA(self, MAKEINTRESOURCEA(1), MAKEINTRESOURCEA(10));
	HGLOBAL g = r ? LoadResource(self, r) : NULL;
	const char *data = g ? LockResource(g) : NULL;
	if (!data || SizeofResource(self, r) < 12 || data[0] != 'k' ||
	    data[11] != 'e')
		return 5;

	/* 6: GetProcAddress agrees with the import */
	DWORD (WINAPI *tid)(void) = (DWORD (WINAPI *)(void))
		GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetCurrentThreadId");
	if (!tid || tid() != GetCurrentThreadId())
		return 6;

	/* 7: code pages */
	if (MultiByteToWideChar(CP_ACP, 0, "\x80x", -1, wide, 16) != 3 ||
	    wide[0] != 0x20ac)
		return 7;

	/* 8: virtual memory */
	BYTE *v = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!v)
		return 8;
	v[65535] = 1;
	if (!VirtualFree(v, 0, MEM_RELEASE))
		return 8;

	/* 9: last error lives in the TEB */
	SetLastError(ERROR_ACCESS_DENIED);
	if (GetLastError() != ERROR_ACCESS_DENIED)
		return 9;

	/* 10: a file in the sandbox */
	HANDLE f = CreateFileA("C:\\WINDOWS\\TEMP\\pe_k32.tmp", GENERIC_READ |
			       GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			       FILE_FLAG_DELETE_ON_CLOSE, NULL);
	DWORD n = 0;
	if (f == INVALID_HANDLE_VALUE || !WriteFile(f, "abc", 3, &n, NULL) || n != 3)
		return 10;
	CloseHandle(f);
	if (GetFileAttributesA("C:\\WINDOWS\\TEMP\\pe_k32.tmp") != INVALID_FILE_ATTRIBUTES)
		return 10;

	Sleep(1);
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		self = inst;
		DisableThreadLibraryCalls(inst);
	}
	return TRUE;
}
