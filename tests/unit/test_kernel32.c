/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The kernel32 stubs, called through the export table the way a DLL's
 * imports reach them.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "runtime/teb.h"
#include "test.h"

static struct module *k32;

static void *get(const char *name)
{
	void *p = mod_get_proc(k32, name);
	if (!p) {
		fprintf(stderr, "kernel32 has no %s\n", name);
		exit(1);
	}
	return p;
}

/* Declares a local pointer to a kernel32 export with the given signature. */
#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = (ret (WINAPI *)(__VA_ARGS__))get(#name)

static char root[256];

/* Memory. */

static void test_heaps(void)
{
	FN(HANDLE, GetProcessHeap, void);
	FN(LPVOID, HeapAlloc, HANDLE, DWORD, SIZE_T);
	FN(BOOL, HeapFree, HANDLE, DWORD, LPVOID);
	FN(LPVOID, HeapReAlloc, HANDLE, DWORD, LPVOID, SIZE_T);
	FN(SIZE_T, HeapSize, HANDLE, DWORD, LPCVOID);
	FN(HANDLE, HeapCreate, DWORD, SIZE_T, SIZE_T);
	FN(BOOL, HeapDestroy, HANDLE);
	FN(BOOL, HeapValidate, HANDLE, DWORD, LPCVOID);
	FN(DWORD, GetLastError, void);

	HANDLE ph = GetProcessHeap();
	CHECK(ph != NULL);
	CHECK(peb_get()->ProcessHeap == ph);

	BYTE *p = HeapAlloc(ph, HEAP_ZERO_MEMORY, 100);
	CHECK(p != NULL);
	CHECK_EQ((uintptr_t)p % 16, 0);
	int zero = 1;
	for (int i = 0; i < 100; i++)
		zero &= p[i] == 0;
	CHECK(zero);
	CHECK_EQ(HeapSize(ph, 0, p), 100);
	memset(p, 0xab, 100);

	CHECK(HeapReAlloc(ph, HEAP_REALLOC_IN_PLACE_ONLY, p, 200) == NULL);
	CHECK_EQ(GetLastError(), ERROR_NOT_ENOUGH_MEMORY);
	CHECK(HeapReAlloc(ph, HEAP_REALLOC_IN_PLACE_ONLY, p, 50) == p);
	CHECK_EQ(HeapSize(ph, 0, p), 50);

	p = HeapReAlloc(ph, HEAP_ZERO_MEMORY, p, 400);
	CHECK(p != NULL);
	CHECK_EQ(p[49], 0xab);
	CHECK_EQ(p[50], 0);
	CHECK_EQ(p[399], 0);
	CHECK(HeapValidate(ph, 0, p));
	CHECK(HeapFree(ph, 0, p));
	CHECK(HeapFree(ph, 0, NULL));

	/* Blocks of another heap are refused, as is a double free. */
	HANDLE h = HeapCreate(0, 0, 0);
	CHECK(h != NULL && h != ph);
	void *a = HeapAlloc(h, 0, 32);
	void *b = HeapAlloc(h, 0, 64);
	CHECK(a && b);
	CHECK(!HeapFree(ph, 0, a));
	CHECK(HeapFree(h, 0, a));
	CHECK(!HeapFree(h, 0, a));
	CHECK(!HeapValidate(h, 0, a));
	/* HeapDestroy releases whatever is still allocated (b). */
	CHECK(HeapDestroy(h));
	CHECK(HeapAlloc(h, 0, 1) == NULL);
	CHECK_EQ(GetLastError(), ERROR_INVALID_HANDLE);
	CHECK(!HeapDestroy(ph));
}

static void test_global(void)
{
	FN(HGLOBAL, GlobalAlloc, UINT, SIZE_T);
	FN(LPVOID, GlobalLock, HGLOBAL);
	FN(BOOL, GlobalUnlock, HGLOBAL);
	FN(SIZE_T, GlobalSize, HGLOBAL);
	FN(UINT, GlobalFlags, HGLOBAL);
	FN(HGLOBAL, GlobalReAlloc, HGLOBAL, SIZE_T, UINT);
	FN(HGLOBAL, GlobalHandle, LPCVOID);
	FN(HGLOBAL, GlobalFree, HGLOBAL);
	FN(HLOCAL, LocalAlloc, UINT, SIZE_T);
	FN(HLOCAL, LocalFree, HLOCAL);

	HGLOBAL h = GlobalAlloc(GHND, 64);
	CHECK(h != NULL);
	BYTE *p = GlobalLock(h);
	CHECK(p != NULL && (void *)p != h);
	CHECK_EQ(p[63], 0);
	CHECK_EQ(GlobalSize(h), 64);
	CHECK_EQ(GlobalFlags(h) & GMEM_LOCKCOUNT, 1);
	CHECK(GlobalHandle(p) == h);
	p[0] = 7;
	CHECK(!GlobalUnlock(h));
	CHECK_EQ(GlobalFlags(h) & GMEM_LOCKCOUNT, 0);
	CHECK(GlobalReAlloc(h, 4096, GMEM_MOVEABLE) == h);
	CHECK_EQ(GlobalSize(h), 4096);
	p = GlobalLock(h);
	CHECK_EQ(p[0], 7);
	GlobalUnlock(h);
	CHECK(GlobalFree(h) == NULL);

	/* Fixed blocks are plain pointers. */
	void *f = GlobalAlloc(GPTR, 16);
	CHECK(f != NULL && GlobalLock(f) == f);
	CHECK_EQ(GlobalSize(f), 16);
	CHECK(GlobalFree(f) == NULL);

	void *l = LocalAlloc(LPTR, 8);
	CHECK(l != NULL);
	CHECK(LocalFree(l) == NULL);
}

