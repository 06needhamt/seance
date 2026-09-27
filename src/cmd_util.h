/*
 * Helpers the CLI commands share: option parsing, format descriptions and
 * opening the codec a command line names.
 */
#ifndef VFW_CMD_UTIL_H
#define VFW_CMD_UTIL_H

#include <stddef.h>

#include "win32.h"

/* Parses a non-negative number for option opt; prints the error. 0 or -1. */
int cli_parse_count(const char *opt, const char *s, unsigned long *out);

/* Parses WxH (each 1..16384) for option opt; prints the error. 0 or -1. */
int cli_parse_size(const char *opt, const char *s, LONG *w, LONG *h);

/* Parses a frame rate: N, N/D or N:D, or a decimal like 29.97. 0 or -1. */
int cli_parse_rate(const char *opt, const char *s, DWORD *rate, DWORD *scale);

/* "320x240, 24 bits, BI_RGB, image 230400 bytes" */
const char *cli_format_desc(const BITMAPINFOHEADER *b, char *buf, size_t size);

/* A codec opened from the command line. */
struct cli_codec {
	char target[4096];	/* the DLL path or FOURCC, as used */
	int is_dll;
	FOURCC handler;		/* the handler it was opened as */
	HIC hic;
	ICINFO info;		/* valid if have_info */
	int have_info;
};

/*
 * Opens the codec `arg` names: a DLL path, or a FOURCC registered in
 * vfw.ini. fourcc is the --fourcc option (or NULL). A DLL is opened as that
 * handler, else as the one vfw.ini gives the DLL, else as guess (when not
 * 0, e.g. an AVI stream's handler) and then as 0; opened without one, it
 * is reopened as the handler it reports. A DLL opened as a handler is
 * registered under it for this process, so the codec can find itself.
 * modes are tried in turn. Errors are printed, with hints.
 * Returns 0, 1 if it doesn't open, or 2 for a bad argument.
 */
int cli_open_codec(const char *arg, const char *fourcc, FOURCC guess,
		   const UINT *modes, size_t nmodes, struct cli_codec *c);

/* The name to show for an open codec: its DLL path, or its driver's. */
const char *cli_codec_name(const struct cli_codec *c);

void cli_close_codec(struct cli_codec *c);

#endif /* VFW_CMD_UTIL_H */
