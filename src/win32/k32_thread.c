/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: threads, TLS and FLS slots, sleeping, process exit.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "win32/handle.h"
#include "win32/internal.h"
#include "win32/k32.h"

#define PSEUDO_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_THREAD  ((HANDLE)(LONG_PTR)-2)
#define DEFAULT_STACK  (1u << 20)	/* Windows' default reservation */
#define NORMAL_PRIORITY_CLASS 0x20

struct thread {
	struct kobject obj;
	DWORD tid;
	int done;
	DWORD exit_code;
	LPTHREAD_START_ROUTINE start;
	LPVOID param;
	int priority;

	/* CREATE_SUSPENDED: the thread waits here until ResumeThread. */
	pthread_mutex_t gate_lock;
	pthread_cond_t gate;
	int suspend_count;

	sigjmp_buf exit_jump;		/* ExitThread returns here */
	int can_jump;
};

/* The calling thread's object: one we created, or made on demand. */
static _Thread_local struct thread *self;

static int thread_signaled(struct kobject *o, DWORD tid)
{
	return ((struct thread *)o)->done;
}

static void thread_destroy(struct kobject *o)
{
	struct thread *t = (struct thread *)o;

	pthread_mutex_destroy(&t->gate_lock);
	pthread_cond_destroy(&t->gate);
	free(t);
}

static struct thread *thread_new(void)
{
	struct thread *t = calloc(1, sizeof(*t));

	if (!t)
		return NULL;
	kobj_init(&t->obj, KOBJ_THREAD, thread_destroy);
	t->obj.signaled = thread_signaled;
	pthread_mutex_init(&t->gate_lock, NULL);
	pthread_cond_init(&t->gate, NULL);
	return t;
}

/* The object for the calling thread, creating one for foreign threads. */
static struct thread *thread_self(void)
{
	if (!self) {
		struct win_teb *teb = teb_init_thread();
		struct thread *t = thread_new();
		if (!t)
			return NULL;
		t->tid = teb ? teb->UniqueThread : (DWORD)syscall(SYS_gettid);
		self = t;
	}
	return self;
}

HANDLE thread_handle_current(void)
{
	struct thread *t = thread_self();

	if (!t) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	kobj_ref(&t->obj);
	return handle_new(&t->obj);
}

/* FLS: TLS slots with a destructor, run when the thread exits. */
static PFLS_CALLBACK_FUNCTION fls_callbacks[TEB_TLS_MAX];
static pthread_mutex_t fls_lock = PTHREAD_MUTEX_INITIALIZER;

void thread_exit_cleanup(void)
{
	for (DWORD i = 0; i < TEB_TLS_MAX; i++) {
		pthread_mutex_lock(&fls_lock);
		PFLS_CALLBACK_FUNCTION cb = fls_callbacks[i];
		pthread_mutex_unlock(&fls_lock);
		if (!cb)
			continue;
		void *v = teb_tls_get(i);
		if (v) {
			teb_tls_set(i, NULL);
			cb(v);
		}
	}
}

/*
 * Runs the FLS callbacks, then, if the DLLs were told the thread started,
 * tells them it is ending, as Windows's thread shutdown does; then marks t
 * finished and wakes its waiters.
 */
static void thread_finish(struct thread *t, DWORD code, int attached)
{
	thread_exit_cleanup();
	if (attached)
		mod_thread_notify(DLL_THREAD_DETACH);
	wait_lock();
	t->exit_code = code;
	t->done = 1;
	wait_broadcast();
	wait_unlock();
}

static void *thread_main(void *arg)
{
	struct thread *t = arg;
	DWORD code;

	self = t;
	if (runtime_enter()) {
		thread_finish(t, (DWORD)-1, 0);
		kobj_unref(&t->obj);
		return NULL;
	}
	__atomic_store_n(&t->tid, teb_current()->UniqueThread, __ATOMIC_RELEASE);

	pthread_mutex_lock(&t->gate_lock);
	while (t->suspend_count > 0)
		pthread_cond_wait(&t->gate, &t->gate_lock);
	pthread_mutex_unlock(&t->gate_lock);

	TRACE("thread %u starting at %p(%p)", t->tid, (void *)t->start, t->param);
	if (!sigsetjmp(t->exit_jump, 0)) {
		t->can_jump = 1;
		mod_thread_notify(DLL_THREAD_ATTACH);
		code = t->start(t->param);
	} else {
		code = t->exit_code;	/* set by ExitThread */
	}
	t->can_jump = 0;

	thread_finish(t, code, 1);
	self = NULL;
	teb_exit_thread();
	kobj_unref(&t->obj);	/* the thread's own reference */
	return NULL;
}

