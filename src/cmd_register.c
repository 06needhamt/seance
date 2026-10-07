/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance register [--dry-run] <server>
 * seance unregister <server>
 *
 * Runs a COM server's DllRegisterServer (or DllUnregisterServer): a
 * DirectShow filter's .ax or a DMO's DLL, so its filters and DMOs are in
 * the registry for this harness the way they would be on Windows, and
 * shows what it registers. --dry-run runs it in a registry sandbox,
 * changing nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "common/error.h"
#include "dmo/discover.h"
#include "dshow/discover.h"
#include "win32/quartz.h"
#include "win32/reg.h"

static int usage(const char *cmd)
{
	if (!strcmp(cmd, "unregister"))
		fprintf(stderr, "usage: seance unregister <filter.ax|dmo.dll>\n");
	else
		fprintf(stderr,
			"usage: seance register [--dry-run] <filter.ax|dmo.dll>\n"
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

	struct dshow_server_found found;
	HRESULT hr;
	if (dry_run ? dshow_server_discover(path, &found, &hr) :
		      dshow_server_register(path, unregister, &found, &hr)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}

	if (unregister) {
		if (SUCCEEDED(hr))
			printf("Unregistered %s.\n", path);
	} else {
		if (dry_run)
			printf("What %s would register:\n", path);
		else
			printf("Registered %s:\n", path);
		if (!found.nfilters && !found.ndmos)
			printf("  no filters or DMOs\n");
		for (size_t i = 0; i < found.nfilters; i++)
			dshow_print_filter(&found.filters[i], NULL, 1);
		for (size_t i = 0; i < found.ndmos; i++)
			dmo_print(&found.dmos[i], NULL, 1);
	}
	dshow_server_found_free(&found);
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
