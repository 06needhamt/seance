/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ACM drivers as transforms, for xform.h: what a driver does (decodes a
 * format tag to PCM, or encodes PCM to one), and a stream through it fed
 * the way a DirectShow filter or a DMO is.
 *
 * A stream converts whole blocks of what it is given and keeps the rest
 * for the next push (ACM_STREAMCONVERTF_BLOCKALIGN); the end flushes it
 * (ACM_STREAMCONVERTF_END). Its output buffers are what the driver asks
 * for (acmStreamSize), but never less than 64 KB or four blocks, since a
 * driver's answer can be too small. Compressed output goes out a block at
 * a time (blocks under 1 KB as many as make 1 KB), untimed: ACM gives no
 * times, and a block's length in samples is the format's business. PCM output is timed from the first input given.
 *
 * ACM's errors are carried as HRESULTs of their own (ACM_HRESULT), which
 * dshow_hr_name names ("ACMERR_NOTPOSSIBLE").
 */
#ifndef VFW_ACM_ACM_H
#define VFW_ACM_ACM_H

#include <stddef.h>

#include "win32.h"

#define ACM_HRESULT(mmr) ((HRESULT)(0xa0ac0000u | ((mmr) & 0xffff)))
#define ACM_IS_HRESULT(hr) (((DWORD)(hr) & 0xffff0000u) == 0xa0ac0000u)
#define ACM_MMRESULT(hr) ((MMRESULT)((DWORD)(hr) & 0xffff))

/*
 * Whether the DLL at path (or a bare name the loader finds) is an ACM
 * driver that decodes (encoder 0) a format tag to PCM, or encodes PCM to
 * one; want, when not 0, is the tag it must do it for. S_OK and the tag,
 * CLASS_E_CLASSNOTAVAILABLE if it isn't or doesn't, VFW_E_DUPLICATE_NAME
 * for an encoder of several tags when want is 0.
 */
HRESULT acm_find(const char *path, int encoder, WORD want, WORD *tag);

/*
 * A driver in a line: "TSTA (seance TSTA test codec), codec: 0x0001 PCM,
 * 0x7354 TSTA". 0, or -1 if it gives no details.
 */
int acm_describe(HACMDRIVERID id, char *buf, size_t size);

/* What its support flags say: "codec, converter, filter, async". */
const char *acm_support_names(DWORD support, char *buf, size_t size);

struct acm_codec;

/* Opens the driver (msacm_add_path, acmDriverOpen). */
HRESULT acm_codec_open(const char *path, struct acm_codec **c);
/* Closes it; its streams must be closed first. */
void acm_codec_close(struct acm_codec *c);

/* One output: a block of compressed audio, or PCM. */
struct acm_output {
	const BYTE *data;
	size_t size;
	int timed;
	REFERENCE_TIME start, stop;
};

typedef HRESULT (*acm_output_fn)(void *opaque, const struct acm_output *o);

struct acm_stream;

/*
 * Opens a stream from in to out. out_size 0 makes out partial, its tag
 * alone: the driver suggests the rest (acmFormatSuggest), or, with a bit
 * rate (bits per second), the format of that tag for in's rate and
 * channels whose data rate is nearest is taken. VFW_E_TYPE_NOT_ACCEPTED
 * when the driver won't.
 */
HRESULT acm_stream_open(struct acm_codec *c, const WAVEFORMATEX *in,
			size_t in_size, const WAVEFORMATEX *out,
			size_t out_size, DWORD bitrate, acm_output_fn callback,
			void *opaque, struct acm_stream **s);
/* The output format, as opened. */
const WAVEFORMATEX *acm_stream_format(const struct acm_stream *s,
				      size_t *size);
/* Converts what it can of data (and what was kept); start times PCM out. */
HRESULT acm_stream_push(struct acm_stream *s, const void *data, size_t size,
			REFERENCE_TIME start);
/* Converts the rest, flushing the driver. */
HRESULT acm_stream_end(struct acm_stream *s);
/* Forgets what was kept, to begin again (acmStreamReset). */
HRESULT acm_stream_reset(struct acm_stream *s);
void acm_stream_close(struct acm_stream *s);

#endif /* VFW_ACM_ACM_H */