static void test_virtual(void)
{
	FN(LPVOID, VirtualAlloc, LPVOID, SIZE_T, DWORD, DWORD);
	FN(BOOL, VirtualFree, LPVOID, SIZE_T, DWORD);
	FN(SIZE_T, VirtualQuery, LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
	FN(BOOL, VirtualProtect, LPVOID, SIZE_T, DWORD, PDWORD);
	FN(BOOL, IsBadReadPtr, LPCVOID, UINT_PTR);
	FN(BOOL, IsBadWritePtr, LPVOID, UINT_PTR);
	MEMORY_BASIC_INFORMATION mbi;

	BYTE *r = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE, PAGE_NOACCESS);
	CHECK(r != NULL);
	CHECK_EQ((uintptr_t)r % 0x10000, 0);
	CHECK_EQ(VirtualQuery(r, &mbi, sizeof(mbi)), sizeof(mbi));
	CHECK_EQ(mbi.State, MEM_RESERVE);
	CHECK(mbi.AllocationBase == r);
	CHECK(IsBadReadPtr(r, 1));

	BYTE *c = VirtualAlloc(r + 0x3000, 0x2000, MEM_COMMIT, PAGE_READWRITE);
	CHECK(c == r + 0x3000);
	c[0] = 1;
	c[0x1fff] = 2;
	VirtualQuery(c, &mbi, sizeof(mbi));
	CHECK_EQ(mbi.State, MEM_COMMIT);
	CHECK_EQ(mbi.Protect, PAGE_READWRITE);
	CHECK(mbi.AllocationBase == r);
	CHECK_EQ(mbi.Type, MEM_PRIVATE);
	CHECK_EQ(mbi.RegionSize, 0x2000);
	CHECK(!IsBadWritePtr(c, 0x2000));

	DWORD old = 0;
	CHECK(VirtualProtect(c, 0x1000, PAGE_READONLY, &old));
	CHECK_EQ(old, PAGE_READWRITE);
	VirtualQuery(c, &mbi, sizeof(mbi));
	CHECK_EQ(mbi.Protect, PAGE_READONLY);
	CHECK(IsBadWritePtr(c, 1));
	CHECK(!IsBadReadPtr(c, 1));

	CHECK(VirtualFree(c, 0x2000, MEM_DECOMMIT));
	VirtualQuery(c, &mbi, sizeof(mbi));
	CHECK_EQ(mbi.State, MEM_RESERVE);
	CHECK(!VirtualFree(r + 0x1000, 0, MEM_RELEASE));	/* not the base */
	CHECK(VirtualFree(r, 0, MEM_RELEASE));
	VirtualQuery(r, &mbi, sizeof(mbi));
	CHECK_EQ(mbi.State, MEM_FREE);

	/* Reserving at an unaligned address gives the 64 KiB boundary below. */
	BYTE *at = VirtualAlloc(r + 0x1234, 0x1000, MEM_RESERVE | MEM_COMMIT,
				PAGE_READWRITE);
	CHECK(at == r);
	if (at) {
		at[0x1234] = 3;
		CHECK(VirtualFree(at, 0, MEM_RELEASE));
	}

	/* Commit and reserve in one go, at no particular address. */
	int *ints = VirtualAlloc(NULL, 5000, MEM_COMMIT | MEM_RESERVE,
				 PAGE_EXECUTE_READWRITE);
	CHECK(ints != NULL);
	ints[1000] = 5;
	VirtualQuery(ints, &mbi, sizeof(mbi));
	CHECK_EQ(mbi.Protect, PAGE_EXECUTE_READWRITE);
	CHECK(VirtualFree(ints, 0, MEM_RELEASE));

	/* Module memory reads as an image. */
	VirtualQuery(k32->base + 8, &mbi, sizeof(mbi));
	CHECK_EQ(mbi.Type, MEM_IMAGE);
	CHECK(mbi.AllocationBase == k32->base);
	CHECK(IsBadReadPtr(NULL, 1));
	CHECK(!IsBadReadPtr(NULL, 0));
}

/* Synchronisation and threads. */

static CRITICAL_SECTION cs;
static LONG counter;
static LONG volatile atomic_counter;

static DWORD WINAPI worker(LPVOID arg)
{
	void (WINAPI *enter)(LPCRITICAL_SECTION) = get("EnterCriticalSection");
	void (WINAPI *leave)(LPCRITICAL_SECTION) = get("LeaveCriticalSection");
	LONG (WINAPI *inc)(LONG volatile *) = get("InterlockedIncrement");

	for (int i = 0; i < 20000; i++) {
		enter(&cs);
		enter(&cs);		/* recursion */
		counter++;
		leave(&cs);
		leave(&cs);
		inc(&atomic_counter);
	}
	return (DWORD)(uintptr_t)arg;
}

static HANDLE try_result_event;

static DWORD WINAPI try_enter_thread(LPVOID arg)
{
	BOOL (WINAPI *try_enter)(LPCRITICAL_SECTION) = get("TryEnterCriticalSection");
	return try_enter((LPCRITICAL_SECTION)arg);
}

static void test_critical_sections(void)
{
	FN(void, InitializeCriticalSection, LPCRITICAL_SECTION);
	FN(void, DeleteCriticalSection, LPCRITICAL_SECTION);
	FN(void, EnterCriticalSection, LPCRITICAL_SECTION);
	FN(void, LeaveCriticalSection, LPCRITICAL_SECTION);
	FN(HANDLE, CreateThread, LPSECURITY_ATTRIBUTES, SIZE_T,
	   LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
	FN(DWORD, WaitForMultipleObjects, DWORD, const HANDLE *, BOOL, DWORD);
	FN(DWORD, WaitForSingleObject, HANDLE, DWORD);
	FN(BOOL, GetExitCodeThread, HANDLE, LPDWORD);
	FN(BOOL, CloseHandle, HANDLE);

	InitializeCriticalSection(&cs);
	CHECK_EQ(cs.LockCount, -1);
	HANDLE threads[4];
	for (int i = 0; i < 4; i++)
		threads[i] = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)(10 + i),
					  0, NULL);
	CHECK_EQ(WaitForMultipleObjects(4, threads, TRUE, 10000), WAIT_OBJECT_0);
	CHECK_EQ(counter, 80000);
	CHECK_EQ(atomic_counter, 80000);
	for (int i = 0; i < 4; i++) {
		DWORD code = 0;
		CHECK(GetExitCodeThread(threads[i], &code));
		CHECK_EQ(code, 10 + i);
		CloseHandle(threads[i]);
	}

	/* Ownership is visible, and blocks other threads. */
	EnterCriticalSection(&cs);
	CHECK_EQ(cs.RecursionCount, 1);
	CHECK((DWORD)(uintptr_t)cs.OwningThread == teb_current()->UniqueThread);
	HANDLE t = CreateThread(NULL, 0, try_enter_thread, &cs, 0, NULL);
	WaitForSingleObject(t, INFINITE);
	DWORD got = 1;
	GetExitCodeThread(t, &got);
	CHECK_EQ(got, FALSE);
	CloseHandle(t);
	LeaveCriticalSection(&cs);
	CHECK(cs.OwningThread == NULL);
	DeleteCriticalSection(&cs);

	/* A zero-filled critical section works without initialisation. */
	CRITICAL_SECTION z;
	memset(&z, 0, sizeof(z));
	EnterCriticalSection(&z);
	LeaveCriticalSection(&z);
	DeleteCriticalSection(&z);
	(void)try_result_event;
}

