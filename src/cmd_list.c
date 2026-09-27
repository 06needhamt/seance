/*
 * vfw-harness list [--no-open]: the codecs registered in vfw.ini.
 */
#include <stdio.h>
#include <string.h>

#include "commands.h"
#include "common/error.h"
#include "common/fourcc.h"
#include "common/unicode.h"
#include "runtime/runtime.h"
#include "vfw/icm.h"
#include "vfw/registry.h"

int cmd_list(int argc, char **argv)
{
	int open = 1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-open")) {
			open = 0;
		} else {
			fprintf(stderr, "usage: vfw-harness list [--no-open]\n");
			return 2;
		}
	}
	if (runtime_enter()) {
		fprintf(stderr, "vfw-harness: %s\n", vfw_error());
		return 1;
	}

	printf("Codecs in %s:\n", vfw_ini_path());
	const struct vfw_codec *c = vfw_enum(0, 0);
	if (!c) {
		printf("  none. Add a [drivers32] section with lines such as\n"
		       "  vidc.XXXX=/path/to/codec.dll (or set VFW_INI).\n");
		return 0;
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
	return 0;
}
