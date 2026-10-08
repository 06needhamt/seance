/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio sources: a stream of packets in one format, from
 *
 *   WAV      PCM, float or a compressed format tag; "-" reads standard
 *            input
 *   AVI      an audio stream, one packet per chunk
 *   raw      PCM in a named sample format, channel count and rate
 *
 * WAV and raw data come in whole blocks (nBlockAlign), so a packet never
 * splits a PCM sample frame or a block of a compressed format: PCM in runs
 * of many frames, a compressed format a block (its packet) at a time, or
 * for one whose block is under 64 bytes (a byte for an MP3 stream), about
 * 4 KB at a time. Only the last packet of compressed audio may end in part
 * of a block.
 */
#ifndef VFW_IN_AUDIO_H
#define VFW_IN_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "media/audio.h"
#include "win32.h"

enum audio_source_kind {
	AUDIO_SOURCE_AUTO,	/* from the name and contents */
	AUDIO_SOURCE_WAV,
	AUDIO_SOURCE_AVI,
	AUDIO_SOURCE_RAW,
};

struct audio_source_options {
	enum audio_source_kind kind;
	int stream;			/* AVI: the stream, or -1: the first audio */
	enum audio_sample raw_sample;	/* raw: the sample format */
	unsigned channels;		/* raw */
	DWORD rate;			/* raw */
};

struct audio_source;

/* Opens a source. NULL with vfw_error() on failure. */
struct audio_source *audio_source_open(const char *path,
				       const struct audio_source_options *o);
void audio_source_close(struct audio_source *s);

/* The format and its size in bytes (at least 18). */
const WAVEFORMATEX *audio_source_format(const struct audio_source *s,
					size_t *size);

enum audio_source_kind audio_source_kind(const struct audio_source *s);
const char *audio_source_kind_name(enum audio_source_kind kind);

/*
 * The kind a path names by its extension (.wav, .avi, .pcm or .raw), or
 * AUDIO_SOURCE_AUTO.
 */
enum audio_source_kind audio_source_kind_of_path(const char *path);

/*
 * What a file holds, by its first bytes: AUDIO_SOURCE_WAV, AUDIO_SOURCE_AVI,
 * or AUDIO_SOURCE_AUTO for anything else (or a file that can't be read).
 */
enum audio_source_kind audio_source_sniff(const char *path);

/* An AVI source's stream header, or NULL. */
const AVISTREAMHEADER *audio_source_avi_header(const struct audio_source *s);

/* The length in 100 ns units, or -1 when it isn't known in advance. */
int64_t audio_source_duration(const struct audio_source *s);

/*
 * Reads the next packet into an internal buffer, valid until the next
 * call. *time is when it starts, in 100 ns units from the start of the
 * stream, or -1 when that can't be told (a compressed WAV without a byte
 * rate). Returns 1, 0 at the end, or -1 with vfw_error().
 */
int audio_source_read(struct audio_source *s, const BYTE **data,
		      size_t *len, int64_t *time);

/*
 * When the packet audio_source_read last gave ends (where the next would
 * start), as *time is given; -1 when it can't be told.
 */
int64_t audio_source_packet_end(const struct audio_source *s);

/* Whether the data ended early, or PCM in the middle of a sample frame. */
int audio_source_truncated(const struct audio_source *s);

#endif /* VFW_IN_AUDIO_H */
