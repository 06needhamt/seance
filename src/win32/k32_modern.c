/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: the synchronisation primitives added after Windows XP that
 * current runtimes (the C++ and FFmpeg ones) use without checking for
 * them: slim reader/writer locks, condition variables, one-time
 * initialisation and interlocked singly linked lists.
 *
 * Each of the first three is a single zero-initialised word the DLL owns,
 * so its state lives in that word and is guarded by the wait lock that
 * the events and semaphores use; blocking is on the shared condition,
 * woken by every change. That makes them slower than Windows' but simple,
 * and fair enough for a test harness.
 */
#include <pthread.h>
#include <stdint.h>

#include "win32/handle.h"
#include "win32/internal.h"
#include "win32/k32.h"

/* The word behind a lock, as the integer we keep in it. */
static uint32_t *word(void *p)
{
	return (uint32_t *)p;
}

/*
 * Slim reader/writer locks: bit 0 is set while a writer holds the lock;
 * the readers holding it are counted in the bits above.
 */

#define SRW_WRITER 1u
#define SRW_READER 2u

WINFN(void) k32_InitializeSRWLock(PSRWLOCK l)
{
	l->Ptr = NULL;
}

static void srw_acquire(PSRWLOCK l, int exclusive)
{
	uint32_t *w = word(&l->Ptr);

	wait_lock();
	while (exclusive ? *w != 0 : (*w & SRW_WRITER))
		wait_block(NULL);
	*w += exclusive ? SRW_WRITER : SRW_READER;
	wait_unlock();
}

static BOOL srw_try(PSRWLOCK l, int exclusive)
{
	uint32_t *w = word(&l->Ptr);
	BOOL got;

	wait_lock();
	got = exclusive ? *w == 0 : !(*w & SRW_WRITER);
	if (got)
		*w += exclusive ? SRW_WRITER : SRW_READER;
	wait_unlock();
	return got;
}

/* With the wait lock held. */
static void srw_release_locked(PSRWLOCK l, int exclusive)
{
	uint32_t *w = word(&l->Ptr);

	if (exclusive)
		*w &= ~SRW_WRITER;
	else if (*w >= SRW_READER)
		*w -= SRW_READER;
	wait_broadcast();
}

static void srw_release(PSRWLOCK l, int exclusive)
{
	wait_lock();
	srw_release_locked(l, exclusive);
	wait_unlock();
}

WINFN(void) k32_AcquireSRWLockExclusive(PSRWLOCK l) { srw_acquire(l, 1); }
WINFN(void) k32_AcquireSRWLockShared(PSRWLOCK l) { srw_acquire(l, 0); }
WINFN(void) k32_ReleaseSRWLockExclusive(PSRWLOCK l) { srw_release(l, 1); }
WINFN(void) k32_ReleaseSRWLockShared(PSRWLOCK l) { srw_release(l, 0); }
WINFN(BOOLEAN) k32_TryAcquireSRWLockExclusive(PSRWLOCK l) { return srw_try(l, 1); }
WINFN(BOOLEAN) k32_TryAcquireSRWLockShared(PSRWLOCK l) { return srw_try(l, 0); }

/*
 * Condition variables: the low half of the word counts the threads
 * waiting, the high half the wake-ups handed out and not yet taken, so a
 * wake-up reaches only a thread that was already waiting.
 */

#define CV_WAITING(w) ((w) & 0xffffu)
#define CV_SIGNALS(w) ((w) >> 16)

WINFN(void) k32_InitializeConditionVariable(PCONDITION_VARIABLE cv)
{
	cv->Ptr = NULL;
}

/*
 * Releases the caller's lock, waits to be woken for up to ms, and takes the
 * lock again. release runs with the wait lock held, acquire without it.
 */
struct cv_lock {
	void (*release)(void *ctx);
	void (*acquire)(void *ctx);
	void *ctx;
};

static BOOL cv_sleep(PCONDITION_VARIABLE cv, DWORD ms, const struct cv_lock *l)
{
	uint32_t *w = word(&cv->Ptr);
	struct timespec deadline;
	int expired = 0;
	BOOL woken = TRUE;

	if (ms != INFINITE)
		wait_deadline(ms, &deadline);
	wait_lock();
	*w += 1;
	l->release(l->ctx);
	for (;;) {
		if (CV_SIGNALS(*w)) {
			*w -= 0x10000u + 1;
			break;
		}
		if (expired) {
			*w -= 1;
			woken = FALSE;
			break;
		}
		if (wait_block(ms == INFINITE ? NULL : &deadline))
			expired = 1;
	}
	wait_unlock();
	l->acquire(l->ctx);
	if (!woken)
		win_set_error(ERROR_TIMEOUT);
	return woken;
}

