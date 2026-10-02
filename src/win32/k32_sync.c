/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: critical sections, interlocked operations, events, mutexes,
 * semaphores and waiting.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "win32/codepage.h"
#include "win32/handle.h"
#include "win32/internal.h"

static DWORD current_tid(void)
{
	struct win_teb *teb = teb_init_thread();
	return teb ? teb->UniqueThread : 0;
}

/* Critical sections: the Windows struct points (via LockSemaphore) at a
 * recursive pthread mutex; OwningThread and RecursionCount are kept up to
 * date because some code inspects them. The mutexes stay on a list so they
 * remain reachable (and LeakSanitizer quiet) while the DLL holds them. */

struct cs_impl {
	pthread_mutex_t mutex;
	struct cs_impl *prev, *next;
};

static struct cs_impl *cs_list;
static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;

static struct cs_impl *cs_create(void)
{
	struct cs_impl *c = calloc(1, sizeof(*c));
	pthread_mutexattr_t attr;

	if (!c)
		return NULL;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&c->mutex, &attr);
	pthread_mutexattr_destroy(&attr);
	c->next = cs_list;
	if (cs_list)
		cs_list->prev = c;
	cs_list = c;
	return c;
}

/* The mutex behind cs, created on first use for a zero-filled one. */
static struct cs_impl *cs_get(CRITICAL_SECTION *cs)
{
	struct cs_impl *c = __atomic_load_n((struct cs_impl **)&cs->LockSemaphore,
					    __ATOMIC_ACQUIRE);
	if (c)
		return c;
	pthread_mutex_lock(&cs_lock);
	c = cs->LockSemaphore;
	if (!c) {
		c = cs_create();
		__atomic_store_n((struct cs_impl **)&cs->LockSemaphore, c,
				 __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&cs_lock);
	return c;
}

static void cs_init(CRITICAL_SECTION *cs, DWORD spin)
{
	memset(cs, 0, sizeof(*cs));
	cs->DebugInfo = (PVOID)(LONG_PTR)-1;
	cs->LockCount = -1;
	cs->SpinCount = spin;
	pthread_mutex_lock(&cs_lock);
	cs->LockSemaphore = cs_create();
	pthread_mutex_unlock(&cs_lock);
}

WINFN(void) k32_InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
	cs_init(cs, 0);
}

WINFN(BOOL) k32_InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs,
						      DWORD spin)
{
	cs_init(cs, spin);
	return TRUE;
}

WINFN(BOOL) k32_InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin,
					    DWORD flags)
{
	cs_init(cs, spin);
	return TRUE;
}

WINFN(DWORD) k32_SetCriticalSectionSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
	DWORD old = cs->SpinCount;
	cs->SpinCount = spin;
	return old;
}

static void cs_owned(CRITICAL_SECTION *cs)
{
	cs->OwningThread = (HANDLE)(uintptr_t)current_tid();
	cs->RecursionCount++;
	__atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
}

WINFN(void) k32_EnterCriticalSection(LPCRITICAL_SECTION cs)
{
	struct cs_impl *c = cs_get(cs);
	if (!c)
		return;
	pthread_mutex_lock(&c->mutex);
	cs_owned(cs);
}

WINFN(BOOL) k32_TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
	struct cs_impl *c = cs_get(cs);
	if (!c || pthread_mutex_trylock(&c->mutex))
		return FALSE;
	cs_owned(cs);
	return TRUE;
}

WINFN(void) k32_LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
	struct cs_impl *c = cs->LockSemaphore;
	if (!c || cs->RecursionCount <= 0)
		return;
	if (--cs->RecursionCount == 0)
		cs->OwningThread = NULL;
	__atomic_sub_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
	pthread_mutex_unlock(&c->mutex);
}

WINFN(void) k32_DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
	struct cs_impl *c = cs->LockSemaphore;
	if (!c)
		return;
	pthread_mutex_lock(&cs_lock);
	if (c->prev)
		c->prev->next = c->next;
	else
		cs_list = c->next;
	if (c->next)
		c->next->prev = c->prev;
	pthread_mutex_unlock(&cs_lock);
	pthread_mutex_destroy(&c->mutex);
	free(c);
	cs->LockSemaphore = NULL;
	cs->LockCount = -1;
	cs->RecursionCount = 0;
	cs->OwningThread = NULL;
}