static void test_waitables(void)
{
	FN(HANDLE, CreateEventA, LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
	FN(HANDLE, OpenEventA, DWORD, BOOL, LPCSTR);
	FN(BOOL, SetEvent, HANDLE);
	FN(BOOL, ResetEvent, HANDLE);
	FN(HANDLE, CreateMutexA, LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
	FN(BOOL, ReleaseMutex, HANDLE);
	FN(HANDLE, CreateSemaphoreA, LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
	FN(BOOL, ReleaseSemaphore, HANDLE, LONG, LPLONG);
	FN(DWORD, WaitForSingleObject, HANDLE, DWORD);
	FN(DWORD, WaitForMultipleObjects, DWORD, const HANDLE *, BOOL, DWORD);
	FN(DWORD, GetTickCount, void);
	FN(DWORD, GetLastError, void);
	FN(BOOL, CloseHandle, HANDLE);

	/* Auto-reset: one wait consumes the signal. */
	HANDLE e = CreateEventA(NULL, FALSE, TRUE, NULL);
	CHECK_EQ(WaitForSingleObject(e, 0), WAIT_OBJECT_0);
	CHECK_EQ(WaitForSingleObject(e, 0), WAIT_TIMEOUT);
	DWORD t0 = GetTickCount();
	CHECK_EQ(WaitForSingleObject(e, 50), WAIT_TIMEOUT);
	CHECK(GetTickCount() - t0 >= 40);
	CloseHandle(e);

	/* Manual reset stays signalled. */
	HANDLE m = CreateEventA(NULL, TRUE, FALSE, "vfw-test-event");
	CHECK_EQ(GetLastError(), ERROR_SUCCESS);
	HANDLE m2 = CreateEventA(NULL, TRUE, FALSE, "vfw-test-event");
	CHECK_EQ(GetLastError(), ERROR_ALREADY_EXISTS);
	HANDLE m3 = OpenEventA(0, FALSE, "vfw-test-event");
	CHECK(m2 && m3 && m2 != m);
	SetEvent(m2);
	CHECK_EQ(WaitForSingleObject(m, 0), WAIT_OBJECT_0);
	CHECK_EQ(WaitForSingleObject(m3, 0), WAIT_OBJECT_0);
	ResetEvent(m);
	CHECK_EQ(WaitForSingleObject(m3, 0), WAIT_TIMEOUT);
	CloseHandle(m);
	CloseHandle(m2);
	CloseHandle(m3);
	CHECK(OpenEventA(0, FALSE, "vfw-test-event") == NULL);

	/* Mutexes are owned and recursive. */
	HANDLE mx = CreateMutexA(NULL, TRUE, NULL);
	CHECK_EQ(WaitForSingleObject(mx, 0), WAIT_OBJECT_0);
	CHECK(ReleaseMutex(mx));
	CHECK(ReleaseMutex(mx));
	CHECK(!ReleaseMutex(mx));
	CHECK_EQ(GetLastError(), ERROR_NOT_OWNER);
	CloseHandle(mx);

	/* Semaphores count. */
	HANDLE s = CreateSemaphoreA(NULL, 2, 3, NULL);
	HANDLE both[2] = { s, s };
	CHECK_EQ(WaitForSingleObject(s, 0), WAIT_OBJECT_0);
	CHECK_EQ(WaitForSingleObject(s, 0), WAIT_OBJECT_0);
	CHECK_EQ(WaitForSingleObject(s, 0), WAIT_TIMEOUT);
	LONG prev = -1;
	CHECK(ReleaseSemaphore(s, 3, &prev));
	CHECK_EQ(prev, 0);
	CHECK(!ReleaseSemaphore(s, 1, NULL));
	CHECK_EQ(GetLastError(), ERROR_TOO_MANY_POSTS);
	CHECK_EQ(WaitForMultipleObjects(2, both, FALSE, 0), WAIT_OBJECT_0);
	CloseHandle(s);

	CHECK_EQ(WaitForSingleObject((HANDLE)0x7777, 0), WAIT_FAILED);
	CHECK_EQ(GetLastError(), ERROR_INVALID_HANDLE);
	CHECK(!CloseHandle((HANDLE)0x7777));
}

static DWORD tls_slot, fls_slot;
static int fls_freed;

static void WINAPI fls_callback(PVOID data)
{
	fls_freed += (int)(uintptr_t)data;
}

static DWORD WINAPI exiting_thread(LPVOID arg)
{
	void (WINAPI *exit_thread)(DWORD) = get("ExitThread");
	BOOL (WINAPI *tls_set)(DWORD, LPVOID) = get("TlsSetValue");
	LPVOID (WINAPI *tls_get)(DWORD) = get("TlsGetValue");
	BOOL (WINAPI *fls_set)(DWORD, PVOID) = get("FlsSetValue");

	if (tls_get(tls_slot))		/* each thread starts with NULL */
		return 1;
	tls_set(tls_slot, (LPVOID)0x42);
	fls_set(fls_slot, (PVOID)5);
	exit_thread(42);
	return 2;			/* not reached */
}

static void test_threads(void)
{
	FN(HANDLE, CreateThread, LPSECURITY_ATTRIBUTES, SIZE_T,
	   LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
	FN(DWORD, ResumeThread, HANDLE);
	FN(DWORD, WaitForSingleObject, HANDLE, DWORD);
	FN(BOOL, GetExitCodeThread, HANDLE, LPDWORD);
	FN(DWORD, GetThreadId, HANDLE);
	FN(BOOL, CloseHandle, HANDLE);
	FN(DWORD, TlsAlloc, void);
	FN(BOOL, TlsFree, DWORD);
	FN(LPVOID, TlsGetValue, DWORD);
	FN(BOOL, TlsSetValue, DWORD, LPVOID);
	FN(DWORD, FlsAlloc, PFLS_CALLBACK_FUNCTION);
	FN(BOOL, FlsFree, DWORD);
	FN(DWORD, GetLastError, void);
	FN(void, SetLastError, DWORD);
	FN(HANDLE, GetCurrentThread, void);
	FN(DWORD, GetCurrentThreadId, void);
	FN(BOOL, DuplicateHandle, HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD);
	FN(HANDLE, GetCurrentProcess, void);

	tls_slot = TlsAlloc();
	fls_slot = FlsAlloc(fls_callback);
	CHECK(tls_slot != TLS_OUT_OF_INDEXES && fls_slot != FLS_OUT_OF_INDEXES);
	CHECK(TlsSetValue(tls_slot, (LPVOID)0x99));
	SetLastError(1234);
	CHECK(TlsGetValue(tls_slot) == (LPVOID)0x99);
	CHECK_EQ(GetLastError(), 0);	/* success clears the error */
	CHECK(TlsGetValue(TEB_TLS_MAX + 5) == NULL);
	CHECK_EQ(GetLastError(), ERROR_INVALID_PARAMETER);

	DWORD tid = 0;
	HANDLE t = CreateThread(NULL, 0, exiting_thread, NULL, CREATE_SUSPENDED,
				&tid);
	CHECK(t != NULL && tid != 0);
	CHECK_EQ(GetThreadId(t), tid);
	DWORD code = 0;
	CHECK(GetExitCodeThread(t, &code));
	CHECK_EQ(code, STILL_ACTIVE);
	CHECK_EQ(WaitForSingleObject(t, 50), WAIT_TIMEOUT);	/* suspended */
	CHECK_EQ(ResumeThread(t), 1);
	CHECK_EQ(WaitForSingleObject(t, 10000), WAIT_OBJECT_0);
	CHECK(GetExitCodeThread(t, &code));
	CHECK_EQ(code, 42);
	CHECK_EQ(fls_freed, 5);		/* the FLS callback ran at exit */
	CHECK(TlsGetValue(tls_slot) == (LPVOID)0x99);	/* ours is untouched */
	CloseHandle(t);

	/* A real handle to the current thread. */
	HANDLE self = NULL;
	CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
			      GetCurrentProcess(), &self, 0, FALSE, 0));
	CHECK(self != NULL && self != GetCurrentThread());
	CHECK_EQ(GetThreadId(self), GetCurrentThreadId());
	CHECK_EQ(WaitForSingleObject(self, 0), WAIT_TIMEOUT);
	CloseHandle(self);

	CHECK(TlsFree(tls_slot));
	CHECK(FlsFree(fls_slot));
}

/* Strings. */

static void test_strings(void)
{
	FN(int, MultiByteToWideChar, UINT, DWORD, LPCSTR, int, LPWSTR, int);
	FN(int, WideCharToMultiByte, UINT, DWORD, LPCWSTR, int, LPSTR, int,
	   LPCSTR, LPBOOL);
	FN(int, CompareStringA, LCID, DWORD, LPCSTR, int, LPCSTR, int);
	FN(int, lstrcmpA, LPCSTR, LPCSTR);
	FN(int, lstrcmpiA, LPCSTR, LPCSTR);
	FN(LPSTR, lstrcpynA, LPSTR, LPCSTR, int);
	FN(int, LCMapStringA, LCID, DWORD, LPCSTR, int, LPSTR, int);
	FN(BOOL, GetStringTypeW, DWORD, LPCWSTR, int, LPWORD);
	FN(int, GetLocaleInfoA, LCID, DWORD, LPSTR, int);
	FN(BOOL, GetCPInfo, UINT, LPCPINFO);
	FN(UINT, GetACP, void);
	FN(DWORD, GetLastError, void);
	WCHAR w[32];
	char a[64];

	CHECK_EQ(GetACP(), 1252);
	/* Windows-1252: 0x80 is the euro sign. */
	CHECK_EQ(MultiByteToWideChar(CP_ACP, 0, "\x80" "A", -1, w, 32), 3);
	CHECK_EQ(w[0], 0x20ac);
	CHECK_EQ(w[1], 'A');
	CHECK_EQ(w[2], 0);
	CHECK_EQ(MultiByteToWideChar(CP_ACP, 0, "abc", 3, NULL, 0), 3);

	/* UTF-8 round trip, with a character outside the BMP. */
	const char *utf8 = "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 32);
	CHECK_EQ(n, 5);
	CHECK_EQ(w[0], 0xe9);
	CHECK_EQ(w[1], 0x20ac);
	CHECK_EQ(w[2], 0xd83d);
	CHECK_EQ(w[3], 0xde00);
	CHECK_EQ(WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof(a), NULL, NULL),
		 10);
	CHECK_STR(a, utf8);

	CHECK_EQ(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, "\xc3(", -1,
				     w, 32), 0);
	CHECK_EQ(GetLastError(), ERROR_NO_UNICODE_TRANSLATION);
	CHECK_EQ(MultiByteToWideChar(CP_UTF8, 0, "\xc3(", -1, w, 32), 3);
	CHECK_EQ(w[0], 0xfffd);
	CHECK_EQ(MultiByteToWideChar(CP_ACP, 0, "abcdef", -1, w, 3), 0);
	CHECK_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);

	/* Unmappable characters become the default character. */
	WCHAR greek[] = { 'x', 0x3a9, 0 };
	BOOL used = FALSE;
	CHECK_EQ(WideCharToMultiByte(CP_ACP, 0, greek, -1, a, sizeof(a), NULL,
				     &used), 3);
	CHECK_STR(a, "x?");
	CHECK(used);
	CHECK_EQ(WideCharToMultiByte(CP_ACP, 0, greek, -1, a, sizeof(a), "#",
				     NULL), 3);
	CHECK_STR(a, "x#");
	CHECK_EQ(WideCharToMultiByte(CP_UTF8, 0, greek, -1, a, sizeof(a), "#",
				     NULL), 0);
	CHECK_EQ(GetLastError(), ERROR_INVALID_PARAMETER);

	CPINFO cpi;
	CHECK(GetCPInfo(CP_ACP, &cpi));
	CHECK_EQ(cpi.MaxCharSize, 1);
	CHECK(GetCPInfo(CP_UTF8, &cpi));
	CHECK_EQ(cpi.MaxCharSize, 4);
	CHECK(!GetCPInfo(932, &cpi));

	/* Comparison: case-folded first, lower case breaks ties. */
	CHECK_EQ(CompareStringA(0x409, NORM_IGNORECASE, "abc", -1, "ABC", -1),
		 CSTR_EQUAL);
	CHECK_EQ(CompareStringA(0x409, 0, "a", -1, "B", -1), CSTR_LESS_THAN);
	CHECK(lstrcmpA("a", "A") < 0);
	CHECK(lstrcmpA("abc", "abd") < 0);
	CHECK_EQ(lstrcmpiA("Hello", "hELLO"), 0);
	CHECK(lstrcmpiA("abc", "ab") > 0);
	CHECK(lstrcmpA(NULL, "") == 0);

	CHECK_STR(lstrcpynA(a, "truncate me", 5), "trun");

	CHECK_EQ(LCMapStringA(0x409, LCMAP_UPPERCASE, "Mixed \xe9", -1, a,
			      sizeof(a)), 8);
	CHECK_STR(a, "MIXED \xc9");

	WCHAR chars[] = { 'a', 'Z', '5', ' ', '!', 0xe9, 0x3a9, 0 };
	WORD types[7];
	CHECK(GetStringTypeW(CT_CTYPE1, chars, 7, types));
	CHECK_EQ(types[0], C1_ALPHA | C1_LOWER | C1_XDIGIT);
	CHECK_EQ(types[1], C1_ALPHA | C1_UPPER);
	CHECK_EQ(types[2], C1_DIGIT | C1_XDIGIT);
	CHECK_EQ(types[3], C1_SPACE | C1_BLANK);
	CHECK_EQ(types[4], C1_PUNCT);
	CHECK_EQ(types[5], C1_ALPHA | C1_LOWER);
	CHECK(types[6] & C1_ALPHA);

	CHECK_EQ(GetLocaleInfoA(0x409, LOCALE_SDECIMAL, a, sizeof(a)), 2);
	CHECK_STR(a, ".");
	CHECK_EQ(GetLocaleInfoA(0x409, LOCALE_SDECIMAL, NULL, 0), 2);
	DWORD lang = 0;
	CHECK_EQ(GetLocaleInfoA(0x409, LOCALE_ILANGUAGE | LOCALE_RETURN_NUMBER,
				(LPSTR)&lang, sizeof(lang)), sizeof(DWORD));
	CHECK_EQ(lang, 0x409);
	CHECK_EQ(GetLocaleInfoA(0x409, 0x7777, a, sizeof(a)), 0);
	CHECK_EQ(GetLastError(), ERROR_INVALID_FLAGS);
}

