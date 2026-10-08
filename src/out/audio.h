/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio sinks:
 *
 *   AUDIO_OUT_WAV  a WAV file (RF64 past 4 GB), or a stream on stdout
 *   AUDIO_OUT_PCM  the data alone, as raw PCM (or the compressed bytes)
 *   AUDIO_OUT_AVI  an AVI file with one audio stream, a chunk per packet
 *
 * WAV and raw PCM write to stdout when the path is "-". PCM can be
 * converted to another sample format on the way.
 */
#ifndef VFW_OUT_AUDIO_H
#define VFW_OUT_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "media/audio.h"
#include "out/wav.h"
#include "win32.h"

enum audio_output_kind {
	AUDIO_OUT_WAV,
	AUDIO_OUT_PCM,
	AUDIO_OUT_AVI,
};

/* Picks a kind from a name ("wav", "pcm", "avi"). Returns 0 or -1. */
int audio_output_kind_from_name(const char *name, enum audio_output_kind *k);

/*
 * Picks a kind from a path's extension (.wav, .pcm or .raw, .avi).
 * Returns 0, or -1 (with vfw_error()) when it names none.
 */
int audio_output_kind_from_path(const char *path, enum audio_output_kind *k);

const char *audio_output_kind_name(enum audio_output_kind kind);

struct audio_output_options {
	/* Writes PCM in this sample format; AUDIO_NONE writes it as it is. */
	enum audio_sample convert;
	/* AVI: the stream header's rate, scale and sample size (see
	   avi_writer_add_audio); 0 for a constant-rate stream. */
	DWORD rate, scale, sample_size;
	struct wav_writer_options wav;
};

struct audio_output;

/*
 * Opens a sink for audio in format wf (size bytes). opts may be NULL.
 * NULL with vfw_error() on failure.
 */
struct audio_output *audio_output_open(const char *path,
				       enum audio_output_kind kind,
				       const WAVEFORMATEX *wf, size_t size,
				       const struct audio_output_options *opts);

/* The format written, and its size. */
const WAVEFORMATEX *audio_output_format(const struct audio_output *o,
					size_t *size);

/*
 * Writes a packet in the format given to audio_output_open (for PCM,
 * whole sample frames). Returns 0 or -1.
 */
int audio_output_write(struct audio_output *o, const void *data, size_t len);

/* The sample count for a WAV's fact chunk (see wav_writer_set_samples). */
void audio_output_set_samples(struct audio_output *o, uint64_t samples);

/* The bytes written, as the output format has them. */
uint64_t audio_output_bytes(const struct audio_output *o);

/* Finishes and closes. Returns 0 or -1; frees o. */
int audio_output_close(struct audio_output *o);

#endif /* VFW_OUT_AUDIO_H */
