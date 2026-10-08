/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * decode and encode for audio: a DirectShow filter (or a DMO) between an
 * audio source and an audio sink.
 */
#ifndef VFW_CMD_AUDIO_H
#define VFW_CMD_AUDIO_H

#include "cmd_util.h"
#include "in/audio.h"
#include "media/audio.h"
#include "win32.h"

struct audio_codec_job {
	const char *codec;		/* the server (a DLL), or "auto" (decode) */
	const CLSID *clsid;		/* the class in it, or NULL */
	CLSID clsid_value;		/* somewhere for clsid to point */
	const char *in, *out;
	const char *format;		/* the output kind, else out's extension */
	struct audio_source_options source;
	struct cli_position start, duration;
	int has_start, has_duration;
	enum audio_sample sample;	/* decode: the PCM to write */
	const char *state, *save_state;
	int keep_going;			/* decode: past packets that fail */
	long quality;			/* encode: 0 to 10000, or -1 */
	unsigned long bitrate;		/* encode: kbit/s, or 0 */
};

/* Exit statuses: 0, 1 for a failure, 2 for a bad argument. */
int decode_audio(const struct audio_codec_job *j);
int encode_audio(const struct audio_codec_job *j);

/* The output kinds only audio has, for cli_wants_audio. */
extern const char *const audio_only_kinds[];

#endif /* VFW_CMD_AUDIO_H */
