/*
 * SEH dispatch: pe_seh.dll installs fs:[0] frames by hand and faults under
 * them; the harness has to build EXCEPTION_RECORD/CONTEXT, walk the chain,
 * run RtlUnwind's intervening frames, and resume where a handler says to.
 * RaiseException goes through the same chain without a fault. An unhandled
 * fault still has to crash normally, checked in a child as test_crash does.
 *
 * The signal-based checks need the harness's own SIGSEGV handler, which
 * sanitizer builds leave uninstalled (ASan's own runs instead); only the
 * RaiseException path, which never touches a signal, still applies there.
 */
#include <signal.h>
#include <sys/wait.h>

#include "common/sanitizer.h"
#include "loader/modules.h"
#include "runtime/crash.h"
#include "runtime/runtime.h"
#include "test.h"

typedef int (WINAPI *int_fn)(void);

static int_fn get(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "pe_seh.dll has no %s\n", name);
		exit(1);
	}
	return (int_fn)p;
}

/* Runs seh_unhandled_crash in a child; returns the wait status and stderr. */
static int run_unhandled(const char *dll, char *err, size_t size)
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
		struct module *m = mod_load(dll, NULL);
		if (!m)
			_exit(91);
		get(m, "seh_unhandled_crash")();
		_exit(92);	/* it should not come back */
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
	const char *dll = fixture("pe_seh.dll");

	CHECK_EQ(runtime_enter(), 0);
	CHECK_EQ(crash_install(), 0);

	struct module *m = mod_load(dll, NULL);
	CHECK(m != NULL);
	if (m) {
		CHECK_EQ(get(m, "seh_raise_test")(), 1);
		if (!VFW_SANITIZER_BUILD) {
			CHECK_EQ(get(m, "seh_recover")(), 1);
			CHECK_EQ(get(m, "seh_chain_unwind")(), 1);
			CHECK_EQ(get(m, "seh_nonlocal_twice")(), 1);
			CHECK_EQ(get(m, "seh_unhandled_filter")(), 1);
		} else {
			printf("sanitizer build: skipping the fault-based checks\n");
		}
		mod_free(m);
	}

	if (VFW_SANITIZER_BUILD) {
		printf("sanitizer build: skipping the unhandled-fault check\n");
	} else {
		char err[4096];
		int st = run_unhandled(dll, err, sizeof(err));
		CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
		CHECK(strstr(err, "vfw-harness: crash: SIGSEGV at pe_seh.dll+0x"));
		CHECK(strstr(err, "accessing (nil)") || strstr(err, "accessing 0x0"));
		if (test_failures)
			fprintf(stderr, "child said:\n%s", err);
	}

	return test_summary("test_seh");
}