/* Interlocked operations. */

WINFN(LONG) k32_InterlockedIncrement(LONG volatile *p)
{
	return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}

WINFN(LONG) k32_InterlockedDecrement(LONG volatile *p)
{
	return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
}

WINFN(LONG) k32_InterlockedExchange(LONG volatile *p, LONG v)
{
	return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

WINFN(LONG) k32_InterlockedExchangeAdd(LONG volatile *p, LONG v)
{
	return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

WINFN(LONG) k32_InterlockedCompareExchange(LONG volatile *p, LONG exchange,
					   LONG comparand)
{
	__atomic_compare_exchange_n(p, &comparand, exchange, 0,
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return comparand;
}

WINFN(LONGLONG) k32_InterlockedCompareExchange64(LONGLONG volatile *p,
						 LONGLONG exchange,
						 LONGLONG comparand)
{
	__atomic_compare_exchange_n(p, &comparand, exchange, 0,
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return comparand;
}

/* Waitable objects. */

struct event {
	struct kobject obj;
	int manual;
	int state;
};

struct mutex {
	struct kobject obj;
	DWORD owner;
	int count;
};

struct semaphore {
	struct kobject obj;
	LONG count, max;
};

static int event_signaled(struct kobject *o, DWORD tid)
{
	return ((struct event *)o)->state;
}

static void event_acquire(struct kobject *o, DWORD tid)
{
	struct event *e = (struct event *)o;
	if (!e->manual)
		e->state = 0;
}

static int mutex_signaled(struct kobject *o, DWORD tid)
{
	struct mutex *m = (struct mutex *)o;
	return !m->owner || m->owner == tid;
}

static void mutex_acquire(struct kobject *o, DWORD tid)
{
	struct mutex *m = (struct mutex *)o;
	m->owner = tid;
	m->count++;
}

static int semaphore_signaled(struct kobject *o, DWORD tid)
{
	return ((struct semaphore *)o)->count > 0;
}

static void semaphore_acquire(struct kobject *o, DWORD tid)
{
	((struct semaphore *)o)->count--;
}

/*
 * The common part of CreateEvent/CreateMutex/CreateSemaphore: an existing
 * object of that name is opened instead (ERROR_ALREADY_EXISTS).
 * Returns the handle, with *created set if obj was used.
 */
static HANDLE create_named(struct kobject *obj, const char *name,
			   int *created)
{
	*created = 0;
	if (name && *name) {
		struct kobject *existing = kobj_find_named(name);
		if (existing) {
			int same = existing->type == obj->type;
			kobj_unref(obj);
			if (!same) {
				kobj_unref(existing);
				win_set_error(ERROR_INVALID_HANDLE);
				return NULL;
			}
			HANDLE h = handle_new(existing);
			if (h)
				win_set_error(ERROR_ALREADY_EXISTS);
			return h;
		}
		kobj_set_name(obj, name);
	}
	*created = 1;
	HANDLE h = handle_new(obj);
	if (h)
		win_set_error(ERROR_SUCCESS);
	return h;
}

static HANDLE open_named(const char *name, enum kobj_type type)
{
	struct kobject *obj = name ? kobj_find_named(name) : NULL;

	if (!obj || obj->type != type) {
		kobj_unref(obj);
		win_set_error(ERROR_FILE_NOT_FOUND);
		return NULL;
	}
	return handle_new(obj);
}

static HANDLE event_create(BOOL manual, BOOL initial, const char *name)
{
	struct event *e = calloc(1, sizeof(*e));
	int created;

	if (!e) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	kobj_init(&e->obj, KOBJ_EVENT, NULL);
	e->obj.signaled = event_signaled;
	e->obj.acquire = event_acquire;
	e->manual = manual;
	e->state = initial;
	return create_named(&e->obj, name, &created);
}

WINFN(HANDLE) k32_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual,
			       BOOL initial, LPCSTR name)
{
	char *u = acp_to_utf8(name);
	HANDLE h = event_create(manual, initial, u);
	free(u);
	return h;
}

WINFN(HANDLE) k32_CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual,
			       BOOL initial, LPCWSTR name)
{
	char *u = utf16_to_utf8_dup(name);
	HANDLE h = event_create(manual, initial, u);
	free(u);
	return h;
}

WINFN(HANDLE) k32_OpenEventA(DWORD access, BOOL inherit, LPCSTR name)
{
	char *u = acp_to_utf8(name);
	HANDLE h = open_named(u, KOBJ_EVENT);
	free(u);
	return h;
}

WINFN(HANDLE) k32_OpenEventW(DWORD access, BOOL inherit, LPCWSTR name)
{
	char *u = utf16_to_utf8_dup(name);
	HANDLE h = open_named(u, KOBJ_EVENT);
	free(u);
	return h;
}

static BOOL event_set(HANDLE h, int state, int pulse)
{
	struct event *e = (struct event *)handle_ref(h, KOBJ_EVENT);

	if (!e)
		return FALSE;
	wait_lock();
	e->state = state;
	wait_broadcast();
	/*
	 * PulseEvent is approximate: for a manual-reset event, threads that
	 * don't get to run before the reset below miss the pulse.
	 */
	if (pulse && e->manual)
		e->state = 0;
	wait_unlock();
	kobj_unref(&e->obj);
	return TRUE;
}

WINFN(BOOL) k32_SetEvent(HANDLE h) { return event_set(h, 1, 0); }
WINFN(BOOL) k32_ResetEvent(HANDLE h) { return event_set(h, 0, 0); }
WINFN(BOOL) k32_PulseEvent(HANDLE h) { return event_set(h, 1, 1); }

static HANDLE mutex_create(BOOL owned, const char *name)
{
	struct mutex *m = calloc(1, sizeof(*m));
	int created;

	if (!m) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	kobj_init(&m->obj, KOBJ_MUTEX, NULL);
	m->obj.signaled = mutex_signaled;
	m->obj.acquire = mutex_acquire;
	HANDLE h = create_named(&m->obj, name, &created);
	if (h && created && owned) {
		wait_lock();
		mutex_acquire(&m->obj, current_tid());
		wait_unlock();
	}
	return h;
}

WINFN(HANDLE) k32_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCSTR name)
{
	char *u = acp_to_utf8(name);
	HANDLE h = mutex_create(owned, u);
	free(u);
	return h;
}

WINFN(HANDLE) k32_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCWSTR name)
{
	char *u = utf16_to_utf8_dup(name);
	HANDLE h = mutex_create(owned, u);
	free(u);
	return h;
}

