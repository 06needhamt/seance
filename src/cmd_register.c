/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance register [--dry-run] <filter.ax>
 * seance unregister <filter.ax>
 *
 * Runs a DirectShow filter server's DllRegisterServer (or
 * DllUnregisterServer), so its filters are in the registry for this
 * harness the way they would be on Windows, and shows the filters it
 * registers. --dry-run runs it in a registry sandbox, changing nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "common/error.h"
#include "dshow/discover.h"
#include "win32/quartz.h"
#include "win32/reg.h"

static int usage(const char *cmd)
{
	if (!strcmp(cmd, "unregister"))
		fprintf(stderr, "usage: seance unregister <filter.ax>\n");
	else
		fprintf(stderr,
			"usage: seance register [--dry-run] <filter.ax>\n"
			"\n"
			"  --dry-run   show what it registers without changing "
			"the registry\n");
	return 2;
}

int cmd_register(int argc, char **argv)
{
	int unregister = !strcmp(argv[0], "unregister");
	int dry_run = 0;
	const char *path = NULL;

	for (int i = 1; i < argc; i++) {
		if (!unregister && !strcmp(argv[i], "--dry-run"))
			dry_run = 1;
		else if (argv[i][0] == '-' || path)
			return usage(argv[0]);
		else
			path = argv[i];
	}
	if (!path)
		return usage(argv[0]);

	struct qz_filter *f = NULL;
	size_t n = 0;
	HRESULT hr;
	if (dry_run ? dshow_discover(path, &f, &n, &hr) :
		      dshow_register(path, unregister, &f, &n, &hr)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}

	if (unregister) {
		if (SUCCEEDED(hr))
			printf("Unregistered %s.\n", path);
	} else {
		if (dry_run)
			printf("Filters %s would register:\n", path);
		else
			printf("Registered %s:\n", path);
		if (!n)
			printf("  no filters\n");
		for (size_t i = 0; i < n; i++)
			dshow_print_filter(&f[i], NULL, 1);
	}
	qz_filters_free(f, n);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: %s failed: 0x%08lx\n",
			unregister ? "DllUnregisterServer" : "DllRegisterServer",
			(unsigned long)hr);
		return 1;
	}
	if (!dry_run) {
		const char *file = reg_file_path();
		if (*file)
			printf("The registry is %s.\n", file);
		else
			printf("The registry is kept in memory only "
			       "(VFW_REGISTRY is empty), so this is lost "
			       "on exit.\n");
	}
	return 0;
}
