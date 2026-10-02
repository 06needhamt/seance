/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Crash and hang reports.
 *
 * Each thread that runs DLL code has a slot in a static table saying which
 * call into a DLL is in progress (crash_enter / crash_leave). A fault
 * handler on its own stack formats a report from it; a watchdog thread
 * checks the slots' start times when VFW_TIMEOUT is set.
 *
 * The reports are made in signal context, from a crash: they format with
 * snprintf and look up the module without waiting for the loader lock,
 * which is as careful as a crash report can usefully be.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "common/sanitizer.h"
#include "loader/modules.h"
#include "runtime/crash.h"
#include "runtime/seh.h"

#define MAX_THREADS 256
#define ALT_STACK   (64 * 1024)

struct slot {
	int used;
	struct crash_frame cur;	/* what is being called; cur.what NULL if none */
	volatile int depth;
	struct timespec start;	/* of the outermost call in progress */
	void *alt_stack;
};

static struct slot slots[MAX_THREADS];
static pthread_mutex_t slots_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct slot *mine;
static int timeout_secs;

static void put(const char *s)
{
	size_t n = strlen(s);
	while (n) {
		ssize_t w = write(STDERR_FILENO, s, n);
		if (w <= 0)
			return;
		s += w;
		n -= (size_t)w;
	}
}

/* addr as module+offset, without blocking on the loader lock. */
static void describe(const void *addr, char *out, size_t size)
{
	if (!mod_describe_address_try(addr, out, size))
		snprintf(out, size, "%p", addr);
}

/* "  in DriverProc(ICM_DECOMPRESS) of testcodec.dll" for a slot. */
static void report_call(const struct slot *s)
{
	char where[160], name[24], line[320];

	if (!s || !s->depth || !s->cur.what)
		return;
	describe(s->cur.fn, where, sizeof(where));
	/* Just the module: cut "+0x..." off the function's address. */
	char *plus = strchr(where, '+');
	if (plus)
		*plus = 0;
	const char *code = s->cur.namer ? s->cur.namer(s->cur.code, name) : NULL;
	if (!code) {
		snprintf(name, sizeof(name), "%u", s->cur.code);
		code = name;
	}
	snprintf(line, sizeof(line), "seance:   in %s(%s) of %s\n",
		 s->cur.what, code, where);
	put(line);
}

static const char *signame(int sig)
{
	switch (sig) {
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS:  return "SIGBUS";
	case SIGILL:  return "SIGILL";
	case SIGFPE:  return "SIGFPE";
	default:      return "signal";
	}
}

static void report_fault(int sig, const void *pc, const void *fault_addr)
{
	char where[160], line[400];

	describe(pc, where, sizeof(where));
	if (sig == SIGSEGV || sig == SIGBUS)
		snprintf(line, sizeof(line), "seance: crash: %s at %s, "
			 "accessing %p\n", signame(sig), where, fault_addr);
	else
		snprintf(line, sizeof(line), "seance: crash: %s at %s\n",
			 signame(sig), where);
	put(line);
	report_call(mine);
}

void crash_report_and_die(int sig, const void *pc, const void *fault_addr)
{
	report_fault(sig, pc, fault_addr);
	signal(sig, SIG_DFL);
	raise(sig);
	_exit(128 + sig);	/* raise() should not return */
}

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;
	const void *pc = (const void *)uc->uc_mcontext.gregs[REG_EIP];

	/*
	 * Give seh.c first chance at it: a thread running DLL code may have
	 * __try frames in its fs:[0] chain, and it walks them on the
	 * thread's own stack rather than here on the signal's alternate
	 * one. It reports the crash itself, the same way, if nothing in the
	 * chain handles it.
	 */
	if (seh_take_fault(sig, si, uc))
		return;

	report_fault(sig, pc, si->si_addr);

	/* Die of the signal, as without the handler. */
	signal(sig, SIG_DFL);
	if (si->si_code <= 0)		/* sent, not a fault: send it again */
		raise(sig);
}

