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
 *
 * One fault isn't a crash: running code from memory that is readable but
 * not executable, such as the heap or the stack. Windows ran programs
 * without DEP (data execution prevention) unless they asked for it, and
 * codecs relied on that, building code at run time (pvwv220.dll loads
 * the DLLs it carries into heap memory with a PE loader of its own). As
 * Wine does, the pages are made executable and the code carries on.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "common/log.h"
#include "common/sanitizer.h"
#include "loader/modules.h"
#include "runtime/crash.h"
#include "runtime/guard.h"
#include "runtime/seh.h"
#include "runtime/teb.h"

#define MAX_THREADS 256
#define ALT_STACK   (64 * 1024)

struct slot {
	int used;
	struct crash_frame cur;	/* what is being called; cur.what NULL if none */
	volatile int depth;
	struct timespec start;	/* of the outermost call in progress */
	void *alt_stack;
	sigjmp_buf *recover;	/* crash_try's, while one is in progress */
};

static struct slot slots[MAX_THREADS];
static pthread_mutex_t slots_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct slot *mine;
static int timeout_secs;
static int keep_core = -1;	/* VFW_CORE=1; -1 until crash_install */

void crash_exit(int sig)
{
	if (keep_core < 0) {
		const char *c = getenv("VFW_CORE");
		keep_core = c && !strcmp(c, "1");
	}
	if (keep_core) {
		signal(sig, SIG_DFL);
		raise(sig);
	}
	_exit(128 + sig);
}

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

	char buffer[160];

	describe(pc, where, sizeof(where));
	if ((sig == SIGSEGV || sig == SIGBUS) &&
	    guard_describe(fault_addr, buffer, sizeof(buffer)))
		snprintf(line, sizeof(line), "seance: crash: %s at %s, "
			 "accessing %p, %s\n", signame(sig), where, fault_addr,
			 buffer);
	else if (sig == SIGSEGV || sig == SIGBUS)
		snprintf(line, sizeof(line), "seance: crash: %s at %s, "
			 "accessing %p\n", signame(sig), where, fault_addr);
	else
		snprintf(line, sizeof(line), "seance: crash: %s at %s\n",
			 signame(sig), where);
	put(line);
	report_call(mine);
}

/* Back to crash_try, if the thread is in one. */
static void recover(int sig)
{
	struct slot *s = mine;

	if (s && s->recover) {
		sigjmp_buf *jb = s->recover;
		s->recover = NULL;
		siglongjmp(*jb, sig);
	}
}

void crash_report_and_die(int sig, const void *pc, const void *fault_addr)
{
	report_fault(sig, pc, fault_addr);
	recover(sig);
	crash_exit(sig);
}

/* x86 page fault error code bits, in the context's REG_ERR. */
#define PF_PROTECTION 0x01		/* the page was present */
#define PF_FETCH      0x10		/* an instruction fetch */

/*
 * The protection of the mapping holding addr, from /proc/self/maps, read
 * without stdio as this is signal context: PROT_* bits, or -1 if no
 * mapping holds it.
 */
static int mapping_prot(uintptr_t addr)
{
	char buf[4096], line[256];
	size_t len = 0;
	ssize_t n;
	int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC), prot = -1;

	if (fd < 0)
		return -1;
	while (prot < 0 && (n = read(fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n && prot < 0; i++) {
			if (buf[i] != '\n') {
				if (len + 1 < sizeof(line))
					line[len++] = buf[i];
				continue;
			}
			line[len] = 0;
			len = 0;
			/* "08048000-08056000 r-xp ..." */
			char *end;
			unsigned long lo = strtoul(line, &end, 16), hi;
			if (*end != '-')
				continue;
			hi = strtoul(end + 1, &end, 16);
			if (*end != ' ' || addr < lo || addr >= hi)
				continue;
			prot = (end[1] == 'r' ? PROT_READ : 0) |
			       (end[2] == 'w' ? PROT_WRITE : 0) |
			       (end[3] == 'x' ? PROT_EXEC : 0);
		}
	}
	close(fd);
	return prot;
}

/*
 * An instruction fetch from a readable page that isn't executable: the
 * page is made executable, as Windows without DEP would have it, and 1
 * returned for the code to go on. 0 for any other fault.
 */
static int run_data(int sig, const siginfo_t *si, const ucontext_t *uc)
{
	unsigned long err = (unsigned long)uc->uc_mcontext.gregs[REG_ERR];
	uintptr_t page = (uintptr_t)si->si_addr & ~(uintptr_t)(getpagesize() - 1);

	if (sig != SIGSEGV || si->si_code != SEGV_ACCERR ||
	    (err & (PF_PROTECTION | PF_FETCH)) != (PF_PROTECTION | PF_FETCH))
		return 0;
	int prot = mapping_prot(page);
	if (prot < 0 || !(prot & PROT_READ) || (prot & PROT_EXEC) ||
	    mprotect((void *)page, (size_t)getpagesize(), prot | PROT_EXEC))
		return 0;
	if (vfw_trace_enabled) {
		char where[160], line[256];
		describe((const void *)uc->uc_mcontext.gregs[REG_EIP], where,
			 sizeof(where));
		snprintf(line, sizeof(line), "trace[%d] running code at %s from "
			 "data: page %#lx made executable (no DEP)\n",
			 (int)getpid(), where, (unsigned long)page);
		put(line);
	}
	return 1;
}

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;
	const void *pc = (const void *)uc->uc_mcontext.gregs[REG_EIP];

	if (run_data(sig, si, uc))
		return;

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
	if (si->si_code > 0) {		/* a fault, not a signal sent */
		recover(sig);
		crash_exit(sig);
	}

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
	const char *c = getenv("VFW_CORE");
	keep_core = c && !strcmp(c, "1");

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

int crash_try(void (*fn)(void *arg), void *arg)
{
	struct slot *s = mine;
	struct win_teb *teb = teb_current();

	if (!s) {
		fn(arg);
		return 0;
	}

	sigjmp_buf jb;
	sigjmp_buf *volatile outer = s->recover;
	struct crash_frame cur = s->cur;
	int depth = s->depth;
	void *chain = teb ? teb->ExceptionList : NULL;
	unsigned short fpcw;
	unsigned int mxcsr;
	__asm__ volatile("fnstcw %0" : "=m"(fpcw));
	__asm__ volatile("stmxcsr %0" : "=m"(mxcsr));

	int sig = sigsetjmp(jb, 1);
	if (!sig) {
		s->recover = &jb;
		fn(arg);
		s->recover = outer;
		return 0;
	}
	s->recover = outer;
	s->cur = cur;
	__atomic_store_n(&s->depth, depth, __ATOMIC_RELEASE);
	if (teb)
		teb->ExceptionList = chain;
	__asm__ volatile("cld; fninit; fldcw %0" : : "m"(fpcw));
	__asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
	return sig;
}
