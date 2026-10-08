/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * WAV writer: RIFF WAVE, becoming RF64 when the file passes 4 GB.
 *
 * Layout: RIFF WAVE, a JUNK chunk that RF64 turns into its ds64 chunk,
 * fmt (16 bytes for plain PCM, else the WAVEFORMATEX and its extra bytes),
 * fact for formats other than integer PCM, then data. Sizes are filled in
 * on close. On standard output ("-") or another pipe they can't be, and
 * are left as 0xffffffff, which readers take as "to the end".
 */
#ifndef VFW_OUT_WAV_H
#define VFW_OUT_WAV_H

#include <stddef.h>
#include <stdint.h>

#include "win32.h"

enum wav_rf64 {
	WAV_RF64_AUTO,		/* when it has to be */
	WAV_RF64_ALWAYS,
	WAV_RF64_NEVER,		/* no JUNK chunk; over 4 GB fails */
};

struct wav_writer_options {
	enum wav_rf64 rf64;
};

struct wav_writer;

/*
 * Starts a WAV file whose format is wf (size bytes: a WAVEFORMATEX and its
 * extra bytes, or a 16-byte WAVEFORMAT). opts may be NULL. NULL with
 * vfw_error() on failure.
 */
struct wav_writer *wav_writer_open(const char *path, const WAVEFORMATEX *wf,
				   size_t size,
				   const struct wav_writer_options *opts);

/* Appends data. Returns 0 or -1. */
int wav_writer_write(struct wav_writer *w, const void *data, size_t len);

/*
 * The number of samples (per channel) for the fact chunk of a compressed
 * format; for PCM and float it is worked out from the data. A compressed
 * format whose count isn't given gets no fact chunk.
 */
void wav_writer_set_samples(struct wav_writer *w, uint64_t samples);

/* The bytes of data written. */
uint64_t wav_writer_bytes(const struct wav_writer *w);

/* Finishes the file. Returns 0 or -1; frees w. */
int wav_writer_close(struct wav_writer *w);

#endif /* VFW_OUT_WAV_H */
