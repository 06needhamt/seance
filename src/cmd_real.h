/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Decoding RealMedia files with RealPlayer's codec DLLs, for `seance
 * decode`.
 */
#ifndef VFW_CMD_REAL_H
#define VFW_CMD_REAL_H

struct real_decode_options {
	const char *out;		/* the output path */
	const char *format;		/* its kind, or NULL for its extension */
	int stream;			/* the stream, or -1 for the codec's */
	unsigned long frames;		/* video: at most this many */
	int keep_going;			/* past frames the decoder refuses */
};

/* Decodes path's stream with dll. Returns the exit status. */
int real_decode(const char *dll, const char *path,
		const struct real_decode_options *o);

#endif /* VFW_CMD_REAL_H */