WINFN(HANDLE) k32_OpenMutexA(DWORD access, BOOL inherit, LPCSTR name)
{
	char *u = acp_to_utf8(name);
	HANDLE h = open_named(u, KOBJ_MUTEX);
	free(u);
	return h;
}

WINFN(BOOL) k32_ReleaseMutex(HANDLE h)
{
	struct mutex *m = (struct mutex *)handle_ref(h, KOBJ_MUTEX);
	BOOL ok = FALSE;

	if (!m)
		return FALSE;
	wait_lock();
	if (m->owner == current_tid() && m->count > 0) {
		if (--m->count == 0) {
			m->owner = 0;
			wait_broadcast();
		}
		ok = TRUE;
	}
	wait_unlock();
	kobj_unref(&m->obj);
	if (!ok)
		win_set_error(ERROR_NOT_OWNER);
	return ok;
}

static HANDLE semaphore_create(LONG initial, LONG max, const char *name)
{
	struct semaphore *s;
	int created;

	if (max <= 0 || initial < 0 || initial > max) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	if (!(s = calloc(1, sizeof(*s)))) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	kobj_init(&s->obj, KOBJ_SEMAPHORE, NULL);
	s->obj.signaled = semaphore_signaled;
	s->obj.acquire = semaphore_acquire;
	s->count = initial;
	s->max = max;
	return create_named(&s->obj, name, &created);
}

WINFN(HANDLE) k32_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial,
				   LONG max, LPCSTR name)
{
	char *u = acp_to_utf8(name);
	HANDLE h = semaphore_create(initial, max, u);
	free(u);
	return h;
}

WINFN(HANDLE) k32_CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial,
				   LONG max, LPCWSTR name)
{
	char *u = utf16_to_utf8_dup(name);
	HANDLE h = semaphore_create(initial, max, u);
	free(u);
	return h;
}

WINFN(HANDLE) k32_OpenSemaphoreA(DWORD access, BOOL inherit, LPCSTR name)
{
	char *u = acp_to_utf8(name);
	HANDLE h = open_named(u, KOBJ_SEMAPHORE);
	free(u);
	return h;
}

