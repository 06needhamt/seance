/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Crash and hang reports: testcodec.dll faults or hangs on request, in a
 * child process, and the report has to say where and in which call.
 */
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "common/sanitizer.h"
#include "runtime/crash.h"
#include "runtime/runtime.h"
#include "test.h"
#include "vfw/icm.h"

#define TST0 mmioFOURCC('T', 'S', 'T', '0')
#define TESTCODEC_CRASH (ICM_USER + 100)
#define TESTCODEC_HANG  (ICM_USER + 101)

/*
 * Runs msg against the codec in a child with VFW_TIMEOUT set to timeout
 * (or unset for 0). Returns the wait status and the child's stderr.
 */
static int run_child(const char *codec, UINT msg, const char *timeout,
		     char *err, size_t size)
{
	int fds[2];
	if (pipe(fds))
		return -1;
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		if (timeout)
			setenv("VFW_TIMEOUT", timeout, 1);
		else
			unsetenv("VFW_TIMEOUT");
		if (crash_install() || runtime_enter())
			_exit(90);
		HIC hic = ic_open_dll(codec, ICTYPE_VIDEO, TST0, ICMODE_QUERY);
		if (!hic)
			_exit(91);
		ic_send(hic, msg, 0, 0);
		_exit(92);		/* it should not come back */
	}
	close(fds[1]);
	size_t n = 0;
	ssize_t r;
	while (n + 1 < size && (r = read(fds[0], err + n, size - 1 - n)) > 0)
		n += (size_t)r;
	err[n] = 0;
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	return status;
}

/* mov eax, 42; ret */
static const unsigned char ret42[] = { 0xb8, 42, 0, 0, 0, 0xc3 };

/*
 * Code run from where it was put: the heap, the stack or a page the
 * caller maps. Exits 42 when it ran and returned.
 */
static int run_from(int where, char *err, size_t size)
{
	int fds[2];
	if (pipe(fds))
		return -1;
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		if (crash_install() || runtime_enter())
			_exit(90);
		unsigned char on_stack[64], *code = on_stack;
		if (where == 0)
			code = malloc(64);
		else if (where == 2)
			code = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		memcpy(code, ret42, sizeof(ret42));
		/* Running it reads it, which the compiler can't see. */
		__asm__ volatile("" : : "r"(code) : "memory");
		if (where == 2)
			mprotect(code, 4096, PROT_NONE);
		int (*fn)(void) = (int (*)(void))(void *)code;
		_exit(fn());
	}
	close(fds[1]);
	size_t n = 0;
	ssize_t r;
	while (n + 1 < size && (r = read(fds[0], err + n, size - 1 - n)) > 0)
		n += (size_t)r;
	err[n] = 0;
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	return status;
}

int main(void)
{
	const char *codec = fixture("testcodec.dll");
	char err[4096];

	if (VFW_SANITIZER_BUILD) {
		/* The sanitizer reports faults itself; only the timeout is ours. */
		printf("sanitizer build: checking the timeout only\n");
	} else {
		int st = run_child(codec, TESTCODEC_CRASH, NULL, err, sizeof(err));
		CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
		CHECK(strstr(err, "seance: crash: SIGSEGV at testcodec.dll+0x"));
		CHECK(strstr(err, "accessing (nil)") || strstr(err, "accessing 0x0"));
		CHECK(strstr(err, "in DriverProc(DRV_USER+0x") &&
		      strstr(err, ") of testcodec.dll"));
		printf("crash report:\n%s", err);

		/*
		 * Code in the heap or on the stack runs, as on Windows without
		 * DEP; a page that can't be read is still a crash.
		 */
		st = run_from(0, err, sizeof(err));
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);
		st = run_from(1, err, sizeof(err));
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);
		st = run_from(2, err, sizeof(err));
		CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
		CHECK(strstr(err, "seance: crash: SIGSEGV"));
	}

	int st = run_child(codec, TESTCODEC_HANG, "1", err, sizeof(err));
	printf("timeout report:\n%s", err);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 124);
	CHECK(strstr(err, "seance: timeout: a call into a DLL took more "
		      "than 1 s"));
	CHECK(strstr(err, "in DriverProc(DRV_USER+0x") &&
	      strstr(err, ") of testcodec.dll"));

	/* A bad VFW_TIMEOUT is refused. */
	st = run_child(codec, TESTCODEC_HANG, "soon", err, sizeof(err));
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 90);
	CHECK(strstr(err, "VFW_TIMEOUT wants seconds"));

	if (test_failures)
		fprintf(stderr, "last child said:\n%s", err);
	return test_summary("test_crash");
}
