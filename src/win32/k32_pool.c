/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: thread-pool objects: timers, waits and work items, the way
 * Windows' pool presents them to code that doesn't create threads itself.
 *
 * There is no pool of workers. Each armed timer or wait gets a thread that
 * sleeps until it is due or its handle is signalled, then runs the callback;
 * each submitted work item gets a thread that runs it. They are ordinary
 * guest threads (with a TEB), so callbacks can call anything a thread can.
 * State is kept under the wait lock, whose condition every change
 * broadcasts, and re-arming or closing an object cancels what it had armed
 * by bumping a generation the sleeping thread checks.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "win32/handle.h"
#include "win32/internal.h"
#include "win32/k32.h"

enum tp_kind { TP_TIMER = 1, TP_WAIT, TP_WORK };

#define TP_WAIT_OBJECT_0 0
#define TP_WAIT_TIMEOUT  0x102
#define TP_MAGIC_OBJECT  0x54504f42u	/* "TPOB" */
#define TP_MAGIC_INST    0x54504953u	/* "TPIS" */

struct tp_object {
	DWORD magic;
	enum tp_kind kind;
	PVOID callback, context;
	int refs;	/* the creator's, and one per thread working for it */
	int running;	/* callbacks executing now */
	int pending;	/* work submitted and not yet finished */
	int armed;	/* a timer or wait is set and hasn't fired */
	unsigned gen;	/* changes to cancel what is armed */
	int closed;
};

/* What a callback may ask to have done as it returns. */
struct tp_instance {
	DWORD magic;
	HANDLE event;
	LPCRITICAL_SECTION cs;
};

/* An armed timer's or wait's thread: its own copy of what it waits for. */
struct tp_arm {
	struct tp_object *o;
	unsigned gen;
	struct timespec due;		/* when to fire, or the wait's timeout */
	int has_due;
	DWORD period;			/* ms, for a repeating timer */
	struct kobject *target;		/* what a wait waits on */
};

typedef void (WINAPI *tp_simple_cb)(PVOID inst, PVOID ctx);
typedef void (WINAPI *tp_object_cb)(PVOID inst, PVOID ctx, PVOID object);
typedef void (WINAPI *tp_wait_cb)(PVOID inst, PVOID ctx, PVOID object, ULONG result);

