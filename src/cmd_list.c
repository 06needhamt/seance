/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance list [--no-open] [--all]: the codecs and ACM drivers registered
 * in vfw.ini, and the DirectShow filters and DMOs registered in the
 * registry.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "acm/acm.h"
#include "commands.h"
#include "common/error.h"
#include "common/fourcc.h"
#include "common/unicode.h"
#include "dmo/discover.h"
#include "dshow/discover.h"
#include "runtime/runtime.h"
#include "vfw/icm.h"
#include "vfw/registry.h"
#include "win32/msacm32.h"
#include "win32/reg.h"

/* The DirectShow filters registered, video ones unless all. */
static void list_filters(int all)
{
	struct dshow_registered *r = NULL;
	size_t n = dshow_registered_filters(&r), shown = 0;
	const char *file = reg_file_path();

	printf("\nDirectShow %sfilters registered in %s:\n",
	       all ? "" : "video ", *file ? file : "the registry (in memory)");
	for (size_t i = 0; i < n; i++) {
		if (!all && dshow_role(&r[i].f) == DSHOW_OTHER)
			continue;
		dshow_print_filter(&r[i].f, r[i].path ? r[i].path :
				   "(no InprocServer32)", 0);
		shown++;
	}
	if (!shown)
		printf("  none%s. seance register <filter.ax> registers a "
		       "filter server's filters.\n",
		       all || !n ? "" : " that decode or encode video");
	dshow_registered_free(r, n);
}

/* The DMOs registered, decoders and encoders unless all. */
static void list_dmos(int all)
{
	struct dmo_registered *r = NULL;
	size_t n = dmo_registered(&r), shown = 0;
	const char *file = reg_file_path();

	printf("\n%s registered in %s:\n",
	       all ? "DMOs" : "DMO decoders and encoders",
	       *file ? file : "the registry (in memory)");
	for (size_t i = 0; i < n; i++) {
		if (!all && !dmo_role_is_codec(dmo_role(&r[i].d)))
			continue;
		dmo_print(&r[i].d, r[i].path ? r[i].path : "(no InprocServer32)",
			  0);
		shown++;
	}
	if (!shown)
		printf("  none%s. seance register <dmo.dll> registers a DLL's "
		       "DMOs.\n", all || !n ? "" : " that decode or encode");
	dmo_registered_free(r, n);
}

static void list_codecs(int open)
{
	printf("Codecs in %s:\n", vfw_ini_path());
	const struct vfw_codec *c = vfw_enum(0, 0);
	if (!c) {
		printf("  none. Add a [drivers32] section with lines such as\n"
		       "  vidc.XXXX=/path/to/codec.dll (or set VFW_INI).\n");
		return;
	}
	for (unsigned i = 0; (c = vfw_enum(0, i)); i++) {
		char t[FOURCC_STR_SIZE], h[FOURCC_STR_SIZE];
		FOURCC type = c->type, handler = c->handler;

		printf("  %s.%s  %s\n", fourcc_str(type, t), fourcc_str(handler, h),
		       c->path ? c->path : "(function)");
		if (!open)
			continue;
		HIC hic = ic_open(type, handler, ICMODE_QUERY);
		if (!hic) {
			printf("      can't open: %s\n", vfw_error());
			continue;
		}
		ICINFO info;
		memset(&info, 0, sizeof(info));
		if (ic_get_info(hic, &info, sizeof(info))) {
			char name[64], desc[400];
			info.szName[15] = 0;
			info.szDescription[127] = 0;
			utf16_to_utf8(info.szName, (size_t)-1, name, sizeof(name));
			utf16_to_utf8(info.szDescription, (size_t)-1, desc, sizeof(desc));
			printf("      %s: %s\n", name, desc);
		} else {
			printf("      no answer to ICM_GETINFO\n");
		}
		ic_close(hic);
	}
}

#define MAX_ACM 64

static BOOL WINAPI collect_driver(HACMDRIVERID id, DWORD_PTR inst,
				 DWORD support)
{
	HACMDRIVERID *ids = (HACMDRIVERID *)inst;

	(void)support;
	for (int i = 0; i < MAX_ACM; i++)
		if (!ids[i]) {
			ids[i] = id;
			break;
		}
	return TRUE;
}

/* The ACM drivers vfw.ini names, and with open, what each says it is. */
static void list_acm(int open)
{
	HACMDRIVERID ids[MAX_ACM] = { 0 };
	char *alias, *path;

	printf("\nACM drivers in %s:\n", vfw_ini_path());
	if (vfw_acm_get(0, &alias, &path)) {
		printf("  none. Add lines such as msacm.<name>=/path/to/driver.acm "
		       "to its [drivers32]\n  section. (The built-in PCM "
		       "converter is always there.)\n");
		return;
	}
	free(alias);
	free(path);
	if (open)
		msacm_acmDriverEnum(collect_driver, (DWORD_PTR)ids,
				    ACM_DRIVERENUMF_DISABLED);
	for (unsigned n = 0; !vfw_acm_get(n, &alias, &path); n++) {
		printf("  %s  %s\n", alias, path);
		if (open) {
			char line[600];
			int found = 0;
			for (int i = 0; i < MAX_ACM && ids[i] && !found; i++) {
				char *a = NULL, *p = msacm_driver_path(ids[i], &a);
				if (a && !strcasecmp(a, alias)) {
					found = 1;
					if (!acm_describe(ids[i], line, sizeof(line)))
						printf("      %s\n", line);
				}
				free(a);
				free(p);
			}
			if (!found)
				printf("      doesn't open as an ACM driver\n");
		}
		free(alias);
		free(path);
	}
}

int cmd_list(int argc, char **argv)
{
	int open = 1, all = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-open")) {
			open = 0;
		} else if (!strcmp(argv[i], "--all")) {
			all = 1;
		} else {
			fprintf(stderr, "usage: seance list [--no-open] [--all]\n"
				"\n"
				"  --no-open   don't open the codecs and drivers to "
				"ask their names\n"
				"  --all       list every DirectShow filter and "
				"DMO registered, not just codecs\n");
			return 2;
		}
	}
	if (runtime_enter()) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}
	list_codecs(open);
	list_acm(open);
	list_filters(all);
	list_dmos(all);
	return 0;
}