static double elapsed(const struct timespec *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t->tv_sec) + (now.tv_nsec - t->tv_nsec) / 1e9;
}

static void *watchdog(void *arg)
{
	for (;;) {
		struct timespec nap = { 0, 100 * 1000 * 1000 };
		nanosleep(&nap, NULL);
		for (int i = 0; i < MAX_THREADS; i++) {
			struct slot *s = &slots[i];
			if (!s->used || !s->depth || elapsed(&s->start) < timeout_secs)
				continue;
			char line[160];
			snprintf(line, sizeof(line), "seance: timeout: a call "
				 "into a DLL took more than %d s (VFW_TIMEOUT)\n",
				 timeout_secs);
			put(line);
			report_call(s);
			_exit(124);
		}
	}
	return NULL;
}

int crash_install(void)
{
	const char *t = getenv("VFW_TIMEOUT");
	if (t && *t) {
		char *end;
		long v = strtol(t, &end, 10);
		if (*end || v <= 0 || v > 86400) {
			fprintf(stderr, "seance: VFW_TIMEOUT wants seconds, not "
				"'%s'\n", t);
			return -1;
		}
		timeout_secs = (int)v;
	}

	/* Sanitizer builds keep their own, more detailed, fault reports. */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };
	for (size_t i = 0; !VFW_SANITIZER_BUILD && i < sizeof(sigs) / sizeof(sigs[0]);
	     i++)
		sigaction(sigs[i], &sa, NULL);

	if (timeout_secs) {
		pthread_t w;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		int err = pthread_create(&w, &attr, watchdog, NULL);
		pthread_attr_destroy(&attr);
		if (err) {
			fprintf(stderr, "seance: cannot start the watchdog: %s\n",
				strerror(err));
			return -1;
		}
	}
	return 0;
}

void crash_thread_init(void)
{
	if (mine)
		return;
	pthread_mutex_lock(&slots_lock);
	for (int i = 0; i < MAX_THREADS && !mine; i++)
		if (!slots[i].used) {
			memset(&slots[i], 0, sizeof(slots[i]));
			slots[i].used = 1;
			mine = &slots[i];
		}
	pthread_mutex_unlock(&slots_lock);
	if (!mine)
		return;		/* no reports for this thread: not fatal */

	/* Faults on a blown or clobbered stack still get reported. */
	void *st = mmap(NULL, ALT_STACK, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (st != MAP_FAILED) {
		stack_t ss = { .ss_sp = st, .ss_size = ALT_STACK, .ss_flags = 0 };
		if (sigaltstack(&ss, NULL))
			munmap(st, ALT_STACK);
		else
			mine->alt_stack = st;
	}
}

void crash_thread_exit(void)
{
	struct slot *s = mine;

	if (!s)
		return;
	mine = NULL;
	if (s->alt_stack) {
		stack_t ss = { .ss_flags = SS_DISABLE };
		sigaltstack(&ss, NULL);
		munmap(s->alt_stack, ALT_STACK);
	}
	pthread_mutex_lock(&slots_lock);
	s->depth = 0;
	s->used = 0;
	pthread_mutex_unlock(&slots_lock);
}

void crash_enter(const char *what, const void *fn, UINT code,
		 crash_namer namer, struct crash_frame *saved)
{
	struct slot *s = mine;

	if (!s) {
		saved->what = NULL;
		return;
	}
	*saved = s->cur;
	if (!s->depth)
		clock_gettime(CLOCK_MONOTONIC, &s->start);
	s->cur = (struct crash_frame){ what, fn, code, namer };
	__atomic_store_n(&s->depth, s->depth + 1, __ATOMIC_RELEASE);
}

void crash_leave(const struct crash_frame *saved)
{
	struct slot *s = mine;

	if (!s || !s->depth)
		return;
	__atomic_store_n(&s->depth, s->depth - 1, __ATOMIC_RELEASE);
	s->cur = *saved;
}
