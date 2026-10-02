/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Frame sources for encoding: uncompressed video from
 *
 *   Y4M      YUV4MPEG2, 4:2:0 (any chroma siting) as I420, or mono as Y800;
 *            "-" reads standard input
 *   AVI      an uncompressed video stream, frames as stored; RLE and
 *            streams with palette changes are turned into BGR24
 *   BMP      one file, or a numbered sequence given as a pattern with %d,
 *            %u or %0Nd (as the BMP writer takes), from 0 or 1 up
 *   raw      frames back to back in a named format at a given size (RGB
 *            ones top-down with unpadded rows, as .bgr files and FFmpeg's
 *            rawvideo are)
 *
 * Each gives frames in its own format, a BITMAPINFOHEADER (plus palette or
 * masks) that stays the same for the whole source.
 */
#ifndef VFW_IN_SOURCE_H
#define VFW_IN_SOURCE_H

#include <stddef.h>

#include "win32.h"

enum source_kind {
	SOURCE_AUTO,	/* from the name and contents */
	SOURCE_Y4M,
	SOURCE_AVI,
	SOURCE_BMP,
	SOURCE_RAW,
};

struct source_options {
	enum source_kind kind;
	const char *raw_format;	/* raw: "I420", "BGR24", ... (vfw_std_format) */
	int width, height;	/* raw: the frame size */
	int stream;		/* AVI: the stream, or -1 for the first video */
};

struct source;

/* Opens a source. NULL with vfw_error() on failure. */
struct source *source_open(const char *path, const struct source_options *o);
void source_close(struct source *s);

/* The frames' format and its size in bytes (header plus colours). */
const BITMAPINFOHEADER *source_format(const struct source *s, size_t *size);

/* A name for the format, e.g. "I420" or "RGB24 bottom-up". */
const char *source_format_name(const struct source *s);

enum source_kind source_kind(const struct source *s);
const char *source_kind_name(enum source_kind kind);

/* The frame rate, or 0 / 0 if the source doesn't say (BMP, raw). */
void source_rate(const struct source *s, DWORD *rate, DWORD *scale);

/* The number of frames, or -1 when it isn't known in advance. */
long source_frames(const struct source *s);

/*
 * Reads the next frame into an internal buffer, valid until the next call.
 * Returns 1 with *frame and *len set, 0 at the end, -1 with vfw_error().
 */
int source_read(struct source *s, const BYTE **frame, size_t *len);

#endif /* VFW_IN_SOURCE_H */