static int ts_before(const struct timespec *a, const struct timespec *b)
{
	return a->tv_sec < b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static void ts_add_ns(struct timespec *t, long long ns)
{
	long long total = (long long)t->tv_nsec + ns % 1000000000LL;

	t->tv_sec += ns / 1000000000LL;
	if (total >= 1000000000LL) {
		t->tv_sec++;
		total -= 1000000000LL;
	} else if (total < 0) {
		t->tv_sec--;
		total += 1000000000LL;
	}
	t->tv_nsec = (long)total;
}

/*
 * A FILETIME timeout as a monotonic deadline: negative is relative to now,
 * positive absolute (100 ns since 1601). NULL means never; the result is
 * 1 if there is a deadline.
 */
static int ft_deadline(const FILETIME *ft, struct timespec *out)
{
	long long v;

	if (!ft)
		return 0;
	v = (long long)(((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime);
	clock_gettime(CLOCK_MONOTONIC, out);
	if (v < 0) {
		ts_add_ns(out, (-v) * 100);
	} else {
		struct timespec real;
		long long unix_ns = (v - 116444736000000000LL) * 100;

		clock_gettime(CLOCK_REALTIME, &real);
		unix_ns -= (long long)real.tv_sec * 1000000000LL + real.tv_nsec;
		if (unix_ns > 0)
			ts_add_ns(out, unix_ns);
	}
	return 1;
}

static struct tp_object *tp_new(enum tp_kind kind, PVOID cb, PVOID ctx)
{
	struct tp_object *o = calloc(1, sizeof(*o));

	if (!o) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	o->magic = TP_MAGIC_OBJECT;
	o->kind = kind;
	o->callback = cb;
	o->context = ctx;
	o->refs = 1;
	return o;
}

/* The object, or NULL (and ERROR_INVALID_HANDLE) if p isn't a live one. */
static struct tp_object *tp_of(PVOID p, enum tp_kind kind)
{
	struct tp_object *o = p;

	if (!o || o->magic != TP_MAGIC_OBJECT || o->kind != kind) {
		win_set_error(ERROR_INVALID_HANDLE);
		return NULL;
	}
	return o;
}

/* Drops one reference, with the wait lock held; frees at the last. */
static void tp_unref_locked(struct tp_object *o)
{
	if (--o->refs)
		return;
	o->magic = 0;
	free(o);
}

/* Runs a callback with the lock dropped, then what it asked for as it returned. */
static void tp_run(struct tp_object *o, struct tp_arm *arm, ULONG result)
{
	struct tp_instance inst = { TP_MAGIC_INST, NULL, NULL };

	o->running++;
	wait_unlock();
	switch (o->kind) {
	case TP_TIMER:
	case TP_WORK:
		((tp_object_cb)o->callback)(&inst, o->context, o);
		break;
	case TP_WAIT:
		((tp_wait_cb)o->callback)(&inst, o->context, o, result);
		break;
	}
	if (inst.cs)
		k32_cs_leave(inst.cs);
	if (inst.event)
		event_signal(inst.event, 0);
	wait_lock();
	o->running--;
	wait_broadcast();
}

static DWORD STUB_WINAPI tp_timer_thread(LPVOID param)
{
	struct tp_arm *a = param;
	struct tp_object *o = a->o;
	struct timespec now;

	wait_lock();
	while (o->gen == a->gen && !o->closed) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (ts_before(&now, &a->due)) {
			wait_block(&a->due);
			continue;
		}
		if (!a->period)
			o->armed = 0;
		tp_run(o, a, 0);
		if (!a->period || o->gen != a->gen)
			break;
		/* Next period from when this one was due; from now if behind. */
		ts_add_ns(&a->due, (long long)a->period * 1000000);
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (ts_before(&a->due, &now))
			a->due = now;
	}
	tp_unref_locked(o);
	wait_broadcast();
	wait_unlock();
	free(a);
	return 0;
}

static DWORD STUB_WINAPI tp_wait_thread(LPVOID param)
{
	struct tp_arm *a = param;
	struct tp_object *o = a->o;
	DWORD tid = thread_current_id();
	struct timespec now;

	wait_lock();
	while (o->gen == a->gen && !o->closed) {
		if (a->target->signaled(a->target, tid)) {
			if (a->target->acquire)
				a->target->acquire(a->target, tid);
			o->armed = 0;
			tp_run(o, a, TP_WAIT_OBJECT_0);
			break;
		}
		if (a->has_due) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (!ts_before(&now, &a->due)) {
				o->armed = 0;
				tp_run(o, a, TP_WAIT_TIMEOUT);
				break;
			}
			wait_block(&a->due);
		} else {
			wait_block(NULL);
		}
	}
	tp_unref_locked(o);
	wait_broadcast();
	wait_unlock();
	kobj_unref(a->target);
	free(a);
	return 0;
}

static DWORD STUB_WINAPI tp_work_thread(LPVOID param)
{
	struct tp_arm *a = param;
	struct tp_object *o = a->o;

	wait_lock();
	tp_run(o, a, 0);
	o->pending--;
	tp_unref_locked(o);
	wait_broadcast();
	wait_unlock();
	free(a);
	return 0;
}

/* Starts a thread for o; the thread's reference to o is taken by the caller. */
static BOOL tp_spawn(LPTHREAD_START_ROUTINE fn, struct tp_arm *a)
{
	HANDLE h = thread_create(fn, a);

	if (!h) {
		wait_lock();
		tp_unref_locked(a->o);
		wait_unlock();
		if (a->target)
			kobj_unref(a->target);
		free(a);
		return FALSE;
	}
	handle_close(h);
	return TRUE;
}

static struct tp_arm *arm_new(struct tp_object *o)
{
	struct tp_arm *a = calloc(1, sizeof(*a));

	if (a) {
		a->o = o;
		a->gen = o->gen;
	}
	return a;
}

/* Timers. */

WINFN(PVOID) k32_CreateThreadpoolTimer(PVOID cb, PVOID ctx, PVOID env)
{
	return tp_new(TP_TIMER, cb, ctx);
}

WINFN(void) k32_SetThreadpoolTimer(PVOID timer, PFILETIME due, DWORD period,
				   DWORD window)
{
	struct tp_object *o = tp_of(timer, TP_TIMER);
	struct tp_arm *a = NULL;
	struct timespec when;

	if (!o)
		return;
	wait_lock();
	o->gen++;		/* whatever was armed is cancelled */
	o->armed = 0;
	wait_broadcast();
	if (due && !o->closed && (a = arm_new(o))) {
		ft_deadline(due, &when);
		a->due = when;
		a->has_due = 1;
		a->period = period;
		o->armed = 1;
		o->refs++;
	}
	wait_unlock();
	if (a)
		tp_spawn(tp_timer_thread, a);
}

WINFN(BOOL) k32_IsThreadpoolTimerSet(PVOID timer)
{
	struct tp_object *o = tp_of(timer, TP_TIMER);
	BOOL set;

	if (!o)
		return FALSE;
	wait_lock();
	set = o->armed;
	wait_unlock();
	return set;
}

/* Waits for the callbacks running now; with cancel, drops what isn't yet due. */
static void tp_wait_callbacks(struct tp_object *o, BOOL cancel)
{
	wait_lock();
	if (cancel) {
		o->gen++;
		o->armed = 0;
		wait_broadcast();
	}
	while (o->running || o->pending)
		wait_block(NULL);
	wait_unlock();
}

WINFN(void) k32_WaitForThreadpoolTimerCallbacks(PVOID timer, BOOL cancel)
{
	struct tp_object *o = tp_of(timer, TP_TIMER);

	if (o)
		tp_wait_callbacks(o, cancel);
}

/* Closing disarms, waits for running callbacks, and frees once unused. */
static void tp_close(struct tp_object *o)
{
	wait_lock();
	o->closed = 1;
	o->gen++;
	o->armed = 0;
	wait_broadcast();
	while (o->running || o->pending)
		wait_block(NULL);
	tp_unref_locked(o);
	wait_unlock();
}

WINFN(void) k32_CloseThreadpoolTimer(PVOID timer)
{
	struct tp_object *o = tp_of(timer, TP_TIMER);

	if (o)
		tp_close(o);
}

/* Waits. */

WINFN(PVOID) k32_CreateThreadpoolWait(PVOID cb, PVOID ctx, PVOID env)
{
	return tp_new(TP_WAIT, cb, ctx);
}

WINFN(void) k32_SetThreadpoolWait(PVOID wait, HANDLE h, PFILETIME timeout)
{
	struct tp_object *o = tp_of(wait, TP_WAIT);
	struct kobject *target = NULL;
	struct tp_arm *a = NULL;

	if (!o)
		return;
	if (h && !(target = handle_ref(h, KOBJ_ANY)))
		return;
	wait_lock();
	o->gen++;
	o->armed = 0;
	wait_broadcast();
	if (target && !o->closed && target->signaled && (a = arm_new(o))) {
		a->target = target;
		a->has_due = ft_deadline(timeout, &a->due);
		o->armed = 1;
		o->refs++;
		target = NULL;
	}
	wait_unlock();
	if (target)
		kobj_unref(target);
	if (a)
		tp_spawn(tp_wait_thread, a);
}

WINFN(void) k32_WaitForThreadpoolWaitCallbacks(PVOID wait, BOOL cancel)
{
	struct tp_object *o = tp_of(wait, TP_WAIT);

	if (o)
		tp_wait_callbacks(o, cancel);
}

WINFN(void) k32_CloseThreadpoolWait(PVOID wait)
{
	struct tp_object *o = tp_of(wait, TP_WAIT);

	if (o)
		tp_close(o);
}

/* Work. */

WINFN(PVOID) k32_CreateThreadpoolWork(PVOID cb, PVOID ctx, PVOID env)
{
	return tp_new(TP_WORK, cb, ctx);
}

WINFN(void) k32_SubmitThreadpoolWork(PVOID work)
{
	struct tp_object *o = tp_of(work, TP_WORK);
	struct tp_arm *a;

	if (!o)
		return;
	wait_lock();
	if (o->closed || !(a = arm_new(o))) {
		wait_unlock();
		return;
	}
	o->refs++;
	o->pending++;
	wait_unlock();
	if (!tp_spawn(tp_work_thread, a)) {
		wait_lock();
		o->pending--;
		wait_broadcast();
		wait_unlock();
	}
}

WINFN(void) k32_WaitForThreadpoolWorkCallbacks(PVOID work, BOOL cancel)
{
	struct tp_object *o = tp_of(work, TP_WORK);

	if (o)
		tp_wait_callbacks(o, FALSE);
}

WINFN(void) k32_CloseThreadpoolWork(PVOID work)
{
	struct tp_object *o = tp_of(work, TP_WORK);

	if (o)
		tp_close(o);
}

/* A one-off callback: a work item that frees itself afterwards. */

struct tp_simple {
	tp_simple_cb cb;
	PVOID ctx;
};

static DWORD STUB_WINAPI tp_simple_thread(LPVOID param)
{
	struct tp_simple *s = param;
	struct tp_instance inst = { TP_MAGIC_INST, NULL, NULL };

	s->cb(&inst, s->ctx);
	if (inst.cs)
		k32_cs_leave(inst.cs);
	if (inst.event)
		event_signal(inst.event, 0);
	free(s);
	return 0;
}

WINFN(BOOL) k32_TrySubmitThreadpoolCallback(PVOID cb, PVOID ctx, PVOID env)
{
	struct tp_simple *s = malloc(sizeof(*s));
	HANDLE h;

	if (!s || !cb) {
		free(s);
		win_set_error(cb ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	s->cb = cb;
	s->ctx = ctx;
	h = thread_create(tp_simple_thread, s);
	if (!h) {
		free(s);
		return FALSE;
	}
	handle_close(h);
	return TRUE;
}

/* What a callback asks for as it returns. */

static struct tp_instance *instance_of(PVOID p)
{
	struct tp_instance *i = p;

	return i && i->magic == TP_MAGIC_INST ? i : NULL;
}

WINFN(void) k32_SetEventWhenCallbackReturns(PVOID instance, HANDLE event)
{
	struct tp_instance *i = instance_of(instance);

	if (i)
		i->event = event;
}

WINFN(void) k32_LeaveCriticalSectionWhenCallbackReturns(PVOID instance,
							LPCRITICAL_SECTION cs)
{
	struct tp_instance *i = instance_of(instance);

	if (i)
		i->cs = cs;
}

/* A module the callback is in is left loaded, which is always safe. */
WINFN(void) k32_FreeLibraryWhenCallbackReturns(PVOID instance, HMODULE m) { }
WINFN(void) k32_DisassociateCurrentThreadFromCallback(PVOID instance) { }
WINFN(BOOL) k32_CallbackMayRunLong(PVOID instance) { return TRUE; }

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_pool_exports[] = {
	E(CallbackMayRunLong),
	E(CloseThreadpoolTimer),
	E(CloseThreadpoolWait),
	E(CloseThreadpoolWork),
	E(CreateThreadpoolTimer),
	E(CreateThreadpoolWait),
	E(CreateThreadpoolWork),
	E(DisassociateCurrentThreadFromCallback),
	E(FreeLibraryWhenCallbackReturns),
	E(IsThreadpoolTimerSet),
	E(LeaveCriticalSectionWhenCallbackReturns),
	E(SetEventWhenCallbackReturns),
	E(SetThreadpoolTimer),
	E(SetThreadpoolWait),
	E(SubmitThreadpoolWork),
	E(TrySubmitThreadpoolCallback),
	E(WaitForThreadpoolTimerCallbacks),
	E(WaitForThreadpoolWaitCallbacks),
	E(WaitForThreadpoolWorkCallbacks),
	BUILTIN_END
};
