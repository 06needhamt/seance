/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance: load and run 32-bit Video for Windows codecs on Linux.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "handoff.h"
#include "runtime/crash.h"
#include "runtime/lowmem.h"
#include "win32/k32.h"

#ifndef SEANCE_VERSION
#define SEANCE_VERSION "0.1.0-dev"
#endif

struct command {
	const char *name;
	const char *args;
	const char *summary;
	int (*run)(int argc, char **argv);	/* NULL until implemented */
	int takes_codec;	/* its first argument names one */
};

static const struct command commands[] = {
	{ "info", "<dll>",
	  "show PE headers, sections, exports, imports and resources",
	  cmd_info, 0 },
	{ "avi", "<in.avi> [--chunks]",
	  "show an AVI file's headers, streams and index", cmd_avi, 0 },
	{ "convert", "<in.avi|wav> -o <out>",
	  "write uncompressed video as Y4M, BMP or raw frames, or audio as "
	  "WAV, PCM or AVI", cmd_convert, 0 },
	{ "list", "[--no-open] [--all]",
	  "list the codecs and DirectShow filters registered", cmd_list, 0 },
	{ "probe", "<dll|fourcc> [in.avi]",
	  "show codec info and the formats it accepts", cmd_probe, 1 },
	{ "decode", "<dll|fourcc|auto> <in.avi|wav> -o <out>",
	  "decompress video to Y4M, BMP or raw frames, or audio to WAV or PCM",
	  cmd_decode, 1 },
	{ "encode", "<dll|fourcc> <in.y4m|avi|bmp|raw|wav> -o <out.avi>",
	  "compress video or audio to an AVI", cmd_encode, 1 },
	{ "sweep", "<dir|dll>... [--markdown <f>]",
	  "try every codec in a directory: encode, decode, PSNR",
	  cmd_sweep, 0 },
	{ "register", "[--dry-run] <filter.ax>",
	  "register a DirectShow filter server's filters", cmd_register, 1 },
	{ "unregister", "<filter.ax>",
	  "unregister a DirectShow filter server's filters", cmd_register, 1 },
};

/* Prints the command summary. */
static void usage(FILE *out)
{
	fprintf(out,
		"usage: seance <command> [args]\n"
		"       seance --help | --version\n"
		"\n"
		"commands:\n");
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		const struct command *c = &commands[i];
		fprintf(out, "  %-10s %-36s %s%s\n", c->name, c->args,
			c->summary, c->run ? "" : " (not implemented yet)");
	}
}

/*
 * Hands off to the sibling binary if the codec is the other architecture,
 * then runs the command on a low-memory stack.
 */
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
		printf("seance %s\n", SEANCE_VERSION);
		return 0;
	}

	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		const struct command *c = &commands[i];
		if (strcmp(cmd, c->name))
			continue;
		if (!c->run) {
			fprintf(stderr, "seance: '%s' is not implemented yet\n",
				cmd);
			return 2;
		}
		/*
		 * A codec of the other architecture runs in the other binary:
		 * one named by path goes there now, one found later (a FOURCC
		 * in vfw.ini, a CLSID) when it is first mapped. "decode auto"
		 * tries every codec and keeps to this binary's.
		 */
		handoff_init(argc, argv);
		/*
		 * The codec is the first word that isn't an option or "auto"
		 * (the value of --fourcc auto is no file). Only the positional
		 * "auto" of decode keeps to this binary's codecs.
		 */
		const char *codec = NULL;
		for (int a = 2; c->takes_codec && a < argc && !codec; a++)
			if (argv[a][0] != '-' && strcmp(argv[a], "auto"))
				codec = argv[a];
		if (codec)
			handoff_if_foreign(codec);
		/* ... wherever it stands among the options. */
		int auto_target = 0;
		for (int a = 2; a < argc; a++)
			if (!strcmp(argv[a], "auto") &&
			    strcmp(argv[a - 1], "--fourcc") &&
			    strcmp(argv[a - 1], "--clsid"))
				auto_target = 1;
		if (c->takes_codec && !auto_target)
			handoff_arm();
		/* Crashes and hangs in DLL code get reported. */
		if (crash_install())
			return 2;
		/* The date and time DLLs see, when not the host's. */
		const char *date = getenv("VFW_DATE");
		if (win_clock_set(date)) {
			fprintf(stderr, "seance: VFW_DATE wants a date, "
				"YYYY-MM-DD[THH:MM[:SS]], not '%s'\n", date);
			return 2;
		}
		/* DLL code only ever sees memory below 2 GB. */
		return lowmem_run(c->run, argc - 1, argv + 1);
	}

	fprintf(stderr, "seance: unknown command '%s'\n\n", cmd);
	usage(stderr);
	return 2;
}