struct srw_ref {
	PSRWLOCK lock;
	int exclusive;
};

static void srw_ref_release(void *ctx)
{
	struct srw_ref *r = ctx;
	srw_release_locked(r->lock, r->exclusive);
}

static void srw_ref_acquire(void *ctx)
{
	struct srw_ref *r = ctx;
	srw_acquire(r->lock, r->exclusive);
}

WINFN(BOOL) k32_SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock,
					  DWORD ms, ULONG flags)
{
	struct srw_ref r = { lock, !(flags & CONDITION_VARIABLE_LOCKMODE_SHARED) };
	struct cv_lock l = { srw_ref_release, srw_ref_acquire, &r };

	return cv_sleep(cv, ms, &l);
}

static void cs_ref_release(void *ctx)
{
	k32_cs_leave(ctx);
}

static void cs_ref_acquire(void *ctx)
{
	k32_cs_enter(ctx);
}

WINFN(BOOL) k32_SleepConditionVariableCS(PCONDITION_VARIABLE cv,
					 LPCRITICAL_SECTION cs, DWORD ms)
{
	struct cv_lock l = { cs_ref_release, cs_ref_acquire, cs };

	return cv_sleep(cv, ms, &l);
}

WINFN(void) k32_WakeConditionVariable(PCONDITION_VARIABLE cv)
{
	uint32_t *w = word(&cv->Ptr);

	wait_lock();
	if (CV_SIGNALS(*w) < CV_WAITING(*w)) {
		*w += 0x10000u;
		wait_broadcast();
	}
	wait_unlock();
}

WINFN(void) k32_WakeAllConditionVariable(PCONDITION_VARIABLE cv)
{
	uint32_t *w = word(&cv->Ptr);

	wait_lock();
	if (CV_SIGNALS(*w) < CV_WAITING(*w)) {
		*w = (CV_WAITING(*w) << 16) | CV_WAITING(*w);
		wait_broadcast();
	}
	wait_unlock();
}

/*
 * One-time initialisation: 0 is not started, 1 is running, and a finished
 * one holds its context (whose low two bits are free) with bit 1 set.
 */

#define ONCE_RUNNING 1u
#define ONCE_DONE    2u

WINFN(void) k32_InitOnceInitialize(PINIT_ONCE once)
{
	once->Ptr = NULL;
}