WINFN(HANDLE) k32_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack,
			       LPTHREAD_START_ROUTINE start, LPVOID param,
			       DWORD flags, LPDWORD tid)
{
	struct thread *t = thread_new();
	pthread_attr_t attr;
	pthread_t pt;

	if (!t) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	t->start = start;
	t->param = param;
	t->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;

	kobj_ref(&t->obj);		/* for the handle */
	HANDLE h = handle_new(&t->obj);
	if (!h) {
		kobj_unref(&t->obj);
		return NULL;
	}

	if (stack < DEFAULT_STACK)
		stack = DEFAULT_STACK;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stack);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int err = pthread_create(&pt, &attr, thread_main, t);
	pthread_attr_destroy(&attr);
	if (err) {
		handle_close(h);
		kobj_unref(&t->obj);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}

	/* The id is only known once the thread runs; wait for it briefly. */
	if (tid) {
		if (t->suspend_count) {
			/* It is parked at the gate; its TEB is set up first. */
			while (!__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE))
				sched_yield();
		} else {
			while (!__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE) &&
			       !__atomic_load_n(&t->done, __ATOMIC_ACQUIRE))
				sched_yield();
		}
		*tid = t->tid;
	}
	TRACE("CreateThread(%p, %p) = %p", (void *)start, param, h);
	return h;
}

HANDLE thread_create(LPTHREAD_START_ROUTINE start, LPVOID param)
{
	return k32_CreateThread(NULL, 0, start, param, 0, NULL);
}

void thread_exit(DWORD code)
{
	struct thread *t = self;

	if (t && t->can_jump) {
		t->exit_code = code;
		siglongjmp(t->exit_jump, 1);
	}
	/* A thread we didn't create (e.g. the harness's own): just end it. */
	if (t)
		thread_finish(t, code, 0);
	teb_exit_thread();
	pthread_exit(NULL);
}

WINFN(void) k32_ExitThread(DWORD code)
{
	thread_exit(code);
}

static struct thread *thread_from(HANDLE h)
{
	if (h == PSEUDO_THREAD) {
		struct thread *t = thread_self();
		if (t)
			kobj_ref(&t->obj);
		return t;
	}
	return (struct thread *)handle_ref(h, KOBJ_THREAD);
}

WINFN(BOOL) k32_GetExitCodeThread(HANDLE h, LPDWORD code)
{
	struct thread *t = thread_from(h);

	if (!t)
		return FALSE;
	wait_lock();
	*code = t->done ? t->exit_code : STILL_ACTIVE;
	wait_unlock();
	kobj_unref(&t->obj);
	return TRUE;
}

WINFN(DWORD) k32_GetThreadId(HANDLE h)
{
	struct thread *t = thread_from(h);
	DWORD id;

	if (!t)
		return 0;
	id = t->tid;
	kobj_unref(&t->obj);
	return id;
}

WINFN(DWORD) k32_ResumeThread(HANDLE h)
{
	struct thread *t = thread_from(h);
	DWORD prev;

	if (!t)
		return (DWORD)-1;
	pthread_mutex_lock(&t->gate_lock);
	prev = t->suspend_count;
	if (t->suspend_count > 0 && --t->suspend_count == 0)
		pthread_cond_broadcast(&t->gate);
	pthread_mutex_unlock(&t->gate_lock);
	kobj_unref(&t->obj);
	return prev;
}

WINFN(DWORD) k32_SuspendThread(HANDLE h)
{
	struct thread *t = thread_from(h);
	DWORD prev = (DWORD)-1;

	if (!t)
		return prev;
	/* Only a thread still parked at its start gate can be suspended. */
	pthread_mutex_lock(&t->gate_lock);
	if (t->suspend_count > 0)
		prev = t->suspend_count++;
	pthread_mutex_unlock(&t->gate_lock);
	kobj_unref(&t->obj);
	if (prev == (DWORD)-1)
		win_set_error(ERROR_NOT_SUPPORTED);
	return prev;
}

WINFN(BOOL) k32_TerminateThread(HANDLE h, DWORD code)
{
	TRACE("TerminateThread(%p, %u) is not supported", h, code);
	win_set_error(ERROR_NOT_SUPPORTED);
	return FALSE;
}

WINFN(HANDLE) k32_GetCurrentThread(void) { return PSEUDO_THREAD; }
WINFN(HANDLE) k32_GetCurrentProcess(void) { return PSEUDO_PROCESS; }
WINFN(DWORD) k32_GetCurrentThreadId(void) { return thread_self() ? self->tid : 0; }
WINFN(DWORD) k32_GetCurrentProcessId(void) { return getpid(); }

DWORD thread_current_id(void)
{
	return k32_GetCurrentThreadId();
}

WINFN(int) k32_GetThreadPriority(HANDLE h)
{
	struct thread *t = thread_from(h);
	int p;

	if (!t)
		return THREAD_PRIORITY_ERROR_RETURN;
	p = t->priority;
	kobj_unref(&t->obj);
	return p;
}

/* Priorities are recorded but not applied. */
WINFN(BOOL) k32_SetThreadPriority(HANDLE h, int priority)
{
	struct thread *t = thread_from(h);

	if (!t)
		return FALSE;
	t->priority = priority;
	kobj_unref(&t->obj);
	return TRUE;
}

WINFN(DWORD_PTR) k32_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
	DWORD n = peb_get()->NumberOfProcessors;
	return n >= 32 ? 0xffffffff : (1u << n) - 1;
}

WINFN(DWORD) k32_SetThreadIdealProcessor(HANDLE h, DWORD cpu) { return 0; }

