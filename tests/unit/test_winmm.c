/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * winmm's multimedia timers through its export table: periodic and
 * one-shot callbacks on winmm's timer thread, event timers, and killing
 * timers, synchronously and from their own callbacks.
 */
#include <stdlib.h>
#include <time.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "win32/internal.h"
#include "test.h"

static struct module *mm, *k32;

static void *get_from(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "%s has no %s\n", m->name, name);
		exit(1);
	}
	return p;
}

#define FN(m, ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(m, #name)

static UINT (WINAPI *timeSetEvent)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR,
				   UINT);
static UINT (WINAPI *timeKillEvent)(UINT);
static DWORD (WINAPI *GetCurrentThreadId)(void);

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
	nanosleep(&ts, NULL);
}

/* Waits up to two seconds for *v to reach at least n. */
static int wait_for(volatile LONG *v, LONG n)
{
	for (int i = 0; i < 400 && __atomic_load_n(v, __ATOMIC_SEQ_CST) < n;
	     i++)
		sleep_ms(5);
	return __atomic_load_n(v, __ATOMIC_SEQ_CST) >= n;
}

static volatile LONG ticks;
static volatile DWORD callback_thread;
static volatile UINT last_id;

static void WINAPI count(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR dw1,
			 DWORD_PTR dw2)
{
	callback_thread = GetCurrentThreadId();
	last_id = id;
	__atomic_add_fetch((LONG *)user, 1, __ATOMIC_SEQ_CST);
}

static void test_periodic(void)
{
	ticks = 0;
	UINT id = timeSetEvent(10, 1, count, (DWORD_PTR)&ticks,
			       TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
	CHECK(id != 0);
	CHECK(wait_for(&ticks, 3));
	CHECK_EQ(last_id, id);
	CHECK(callback_thread && callback_thread != GetCurrentThreadId());
	/* Synchronous: no callback runs once the kill returns. */
	CHECK_EQ(timeKillEvent(id), TIMERR_NOERROR);
	LONG n = ticks;
	sleep_ms(50);
	CHECK_EQ(ticks, n);
	CHECK_EQ(timeKillEvent(id), MMSYSERR_INVALPARAM);
}

static void test_one_shot(void)
{
	static volatile LONG once;
	UINT id = timeSetEvent(5, 1, count, (DWORD_PTR)&once, TIME_ONESHOT);
	CHECK(id != 0);
	CHECK(wait_for(&once, 1));
	sleep_ms(40);
	CHECK_EQ(once, 1);
	CHECK_EQ(timeKillEvent(id), MMSYSERR_INVALPARAM);
}

static void test_event(void)
{
	FN(k32, HANDLE, CreateEventA, LPSECURITY_ATTRIBUTES, BOOL, BOOL,
	   LPCSTR);
	FN(k32, DWORD, WaitForSingleObject, HANDLE, DWORD);
	FN(k32, BOOL, CloseHandle, HANDLE);

	HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
	CHECK(ev != NULL);
	UINT id = timeSetEvent(5, 1, (LPTIMECALLBACK)ev, 0,
			       TIME_ONESHOT | TIME_CALLBACK_EVENT_SET);
	CHECK(id != 0);
	CHECK_EQ(WaitForSingleObject(ev, 2000), WAIT_OBJECT_0);
	CloseHandle(ev);
}

static volatile LONG in_callback, finished;

static void WINAPI slow(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR dw1,
			DWORD_PTR dw2)
{
	__atomic_store_n(&in_callback, 1, __ATOMIC_SEQ_CST);
	sleep_ms(100);
	__atomic_store_n(&finished, 1, __ATOMIC_SEQ_CST);
}

/* A synchronous kill waits for the callback that is running. */
static void test_kill_synchronous(void)
{
	UINT id = timeSetEvent(5, 1, slow, 0,
			       TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
	CHECK(id != 0);
	CHECK(wait_for(&in_callback, 1));
	CHECK_EQ(timeKillEvent(id), TIMERR_NOERROR);
	CHECK_EQ(finished, 1);
}

static volatile LONG self_kills;
static volatile UINT self_result = 12345;

static void WINAPI kill_self(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR dw1,
			     DWORD_PTR dw2)
{
	self_result = timeKillEvent(id);
	__atomic_add_fetch(&self_kills, 1, __ATOMIC_SEQ_CST);
}

/* A timer may kill itself from its callback, without waiting on itself. */
static void test_kill_self(void)
{
	UINT id = timeSetEvent(5, 1, kill_self, 0,
			       TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
	CHECK(id != 0);
	CHECK(wait_for(&self_kills, 1));
	sleep_ms(40);
	CHECK_EQ(self_kills, 1);
	CHECK_EQ(self_result, TIMERR_NOERROR);
}

static void test_bad_arguments(void)
{
	CHECK_EQ(timeSetEvent(0, 1, count, (DWORD_PTR)&ticks, TIME_ONESHOT), 0);
	CHECK_EQ(timeSetEvent(10, 1, NULL, 0, TIME_ONESHOT), 0);
	CHECK_EQ(timeSetEvent(2000000, 1, count, (DWORD_PTR)&ticks,
			      TIME_ONESHOT), 0);
	CHECK_EQ(timeKillEvent(0), MMSYSERR_INVALPARAM);
	CHECK_EQ(timeKillEvent(0x7fffffff), MMSYSERR_INVALPARAM);
}

int main(void)
{
	CHECK_EQ(runtime_enter(), 0);
	mm = mod_load("winmm.dll", NULL);
	k32 = mod_load("kernel32.dll", NULL);
	CHECK(mm && k32);
	if (!mm || !k32)
		return test_summary("test_winmm");
	timeSetEvent = get_from(mm, "timeSetEvent");
	timeKillEvent = get_from(mm, "timeKillEvent");
	GetCurrentThreadId = get_from(k32, "GetCurrentThreadId");

	test_bad_arguments();
	test_periodic();
	test_one_shot();
	test_event();
	test_kill_synchronous();
	test_kill_self();

	mod_free(k32);
	mod_free(mm);
	return test_summary("test_winmm");
}