WINFN(BOOL) k32_ReleaseSemaphore(HANDLE h, LONG n, LPLONG previous)
{
	struct semaphore *s = (struct semaphore *)handle_ref(h, KOBJ_SEMAPHORE);
	BOOL ok = FALSE;

	if (!s)
		return FALSE;
	wait_lock();
	if (n > 0 && s->count <= s->max - n) {
		if (previous)
			*previous = s->count;
		s->count += n;
		wait_broadcast();
		ok = TRUE;
	}
	wait_unlock();
	kobj_unref(&s->obj);
	if (!ok)
		win_set_error(n > 0 ? ERROR_TOO_MANY_POSTS : ERROR_INVALID_PARAMETER);
	return ok;
}

/* Waiting. */

WINFN(DWORD) k32_WaitForMultipleObjectsEx(DWORD n, const HANDLE *handles,
					  BOOL all, DWORD ms, BOOL alertable)
{
	struct kobject *objs[MAXIMUM_WAIT_OBJECTS];
	DWORD r;

	if (!n || n > MAXIMUM_WAIT_OBJECTS) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return WAIT_FAILED;
	}
	current_tid();
	for (DWORD i = 0; i < n; i++) {
		if (!(objs[i] = handle_ref(handles[i], KOBJ_ANY))) {
			while (i--)
				kobj_unref(objs[i]);
			return WAIT_FAILED;
		}
	}
	r = wait_objects(n, objs, all, ms);
	for (DWORD i = 0; i < n; i++)
		kobj_unref(objs[i]);
	return r;
}

WINFN(DWORD) k32_WaitForMultipleObjects(DWORD n, const HANDLE *handles,
					BOOL all, DWORD ms)
{
	return k32_WaitForMultipleObjectsEx(n, handles, all, ms, FALSE);
}

WINFN(DWORD) k32_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
	return k32_WaitForMultipleObjectsEx(1, &h, FALSE, ms, alertable);
}

WINFN(DWORD) k32_WaitForSingleObject(HANDLE h, DWORD ms)
{
	return k32_WaitForMultipleObjectsEx(1, &h, FALSE, ms, FALSE);
}

WINFN(DWORD) k32_SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD ms,
				     BOOL alertable)
{
	struct kobject *obj = handle_ref(signal, KOBJ_ANY);
	BOOL ok;

	if (!obj)
		return WAIT_FAILED;
	switch (obj->type) {
	case KOBJ_EVENT:     ok = k32_SetEvent(signal); break;
	case KOBJ_MUTEX:     ok = k32_ReleaseMutex(signal); break;
	case KOBJ_SEMAPHORE: ok = k32_ReleaseSemaphore(signal, 1, NULL); break;
	default:
		win_set_error(ERROR_INVALID_HANDLE);
		ok = FALSE;
	}
	kobj_unref(obj);
	return ok ? k32_WaitForSingleObject(wait, ms) : WAIT_FAILED;
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_sync_exports[] = {
	E(CreateEventA),
	E(CreateEventW),
	E(CreateMutexA),
	E(CreateMutexW),
	E(CreateSemaphoreA),
	E(CreateSemaphoreW),
	E(DeleteCriticalSection),
	E(EnterCriticalSection),
	E(InitializeCriticalSection),
	E(InitializeCriticalSectionAndSpinCount),
	E(InitializeCriticalSectionEx),
	E(InterlockedCompareExchange),
	E(InterlockedCompareExchange64),
	E(InterlockedDecrement),
	E(InterlockedExchange),
	E(InterlockedExchangeAdd),
	E(InterlockedIncrement),
	E(LeaveCriticalSection),
	E(OpenEventA),
	E(OpenEventW),
	E(OpenMutexA),
	E(OpenSemaphoreA),
	E(PulseEvent),
	E(ReleaseMutex),
	E(ReleaseSemaphore),
	E(ResetEvent),
	E(SetCriticalSectionSpinCount),
	E(SetEvent),
	E(SignalObjectAndWait),
	E(TryEnterCriticalSection),
	E(WaitForMultipleObjects),
	E(WaitForMultipleObjectsEx),
	E(WaitForSingleObject),
	E(WaitForSingleObjectEx),
	BUILTIN_END
};
