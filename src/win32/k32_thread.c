/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: threads, TLS and FLS slots, sleeping, process exit.
 */
#include <errno.h>
#include <link.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "loader/modules.h"
#include "runtime/gate.h"
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
	HANDLE close_on_exit;		/* _beginthread's handle */

	/* TerminateThread: asked to end, with this code. */
	pthread_t pt;
	int terminate;
	DWORD term_code;
	int in_routine;			/* past the point where it checks */
};

/* What thread_main's exit_jump is given by TerminateThread (ExitThread: 1). */
#define JUMP_TERMINATED 2

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
		if (t->close_on_exit)
			handle_close(t->close_on_exit);
		kobj_unref(&t->obj);
		return NULL;
	}
	__atomic_store_n(&t->tid, teb_current()->UniqueThread, __ATOMIC_RELEASE);

	t->pt = pthread_self();
	pthread_mutex_lock(&t->gate_lock);
	while (t->suspend_count > 0)
		pthread_cond_wait(&t->gate, &t->gate_lock);
	pthread_mutex_unlock(&t->gate_lock);

	int jumped = 0, terminated = 0;
	if (__atomic_load_n(&t->terminate, __ATOMIC_ACQUIRE)) {
		/* Terminated before it ran: its routine never does. */
		TRACE("thread %u terminated before starting", t->tid);
		code = t->term_code;
		terminated = 1;
	} else if (!(jumped = sigsetjmp(t->exit_jump, 0))) {
		TRACE("thread %u starting at %p(%p)", t->tid, (void *)t->start,
		      t->param);
		t->can_jump = 1;
		mod_thread_notify(DLL_THREAD_ATTACH);
		/*
		 * Through the gate: a cdecl routine passed as the start
		 * routine leaves its argument on the stack, which Windows'
		 * BaseThreadInitThunk never notices but would shift this
		 * frame.
		 */
		uintptr_t arg = (uintptr_t)t->param;
		/* Ordered against TerminateThread's store and load. */
		__atomic_store_n(&t->in_routine, 1, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&t->terminate, __ATOMIC_SEQ_CST)) {
			TRACE("thread %u terminated before starting", t->tid);
			code = t->term_code;
			terminated = 1;
		} else {
			code = (DWORD)gate_call((const void *)t->start, 1, &arg);
		}
	} else {
		code = t->exit_code;	/* set by ExitThread or TerminateThread */
		terminated = jumped == JUMP_TERMINATED;
	}
	t->can_jump = 0;

	if (terminated) {
		/* As Windows: no FLS callbacks, and no DLL_THREAD_DETACH (the
		 * DLL ending it may well be in its DllMain, holding the loader
		 * lock that sending one would wait for). */
		wait_lock();
		t->exit_code = code;
		t->done = 1;
		wait_broadcast();
		wait_unlock();
	} else {
		thread_finish(t, code, 1);
	}
	if (t->close_on_exit)
		handle_close(t->close_on_exit);
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

HANDLE thread_create_ex(SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param,
			DWORD flags, LPDWORD tid)
{
	if (!(flags & THREAD_CLOSE_ON_EXIT))
		return k32_CreateThread(NULL, stack, start, param, flags, tid);
	/* Suspended until the handle is recorded, so it can't end first. */
	flags &= ~THREAD_CLOSE_ON_EXIT;
	HANDLE h = k32_CreateThread(NULL, stack, start, param,
				    flags | CREATE_SUSPENDED, tid);
	struct thread *t = h ? thread_from(h) : NULL;
	if (t) {
		t->close_on_exit = h;
		kobj_unref(&t->obj);
		if (!(flags & CREATE_SUSPENDED))
			k32_ResumeThread(h);
	}
	return h;
}

/*
 * TerminateThread. A DLL ends its worker threads so as it unloads (in
 * DllMain), and its code is unmapped straight after, so the thread is
 * stopped before this returns where that can be done safely:
 *  - one that hasn't started its routine never does;
 *  - one running the DLL's code is interrupted by SIGTERM_THREAD, whose
 *    handler goes to the thread's exit, as ExitThread would;
 *  - one in the harness's own code (waiting, sleeping) ends when it next
 *    wakes there, which it is made to; it isn't interrupted where it may
 *    hold a lock.
 * Like Windows, this runs none of the thread's cleanup.
 */
#define SIGTERM_THREAD (SIGRTMIN + 3)

static void on_terminate(int sig, siginfo_t *info, void *ctx);

static void terminate_init(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_terminate;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM_THREAD, &sa, NULL);
}

/* The executable ranges of the harness and its libraries. */
static struct { uintptr_t lo, hi; } host_text[64];
static int nhost_text;

static int add_host_text(struct dl_phdr_info *info, size_t size, void *ctx)
{
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X) ||
		    nhost_text == (int)(sizeof(host_text) / sizeof(*host_text)))
			continue;
		host_text[nhost_text].lo = info->dlpi_addr + ph->p_vaddr;
		host_text[nhost_text].hi = info->dlpi_addr + ph->p_vaddr +
					   ph->p_memsz;
		nhost_text++;
	}
	return 0;
}

static int in_host_text(uintptr_t pc)
{
	for (int i = 0; i < nhost_text; i++)
		if (pc >= host_text[i].lo && pc < host_text[i].hi)
			return 1;
	return 0;
}

static void end_terminated(struct thread *t)
{
	t->exit_code = t->term_code;
	siglongjmp(t->exit_jump, JUMP_TERMINATED);
}

