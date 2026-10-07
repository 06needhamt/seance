/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The kernel32 functions added after Windows XP that current runtimes use
 * without checking: SRW locks, condition variables, one-time
 * initialisation, interlocked lists, and the smaller ones that go with
 * them. Called through the export table, as a DLL's imports are.
 */
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
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

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = (ret (WINAPI *)(__VA_ARGS__))get(#name)

/* Run `body` on a new thread and wait for it. */
static HANDLE start_thread(LPTHREAD_START_ROUTINE fn, LPVOID arg)
{
	FN(HANDLE, CreateThread, LPSECURITY_ATTRIBUTES, SIZE_T,
	   LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);

	return CreateThread(NULL, 0, fn, arg, 0, NULL);
}

static void join(HANDLE t)
{
	FN(DWORD, WaitForSingleObject, HANDLE, DWORD);
	FN(BOOL, CloseHandle, HANDLE);

	CHECK_EQ(WaitForSingleObject(t, 10000), WAIT_OBJECT_0);
	CloseHandle(t);
}

/* SRW locks. */

static SRWLOCK lock;
static volatile LONG entered;

static DWORD WINAPI exclusive_thread(LPVOID arg)
{
	FN(void, AcquireSRWLockExclusive, PSRWLOCK);
	FN(void, ReleaseSRWLockExclusive, PSRWLOCK);

	AcquireSRWLockExclusive(&lock);
	__atomic_store_n(&entered, 1, __ATOMIC_SEQ_CST);
	ReleaseSRWLockExclusive(&lock);
	return 0;
}

static void test_srw(void)
{
	FN(void, InitializeSRWLock, PSRWLOCK);
	FN(void, AcquireSRWLockExclusive, PSRWLOCK);
	FN(void, ReleaseSRWLockExclusive, PSRWLOCK);
	FN(void, AcquireSRWLockShared, PSRWLOCK);
	FN(void, ReleaseSRWLockShared, PSRWLOCK);
	FN(BOOLEAN, TryAcquireSRWLockExclusive, PSRWLOCK);
	FN(BOOLEAN, TryAcquireSRWLockShared, PSRWLOCK);
	FN(void, Sleep, DWORD);

	InitializeSRWLock(&lock);
	CHECK(lock.Ptr == NULL);

	/* A writer excludes everyone. */
	AcquireSRWLockExclusive(&lock);
	CHECK(!TryAcquireSRWLockExclusive(&lock));
	CHECK(!TryAcquireSRWLockShared(&lock));
	ReleaseSRWLockExclusive(&lock);

	/* Readers share, and keep a writer out until the last leaves. */
	AcquireSRWLockShared(&lock);
	CHECK(TryAcquireSRWLockShared(&lock));
	CHECK(!TryAcquireSRWLockExclusive(&lock));
	ReleaseSRWLockShared(&lock);
	CHECK(!TryAcquireSRWLockExclusive(&lock));
	ReleaseSRWLockShared(&lock);
	CHECK(TryAcquireSRWLockExclusive(&lock));

	/* A thread waiting for the lock gets in once it is released. */
	entered = 0;
	HANDLE t = start_thread(exclusive_thread, NULL);
	Sleep(30);
	CHECK_EQ(__atomic_load_n(&entered, __ATOMIC_SEQ_CST), 0);
	ReleaseSRWLockExclusive(&lock);
	join(t);
	CHECK_EQ(entered, 1);
	CHECK(lock.Ptr == NULL);
}

/* Condition variables. */

static CONDITION_VARIABLE cv;
static CRITICAL_SECTION cs;
static volatile LONG ready, woken;

static DWORD WINAPI srw_waiter(LPVOID arg)
{
	FN(void, AcquireSRWLockExclusive, PSRWLOCK);
	FN(void, ReleaseSRWLockExclusive, PSRWLOCK);
	FN(BOOL, SleepConditionVariableSRW, PCONDITION_VARIABLE, PSRWLOCK, DWORD,
	   ULONG);

	AcquireSRWLockExclusive(&lock);
	ready++;
	while (!__atomic_load_n(&woken, __ATOMIC_SEQ_CST))
		SleepConditionVariableSRW(&cv, &lock, INFINITE, 0);
	woken++;
	ReleaseSRWLockExclusive(&lock);
	return 0;
}

static DWORD WINAPI cs_waiter(LPVOID arg)
{
	FN(void, EnterCriticalSection, LPCRITICAL_SECTION);
	FN(void, LeaveCriticalSection, LPCRITICAL_SECTION);
	FN(BOOL, SleepConditionVariableCS, PCONDITION_VARIABLE, LPCRITICAL_SECTION,
	   DWORD);

	EnterCriticalSection(&cs);
	ready++;
	while (!__atomic_load_n(&woken, __ATOMIC_SEQ_CST))
		SleepConditionVariableCS(&cv, &cs, INFINITE);
	woken++;
	LeaveCriticalSection(&cs);
	return 0;
}

/* Waits (briefly) until n threads are inside their wait. */
static void wait_ready(LONG n)
{
	FN(void, Sleep, DWORD);

	for (int i = 0; i < 1000 && __atomic_load_n(&ready, __ATOMIC_SEQ_CST) < n; i++)
		Sleep(5);
	Sleep(20);	/* and past releasing the lock to wait */
}

static void test_condition_variables(void)
{
	FN(void, InitializeConditionVariable, PCONDITION_VARIABLE);
	FN(void, InitializeSRWLock, PSRWLOCK);
	FN(void, AcquireSRWLockExclusive, PSRWLOCK);
	FN(void, ReleaseSRWLockExclusive, PSRWLOCK);
	FN(BOOLEAN, TryAcquireSRWLockExclusive, PSRWLOCK);
	FN(BOOL, SleepConditionVariableSRW, PCONDITION_VARIABLE, PSRWLOCK, DWORD,
	   ULONG);
	FN(BOOL, SleepConditionVariableCS, PCONDITION_VARIABLE, LPCRITICAL_SECTION,
	   DWORD);
	FN(void, WakeConditionVariable, PCONDITION_VARIABLE);
	FN(void, WakeAllConditionVariable, PCONDITION_VARIABLE);
	FN(void, InitializeCriticalSection, LPCRITICAL_SECTION);
	FN(void, EnterCriticalSection, LPCRITICAL_SECTION);
	FN(void, LeaveCriticalSection, LPCRITICAL_SECTION);
	FN(BOOL, TryEnterCriticalSection, LPCRITICAL_SECTION);
	FN(DWORD, GetLastError, void);

	InitializeConditionVariable(&cv);
	InitializeSRWLock(&lock);
	InitializeCriticalSection(&cs);

	/* A sleep that nobody ends times out, holding the lock again. */
	AcquireSRWLockExclusive(&lock);
	CHECK(!SleepConditionVariableSRW(&cv, &lock, 20, 0));
	CHECK_EQ(GetLastError(), ERROR_TIMEOUT);
	CHECK(!TryAcquireSRWLockExclusive(&lock));
	ReleaseSRWLockExclusive(&lock);

	EnterCriticalSection(&cs);
	CHECK(!SleepConditionVariableCS(&cv, &cs, 20));
	CHECK_EQ(GetLastError(), ERROR_TIMEOUT);
	CHECK(TryEnterCriticalSection(&cs));	/* ours, so recursion works */
	LeaveCriticalSection(&cs);
	LeaveCriticalSection(&cs);

	/* Waking one thread of two wakes one; waking all wakes the rest. */
	ready = woken = 0;
	HANDLE a = start_thread(srw_waiter, NULL);
	HANDLE b = start_thread(srw_waiter, NULL);
	wait_ready(2);
	AcquireSRWLockExclusive(&lock);
	woken = 0;
	ReleaseSRWLockExclusive(&lock);
	WakeConditionVariable(&cv);
	/* The predicate is still false for them: the wake-up alone lets no
	 * thread finish. Set it, then wake both. */
	AcquireSRWLockExclusive(&lock);
	woken = 1;
	ReleaseSRWLockExclusive(&lock);
	WakeAllConditionVariable(&cv);
	join(a);
	join(b);
	CHECK_EQ(woken, 3);

	/* The same through a critical section. */
	ready = woken = 0;
	a = start_thread(cs_waiter, NULL);
	wait_ready(1);
	EnterCriticalSection(&cs);
	woken = 1;
	LeaveCriticalSection(&cs);
	WakeConditionVariable(&cv);
	join(a);
	CHECK_EQ(woken, 2);

	/*
	 * A wake-up goes to a thread that was already waiting, not to the one that
	 * signals and then waits itself: code that wakes a worker and then waits
	 * for its answer on the same variable would otherwise take its own wake-up
	 * and leave both asleep for ever.
	 */
	ready = woken = 0;
	a = start_thread(srw_waiter, NULL);
	wait_ready(1);
	AcquireSRWLockExclusive(&lock);
	WakeConditionVariable(&cv);	/* meant for the thread above */
	CHECK(!SleepConditionVariableSRW(&cv, &lock, 100, 0));	/* so not for us */
	CHECK_EQ(GetLastError(), ERROR_TIMEOUT);
	woken = 1;			/* let it finish: it was woken, and sees this */
	ReleaseSRWLockExclusive(&lock);
	WakeAllConditionVariable(&cv);
	join(a);
	CHECK_EQ(woken, 2);

	/* Waking with nobody waiting does nothing, and isn't remembered. */
	WakeConditionVariable(&cv);
	WakeAllConditionVariable(&cv);
	AcquireSRWLockExclusive(&lock);
	CHECK(!SleepConditionVariableSRW(&cv, &lock, 20, 0));
	ReleaseSRWLockExclusive(&lock);
}

/* One-time initialisation. */

static int once_runs;

static BOOL WINAPI once_ok(PINIT_ONCE once, PVOID param, PVOID *context)
{
	once_runs++;
	*context = param;
	return TRUE;
}

static BOOL WINAPI once_fails(PINIT_ONCE once, PVOID param, PVOID *context)
{
	once_runs++;
	return FALSE;
}

static void test_init_once(void)
{
	FN(void, InitOnceInitialize, PINIT_ONCE);
	FN(BOOL, InitOnceBeginInitialize, PINIT_ONCE, DWORD, PBOOL, PVOID *);
	FN(BOOL, InitOnceComplete, PINIT_ONCE, DWORD, PVOID);
	FN(BOOL, InitOnceExecuteOnce, PINIT_ONCE, PINIT_ONCE_FN, PVOID, PVOID *);
	FN(DWORD, GetLastError, void);

	INIT_ONCE once;
	BOOL pending = FALSE;
	PVOID ctx = NULL;

	InitOnceInitialize(&once);
	CHECK(!InitOnceBeginInitialize(&once, INIT_ONCE_CHECK_ONLY, &pending, &ctx));
	CHECK(pending);
	CHECK(InitOnceBeginInitialize(&once, 0, &pending, &ctx));
	CHECK(pending);
	CHECK(InitOnceComplete(&once, 0, (PVOID)0x1000));
	CHECK(InitOnceBeginInitialize(&once, 0, &pending, &ctx));
	CHECK(!pending);
	CHECK(ctx == (PVOID)0x1000);
	/* A context needs its two low bits free. */
	INIT_ONCE bad = { 0 };
	CHECK(InitOnceBeginInitialize(&bad, 0, &pending, &ctx));
	CHECK(!InitOnceComplete(&bad, 0, (PVOID)0x1001));
	CHECK_EQ(GetLastError(), ERROR_INVALID_PARAMETER);

	/* A failed initialisation can be tried again. */
	INIT_ONCE retry = { 0 };
	CHECK(InitOnceBeginInitialize(&retry, 0, &pending, &ctx));
	CHECK(InitOnceComplete(&retry, INIT_ONCE_INIT_FAILED, NULL));
	CHECK(InitOnceBeginInitialize(&retry, 0, &pending, &ctx));
	CHECK(pending);
	CHECK(InitOnceComplete(&retry, 0, NULL));

	/* ExecuteOnce runs the callback the first time only. */
	INIT_ONCE exec = { 0 };
	once_runs = 0;
	CHECK(InitOnceExecuteOnce(&exec, once_ok, (PVOID)0x2000, &ctx));
	CHECK(ctx == (PVOID)0x2000);
	ctx = NULL;
	CHECK(InitOnceExecuteOnce(&exec, once_ok, (PVOID)0x3000, &ctx));
	CHECK(ctx == (PVOID)0x2000);
	CHECK_EQ(once_runs, 1);

	INIT_ONCE fails = { 0 };
	once_runs = 0;
	CHECK(!InitOnceExecuteOnce(&fails, once_fails, NULL, NULL));
	CHECK(InitOnceExecuteOnce(&fails, once_ok, NULL, NULL));
	CHECK_EQ(once_runs, 2);
}

/* Interlocked lists. */

static void test_slist(void)
{
	FN(void, InitializeSListHead, PSLIST_HEADER);
	FN(PSLIST_ENTRY, InterlockedPushEntrySList, PSLIST_HEADER, PSLIST_ENTRY);
	FN(PSLIST_ENTRY, InterlockedPopEntrySList, PSLIST_HEADER);
	FN(PSLIST_ENTRY, InterlockedFlushSList, PSLIST_HEADER);
	FN(PSLIST_ENTRY, InterlockedPushListSList, PSLIST_HEADER, PSLIST_ENTRY,
	   PSLIST_ENTRY, ULONG);
	FN(WORD, QueryDepthSList, PSLIST_HEADER);

	SLIST_HEADER head;
	SLIST_ENTRY e[4];

	memset(&head, 0xff, sizeof(head));
	InitializeSListHead(&head);
	CHECK(head.Alignment == 0);
	CHECK_EQ(QueryDepthSList(&head), 0);
	CHECK(InterlockedPopEntrySList(&head) == NULL);

	CHECK(InterlockedPushEntrySList(&head, &e[0]) == NULL);
	CHECK(InterlockedPushEntrySList(&head, &e[1]) == &e[0]);
	CHECK(InterlockedPushEntrySList(&head, &e[2]) == &e[1]);
	CHECK_EQ(QueryDepthSList(&head), 3);
	CHECK(InterlockedPopEntrySList(&head) == &e[2]);	/* last in, first out */
	CHECK_EQ(QueryDepthSList(&head), 2);

	/* Two linked entries go on at once. */
	e[2].Next = &e[3];
	CHECK(InterlockedPushListSList(&head, &e[2], &e[3], 2) == &e[1]);
	CHECK_EQ(QueryDepthSList(&head), 4);
	CHECK(e[3].Next == &e[1]);

	/* Flushing hands back the whole chain, in order, and empties the list. */
	PSLIST_ENTRY all = InterlockedFlushSList(&head);
	CHECK(all == &e[2]);
	CHECK(all->Next == &e[3] && e[3].Next == &e[1] && e[1].Next == &e[0]);
	CHECK(e[0].Next == NULL);
	CHECK_EQ(QueryDepthSList(&head), 0);
	CHECK(InterlockedPopEntrySList(&head) == NULL);
}

/* VerifyVersionInfo. */

static void test_version(void)
{
	FN(BOOL, GetVersionExW, OSVERSIONINFOW *);
	FN(ULONGLONG, VerSetConditionMask, ULONGLONG, DWORD, BYTE);
	FN(BOOL, VerifyVersionInfoW, OSVERSIONINFOEXW *, DWORD, ULONGLONG);
	FN(DWORD, GetLastError, void);

	OSVERSIONINFOEXW have = { .dwOSVersionInfoSize = sizeof(have) };
	CHECK(GetVersionExW((OSVERSIONINFOW *)&have));

	/* Each field's condition is three bits, set by VerSetConditionMask. */
	ULONGLONG m = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
	CHECK_EQ(m, (ULONGLONG)VER_GREATER_EQUAL << 3);
	m = VerSetConditionMask(m, VER_MINORVERSION, VER_LESS);
	CHECK_EQ(m, ((ULONGLONG)VER_GREATER_EQUAL << 3) | VER_LESS);
	m = VerSetConditionMask(m, VER_MAJORVERSION | VER_PRODUCT_TYPE, VER_EQUAL);
	CHECK_EQ(m, ((ULONGLONG)VER_EQUAL << 3) | VER_LESS |
		    ((ULONGLONG)VER_EQUAL << 21));

	OSVERSIONINFOEXW want = { .dwOSVersionInfoSize = sizeof(want) };
	ULONGLONG ge = VerSetConditionMask(
		VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL),
		VER_MINORVERSION, VER_GREATER_EQUAL);
	DWORD both = VER_MAJORVERSION | VER_MINORVERSION;

	/* The version itself satisfies "at least". */
	want.dwMajorVersion = have.dwMajorVersion;
	want.dwMinorVersion = have.dwMinorVersion;
	CHECK(VerifyVersionInfoW(&want, both, ge));
	/* An older one does, however high its minor number is. */
	want.dwMajorVersion = have.dwMajorVersion - 1;
	want.dwMinorVersion = 99;
	CHECK(VerifyVersionInfoW(&want, both, ge));
	/* A newer one doesn't, whatever its minor number. */
	want.dwMajorVersion = have.dwMajorVersion + 1;
	want.dwMinorVersion = 0;
	CHECK(!VerifyVersionInfoW(&want, both, ge));
	CHECK_EQ(GetLastError(), ERROR_OLD_WIN_VERSION);
	/* Same major, newer minor. */
	want.dwMajorVersion = have.dwMajorVersion;
	want.dwMinorVersion = have.dwMinorVersion + 1;
	CHECK(!VerifyVersionInfoW(&want, both, ge));

	/* Equality, and strictly greater, which the version itself isn't. */
	want.dwMajorVersion = have.dwMajorVersion;
	want.dwMinorVersion = have.dwMinorVersion;
	CHECK(VerifyVersionInfoW(&want, both,
		VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL),
				    VER_MINORVERSION, VER_EQUAL)));
	CHECK(!VerifyVersionInfoW(&want, both,
		VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER),
				    VER_MINORVERSION, VER_GREATER)));
	CHECK(!VerifyVersionInfoW(&want, 0, 0));	/* nothing to test */
}

