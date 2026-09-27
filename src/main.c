/*
 * vfw-harness: load and run 32-bit Video for Windows codecs on Linux.
 */
#include <stdio.h>
#include <string.h>

#include "commands.h"
#include "runtime/crash.h"
#include "runtime/lowmem.h"

#ifndef VFW_HARNESS_VERSION
#define VFW_HARNESS_VERSION "0.1.0-dev"
#endif

struct command {
	const char *name;
	const char *args;
	const char *summary;
	int (*run)(int argc, char **argv);	/* NULL until implemented */
};

static const struct command commands[] = {
	{ "info", "<dll>",
	  "show PE headers, sections, exports, imports and resources",
	  cmd_info },
	{ "avi", "<in.avi> [--chunks]",
	  "show an AVI file's headers, streams and index", cmd_avi },
	{ "convert", "<in.avi> -o <out>",
	  "write uncompressed video as Y4M, BMP or raw frames", cmd_convert },
	{ "list", "[--no-open]",
	  "list the codecs registered in vfw.ini", cmd_list },
	{ "probe", "<dll|fourcc> [in.avi]",
	  "show codec info and the formats it accepts", cmd_probe },
	{ "decode", "<dll|fourcc|auto> <in.avi> -o <out>",
	  "decompress a video to Y4M, BMP or raw frames", cmd_decode },
	{ "encode", "<dll|fourcc> <in.y4m|avi|bmp|raw> -o <out.avi>",
	  "compress video to an AVI", cmd_encode },
};

static void usage(FILE *out)
{
	fprintf(out,
		"usage: vfw-harness <command> [args]\n"
		"       vfw-harness --help | --version\n"
		"\n"
		"commands:\n");
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		const struct command *c = &commands[i];
		fprintf(out, "  %-7s %-36s %s%s\n", c->name, c->args,
			c->summary, c->run ? "" : " (not implemented yet)");
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 2;
	}

	const char *cmd = argv[1];
	if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h") ||
	    !strcmp(cmd, "help")) {
		usage(stdout);
		return 0;
	}
	if (!strcmp(cmd, "--version")) {
		printf("vfw-harness %s\n", VFW_HARNESS_VERSION);
		return 0;
	}

	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		const struct command *c = &commands[i];
		if (strcmp(cmd, c->name))
			continue;
		if (!c->run) {
			fprintf(stderr, "vfw-harness: '%s' is not implemented yet\n",
				cmd);
			return 2;
		}
		/* Crashes and hangs in DLL code get reported. */
		if (crash_install())
			return 2;
		/* DLL code only ever sees memory below 2 GB. */
		return lowmem_run(c->run, argc - 1, argv + 1);
	}

	fprintf(stderr, "vfw-harness: unknown command '%s'\n\n", cmd);
	usage(stderr);
	return 2;
}
