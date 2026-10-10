/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Frame sinks: Y4M, BMP sequences and raw streams.
 *
 *   OUT_Y4M  YUV4MPEG2, 4:2:0 (C420jpeg)
 *   OUT_BMP  one 24-bit BMP per frame; the path holds a frame number
 *            pattern: exactly one %d, %u or %0Nd (and %% for a '%')
 *   OUT_YUV  raw I420 frames back to back
 *   OUT_BGR  raw top-down BGR24 frames back to back
 *   OUT_RAW  the input frames verbatim (the DIB bits as stored or decoded)
 *
 * Y4M and the raw kinds write to stdout when the path is "-".
 */
#ifndef VFW_OUT_OUTPUT_H
#define VFW_OUT_OUTPUT_H

#include <stddef.h>

#include "out/image.h"
#include "win32.h"

enum output_kind {
	OUT_Y4M,
	OUT_BMP,
	OUT_YUV,
	OUT_BGR,
	OUT_RAW,
};

/* Picks a kind from a name ("y4m", "bmp", ...). Returns 0 or -1. */
int output_kind_from_name(const char *name, enum output_kind *kind);

/*
 * Picks a kind from a path's extension, raw frames for the null device.
 * Returns 0 or -1.
 */
int output_kind_from_path(const char *path, enum output_kind *kind);

/* Whether path is the null device (/dev/null, or another name for it). */
int output_is_null(const char *path);

const char *output_kind_name(enum output_kind kind);

/*
 * Checks a BMP path pattern and formats it for frame n into buf. A pattern
 * without a number is accepted only when single is set (one frame).
 * Returns 0 or -1.
 */
int output_bmp_path(const char *pattern, unsigned long n, int single,
		    char *buf, size_t size);

struct output;

/*
 * Opens a sink for width x height frames at rate / scale frames per
 * second. For OUT_BMP, single says only one frame will be written.
 */
struct output *output_open(const char *path, enum output_kind kind,
			   int width, int height, DWORD rate, DWORD scale,
			   int single);

/*
 * Writes one frame, converting it with conv. raw / raw_len are the frame
 * as given, which OUT_RAW writes as is. Returns 0 or -1.
 */
int output_write(struct output *o, struct image_conv *conv, const void *raw,
		 size_t raw_len);

/* Writes the last frame again (for dropped, zero-length frames). */
int output_repeat(struct output *o);

/* The number of frames written. */
unsigned long output_frames(const struct output *o);

/* Flushes and closes. Returns 0 or -1; frees o. */
int output_close(struct output *o);

#endif /* VFW_OUT_OUTPUT_H */