/* Time. */

static void test_time(void)
{
	FN(BOOL, SystemTimeToFileTime, const SYSTEMTIME *, LPFILETIME);
	FN(BOOL, FileTimeToSystemTime, const FILETIME *, LPSYSTEMTIME);
	FN(void, GetSystemTimeAsFileTime, LPFILETIME);
	FN(BOOL, QueryPerformanceFrequency, LARGE_INTEGER *);
	FN(BOOL, QueryPerformanceCounter, LARGE_INTEGER *);
	FN(DWORD, GetTickCount, void);
	FN(void, Sleep, DWORD);
	FN(void, GetLocalTime, LPSYSTEMTIME);
	FN(BOOL, FileTimeToDosDateTime, const FILETIME *, LPWORD, LPWORD);

	SYSTEMTIME st = { 2000, 1, 0, 1, 0, 0, 0, 0 };
	FILETIME ft;
	CHECK(SystemTimeToFileTime(&st, &ft));
	ULONGLONG v = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	CHECK_EQ(v, 125911584000000000ull);

	SYSTEMTIME back;
	ft.dwLowDateTime += 10000 * 1234;	/* +1.234 s */
	CHECK(FileTimeToSystemTime(&ft, &back));
	CHECK_EQ(back.wYear, 2000);
	CHECK_EQ(back.wMonth, 1);
	CHECK_EQ(back.wDay, 1);
	CHECK_EQ(back.wDayOfWeek, 6);	/* a Saturday */
	CHECK_EQ(back.wSecond, 1);
	CHECK_EQ(back.wMilliseconds, 234);

	SYSTEMTIME leap = { 2024, 2, 0, 29, 23, 59, 59, 999 }, bad = leap;
	CHECK(SystemTimeToFileTime(&leap, &ft));
	CHECK(FileTimeToSystemTime(&ft, &back));
	CHECK(back.wYear == 2024 && back.wMonth == 2 && back.wDay == 29 &&
	      back.wMilliseconds == 999);
	bad.wYear = 2023;
	CHECK(!SystemTimeToFileTime(&bad, &ft));

	WORD dos_date, dos_time;
	SystemTimeToFileTime(&st, &ft);
	CHECK(FileTimeToDosDateTime(&ft, &dos_date, &dos_time));
	CHECK_EQ(dos_date, (20 << 9) | (1 << 5) | 1);

	GetSystemTimeAsFileTime(&ft);
	FileTimeToSystemTime(&ft, &back);
	CHECK(back.wYear >= 2024);
	GetLocalTime(&back);
	CHECK(back.wYear >= 2024);

	LARGE_INTEGER f, a, b;
	CHECK(QueryPerformanceFrequency(&f));
	CHECK_EQ(f.QuadPart, 10000000);
	QueryPerformanceCounter(&a);
	DWORD t0 = GetTickCount();
	Sleep(20);
	QueryPerformanceCounter(&b);
	CHECK(GetTickCount() - t0 >= 15);
	CHECK(b.QuadPart - a.QuadPart >= 150000);
}

