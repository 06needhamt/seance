/*
 * Crash and hang reports: testcodec.dll faults or hangs on request, in a
 * child process, and the report has to say where and in which call.
 */
#include <signal.h>
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