WINFN(BOOL) k32_InitOnceBeginInitialize(PINIT_ONCE once, DWORD flags,
					PBOOL pending, PVOID *context)
{
	uint32_t *w = word(&once->Ptr);
	BOOL ok = TRUE;

	if (flags & ~(DWORD)(INIT_ONCE_CHECK_ONLY | INIT_ONCE_ASYNC)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wait_lock();
	for (;;) {
		if ((*w & 3) == ONCE_DONE) {
			*pending = FALSE;
			if (context)
				*context = (PVOID)(uintptr_t)(*w & ~3u);
			break;
		}
		if (flags & INIT_ONCE_CHECK_ONLY) {
			*pending = TRUE;
			win_set_error(ERROR_GEN_FAILURE);
			ok = FALSE;
			break;
		}
		if (*w == 0 || (flags & INIT_ONCE_ASYNC)) {
			*w = ONCE_RUNNING;
			*pending = TRUE;
			break;
		}
		wait_block(NULL);
	}
	wait_unlock();
	return ok;
}

WINFN(BOOL) k32_InitOnceComplete(PINIT_ONCE once, DWORD flags, PVOID context)
{
	uint32_t *w = word(&once->Ptr);
	BOOL ok = TRUE;

	if ((flags & ~(DWORD)(INIT_ONCE_ASYNC | INIT_ONCE_INIT_FAILED)) ||
	    ((uintptr_t)context & 3)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wait_lock();
	if (*w != ONCE_RUNNING && !(flags & INIT_ONCE_ASYNC)) {
		win_set_error(ERROR_GEN_FAILURE);
		ok = FALSE;
	} else {
		*w = (flags & INIT_ONCE_INIT_FAILED) ? 0 :
		     ((uint32_t)(uintptr_t)context | ONCE_DONE);
		wait_broadcast();
	}
	wait_unlock();
	return ok;
}

WINFN(BOOL) k32_InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn,
				    PVOID param, PVOID *context)
{
	BOOL pending;
	PVOID ctx = NULL;

	if (!k32_InitOnceBeginInitialize(once, 0, &pending, &ctx))
		return FALSE;
	if (pending) {
		BOOL ok = fn(once, param, &ctx);

		if (!k32_InitOnceComplete(once, ok ? 0 : INIT_ONCE_INIT_FAILED,
					  ok ? ctx : NULL))
			return FALSE;
		if (!ok) {
			win_set_error(ERROR_GEN_FAILURE);
			return FALSE;
		}
	}
	if (context)
		*context = ctx;
	return TRUE;
}

/*
 * Interlocked singly linked lists. The header is the first entry's address,
 * the depth and a sequence number that changes with each pop; one lock
 * guards every list.
 */

static pthread_mutex_t slist_lock = PTHREAD_MUTEX_INITIALIZER;

WINFN(void) k32_InitializeSListHead(PSLIST_HEADER h)
{
	h->Alignment = 0;
}

WINFN(PSLIST_ENTRY) k32_InterlockedPushListSList(PSLIST_HEADER h,
						 PSLIST_ENTRY first,
						 PSLIST_ENTRY last, ULONG count)
{
	PSLIST_ENTRY old;

	pthread_mutex_lock(&slist_lock);
	old = h->Next.Next;
	last->Next = old;
	h->Next.Next = first;
	h->Depth += count;
	h->Sequence++;
	pthread_mutex_unlock(&slist_lock);
	return old;
}

WINFN(PSLIST_ENTRY) k32_InterlockedPushListSListEx(PSLIST_HEADER h,
						   PSLIST_ENTRY first,
						   PSLIST_ENTRY last, ULONG count)
{
	return k32_InterlockedPushListSList(h, first, last, count);
}

WINFN(PSLIST_ENTRY) k32_InterlockedPushEntrySList(PSLIST_HEADER h,
						  PSLIST_ENTRY e)
{
	return k32_InterlockedPushListSList(h, e, e, 1);
}

WINFN(PSLIST_ENTRY) k32_InterlockedPopEntrySList(PSLIST_HEADER h)
{
	PSLIST_ENTRY e;

	pthread_mutex_lock(&slist_lock);
	e = h->Next.Next;
	if (e) {
		h->Next.Next = e->Next;
		h->Depth--;
		h->Sequence++;
	}
	pthread_mutex_unlock(&slist_lock);
	return e;
}

WINFN(PSLIST_ENTRY) k32_InterlockedFlushSList(PSLIST_HEADER h)
{
	PSLIST_ENTRY e;

	pthread_mutex_lock(&slist_lock);
	e = h->Next.Next;
	h->Next.Next = NULL;
	h->Depth = 0;
	h->Sequence++;
	pthread_mutex_unlock(&slist_lock);
	return e;
}

WINFN(WORD) k32_QueryDepthSList(PSLIST_HEADER h)
{
	return h->Depth;
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_modern_exports[] = {
	E(AcquireSRWLockExclusive),
	E(AcquireSRWLockShared),
	E(InitOnceBeginInitialize),
	E(InitOnceComplete),
	E(InitOnceExecuteOnce),
	E(InitOnceInitialize),
	E(InitializeConditionVariable),
	E(InitializeSListHead),
	E(InitializeSRWLock),
	E(InterlockedFlushSList),
	E(InterlockedPopEntrySList),
	E(InterlockedPushEntrySList),
	E(InterlockedPushListSList),
	E(InterlockedPushListSListEx),
	E(QueryDepthSList),
	E(ReleaseSRWLockExclusive),
	E(ReleaseSRWLockShared),
	E(SleepConditionVariableCS),
	E(SleepConditionVariableSRW),
	E(TryAcquireSRWLockExclusive),
	E(TryAcquireSRWLockShared),
	E(WakeAllConditionVariable),
	E(WakeConditionVariable),
	BUILTIN_END
};