/* System information and environment. */

static void test_system(void)
{
	FN(DWORD, GetVersion, void);
	FN(BOOL, GetVersionExA, OSVERSIONINFOA *);
	FN(void, GetSystemInfo, LPSYSTEM_INFO);
	FN(BOOL, IsProcessorFeaturePresent, DWORD);
	FN(DWORD, GetEnvironmentVariableA, LPCSTR, LPSTR, DWORD);
	FN(BOOL, SetEnvironmentVariableA, LPCSTR, LPCSTR);
	FN(DWORD, ExpandEnvironmentStringsA, LPCSTR, LPSTR, DWORD);
	FN(LPSTR, GetEnvironmentStringsA, void);
	FN(BOOL, FreeEnvironmentStringsA, LPSTR);
	FN(DWORD, FormatMessageA, DWORD, LPCVOID, DWORD, DWORD, LPSTR, DWORD, void *);
	FN(UINT, GetSystemDirectoryA, LPSTR, UINT);
	FN(BOOL, GetComputerNameA, LPSTR, LPDWORD);
	FN(DWORD, GetLastError, void);
	FN(HLOCAL, LocalFree, HLOCAL);
	char buf[256];

	CHECK_EQ(GetVersion(), 0x0a280105);
	OSVERSIONINFOEXA vi = { .dwOSVersionInfoSize = sizeof(vi) };
	CHECK(GetVersionExA((OSVERSIONINFOA *)&vi));
	CHECK_EQ(vi.dwMajorVersion, 5);
	CHECK_EQ(vi.dwPlatformId, VER_PLATFORM_WIN32_NT);
	CHECK_STR(vi.szCSDVersion, "Service Pack 3");
	CHECK_EQ(vi.wServicePackMajor, 3);
	OSVERSIONINFOA small = { .dwOSVersionInfoSize = 12 };
	CHECK(!GetVersionExA(&small));

	SYSTEM_INFO si;
	GetSystemInfo(&si);
	CHECK_EQ(si.dwPageSize, 4096);
	CHECK_EQ(si.dwAllocationGranularity, 0x10000);
	CHECK_EQ(si.dwNumberOfProcessors, peb_get()->NumberOfProcessors);
	CHECK_EQ(IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE),
		 __builtin_cpu_supports("sse2") != 0);
	CHECK(!IsProcessorFeaturePresent(9999));

	CHECK_EQ(GetSystemDirectoryA(buf, sizeof(buf)), 19);
	CHECK_STR(buf, "C:\\WINDOWS\\system32");
	CHECK_EQ(GetSystemDirectoryA(buf, 5), 20);	/* too small: size needed */
	DWORD len = 3;
	CHECK(!GetComputerNameA(buf, &len));
	CHECK_EQ(GetLastError(), ERROR_BUFFER_OVERFLOW);
	len = sizeof(buf);
	CHECK(GetComputerNameA(buf, &len));
	CHECK_STR(buf, "SEANCE");

	CHECK_EQ(GetEnvironmentVariableA("WINDIR", buf, sizeof(buf)), 10);
	CHECK_STR(buf, "C:\\WINDOWS");
	CHECK_EQ(GetEnvironmentVariableA("VFWTEST", buf, sizeof(buf)), 5);
	CHECK_STR(buf, "hello");		/* passed through from VFW_ENV_ */
	CHECK_EQ(GetEnvironmentVariableA("HOME", buf, sizeof(buf)), 0);
	CHECK_EQ(GetLastError(), ERROR_ENVVAR_NOT_FOUND);
	CHECK(SetEnvironmentVariableA("MyVar", "1"));
	CHECK_EQ(GetEnvironmentVariableA("MYVAR", buf, 1), 2);
	CHECK(SetEnvironmentVariableA("MyVar", NULL));
	CHECK_EQ(GetEnvironmentVariableA("MyVar", buf, sizeof(buf)), 0);

	CHECK_EQ(ExpandEnvironmentStringsA("%windir%\\x %nope%", buf, sizeof(buf)),
		 20);
	CHECK_STR(buf, "C:\\WINDOWS\\x %nope%");

	LPSTR block = GetEnvironmentStringsA();
	int found = 0;
	for (char *p = block; *p; p += strlen(p) + 1)
		found |= !strncmp(p, "OS=Windows_NT", 13);
	CHECK(found);
	FreeEnvironmentStringsA(block);

	CHECK(FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, ERROR_FILE_NOT_FOUND,
			     0, buf, sizeof(buf), NULL) > 0);
	CHECK_STR(buf, "The system cannot find the file specified.\r\n");
	DWORD_PTR args[2] = { (DWORD_PTR)"answer", 42 };
	CHECK_EQ(FormatMessageA(FORMAT_MESSAGE_FROM_STRING |
				FORMAT_MESSAGE_ARGUMENT_ARRAY,
				"%1 is %2!d!%% (%2!04x!)", 0, 0, buf, sizeof(buf),
				args), 20);
	CHECK_STR(buf, "answer is 42% (002a)");
	LPSTR allocated = NULL;
	CHECK(FormatMessageA(FORMAT_MESSAGE_FROM_STRING |
			     FORMAT_MESSAGE_ALLOCATE_BUFFER |
			     FORMAT_MESSAGE_IGNORE_INSERTS, "keep %1", 0, 0,
			     (LPSTR)&allocated, 0, NULL) == 7);
	CHECK_STR(allocated, "keep %1");
	LocalFree(allocated);
}

/* Files, paths and mappings, inside a private VFW_ROOT. */