static void on_terminate(int sig, siginfo_t *info, void *ctx)
{
	struct thread *t = self;
	const ucontext_t *uc = ctx;

	if (!t || !__atomic_load_n(&t->terminate, __ATOMIC_ACQUIRE) ||
	    !t->can_jump || !__atomic_load_n(&t->in_routine, __ATOMIC_ACQUIRE))
		return;
#if defined(__x86_64__)
	uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
	uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
#endif
	if (in_host_text(pc))
		return;		/* it ends when it wakes in thread_wake_check */
	end_terminated(t);
}

/* In a sleep, woken: ends the thread if TerminateThread asked. */
static void thread_terminated_check(void)
{
	struct thread *t = self;

	if (t && t->can_jump && __atomic_load_n(&t->in_routine, __ATOMIC_ACQUIRE) &&
	    __atomic_load_n(&t->terminate, __ATOMIC_ACQUIRE))
		end_terminated(t);
}

void thread_wake_check(void)
{
	struct thread *t = self;

	if (t && t->can_jump && __atomic_load_n(&t->in_routine, __ATOMIC_ACQUIRE) &&
	    __atomic_load_n(&t->terminate, __ATOMIC_ACQUIRE)) {
		wait_unlock();
		wait_release_held();
		end_terminated(t);
	}
}

WINFN(BOOL) k32_TerminateThread(HANDLE h, DWORD code)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	struct thread *t = thread_from(h);

	if (!t)
		return FALSE;
	if (t == self) {
		kobj_unref(&t->obj);
		thread_exit(code);
	}
	TRACE("TerminateThread(%p, %u): thread %u", h, code, t->tid);
	pthread_once(&once, terminate_init);
	nhost_text = 0;
	dl_iterate_phdr(add_host_text, NULL);

	t->term_code = code;
	__atomic_store_n(&t->terminate, 1, __ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&t->in_routine, __ATOMIC_SEQ_CST)) {
		/* It will see the request before it runs its routine; it may
		 * be waiting for the loader lock this thread holds. */
		pthread_mutex_lock(&t->gate_lock);
		t->suspend_count = 0;
		pthread_cond_broadcast(&t->gate);
		pthread_mutex_unlock(&t->gate_lock);
		kobj_unref(&t->obj);
		return TRUE;
	}
	pthread_mutex_lock(&t->gate_lock);
	t->suspend_count = 0;
	pthread_cond_broadcast(&t->gate);
	pthread_mutex_unlock(&t->gate_lock);

	/* Up to 2 s for it to end, asking again as it may have been between
	 * places where it looks. */
	for (int i = 0; i < 200 && !__atomic_load_n(&t->done, __ATOMIC_ACQUIRE);
	     i++) {
		pthread_kill(t->pt, SIGTERM_THREAD);
		wait_lock();
		wait_broadcast();
		wait_unlock();
		struct timespec ts = { 0, 10 * 1000000L };
		nanosleep(&ts, NULL);
	}
	if (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE))
		TRACE("TerminateThread: thread %u is still running", t->tid);
	kobj_unref(&t->obj);
	return TRUE;
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
	return (n >= 8 * sizeof(DWORD_PTR) ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1));
}

WINFN(DWORD) k32_SetThreadIdealProcessor(HANDLE h, DWORD cpu) { return 0; }

WINFN(BOOL) k32_GetProcessAffinityMask(HANDLE process, PDWORD_PTR proc_mask,
				       PDWORD_PTR sys_mask)
{
	DWORD n = peb_get()->NumberOfProcessors;
	DWORD_PTR mask = (n >= 8 * sizeof(DWORD_PTR) ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1));

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
		for (;;) {
			pause();
			thread_terminated_check();
		}
	}
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) && errno == EINTR)
		thread_terminated_check();
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

WINFN(void) k32_FatalAppExitA(UINT action, LPCSTR message)
{
	if (message)
		fprintf(stderr, "FatalAppExit: %s\n", message);
	k32_ExitProcess(0);
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

/* Threads' registers are not readable or writable from outside. */
WINFN(BOOL) k32_GetThreadContext(HANDLE thread, PVOID context)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return FALSE;
}

WINFN(BOOL) k32_SetThreadContext(HANDLE thread, const void *context)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return FALSE;
}

/* Only this process can be opened. */
WINFN(HANDLE) k32_OpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
	if (pid != (DWORD)getpid()) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	return PSEUDO_PROCESS;
}

/* One group of CPUs, all of them allowed: nothing is ever restricted. */
WINFN(BOOL) k32_GetThreadGroupAffinity(HANDLE thread, PGROUP_AFFINITY a)
{
	DWORD n = peb_get()->NumberOfProcessors;

	memset(a, 0, sizeof(*a));
	a->Mask = (n >= 8 * sizeof(DWORD_PTR) ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1));
	return TRUE;
}

WINFN(BOOL) k32_SetThreadGroupAffinity(HANDLE thread, const GROUP_AFFINITY *a,
				       PGROUP_AFFINITY previous)
{
	if (a->Group) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (previous)
		k32_GetThreadGroupAffinity(thread, previous);
	return TRUE;
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_thread_exports[] = {
	E(CreateThread),
	E(ExitProcess),
	E(ExitThread),
	E(FatalAppExitA),
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
	E(GetThreadContext),
	E(GetThreadGroupAffinity),
	E(GetThreadId),
	E(GetThreadPriority),
	E(OpenProcess),
	E(ResumeThread),
	E(SetPriorityClass),
	E(SetProcessAffinityMask),
	E(SetThreadAffinityMask),
	E(SetThreadContext),
	E(SetThreadGroupAffinity),
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