/* The smaller functions. */

static void test_misc(void)
{
	FN(PVOID, EncodePointer, PVOID);
	FN(PVOID, DecodePointer, PVOID);
	FN(BOOL, GetProcessTimes, HANDLE, LPFILETIME, LPFILETIME, LPFILETIME,
	   LPFILETIME);
	FN(BOOL, GetThreadTimes, HANDLE, LPFILETIME, LPFILETIME, LPFILETIME,
	   LPFILETIME);
	FN(HANDLE, GetCurrentProcess, void);
	FN(HANDLE, GetCurrentThread, void);
	FN(DWORD, GetCurrentProcessId, void);
	FN(HANDLE, OpenProcess, DWORD, BOOL, DWORD);
	FN(BOOL, GetSystemTimeAdjustment, PDWORD, PDWORD, PBOOL);
	FN(BOOL, SystemTimeToTzSpecificLocalTime, const TIME_ZONE_INFORMATION *,
	   const SYSTEMTIME *, LPSYSTEMTIME);
	FN(BOOL, HeapQueryInformation, HANDLE, int, PVOID, SIZE_T, PSIZE_T);
	FN(HANDLE, GetProcessHeap, void);
	FN(HANDLE, CreateEventExW, LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
	FN(HANDLE, CreateSemaphoreExW, LPSECURITY_ATTRIBUTES, LONG, LONG, LPCWSTR,
	   DWORD, DWORD);
	FN(DWORD, WaitForSingleObject, HANDLE, DWORD);
	FN(BOOL, ReleaseSemaphore, HANDLE, LONG, LPLONG);
	FN(BOOL, CloseHandle, HANDLE);
	FN(BOOL, GetThreadGroupAffinity, HANDLE, PGROUP_AFFINITY);
	FN(BOOL, SetThreadGroupAffinity, HANDLE, const GROUP_AFFINITY *,
	   PGROUP_AFFINITY);
	FN(void, GetCurrentProcessorNumberEx, PPROCESSOR_NUMBER);
	FN(BOOL, GetNumaHighestNodeNumber, ULONG *);
	FN(BOOL, GetLogicalProcessorInformationEx, DWORD, PVOID, PDWORD);
	FN(void, FlushProcessWriteBuffers, void);
	FN(DWORD, GetLastError, void);

	CHECK(DecodePointer(EncodePointer((PVOID)0x1234)) == (PVOID)0x1234);

	FILETIME c, x, kernel, user;
	CHECK(GetProcessTimes(GetCurrentProcess(), &c, &x, &kernel, &user));
	CHECK(c.dwHighDateTime != 0);
	CHECK(GetThreadTimes(GetCurrentThread(), &c, &x, &kernel, &user));
	CHECK(!GetProcessTimes((HANDLE)0x1234, &c, &x, &kernel, &user));

	CHECK(OpenProcess(0x1fffff, FALSE, GetCurrentProcessId()) ==
	      GetCurrentProcess());
	CHECK(OpenProcess(0x1fffff, FALSE, 0) == NULL);

	DWORD adj = 0, inc = 0;
	BOOL disabled = FALSE;
	CHECK(GetSystemTimeAdjustment(&adj, &inc, &disabled));
	CHECK(inc != 0 && disabled);

	/* The zone is UTC, so local time is the same, and a bias moves it. */
	TIME_ZONE_INFORMATION tz;
	memset(&tz, 0, sizeof(tz));
	SYSTEMTIME utc = { 2024, 3, 0, 9, 22, 30, 0, 0 }, local;
	CHECK(SystemTimeToTzSpecificLocalTime(NULL, &utc, &local));
	CHECK_EQ(local.wHour, 22);
	tz.Bias = 120;		/* two hours west of UTC... */
	CHECK(SystemTimeToTzSpecificLocalTime(&tz, &utc, &local));
	CHECK_EQ(local.wHour, 20);
	CHECK_EQ(local.wDay, 9);
	tz.Bias = -180;		/* ...and three east, into the next day */
	CHECK(SystemTimeToTzSpecificLocalTime(&tz, &utc, &local));
	CHECK_EQ(local.wHour, 1);
	CHECK_EQ(local.wDay, 10);

	ULONG compat = 99;
	SIZE_T need = 0;
	CHECK(HeapQueryInformation(GetProcessHeap(), 0, &compat, sizeof(compat), &need));
	CHECK_EQ(compat, 0);
	CHECK_EQ(need, sizeof(ULONG));
	CHECK(!HeapQueryInformation(GetProcessHeap(), 0, &compat, 1, &need));
	CHECK_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);
	CHECK(!HeapQueryInformation(GetProcessHeap(), 5, &compat, sizeof(compat), &need));

	/* CreateEventEx's flags are CreateEvent's two booleans. */
	HANDLE ev = CreateEventExW(NULL, NULL, CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET, 0);
	CHECK(ev != NULL);
	CHECK_EQ(WaitForSingleObject(ev, 0), WAIT_OBJECT_0);
	CHECK_EQ(WaitForSingleObject(ev, 0), WAIT_OBJECT_0);	/* manual reset */
	CloseHandle(ev);
	ev = CreateEventExW(NULL, NULL, 0, 0);
	CHECK_EQ(WaitForSingleObject(ev, 0), WAIT_TIMEOUT);
	CloseHandle(ev);
	HANDLE sem = CreateSemaphoreExW(NULL, 1, 2, NULL, 0, 0);
	CHECK(sem != NULL);
	CHECK_EQ(WaitForSingleObject(sem, 0), WAIT_OBJECT_0);
	CHECK_EQ(WaitForSingleObject(sem, 0), WAIT_TIMEOUT);
	CHECK(ReleaseSemaphore(sem, 2, NULL));
	CHECK(!ReleaseSemaphore(sem, 1, NULL));		/* over the maximum */
	CloseHandle(sem);

	/* One group, all processors. */
	GROUP_AFFINITY ga, prev;
	CHECK(GetThreadGroupAffinity(GetCurrentThread(), &ga));
	CHECK_EQ(ga.Group, 0);
	CHECK(ga.Mask != 0);
	CHECK(SetThreadGroupAffinity(GetCurrentThread(), &ga, &prev));
	CHECK(prev.Mask == ga.Mask);
	ga.Group = 1;
	CHECK(!SetThreadGroupAffinity(GetCurrentThread(), &ga, NULL));
	PROCESSOR_NUMBER pn = { 9, 9, 9 };
	GetCurrentProcessorNumberEx(&pn);
	CHECK_EQ(pn.Group, 0);
	CHECK(pn.Number < 64);
	ULONG node = 7;
	CHECK(GetNumaHighestNodeNumber(&node));
	CHECK_EQ(node, 0);
	DWORD len = 0;
	CHECK(!GetLogicalProcessorInformationEx(0, NULL, &len));

	FlushProcessWriteBuffers();
}