static void test_files(void)
{
	FN(HANDLE, CreateFileA, LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
	   DWORD, DWORD, HANDLE);
	FN(BOOL, WriteFile, HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
	FN(BOOL, ReadFile, HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
	FN(DWORD, SetFilePointer, HANDLE, LONG, PLONG, DWORD);
	FN(DWORD, GetFileSize, HANDLE, LPDWORD);
	FN(DWORD, GetFileType, HANDLE);
	FN(BOOL, CloseHandle, HANDLE);
	FN(DWORD, GetFileAttributesA, LPCSTR);
	FN(BOOL, DeleteFileA, LPCSTR);
	FN(BOOL, CreateDirectoryA, LPCSTR, LPSECURITY_ATTRIBUTES);
	FN(BOOL, RemoveDirectoryA, LPCSTR);
	FN(HANDLE, FindFirstFileA, LPCSTR, LPWIN32_FIND_DATAA);
	FN(BOOL, FindNextFileA, HANDLE, LPWIN32_FIND_DATAA);
	FN(BOOL, FindClose, HANDLE);
	FN(UINT, GetTempFileNameA, LPCSTR, LPCSTR, UINT, LPSTR);
	FN(DWORD, GetTempPathA, DWORD, LPSTR);
	FN(DWORD, GetFullPathNameA, LPCSTR, DWORD, LPSTR, LPSTR *);
	FN(HANDLE, GetStdHandle, DWORD);
	FN(BOOL, MoveFileA, LPCSTR, LPCSTR);
	FN(HANDLE, CreateFileMappingA, HANDLE, LPSECURITY_ATTRIBUTES, DWORD,
	   DWORD, DWORD, LPCSTR);
	FN(HANDLE, OpenFileMappingA, DWORD, BOOL, LPCSTR);
	FN(LPVOID, MapViewOfFile, HANDLE, DWORD, DWORD, DWORD, SIZE_T);
	FN(BOOL, UnmapViewOfFile, LPCVOID);
	FN(DWORD, GetLastError, void);
	char buf[256];
	DWORD n;

	const char *name = "C:\\WINDOWS\\TEMP\\Test File.txt";
	HANDLE h = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			       FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(h != INVALID_HANDLE_VALUE);
	CHECK_EQ(GetLastError(), ERROR_SUCCESS);
	CHECK(WriteFile(h, "hello, world", 12, &n, NULL));
	CHECK_EQ(n, 12);
	CHECK_EQ(GetFileType(h), FILE_TYPE_DISK);
	CloseHandle(h);

	/* The file really is under the sandbox's drive_c. */
	char unix_name[512];
	struct stat st;
	snprintf(unix_name, sizeof(unix_name), "%s/drive_c/windows/temp/Test File.txt",
		 root);
	CHECK(stat(unix_name, &st) == 0 && st.st_size == 12);

	CHECK(CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL) ==
	      INVALID_HANDLE_VALUE);
	CHECK_EQ(GetLastError(), ERROR_FILE_EXISTS);

	/* Paths are case-insensitive. */
	h = CreateFileA("c:\\windows\\temp\\TEST FILE.TXT", GENERIC_READ,
			FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	CHECK(h != INVALID_HANDLE_VALUE);
	CHECK_EQ(GetFileSize(h, NULL), 12);
	CHECK_EQ(SetFilePointer(h, 7, NULL, FILE_BEGIN), 7);
	CHECK(ReadFile(h, buf, sizeof(buf), &n, NULL));
	CHECK_EQ(n, 5);
	buf[n] = '\0';
	CHECK_STR(buf, "world");
	CHECK(ReadFile(h, buf, sizeof(buf), &n, NULL));
	CHECK_EQ(n, 0);			/* end of file */
	OVERLAPPED ov = { .Offset = 2 };
	CHECK(ReadFile(h, buf, 3, &n, &ov));
	CHECK(n == 3 && !memcmp(buf, "llo", 3));
	CloseHandle(h);

	CHECK_EQ(GetFileAttributesA(name), FILE_ATTRIBUTE_ARCHIVE);
	CHECK_EQ(GetFileAttributesA("C:\\WINDOWS"), FILE_ATTRIBUTE_DIRECTORY);
	CHECK_EQ(GetFileAttributesA("C:\\WINDOWS\\nothing.txt"),
		 INVALID_FILE_ATTRIBUTES);
	CHECK_EQ(GetLastError(), ERROR_FILE_NOT_FOUND);
	CHECK_EQ(GetFileAttributesA("C:\\nowhere\\nothing.txt"),
		 INVALID_FILE_ATTRIBUTES);
	CHECK_EQ(GetLastError(), ERROR_PATH_NOT_FOUND);
	CHECK(CreateFileA("Q:\\x", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) ==
	      INVALID_HANDLE_VALUE);

	/* Search. */
	CHECK(CreateDirectoryA("C:\\WINDOWS\\TEMP\\sub", NULL));
	CHECK(!CreateDirectoryA("C:\\WINDOWS\\TEMP\\sub", NULL));
	CHECK_EQ(GetLastError(), ERROR_ALREADY_EXISTS);
	WIN32_FIND_DATAA fd;
	HANDLE f = FindFirstFileA("C:\\WINDOWS\\TEMP\\*.txt", &fd);
	CHECK(f != INVALID_HANDLE_VALUE);
	CHECK_STR(fd.cFileName, "Test File.txt");
	CHECK_EQ(fd.nFileSizeLow, 12);
	CHECK(!FindNextFileA(f, &fd));
	CHECK_EQ(GetLastError(), ERROR_NO_MORE_FILES);
	FindClose(f);
	int entries = 0, saw_sub = 0;
	f = FindFirstFileA("C:\\WINDOWS\\TEMP\\*.*", &fd);
	do {
		entries++;
		if (!strcmp(fd.cFileName, "sub"))
			saw_sub = fd.dwFileAttributes == FILE_ATTRIBUTE_DIRECTORY;
	} while (FindNextFileA(f, &fd));
	FindClose(f);
	CHECK(entries >= 4 && saw_sub);		/* ".", "..", sub, the file */
	CHECK(FindFirstFileA("C:\\WINDOWS\\TEMP\\*.none", &fd) == INVALID_HANDLE_VALUE);
	CHECK(RemoveDirectoryA("C:\\WINDOWS\\TEMP\\sub"));

	/* Temporary files. */
	CHECK_EQ(GetTempPathA(sizeof(buf), buf), 16);
	CHECK_STR(buf, "C:\\WINDOWS\\TEMP\\");
	char tmp[MAX_PATH];
	UINT u = GetTempFileNameA(buf, "vfw", 0, tmp);
	CHECK(u != 0);
	CHECK(!strncmp(tmp, "C:\\WINDOWS\\TEMP\\vfw", 19));
	CHECK(GetFileAttributesA(tmp) != INVALID_FILE_ATTRIBUTES);
	CHECK(MoveFileA(tmp, "C:\\WINDOWS\\TEMP\\moved.tmp"));
	CHECK(!MoveFileA(name, "C:\\WINDOWS\\TEMP\\moved.tmp"));
	CHECK(DeleteFileA("C:\\WINDOWS\\TEMP\\moved.tmp"));
	CHECK(DeleteFileA(name));
	CHECK(!DeleteFileA(name));
	CHECK_EQ(GetLastError(), ERROR_FILE_NOT_FOUND);

	/* Full paths. */
	LPSTR part = NULL;
	CHECK_EQ(GetFullPathNameA("c:\\a\\b\\..\\.\\c.txt", sizeof(buf), buf, &part),
		 10);
	CHECK_STR(buf, "C:\\a\\c.txt");
	CHECK_STR(part, "c.txt");
	CHECK_EQ(GetFullPathNameA("C:\\x\\..\\..\\y", sizeof(buf), buf, NULL), 4);
	CHECK_STR(buf, "C:\\y");

	/* Standard handles. */
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	CHECK(out != INVALID_HANDLE_VALUE && out != NULL);
	CHECK(GetStdHandle(STD_OUTPUT_HANDLE) == out);
	CHECK(GetStdHandle(1234) == INVALID_HANDLE_VALUE);

	/* Named shared memory. */
	HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
					0, 8192, "vfw-test-map");
	CHECK(map != NULL);
	char *v1 = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	CHECK(v1 != NULL);
	strcpy(v1 + 4096, "shared");
	HANDLE map2 = OpenFileMappingA(FILE_MAP_READ, FALSE, "vfw-test-map");
	char *v2 = map2 ? MapViewOfFile(map2, FILE_MAP_READ, 0, 4096, 4096) : NULL;
	CHECK(v2 != NULL && v2 != v1 + 4096);
	CHECK_STR(v2, "shared");
	CHECK(UnmapViewOfFile(v1));
	CHECK(!UnmapViewOfFile(v1));
	CHECK(UnmapViewOfFile(v2));
	CloseHandle(map);
	CloseHandle(map2);
}

static void test_profile(void)
{
	FN(BOOL, WritePrivateProfileStringA, LPCSTR, LPCSTR, LPCSTR, LPCSTR);
	FN(DWORD, GetPrivateProfileStringA, LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD,
	   LPCSTR);
	FN(UINT, GetPrivateProfileIntA, LPCSTR, LPCSTR, INT, LPCSTR);
	FN(BOOL, WritePrivateProfileStructA, LPCSTR, LPCSTR, LPVOID, UINT, LPCSTR);
	FN(BOOL, GetPrivateProfileStructA, LPCSTR, LPCSTR, LPVOID, UINT, LPCSTR);
	char buf[256];

	CHECK(WritePrivateProfileStringA("Codec", "Quality", "85", "vfwtest.ini"));
	CHECK(WritePrivateProfileStringA("Codec", "Mode", "fast", "vfwtest.ini"));
	CHECK(WritePrivateProfileStringA("Other", "Key", "x", "vfwtest.ini"));
	CHECK(WritePrivateProfileStringA("codec", "quality", "90", "vfwtest.ini"));

	/* It lives in C:\WINDOWS, with Windows line endings. */
	char path[512];
	snprintf(path, sizeof(path), "%s/drive_c/windows/vfwtest.ini", root);
	FILE *f = fopen(path, "rb");
	char text[256] = "";
	if (f) {
		text[fread(text, 1, sizeof(text) - 1, f)] = '\0';
		fclose(f);
	}
	CHECK_STR(text, "[Codec]\r\nQuality=90\r\nMode=fast\r\n[Other]\r\nKey=x\r\n");

	CHECK_EQ(GetPrivateProfileStringA("CODEC", "MODE", "def", buf, sizeof(buf),
					  "vfwtest.ini"), 4);
	CHECK_STR(buf, "fast");
	CHECK_EQ(GetPrivateProfileStringA("Codec", "Missing", "def  ", buf,
					  sizeof(buf), "vfwtest.ini"), 3);
	CHECK_STR(buf, "def");
	CHECK_EQ(GetPrivateProfileIntA("Codec", "Quality", 0, "vfwtest.ini"), 90);
	CHECK_EQ(GetPrivateProfileIntA("Codec", "Mode", 7, "vfwtest.ini"), 7);

	/* Truncation leaves size - 1 characters. */
	CHECK_EQ(GetPrivateProfileStringA("Codec", "Mode", "", buf, 3,
					  "vfwtest.ini"), 2);
	CHECK_STR(buf, "fa");

	/* Lists: sections, then keys, each NUL-terminated, then a NUL. */
	CHECK_EQ(GetPrivateProfileStringA(NULL, NULL, "", buf, sizeof(buf),
					  "vfwtest.ini"), 12);
	CHECK(!memcmp(buf, "Codec\0Other\0\0", 13));
	CHECK_EQ(GetPrivateProfileStringA("Codec", NULL, "", buf, sizeof(buf),
					  "vfwtest.ini"), 13);
	CHECK(!memcmp(buf, "Quality\0Mode\0\0", 14));

	/* Deleting a key, then a section. */
	CHECK(WritePrivateProfileStringA("Codec", "Mode", NULL, "vfwtest.ini"));
	CHECK_EQ(GetPrivateProfileStringA("Codec", "Mode", "gone", buf, sizeof(buf),
					  "vfwtest.ini"), 4);
	CHECK(WritePrivateProfileStringA("Other", NULL, NULL, "vfwtest.ini"));
	CHECK_EQ(GetPrivateProfileStringA(NULL, NULL, "", buf, sizeof(buf),
					  "vfwtest.ini"), 6);

	struct { int a; short b; } in = { 0x12345678, 42 }, out = { 0, 0 };
	CHECK(WritePrivateProfileStructA("Codec", "State", &in, sizeof(in),
					 "vfwtest.ini"));
	CHECK(GetPrivateProfileStructA("Codec", "State", &out, sizeof(out),
				       "vfwtest.ini"));
	CHECK(out.a == in.a && out.b == in.b);
	CHECK(!GetPrivateProfileStructA("Codec", "State", &out, 4, "vfwtest.ini"));
}

static void test_modules(void)
{
	FN(HMODULE, GetModuleHandleA, LPCSTR);
	FN(FARPROC, GetProcAddress, HMODULE, LPCSTR);
	FN(DWORD, GetModuleFileNameA, HMODULE, LPSTR, DWORD);
	FN(HMODULE, LoadLibraryA, LPCSTR);
	FN(BOOL, FreeLibrary, HMODULE);
	FN(DWORD, GetLastError, void);
	char buf[256];

	HMODULE h = GetModuleHandleA("KERNEL32");
	CHECK(h == (HMODULE)k32->base);
	CHECK(GetModuleHandleA("C:\\WINDOWS\\system32\\kernel32.dll") == h);
	CHECK(GetProcAddress(h, "GetTickCount") == (FARPROC)get("GetTickCount"));
	CHECK(GetProcAddress(h, "NoSuchFunction") == NULL);
	CHECK_EQ(GetLastError(), ERROR_PROC_NOT_FOUND);
	CHECK(GetModuleHandleA("notloaded.dll") == NULL);
	CHECK_EQ(GetLastError(), ERROR_MOD_NOT_FOUND);

	CHECK(GetModuleHandleA(NULL) == (HMODULE)mod_main()->base);
	CHECK_EQ(GetModuleFileNameA(NULL, buf, sizeof(buf)), 30);
	CHECK_STR(buf, "C:\\WINDOWS\\system32\\seance.exe");
	CHECK_EQ(GetModuleFileNameA(h, buf, 8), 8);
	CHECK_STR(buf, "C:\\WIND");
	CHECK_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);

	/* Loading a system DLL by path gives the built-in one. */
	HMODULE again = LoadLibraryA("C:\\WINDOWS\\system32\\KERNEL32.DLL");
	CHECK(again == h);
	CHECK(FreeLibrary(again));
	CHECK(LoadLibraryA("definitely-missing.dll") == NULL);
	CHECK_EQ(GetLastError(), ERROR_MOD_NOT_FOUND);
}

