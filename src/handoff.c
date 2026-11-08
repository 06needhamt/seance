/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Running a codec of the other architecture in the sibling binary.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "handoff.h"
#include "loader/pe.h"
#include "common/error.h"

#if defined(__x86_64__)
#define SIBLING_ENV "VFW_SEANCE32"
#define SIBLING_EMU_ENV "VFW_EMULATOR32"
#define SIBLING_NAME "seance"
#else
#define SIBLING_ENV "VFW_SEANCE64"
#define SIBLING_EMU_ENV "VFW_EMULATOR64"
#define SIBLING_NAME "seance64"
#endif

/* Set in the sibling's environment, so it never hands back. */
#define HANDED_ENV "VFW_HANDED_OFF"

static int saved_argc;
static char **saved_argv;

void handoff_init(int argc, char **argv)
{
	saved_argc = argc;
	saved_argv = argv;
}

int handoff_sibling(char *out, size_t size)
{
	const char *env = getenv(SIBLING_ENV);
	if (env && *env) {
		if (access(env, X_OK)) {
			vfw_set_error("%s=%s: %s", SIBLING_ENV, env,
				      strerror(errno));
			return -1;
		}
		snprintf(out, size, "%s", env);
		return 0;
	}
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n <= 0) {
		vfw_set_error("can't find this program: %s", strerror(errno));
		return -1;
	}
	self[n] = 0;
	char *slash = strrchr(self, '/');
	if (slash)
		slash[1] = 0;
	else
		self[0] = 0;
	snprintf(out, size, "%s%s", self, SIBLING_NAME);
	if (access(out, X_OK)) {
		vfw_set_error("%s: %s (set %s to where it is)", out,
			      strerror(errno), SIBLING_ENV);
		return -1;
	}
	return 0;
}

void handoff_exec(WORD machine)
{
	char sibling[PATH_MAX];
	const char *what = pe_machine_name(machine);

	static int tried;

	/* Once: a foreign codec named by path is tried before pe_map is. */
	if (tried || !saved_argv || getenv(HANDED_ENV))
		return;
	tried = 1;
	if (handoff_sibling(sibling, sizeof(sibling))) {
		fprintf(stderr, "seance: the codec is %s, for %s, which isn't "
			"there: %s\n", what, SIBLING_NAME, vfw_error());
		return;
	}

	/*
	 * The sibling's emulator's words (VFW_EMULATOR is this binary's, so
	 * the other has its own), then the sibling, then our arguments.
	 */
	char *emu = getenv(SIBLING_EMU_ENV);
	char *emu_copy = emu && *emu ? strdup(emu) : NULL;
	char **argv = calloc((size_t)saved_argc + 10, sizeof(*argv));
	int n = 0;
	if (!argv) {
		free(emu_copy);
		return;
	}
	for (char *t = emu_copy ? strtok(emu_copy, " ") : NULL; t && n < 8;
	     t = strtok(NULL, " "))
		argv[n++] = t;
	argv[n++] = sibling;
	for (int i = 1; i < saved_argc; i++)
		argv[n++] = saved_argv[i];
	argv[n] = NULL;

	fflush(stdout);
	fflush(stderr);
	if (getenv("VFW_TRACE"))
		fprintf(stderr, "seance: the codec is %s: running %s\n", what,
			sibling);
	setenv(HANDED_ENV, "1", 1);
	if (emu_copy)
		execvp(argv[0], argv);
	else
		execv(argv[0], argv);
	fprintf(stderr, "seance: can't run %s for the %s codec: %s\n",
		argv[0], what, strerror(errno));
	unsetenv(HANDED_ENV);
	free(argv);
	free(emu_copy);
}

void handoff_if_foreign(const char *path)
{
	if (access(path, R_OK))
		return;
	WORD m = pe_peek_machine(path);
	if ((m == IMAGE_FILE_MACHINE_I386 || m == IMAGE_FILE_MACHINE_AMD64) &&
	    m != PE_HOST_MACHINE)
		handoff_exec(m);
}

void handoff_arm(void)
{
	if (!getenv(HANDED_ENV))
		pe_set_foreign_hook(handoff_exec);
}