WINFN(BOOL) k32_GetProcessAffinityMask(HANDLE process, PDWORD_PTR proc_mask,
				       PDWORD_PTR sys_mask)
{
	DWORD n = peb_get()->NumberOfProcessors;
	DWORD_PTR mask = n >= 32 ? 0xffffffff : (1u << n) - 1;

	if (proc_mask)
		*proc_mask = mask;
	if (sys_mask)
		*sys_mask = mask;
	return TRUE;
}

WINFN(BOOL) k32_SetProcessAffinityMask(HANDLE process, DWORD_PTR mask) { return TRUE; }
WINFN(DWORD) k32_GetPriorityClass(HANDLE process) { return NORMAL_PRIORITY_CLASS; }
WINFN(BOOL) k32_SetPriorityClass(HANDLE process, DWORD cls) { return TRUE; }

/* Sleeping. */

WINFN(void) k32_Sleep(DWORD ms)
{
	if (!ms) {
		sched_yield();
		return;
	}
	if (ms == INFINITE) {
		for (;;)
			pause();
	}
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

WINFN(DWORD) k32_SleepEx(DWORD ms, BOOL alertable)
{
	k32_Sleep(ms);
	return 0;
}

WINFN(BOOL) k32_SwitchToThread(void)
{
	sched_yield();
	return TRUE;
}

/* TLS and FLS. */

WINFN(DWORD) k32_TlsAlloc(void)
{
	DWORD i = teb_tls_alloc();
	if (i == TLS_OUT_OF_INDEXES)
		win_set_error(ERROR_NO_MORE_ITEMS);
	return i;
}

WINFN(BOOL) k32_TlsFree(DWORD i)
{
	if (!teb_tls_free(i)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return TRUE;
}

/* Like Windows, a successful TlsGetValue clears the last error: callers
 * tell a stored NULL from a failure that way. */
WINFN(LPVOID) k32_TlsGetValue(DWORD i)
{
	if (!teb_tls_valid(i)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	win_set_error(ERROR_SUCCESS);
	return teb_tls_get(i);
}

WINFN(BOOL) k32_TlsSetValue(DWORD i, LPVOID v)
{
	if (!teb_tls_valid(i) || !teb_tls_set(i, v)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return TRUE;
}

WINFN(DWORD) k32_FlsAlloc(PFLS_CALLBACK_FUNCTION cb)
{
	DWORD i = teb_tls_alloc();

	if (i == FLS_OUT_OF_INDEXES) {
		win_set_error(ERROR_NO_MORE_ITEMS);
		return i;
	}
	pthread_mutex_lock(&fls_lock);
	fls_callbacks[i] = cb;
	pthread_mutex_unlock(&fls_lock);
	return i;
}

WINFN(BOOL) k32_FlsFree(DWORD i)
{
	if (!teb_tls_valid(i)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	pthread_mutex_lock(&fls_lock);
	PFLS_CALLBACK_FUNCTION cb = fls_callbacks[i];
	fls_callbacks[i] = NULL;
	pthread_mutex_unlock(&fls_lock);
	void *v = teb_tls_get(i);
	if (cb && v)
		cb(v);
	return teb_tls_free(i);
}

WINFN(PVOID) k32_FlsGetValue(DWORD i) { return k32_TlsGetValue(i); }
WINFN(BOOL) k32_FlsSetValue(DWORD i, PVOID v) { return k32_TlsSetValue(i, v); }

/* Process exit. */

WINFN(void) k32_ExitProcess(UINT code)
{
	TRACE("ExitProcess(%u)", code);
	exit(code);
}

WINFN(BOOL) k32_TerminateProcess(HANDLE process, UINT code)
{
	if (process != PSEUDO_PROCESS) {
		win_set_error(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	TRACE("TerminateProcess(%u)", code);
	_exit(code);
}

WINFN(BOOL) k32_GetExitCodeProcess(HANDLE process, LPDWORD code)
{
	if (process != PSEUDO_PROCESS) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	*code = STILL_ACTIVE;
	return TRUE;
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_thread_exports[] = {
	E(CreateThread),
	E(ExitProcess),
	E(ExitThread),
	E(FlsAlloc),
	E(FlsFree),
	E(FlsGetValue),
	E(FlsSetValue),
	E(GetCurrentProcess),
	E(GetCurrentProcessId),
	E(GetCurrentThread),
	E(GetCurrentThreadId),
	E(GetExitCodeProcess),
	E(GetExitCodeThread),
	E(GetPriorityClass),
	E(GetProcessAffinityMask),
	E(GetThreadId),
	E(GetThreadPriority),
	E(ResumeThread),
	E(SetPriorityClass),
	E(SetProcessAffinityMask),
	E(SetThreadAffinityMask),
	E(SetThreadIdealProcessor),
	E(SetThreadPriority),
	E(Sleep),
	E(SleepEx),
	E(SuspendThread),
	E(SwitchToThread),
	E(TerminateProcess),
	E(TerminateThread),
	E(TlsAlloc),
	E(TlsFree),
	E(TlsGetValue),
	E(TlsSetValue),
	BUILTIN_END
};
