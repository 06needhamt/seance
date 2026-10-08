/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio formats: WAVEFORMATEX and WAVEFORMATEXTENSIBLE, what they hold, how
 * DirectShow and DMOs describe them (AM_MEDIA_TYPE), the PCM forms a
 * decoder is asked for, and conversion between PCM sample formats.
 *
 * A format is a WAVEFORMATEX followed by cbSize bytes, with its size in
 * bytes beside it. Functions taking one check that size, so a format read
 * from a file can be passed as it is.
 */
#ifndef VFW_MEDIA_AUDIO_H
#define VFW_MEDIA_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "win32.h"

/* PCM sample formats, interleaved and little-endian. */
enum audio_sample {
	AUDIO_NONE,	/* not PCM, or a layout not listed here */
	AUDIO_U8,
	AUDIO_S16,
	AUDIO_S24,	/* three bytes each */
	AUDIO_S32,
	AUDIO_F32,
	AUDIO_F64,
};

/* "u8", "s16", "s24", "s32", "f32" or "f64"; "none" for AUDIO_NONE. */
const char *audio_sample_name(enum audio_sample s);

/* The sample format a name gives. Returns 0, or -1 with vfw_error(). */
int audio_sample_from_name(const char *name, enum audio_sample *s);

/* Bytes per sample (one channel), or 0 for AUDIO_NONE. */
unsigned audio_sample_bytes(enum audio_sample s);

/*
 * The format's tag: wFormatTag, or for WAVE_FORMAT_EXTENSIBLE the tag its
 * SubFormat stands for (WAVE_FORMAT_EXTENSIBLE itself when SubFormat isn't
 * one).
 */
WORD audio_format_tag(const WAVEFORMATEX *wf, size_t size);

/*
 * Whether a format is usable: at least 16 bytes, cbSize within size (a
 * 16-byte WAVEFORMAT counts as cbSize 0), channels, a rate and a block
 * size, and for PCM and float, block and byte rate that agree with the
 * channels and sample size. Returns 0, or -1 with vfw_error().
 */
int audio_format_check(const WAVEFORMATEX *wf, size_t size);

/* The PCM sample format of wf, or AUDIO_NONE when it is compressed. */
enum audio_sample audio_format_sample(const WAVEFORMATEX *wf, size_t size);

/* A format tag's name ("PCM", "MP3", ...), or NULL when unknown. */
const char *audio_tag_name(WORD tag);

/*
 * A description of wf for people, e.g. "PCM s16, 2 channels, 44100 Hz" or
 * "MP3 (0x0055), 2 channels, 44100 Hz, 128 kb/s". Returns buf.
 */
#define AUDIO_DESC_SIZE 160
const char *audio_format_desc(const WAVEFORMATEX *wf, size_t size, char *buf);

/* The usual speaker positions of 1 to 8 channels; 0 for more. */
DWORD audio_channel_mask(WORD channels);

/* Which header a PCM format gets. */
enum audio_form {
	AUDIO_FORM_USUAL,	/* extensible for more than two channels or
				   than 16-bit integers, as Windows prefers */
	AUDIO_FORM_PLAIN,	/* a WAVEFORMATEX */
	AUDIO_FORM_EXTENSIBLE,	/* a WAVEFORMATEXTENSIBLE */
};

/*
 * The PCM format for s, channels and rate, as form says; an extensible one
 * names the speakers in mask, or the usual ones when mask is 0. Returns
 * the format's size (18 or 40), or 0 for AUDIO_NONE.
 */
size_t audio_pcm_format(enum audio_sample s, WORD channels, DWORD rate,
			DWORD mask, enum audio_form form,
			WAVEFORMATEXTENSIBLE *out);

/*
 * The PCM types a decoder of `in` is offered, in order: want alone when it
 * isn't AUDIO_NONE, else 16-bit, 24-bit and 32-bit integers, 32-bit
 * float and 8-bit; each plain and then extensible up to two channels, the
 * other way round above, at the input's channels and rate.
 * Returns how many were written (at most max).
 */
size_t audio_output_preferences(const WAVEFORMATEX *in, size_t size,
				enum audio_sample want,
				WAVEFORMATEXTENSIBLE *out, size_t max);

/*
 * As an AM_MEDIA_TYPE (also a DMO_MEDIA_TYPE): MEDIATYPE_Audio, the subtype
 * of the tag (or the extensible SubFormat), FORMAT_WaveFormatEx, and the
 * format with its extra bytes in COM task memory; fixed-size samples of a
 * block for PCM and float. A 16-byte format is given as 18 bytes.
 */
HRESULT audio_wfx_to_mt(const WAVEFORMATEX *wf, size_t size,
			AM_MEDIA_TYPE *mt);

/*
 * The ways a stream's format can be offered to a decoder, one per variant,
 * in the order to try them: as audio_wfx_to_mt gives it, then the forms
 * decoders register some formats by: AC-3 as MEDIASUBTYPE_DOLBY_AC3, DTS
 * as MEDIASUBTYPE_DTS, MPEG audio as MEDIASUBTYPE_MPEG1Audio and
 * MPEG2_AUDIO, AAC under its other tags as MEDIASUBTYPE_RAW_AAC1, and
 * Vorbis (its three headers Xiph-laced after the WAVEFORMATEX, as AVI files
 * have them) as Vorbis2 with a VORBISFORMAT2. A compressed format wrapped
 * in a WAVEFORMATEXTENSIBLE (FFmpeg writes more than two channels so) is
 * offered unwrapped first, a WAVEFORMATEX of its tag with the codec's own
 * extra bytes, as decoders expect it, then as it is. S_OK with the type,
 * S_FALSE when there are no more variants.
 */
HRESULT audio_input_variant(const WAVEFORMATEX *wf, size_t size,
			    unsigned variant, AM_MEDIA_TYPE *mt);

/*
 * An audio media type's format, malloc()ed, as at least 18 bytes with
 * cbSize trimmed to what is there. E_INVALIDARG when it isn't audio with
 * FORMAT_WaveFormatEx.
 */
HRESULT audio_mt_to_wfx(const AM_MEDIA_TYPE *mt, WAVEFORMATEX **wf,
			size_t *size);

/*
 * Converts count samples (frames times channels) from one PCM sample
 * format to another. Integers widen exactly and narrow by dropping low
 * bits; floats map [-1, 1) onto the integer range, clipped and rounded.
 * out may be in when its samples are no larger; otherwise they mustn't
 * overlap.
 */
void audio_convert(enum audio_sample from, const void *in,
		   enum audio_sample to, void *out, size_t count);

/* The size of `bytes` bytes of `from` samples once converted to `to`. */
size_t audio_converted_size(enum audio_sample from, enum audio_sample to,
			    size_t bytes);

/*
 * A time in 100 ns units: value * scale / rate seconds, without overflow
 * for any 64-bit value and 32-bit rate and scale.
 */
int64_t audio_ticks(uint64_t value, DWORD scale, DWORD rate);

#endif /* VFW_MEDIA_AUDIO_H */