/* The same APIs from Win32 code (with a 4-byte-aligned stack). */
/* Implicit TLS and thread notifications, with pe_tls.dll. */
static int (*tls_read)(void), (*tls_block_ok)(void);
static void (*tls_write)(int);
static LONG (*tls_callback_count)(DWORD), (*tls_entry_count)(DWORD);

struct tls_seen {
	HANDLE ready, go;	/* for the thread started before the load */
	int value, block_ok;
	LONG callbacks, entries;	/* THREAD_ATTACH seen from inside */
};

static DWORD WINAPI tls_thread(LPVOID p)
{
	struct tls_seen *s = p;
	FN(DWORD, WaitForSingleObject, HANDLE, DWORD);

	FN(BOOL, SetEvent, HANDLE);

	if (s->go) {
		SetEvent(s->ready);
		WaitForSingleObject(s->go, INFINITE);
	}
	s->value = tls_read();
	s->block_ok = tls_block_ok();
	s->callbacks = tls_callback_count(DLL_THREAD_ATTACH);
	s->entries = tls_entry_count(DLL_THREAD_ATTACH);
	tls_write(7);		/* only this thread's copy */
	return 0;
}

static void test_static_tls(void)
{
	FN(HANDLE, CreateThread, LPSECURITY_ATTRIBUTES, SIZE_T,
	   LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
	FN(DWORD, WaitForSingleObject, HANDLE, DWORD);
	FN(BOOL, CloseHandle, HANDLE);
	FN(HANDLE, CreateEventA, LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
	FN(BOOL, SetEvent, HANDLE);
	FN(BOOL, DisableThreadLibraryCalls, HMODULE);
	const char *dir = getenv("VFW_FIXTURES");
	char path[512];

	snprintf(path, sizeof(path), "%s/pe_tls.dll", dir ? dir : ".");
	if (!dir || access(path, R_OK)) {
		printf("test_kernel32: pe_tls.dll not built, skipping TLS\n");
		return;
	}

	/* A thread that is running before the DLL is loaded. */
	struct tls_seen early = { 0 };
	early.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
	early.go = CreateEventA(NULL, TRUE, FALSE, NULL);
	HANDLE te = CreateThread(NULL, 0, tls_thread, &early, 0, NULL);
	CHECK(early.ready && early.go && te);
	CHECK_EQ(WaitForSingleObject(early.ready, 10000), WAIT_OBJECT_0);

	struct module *m = mod_load(path, NULL);
	CHECK(m != NULL);
	if (!m) {
		SetEvent(early.go);
		return;
	}
	tls_read = mod_get_proc(m, "tls_read");
	tls_write = mod_get_proc(m, "tls_write");
	tls_block_ok = mod_get_proc(m, "tls_block_ok");
	tls_callback_count = mod_get_proc(m, "tls_callback_count");
	tls_entry_count = mod_get_proc(m, "tls_entry_count");
	CHECK(tls_read && tls_write && tls_block_ok && tls_callback_count &&
	      tls_entry_count);
	if (!tls_read || !tls_write || !tls_block_ok || !tls_callback_count ||
	    !tls_entry_count) {
		SetEvent(early.go);
		mod_free(m);
		return;
	}

	/* The loading thread has its copy of the template, zero fill after it. */
	CHECK_EQ(tls_read(), 1234);
	CHECK(tls_block_ok());
	tls_write(99);
	CHECK_EQ(tls_read(), 99);
	CHECK_EQ(tls_callback_count(DLL_PROCESS_ATTACH), 1);
	CHECK_EQ(tls_entry_count(DLL_PROCESS_ATTACH), 1);

	/*
	 * The earlier thread got a block too, but no THREAD_ATTACH; like
	 * Windows, every loaded DLL still hears of its end.
	 */
	SetEvent(early.go);
	CHECK_EQ(WaitForSingleObject(te, 10000), WAIT_OBJECT_0);
	CHECK_EQ(early.value, 1234);
	CHECK(early.block_ok);
	CHECK_EQ(early.callbacks, 0);
	CHECK_EQ(early.entries, 0);
	CHECK_EQ(tls_callback_count(DLL_THREAD_DETACH), 1);
	CHECK_EQ(tls_entry_count(DLL_THREAD_DETACH), 1);

	/* A new thread: attach before its code runs, detach when it ends. */
	struct tls_seen s = { 0 };
	HANDLE t = CreateThread(NULL, 0, tls_thread, &s, 0, NULL);
	CHECK(t != NULL);
	CHECK_EQ(WaitForSingleObject(t, 10000), WAIT_OBJECT_0);
	CHECK_EQ(s.value, 1234);
	CHECK(s.block_ok);
	CHECK_EQ(s.callbacks, 1);
	CHECK_EQ(s.entries, 1);
	CHECK_EQ(tls_callback_count(DLL_THREAD_DETACH), 2);
	CHECK_EQ(tls_entry_count(DLL_THREAD_DETACH), 2);
	CHECK_EQ(tls_read(), 99);	/* the other thread wrote its own copy */

	/* DisableThreadLibraryCalls stops DllMain's calls, not the callbacks. */
	CHECK(DisableThreadLibraryCalls((HMODULE)m->base));
	struct tls_seen s2 = { 0 };
	HANDLE t2 = CreateThread(NULL, 0, tls_thread, &s2, 0, NULL);
	CHECK(t2 && WaitForSingleObject(t2, 10000) == WAIT_OBJECT_0);
	CHECK_EQ(s2.callbacks, 2);
	CHECK_EQ(s2.entries, 1);
	CHECK_EQ(tls_callback_count(DLL_THREAD_DETACH), 3);
	CHECK_EQ(tls_entry_count(DLL_THREAD_DETACH), 2);

	CloseHandle(te);
	CloseHandle(t);
	CloseHandle(t2);
	CloseHandle(early.go);
	CloseHandle(early.ready);
	mod_free(m);

	/* Loaded again, it starts from the template. */
	m = mod_load(path, NULL);
	CHECK(m != NULL);
	if (m) {
		int (*again)(void) = mod_get_proc(m, "tls_read");
		CHECK(again && again() == 1234);
		mod_free(m);
	}
}

/* SSE code with 16-byte stack assumptions, and threads' FPU defaults. */
static void test_sse(void)
{
	const char *dir = getenv("VFW_FIXTURES");
	char path[512];

	snprintf(path, sizeof(path), "%s/pe_sse.dll", dir ? dir : ".");
	if (!dir || access(path, R_OK)) {
		printf("test_kernel32: pe_sse.dll not built, skipping SSE\n");
		return;
	}
	struct module *m = mod_load(path, NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	float (*dot)(const float *, const float *, int) = mod_get_proc(m, "sse_dot");
	int (*aligned)(void) = mod_get_proc(m, "sse_stack_aligned");
	void (*state)(DWORD *, DWORD *) = mod_get_proc(m, "sse_fpu_state");
	int (*in_thread)(DWORD *, DWORD *, int *, float *) =
		mod_get_proc(m, "sse_in_thread");
	CHECK(dot && aligned && state && in_thread);
	if (dot && aligned && state && in_thread) {
		static const float a[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
		CHECK(dot(a, a, 9) == 285.0f);
		CHECK(aligned());

		DWORD cw = 0, mxcsr = 0;
		state(&cw, &mxcsr);
		CHECK_EQ(cw, 0x27f);
		CHECK_EQ(mxcsr & 0xffbf, 0x1f80);	/* DAZ aside */

		int al = 0;
		float d = 0;
		cw = mxcsr = 0;
		CHECK_EQ(in_thread(&cw, &mxcsr, &al, &d), 0);
		CHECK_EQ(cw, 0x27f);
		CHECK_EQ(mxcsr & 0xffbf, 0x1f80);
		CHECK(al);
		CHECK(d == 204.0f);
	}
	mod_free(m);
}

static void test_fixture(void)
{
	const char *dir = getenv("VFW_FIXTURES");
	char path[512];

	snprintf(path, sizeof(path), "%s/pe_k32.dll", dir ? dir : ".");
	if (!dir || access(path, R_OK)) {
		printf("test_kernel32: pe_k32.dll not built, skipping the fixture\n");
		return;
	}
	struct module *m = mod_load(path, NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	int (*selftest)(void) = mod_get_proc(m, "k32_selftest");
	CHECK(selftest != NULL);
	if (selftest)
		CHECK_EQ(selftest(), 0);
	mod_free(m);
}

int main(void)
{
	/* Before anything reads the environment: see test_system. */
	setenv("VFW_ENV_VFWTEST", "hello", 1);
	const char *tmpdir = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/vfw-k32-XXXXXX",
		 tmpdir && *tmpdir ? tmpdir : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", root, 1);

	CHECK_EQ(runtime_enter(), 0);
	k32 = mod_load("kernel32.dll", NULL);
	CHECK(k32 != NULL);
	if (!k32)
		return test_summary("test_kernel32");

	test_heaps();
	test_global();
	test_virtual();
	test_critical_sections();
	test_waitables();
	test_threads();
	test_strings();
	test_time();
	test_system();
	test_files();
	test_profile();
	test_modules();

	test_fixture();
	test_static_tls();
	test_sse();

	mod_free(k32);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", root);
	return test_summary("test_kernel32");
}