/* Thread-pool timers, waits and work. */

static volatile LONG pool_count;
static volatile ULONG pool_result;
static HANDLE pool_event;
static CRITICAL_SECTION pool_cs;

static void WINAPI count_timer(PVOID inst, PVOID ctx, PVOID timer)
{
	__atomic_add_fetch(&pool_count, 1, __ATOMIC_SEQ_CST);
}

static void WINAPI count_wait(PVOID inst, PVOID ctx, PVOID wait, ULONG result)
{
	__atomic_store_n(&pool_result, result, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&pool_count, 1, __ATOMIC_SEQ_CST);
}

static void WINAPI slow_work(PVOID inst, PVOID ctx, PVOID work)
{
	FN(void, Sleep, DWORD);

	Sleep(40);
	__atomic_add_fetch(&pool_count, 1, __ATOMIC_SEQ_CST);
}

static void WINAPI simple_callback(PVOID inst, PVOID ctx)
{
	FN(void, SetEventWhenCallbackReturns, PVOID, HANDLE);
	FN(void, EnterCriticalSection, LPCRITICAL_SECTION);
	FN(void, LeaveCriticalSectionWhenCallbackReturns, PVOID, LPCRITICAL_SECTION);

	EnterCriticalSection(&pool_cs);
	LeaveCriticalSectionWhenCallbackReturns(inst, &pool_cs);
	SetEventWhenCallbackReturns(inst, pool_event);
	__atomic_add_fetch(&pool_count, 1, __ATOMIC_SEQ_CST);
}

