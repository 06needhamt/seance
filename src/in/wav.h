/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * WAV files: RIFF WAVE, and RF64 and BW64 for files over 4 GB. Read front
 * to back without seeking, so standard input works ("-").
 *
 * The format is the fmt chunk as found, given as at least a WAVEFORMATEX
 * (a 16-byte WAVEFORMAT gets cbSize 0) with cbSize trimmed to the bytes
 * the chunk has. A data size of 0 or 0xffffffff, as streaming writers
 * leave it, means up to the end of the file.
 */
#ifndef VFW_IN_WAV_H
#define VFW_IN_WAV_H

#include <stddef.h>
#include <stdint.h>

#include "win32.h"

struct wav_reader;

/* Opens a WAV file and reads up to its data. NULL with vfw_error(). */
struct wav_reader *wav_open(const char *path);
void wav_close(struct wav_reader *w);

/* The format, and its size in bytes (18 or more). */
const WAVEFORMATEX *wav_format(const struct wav_reader *w, size_t *size);

/* The data's size in bytes, or UINT64_MAX when it runs to the end. */
uint64_t wav_data_size(const struct wav_reader *w);

/* The fact chunk's (or ds64's) sample count, or -1 if there was none. */
int64_t wav_fact_samples(const struct wav_reader *w);

/* Whether the file is an RF64 (or BW64) one. */
int wav_is_rf64(const struct wav_reader *w);

/*
 * Reads up to len bytes of the data. Returns the number read, 0 at the
 * end, or -1 with vfw_error().
 */
long wav_read(struct wav_reader *w, void *buf, size_t len);

/* Whether the data ended before the size its header gave. */
int wav_truncated(const struct wav_reader *w);

#endif /* VFW_IN_WAV_H */
