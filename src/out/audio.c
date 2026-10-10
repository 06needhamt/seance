/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio sinks: WAV, raw and AVI.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "avi/avi.h"
#include "common/error.h"
#include "out/audio.h"
#include "out/output.h"

struct audio_output {
	enum audio_output_kind kind;
	WAVEFORMATEXTENSIBLE pcm;	/* the converted format */
	BYTE *format;
	size_t format_size;
	enum audio_sample from, to;	/* converting, when they differ */
	BYTE *buf;
	size_t cap;
	uint64_t bytes;

	struct wav_writer *wav;
	struct avi_writer *avi;
	int fd, own_fd;
	int failed;
};

static const struct {
	const char *name;
	enum audio_output_kind kind;
} kinds[] = {
	{ "wav", AUDIO_OUT_WAV },
	{ "pcm", AUDIO_OUT_PCM },
	{ "raw", AUDIO_OUT_PCM },
	{ "avi", AUDIO_OUT_AVI },
};

int audio_output_kind_from_name(const char *name, enum audio_output_kind *k)
{
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
		if (!strcasecmp(name, kinds[i].name)) {
			*k = kinds[i].kind;
			return 0;
		}
	vfw_set_error("unknown audio output format \"%s\" (wav, pcm or avi)",
		      name);
	return -1;
}

int audio_output_kind_from_path(const char *path, enum audio_output_kind *k)
{
	const char *dot = strrchr(path, '.');

	if (output_is_null(path)) {
		*k = AUDIO_OUT_PCM;
		return 0;
	}
	if (dot && !strchr(dot, '/') &&
	    !audio_output_kind_from_name(dot + 1, k))
		return 0;
	vfw_set_error("%s: give the audio output a .wav, .pcm or .avi name, "
		      "or a format", path);
	return -1;
}

const char *audio_output_kind_name(enum audio_output_kind kind)
{
	switch (kind) {
	case AUDIO_OUT_WAV: return "WAV";
	case AUDIO_OUT_PCM: return "raw";
	case AUDIO_OUT_AVI: return "AVI";
	}
	return "?";
}

static int write_all(struct audio_output *o, const void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t r = write(o->fd, (const BYTE *)buf + done, len - done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			vfw_set_error("write error: %s", strerror(errno));
			return -1;
		}
		done += r;
	}
	return 0;
}

struct audio_output *audio_output_open(const char *path,
				       enum audio_output_kind kind,
				       const WAVEFORMATEX *wf, size_t size,
				       const struct audio_output_options *opts)
{
	static const struct audio_output_options none;
	struct audio_output *o;

	if (!opts)
		opts = &none;
	if (audio_format_check(wf, size))
		return NULL;
	if (!(o = calloc(1, sizeof(*o)))) {
		vfw_set_error("out of memory");
		return NULL;
	}
	o->kind = kind;
	o->fd = -1;

	o->from = o->to = audio_format_sample(wf, size);
	if (opts->convert != AUDIO_NONE && opts->convert != o->from) {
		DWORD mask = 0;
		if (o->from == AUDIO_NONE) {
			vfw_set_error("only PCM can be converted to %s",
				      audio_sample_name(opts->convert));
			goto fail;
		}
		if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			mask = ((const WAVEFORMATEXTENSIBLE *)wf)->dwChannelMask;
		o->to = opts->convert;
		size = audio_pcm_format(o->to, wf->nChannels, wf->nSamplesPerSec,
					mask, AUDIO_FORM_USUAL, &o->pcm);
		wf = &o->pcm.Format;
	}
	if (!(o->format = malloc(size))) {
		vfw_set_error("out of memory");
		goto fail;
	}
	memcpy(o->format, wf, size);
	o->format_size = size;

	switch (kind) {
	case AUDIO_OUT_WAV:
		if (!(o->wav = wav_writer_open(path, wf, size, &opts->wav)))
			goto fail;
		break;
	case AUDIO_OUT_PCM:
		if (!strcmp(path, "-")) {
			o->fd = 1;
		} else if ((o->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC |
					 O_CLOEXEC, 0644)) < 0) {
			vfw_set_error("%s: %s", path, strerror(errno));
			goto fail;
		} else {
			o->own_fd = 1;
		}
		break;
	case AUDIO_OUT_AVI:
		if (!strcmp(path, "-")) {
			vfw_set_error("AVI files can't be written to stdout");
			goto fail;
		}
		if (!(o->avi = avi_writer_create(path, NULL)) ||
		    avi_writer_add_audio(o->avi, wf, (DWORD)size, opts->rate,
					 opts->scale, opts->sample_size) < 0)
			goto fail;
		break;
	}
	return o;

fail: {
		char msg[512];
		snprintf(msg, sizeof(msg), "%s", vfw_error());
		o->failed = 1;
		audio_output_close(o);
		vfw_set_error("%s", msg);
		return NULL;
	}
}

const WAVEFORMATEX *audio_output_format(const struct audio_output *o,
					size_t *size)
{
	if (size)
		*size = o->format_size;
	return (const WAVEFORMATEX *)o->format;
}

int audio_output_write(struct audio_output *o, const void *data, size_t len)
{
	int r;

	if (o->failed)
		return -1;
	if (o->from != o->to) {
		unsigned ib = audio_sample_bytes(o->from);
		size_t n = audio_converted_size(o->from, o->to, len);
		if (len % ib) {
			vfw_set_error("PCM data of %zu bytes isn't whole samples",
				      len);
			return -1;
		}
		if (n > o->cap) {
			BYTE *b = realloc(o->buf, n);
			if (!b) {
				vfw_set_error("out of memory");
				return -1;
			}
			o->buf = b;
			o->cap = n;
		}
		audio_convert(o->from, data, o->to, o->buf, len / ib);
		data = o->buf;
		len = n;
	}

	switch (o->kind) {
	case AUDIO_OUT_WAV:
		r = wav_writer_write(o->wav, data, len);
		break;
	case AUDIO_OUT_PCM:
		r = write_all(o, data, len);
		break;
	default:
		if (len > 0xffffffffu - 8) {
			vfw_set_error("packet too large for AVI");
			r = -1;
		} else {
			r = avi_writer_write(o->avi, 0, data, (DWORD)len, 1);
		}
	}
	if (r)
		o->failed = 1;
	else
		o->bytes += len;
	return r;
}

void audio_output_set_samples(struct audio_output *o, uint64_t samples)
{
	if (o->wav)
		wav_writer_set_samples(o->wav, samples);
}

uint64_t audio_output_bytes(const struct audio_output *o)
{
	return o->bytes;
}

int audio_output_close(struct audio_output *o)
{
	int r = o->failed ? -1 : 0;

	if (o->wav && wav_writer_close(o->wav))
		r = -1;
	if (o->avi && avi_writer_close(o->avi))
		r = -1;
	if (o->own_fd && close(o->fd) && !r) {
		vfw_set_error("close: %s", strerror(errno));
		r = -1;
	}
	free(o->format);
	free(o->buf);
	free(o);
	return r;
}