static LONG pool_get(void) { return __atomic_load_n(&pool_count, __ATOMIC_SEQ_CST); }

static void test_thread_pool(void)
{
	FN(PVOID, CreateThreadpoolTimer, PVOID, PVOID, PVOID);
	FN(void, SetThreadpoolTimer, PVOID, PFILETIME, DWORD, DWORD);
	FN(BOOL, IsThreadpoolTimerSet, PVOID);
	FN(void, WaitForThreadpoolTimerCallbacks, PVOID, BOOL);
	FN(void, CloseThreadpoolTimer, PVOID);
	FN(PVOID, CreateThreadpoolWait, PVOID, PVOID, PVOID);
	FN(void, SetThreadpoolWait, PVOID, HANDLE, PFILETIME);
	FN(void, WaitForThreadpoolWaitCallbacks, PVOID, BOOL);
	FN(void, CloseThreadpoolWait, PVOID);
	FN(PVOID, CreateThreadpoolWork, PVOID, PVOID, PVOID);
	FN(void, SubmitThreadpoolWork, PVOID);
	FN(void, WaitForThreadpoolWorkCallbacks, PVOID, BOOL);
	FN(void, CloseThreadpoolWork, PVOID);
	FN(BOOL, TrySubmitThreadpoolCallback, PVOID, PVOID, PVOID);
	FN(HANDLE, CreateEventA, LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
	FN(BOOL, SetEvent, HANDLE);
	FN(DWORD, WaitForSingleObject, HANDLE, DWORD);
	FN(BOOL, CloseHandle, HANDLE);
	FN(void, Sleep, DWORD);
	FN(void, InitializeCriticalSection, LPCRITICAL_SECTION);
	FN(BOOL, TryEnterCriticalSection, LPCRITICAL_SECTION);
	FN(void, LeaveCriticalSection, LPCRITICAL_SECTION);

	/* A timer set 30 ms ahead (negative: relative) fires once. */
	PVOID t = CreateThreadpoolTimer(count_timer, NULL, NULL);
	CHECK(t != NULL);
	CHECK(!IsThreadpoolTimerSet(t));
	LONGLONG rel = -30 * 10000LL;
	FILETIME due = { (DWORD)rel, (DWORD)((ULONGLONG)rel >> 32) };
	pool_count = 0;
	SetThreadpoolTimer(t, &due, 0, 0);
	CHECK(IsThreadpoolTimerSet(t));
	Sleep(10);
	CHECK_EQ(pool_get(), 0);
	for (int i = 0; i < 200 && !pool_get(); i++)
		Sleep(5);
	CHECK_EQ(pool_get(), 1);
	Sleep(60);
	CHECK_EQ(pool_get(), 1);
	CHECK(!IsThreadpoolTimerSet(t));

	/* A repeating one keeps firing until it is cancelled with a NULL time. */
	pool_count = 0;
	rel = -10 * 10000LL;
	due.dwLowDateTime = (DWORD)rel;
	due.dwHighDateTime = (DWORD)((ULONGLONG)rel >> 32);
	SetThreadpoolTimer(t, &due, 10, 0);
	for (int i = 0; i < 400 && pool_get() < 4; i++)
		Sleep(5);
	CHECK(pool_get() >= 4);
	SetThreadpoolTimer(t, NULL, 0, 0);
	WaitForThreadpoolTimerCallbacks(t, TRUE);
	LONG stopped = pool_get();
	Sleep(60);
	CHECK_EQ(pool_get(), stopped);

	/* Re-arming replaces: a far-off timer re-set to soon fires once, soon. */
	pool_count = 0;
	rel = -60000 * 10000LL;
	due.dwLowDateTime = (DWORD)rel;
	due.dwHighDateTime = (DWORD)((ULONGLONG)rel >> 32);
	SetThreadpoolTimer(t, &due, 0, 0);
	rel = -10 * 10000LL;
	due.dwLowDateTime = (DWORD)rel;
	due.dwHighDateTime = (DWORD)((ULONGLONG)rel >> 32);
	SetThreadpoolTimer(t, &due, 0, 0);
	for (int i = 0; i < 200 && !pool_get(); i++)
		Sleep(5);
	CHECK_EQ(pool_get(), 1);
	CloseThreadpoolTimer(t);

	/* Closing an armed timer cancels it. */
	t = CreateThreadpoolTimer(count_timer, NULL, NULL);
	pool_count = 0;
	rel = -30 * 10000LL;
	due.dwLowDateTime = (DWORD)rel;
	due.dwHighDateTime = (DWORD)((ULONGLONG)rel >> 32);
	SetThreadpoolTimer(t, &due, 0, 0);
	CloseThreadpoolTimer(t);
	Sleep(80);
	CHECK_EQ(pool_get(), 0);

	/* A wait calls back when its handle is signalled, once... */
	HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
	PVOID w = CreateThreadpoolWait(count_wait, NULL, NULL);
	CHECK(w != NULL);
	pool_count = 0;
	SetThreadpoolWait(w, ev, NULL);
	Sleep(30);
	CHECK_EQ(pool_get(), 0);
	SetEvent(ev);
	for (int i = 0; i < 200 && !pool_get(); i++)
		Sleep(5);
	CHECK_EQ(pool_get(), 1);
	CHECK_EQ(pool_result, WAIT_OBJECT_0);
	/* ...taking the auto-reset event with it, and not again unless re-set. */
	CHECK_EQ(WaitForSingleObject(ev, 0), WAIT_TIMEOUT);
	SetEvent(ev);
	Sleep(40);
	CHECK_EQ(pool_get(), 1);
	/* The event is still set; re-arming sees it at once. */
	SetThreadpoolWait(w, ev, NULL);
	for (int i = 0; i < 200 && pool_get() < 2; i++)
		Sleep(5);
	CHECK_EQ(pool_get(), 2);

	/* A timeout reports WAIT_TIMEOUT. */
	rel = -30 * 10000LL;
	due.dwLowDateTime = (DWORD)rel;
	due.dwHighDateTime = (DWORD)((ULONGLONG)rel >> 32);
	pool_count = 0;
	SetThreadpoolWait(w, ev, &due);
	for (int i = 0; i < 200 && !pool_get(); i++)
		Sleep(5);
	CHECK_EQ(pool_get(), 1);
	CHECK_EQ(pool_result, WAIT_TIMEOUT);

	/* A NULL handle cancels a wait. */
	pool_count = 0;
	SetThreadpoolWait(w, ev, NULL);
	SetThreadpoolWait(w, NULL, NULL);
	WaitForThreadpoolWaitCallbacks(w, TRUE);
	SetEvent(ev);
	Sleep(60);
	CHECK_EQ(pool_get(), 0);
	CloseThreadpoolWait(w);
	CloseHandle(ev);

	/* Work items run on their own threads; waiting covers all submitted. */
	PVOID work = CreateThreadpoolWork(slow_work, NULL, NULL);
	CHECK(work != NULL);
	pool_count = 0;
	for (int i = 0; i < 3; i++)
		SubmitThreadpoolWork(work);
	WaitForThreadpoolWorkCallbacks(work, FALSE);
	CHECK_EQ(pool_get(), 3);
	CloseThreadpoolWork(work);

	/* A one-off callback may ask for an event and a section to be released. */
	pool_event = CreateEventA(NULL, TRUE, FALSE, NULL);
	InitializeCriticalSection(&pool_cs);
	pool_count = 0;
	CHECK(TrySubmitThreadpoolCallback(simple_callback, NULL, NULL));
	CHECK_EQ(WaitForSingleObject(pool_event, 10000), WAIT_OBJECT_0);
	CHECK_EQ(pool_get(), 1);
	CHECK(TryEnterCriticalSection(&pool_cs));	/* the callback left it */
	LeaveCriticalSection(&pool_cs);
	CloseHandle(pool_event);
}

/* The locale-name forms of the string functions. */

static BOOL WINAPI count_locale(LPWSTR name)
{
	pool_count++;
	return name[0] == '0';
}

static void test_locale_names(void)
{
	FN(int, CompareStringEx, LPCWSTR, DWORD, LPCWSTR, int, LPCWSTR, int, LPVOID,
	   LPVOID, LPARAM);
	FN(int, LCMapStringEx, LPCWSTR, DWORD, LPCWSTR, int, LPWSTR, int, LPVOID,
	   LPVOID, LPARAM);
	FN(BOOL, EnumSystemLocalesW, BOOL (WINAPI *)(LPWSTR), DWORD);
	static const WCHAR en[] = { 'e', 'n', '-', 'U', 'S', 0 };
	static const WCHAR lower[] = { 'a', 'b', 'c', 0 };
	static const WCHAR upper[] = { 'A', 'B', 'C', 0 };
	static const WCHAR later[] = { 'a', 'b', 'd', 0 };
	WCHAR out[8];

	CHECK_EQ(CompareStringEx(en, 0, lower, -1, lower, -1, NULL, NULL, 0), 2);	/* CSTR_EQUAL */
	CHECK_EQ(CompareStringEx(en, 0, lower, -1, later, -1, NULL, NULL, 0), 1);	/* CSTR_LESS_THAN */
	CHECK_EQ(CompareStringEx(en, 1 /* NORM_IGNORECASE */, lower, -1, upper, -1,
				 NULL, NULL, 0), 2);
	CHECK_EQ(LCMapStringEx(en, 0x200 /* LCMAP_UPPERCASE */, lower, -1, out, 8,
			       NULL, NULL, 0), 4);
	CHECK(out[0] == 'A' && out[2] == 'C' && out[3] == 0);
	pool_count = 0;
	CHECK(EnumSystemLocalesW(count_locale, 0));
	CHECK_EQ(pool_count, 1);
	CHECK(!EnumSystemLocalesW(NULL, 0));
}

/* Files: the by-handle information and extended searches. */

static void test_files(void)
{
	FN(HANDLE, CreateFileA, LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
	   DWORD, HANDLE);
	FN(BOOL, WriteFile, HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
	FN(BOOL, CloseHandle, HANDLE);
	FN(BOOL, GetFileInformationByHandle, HANDLE, LPBY_HANDLE_FILE_INFORMATION);
	FN(DWORD, GetFinalPathNameByHandleA, HANDLE, LPSTR, DWORD, DWORD);
	FN(DWORD, GetFinalPathNameByHandleW, HANDLE, LPWSTR, DWORD, DWORD);
	FN(HANDLE, FindFirstFileExW, LPCWSTR, DWORD, LPVOID, DWORD, LPVOID, DWORD);
	FN(BOOL, FindNextFileW, HANDLE, LPWIN32_FIND_DATAW);
	FN(BOOL, FindClose, HANDLE);
	FN(BOOL, DeleteFileA, LPCSTR);
	FN(BOOL, CancelIoEx, HANDLE, LPOVERLAPPED);
	FN(BOOL, ReadConsoleW, HANDLE, LPVOID, DWORD, LPDWORD, LPVOID);
	FN(BOOL, GetConsoleScreenBufferInfo, HANDLE, PCONSOLE_SCREEN_BUFFER_INFO);
	FN(BOOL, SetConsoleTextAttribute, HANDLE, WORD);
	FN(DWORD, GetLastError, void);

	HANDLE f = CreateFileA("C:\\WINDOWS\\TEMP\\k32modern.dat", GENERIC_WRITE, 0,
			       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(f != INVALID_HANDLE_VALUE);
	DWORD n;
	CHECK(WriteFile(f, "twelve bytes", 12, &n, NULL));

	BY_HANDLE_FILE_INFORMATION info;
	CHECK(GetFileInformationByHandle(f, &info));
	CHECK_EQ(info.nFileSizeLow, 12);
	CHECK_EQ(info.nFileSizeHigh, 0);
	CHECK(!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
	CHECK_EQ(info.nNumberOfLinks, 1);
	CHECK(info.ftLastWriteTime.dwHighDateTime != 0);

	char path[MAX_PATH];
	DWORD len = GetFinalPathNameByHandleA(f, path, sizeof(path), 0);
	CHECK(len > 0 && len < sizeof(path));
	CHECK(!strncmp(path, "\\\\?\\", 4));
	CHECK(strstr(path, "k32modern.dat") != NULL);
	WCHAR wide[MAX_PATH];
	CHECK_EQ(GetFinalPathNameByHandleW(f, wide, MAX_PATH, 0), len);
	CHECK_EQ(GetFinalPathNameByHandleA(f, path, 4, 0), len + 1);	/* too small */
	CHECK(!GetFileInformationByHandle(INVALID_HANDLE_VALUE, &info));
	CHECK(!CancelIoEx(f, NULL));
	CHECK_EQ(GetLastError(), ERROR_NOT_FOUND);
	/* A file isn't a console. */
	DWORD got;
	WCHAR in[4];
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	CHECK(!ReadConsoleW(f, in, 4, &got, NULL));
	CHECK(!GetConsoleScreenBufferInfo(f, &csbi));
	CHECK(!SetConsoleTextAttribute(f, 7));
	CloseHandle(f);

	/* FindFirstFileEx finds what FindFirstFile does, at either level. */
	static const WCHAR pattern[] = { 'C', ':', '\\', 'W', 'I', 'N', 'D', 'O', 'W', 'S',
		'\\', 'T', 'E', 'M', 'P', '\\', 'k', '3', '2', 'm', '*', '.', 'd', 'a', 't', 0 };
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd,
				    FindExSearchNameMatch, NULL, 0);
	CHECK(h != INVALID_HANDLE_VALUE);
	CHECK_EQ(fd.nFileSizeLow, 12);
	CHECK_EQ(fd.cFileName[0], 'k');
	CHECK(!FindNextFileW(h, &fd));
	CHECK(FindClose(h));
	CHECK(FindFirstFileExW(pattern, 7, &fd, FindExSearchNameMatch, NULL, 0) ==
	      INVALID_HANDLE_VALUE);
	CHECK_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
	CHECK(DeleteFileA("C:\\WINDOWS\\TEMP\\k32modern.dat"));
}

int main(void)
{
	char root[256];
	const char *tmpdir = getenv("TMPDIR");

	snprintf(root, sizeof(root), "%s/vfw-k32m-XXXXXX",
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
		return test_summary("test_k32_modern");

	test_srw();
	test_condition_variables();
	test_init_once();
	test_slist();
	test_version();
	test_misc();
	test_thread_pool();
	test_locale_names();
	test_files();

	mod_free(k32);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", root);
	return test_summary("test_k32_modern");
}
