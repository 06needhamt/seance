/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio sources: WAV files, AVI audio streams and raw PCM.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "avi/avi.h"
#include "common/error.h"
#include "in/audio.h"
#include "in/wav.h"

#define PACKET_BYTES 65536	/* WAV and raw packets: about this much */

struct audio_source {
	enum audio_source_kind kind;
	BYTE *format;
	size_t format_size;
	WORD block;
	int pcm;		/* PCM or float, not compressed */
	DWORD byte_rate;
	int64_t duration;
	int truncated;

	BYTE *buf;
	size_t cap;
	uint64_t pos;		/* WAV, raw: data bytes read */

	struct wav_reader *wav;
	int fd, own_fd;		/* raw */
	struct avi_file *avi;
	const struct avi_stream *stream;
	size_t chunk;
	int64_t end;		/* where the last packet read ends */
};

const char *audio_source_kind_name(enum audio_source_kind kind)
{
	switch (kind) {
	case AUDIO_SOURCE_WAV: return "WAV";
	case AUDIO_SOURCE_AVI: return "AVI";
	case AUDIO_SOURCE_RAW: return "raw PCM";
	default:               return "audio";
	}
}

enum audio_source_kind audio_source_kind_of_path(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (!dot || strchr(dot, '/'))
		return AUDIO_SOURCE_AUTO;
	if (!strcasecmp(dot, ".wav"))
		return AUDIO_SOURCE_WAV;
	if (!strcasecmp(dot, ".avi"))
		return AUDIO_SOURCE_AVI;
	if (!strcasecmp(dot, ".pcm") || !strcasecmp(dot, ".raw"))
		return AUDIO_SOURCE_RAW;
	return AUDIO_SOURCE_AUTO;
}

enum audio_source_kind audio_source_sniff(const char *path)
{
	BYTE b[12];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n = fd < 0 ? -1 : pread(fd, b, sizeof(b), 0);

	if (fd >= 0)
		close(fd);
	if (n == (ssize_t)sizeof(b) && !memcmp(b + 8, "WAVE", 4) &&
	    (!memcmp(b, "RIFF", 4) || !memcmp(b, "RF64", 4) ||
	     !memcmp(b, "BW64", 4)))
		return AUDIO_SOURCE_WAV;
	if (n == (ssize_t)sizeof(b) && !memcmp(b, "RIFF", 4) &&
	    !memcmp(b + 8, "AVI ", 4))
		return AUDIO_SOURCE_AVI;
	return AUDIO_SOURCE_AUTO;
}

static int set_format(struct audio_source *s, const WAVEFORMATEX *wf,
		      size_t size)
{
	if (!(s->format = malloc(size))) {
		vfw_set_error("out of memory");
		return -1;
	}
	memcpy(s->format, wf, size);
	s->format_size = size;
	s->block = wf->nBlockAlign;
	s->byte_rate = wf->nAvgBytesPerSec;
	s->pcm = audio_format_sample(wf, size) != AUDIO_NONE;
	return 0;
}

static int64_t bytes_to_time(const struct audio_source *s, uint64_t bytes)
{
	const WAVEFORMATEX *wf = (const WAVEFORMATEX *)s->format;

	if (audio_format_sample(wf, s->format_size) != AUDIO_NONE)
		return audio_ticks(bytes / s->block, 1, wf->nSamplesPerSec);
	if (!s->byte_rate)
		return -1;
	return audio_ticks(bytes, 1, s->byte_rate);
}

static int open_wav(struct audio_source *s, const char *path)
{
	size_t size;
	const WAVEFORMATEX *wf;
	uint64_t data;
	int64_t fact;

	if (!(s->wav = wav_open(path)))
		return -1;
	wf = wav_format(s->wav, &size);
	if (set_format(s, wf, size))
		return -1;
	data = wav_data_size(s->wav);
	fact = wav_fact_samples(s->wav);
	s->duration = -1;
	if (audio_format_sample(wf, size) != AUDIO_NONE) {
		if (data != UINT64_MAX)
			s->duration = bytes_to_time(s, data / s->block * s->block);
	} else if (fact >= 0) {
		s->duration = audio_ticks((uint64_t)fact, 1, wf->nSamplesPerSec);
	} else if (data != UINT64_MAX) {
		s->duration = bytes_to_time(s, data);
	}
	return 0;
}

static int open_avi(struct audio_source *s, const char *path, int stream)
{
	const WAVEFORMATEX *wf;
	int n;

	if (!(s->avi = avi_open(path)))
		return -1;
	n = stream < 0 ? avi_audio_stream(s->avi) :
	    (unsigned)stream < s->avi->nstreams ? stream : -1;
	if (n < 0) {
		vfw_set_error("%s: %s", path, stream < 0 ? "no audio stream" :
			      "no such stream");
		return -1;
	}
	s->stream = &s->avi->streams[n];
	if (!(wf = avi_stream_wfx(s->stream))) {
		vfw_set_error("%s: stream %d is not audio", path, n);
		return -1;
	}
	if (set_format(s, wf, s->stream->format_size))
		return -1;
	s->duration = avi_stream_time(s->stream, s->stream->length) -
		      avi_stream_time(s->stream, 0);
	s->truncated = s->avi->truncated;
	return 0;
}

static int open_raw(struct audio_source *s, const char *path,
		    const struct audio_source_options *o)
{
	WAVEFORMATEXTENSIBLE wfx;
	size_t size;
	struct stat st;

	if (o->raw_sample == AUDIO_NONE || !o->channels || !o->rate ||
	    o->channels > 64) {
		vfw_set_error("raw PCM needs a sample format, a channel count "
			      "(1 to 64) and a rate");
		return -1;
	}
	size = audio_pcm_format(o->raw_sample, (WORD)o->channels, o->rate, 0,
				AUDIO_FORM_USUAL, &wfx);
	if (set_format(s, &wfx.Format, size))
		return -1;
	if (!strcmp(path, "-")) {
		s->fd = 0;
	} else if ((s->fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
		vfw_set_error("%s: %s", path, strerror(errno));
		return -1;
	} else {
		s->own_fd = 1;
	}
	s->duration = -1;
	if (!fstat(s->fd, &st) && S_ISREG(st.st_mode))
		s->duration = bytes_to_time(s, (uint64_t)st.st_size /
					    s->block * s->block);
	return 0;
}

struct audio_source *audio_source_open(const char *path,
				       const struct audio_source_options *o)
{
	static const struct audio_source_options none = { .stream = -1 };
	struct audio_source *s = calloc(1, sizeof(*s));
	int r;

	if (!o)
		o = &none;
	if (!s) {
		vfw_set_error("out of memory");
		return NULL;
	}
	s->fd = -1;
	s->kind = o->kind;
	if (s->kind == AUDIO_SOURCE_AUTO)
		s->kind = !strcmp(path, "-") ? AUDIO_SOURCE_WAV :
			  audio_source_sniff(path);
	if (s->kind == AUDIO_SOURCE_AUTO)
		s->kind = audio_source_kind_of_path(path);

	switch (s->kind) {
	case AUDIO_SOURCE_WAV:
		r = open_wav(s, path);
		break;
	case AUDIO_SOURCE_AVI:
		r = open_avi(s, path, o->stream);
		break;
	case AUDIO_SOURCE_RAW:
		r = open_raw(s, path, o);
		break;
	default:
		vfw_set_error("%s: not a WAV or AVI file (raw PCM needs its "
			      "format given)", path);
		r = -1;
	}
	if (r) {
		audio_source_close(s);
		return NULL;
	}
	return s;
}

void audio_source_close(struct audio_source *s)
{
	if (!s)
		return;
	wav_close(s->wav);
	avi_close(s->avi);
	if (s->own_fd)
		close(s->fd);
	free(s->format);
	free(s->buf);
	free(s);
}

const WAVEFORMATEX *audio_source_format(const struct audio_source *s,
					size_t *size)
{
	if (size)
		*size = s->format_size;
	return (const WAVEFORMATEX *)s->format;
}

enum audio_source_kind audio_source_kind(const struct audio_source *s)
{
	return s->kind;
}

const AVISTREAMHEADER *audio_source_avi_header(const struct audio_source *s)
{
	return s->stream ? &s->stream->header : NULL;
}

int64_t audio_source_duration(const struct audio_source *s)
{
	return s->duration;
}

int64_t audio_source_packet_end(const struct audio_source *s)
{
	return s->end;
}

int audio_source_truncated(const struct audio_source *s)
{
	return s->truncated || (s->wav && wav_truncated(s->wav));
}

static int reserve(struct audio_source *s, size_t n)
{
	if (n <= s->cap)
		return 0;
	BYTE *b = realloc(s->buf, n);
	if (!b) {
		vfw_set_error("out of memory");
		return -1;
	}
	s->buf = b;
	s->cap = n;
	return 0;
}

/* Raw data: whole blocks, reading until the end. */
static long read_raw(struct audio_source *s, void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t r = read(s->fd, (BYTE *)buf + done, len - done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			vfw_set_error("read error: %s", strerror(errno));
			return -1;
		}
		if (!r)
			break;
		done += r;
	}
	return (long)done;
}

int audio_source_read(struct audio_source *s, const BYTE **data,
		      size_t *len, int64_t *time)
{
	if (s->avi) {
		const struct avi_chunk *k;

		if (s->chunk >= s->stream->nchunks)
			return 0;
		k = &s->stream->chunks[s->chunk++];
		if (reserve(s, k->size ? k->size : 1) ||
		    (k->size && avi_read_chunk(s->avi, k, s->buf)))
			return -1;
		*data = s->buf;
		*len = k->size;
		*time = avi_stream_time(s->stream, k->start) -
			avi_stream_time(s->stream, 0);
		DWORD sample_size = s->stream->header.dwSampleSize;
		s->end = avi_stream_time(s->stream, k->start + (sample_size ?
					 k->size / sample_size : 1)) -
			 avi_stream_time(s->stream, 0);
		return 1;
	}

	/*
	 * PCM in runs of whole sample frames; a compressed format a block
	 * (its packet) at a time, or for a byte stream (a block of 1), a few
	 * kilobytes.
	 */
	size_t want = s->pcm ? PACKET_BYTES / s->block * s->block :
		      s->block > 1 ? s->block : 4096;
	long r;

	if (!want)
		want = s->block;
	if (reserve(s, want))
		return -1;
	r = s->wav ? wav_read(s->wav, s->buf, want) :
	    read_raw(s, s->buf, want);
	if (r < 0)
		return -1;
	if ((size_t)r < want && r % s->block && s->pcm) {
		/* A part sample frame at the end can't be used. */
		s->truncated = 1;
		r -= r % s->block;
	}
	if (!r)
		return 0;
	*data = s->buf;
	*len = (size_t)r;
	*time = bytes_to_time(s, s->pos);
	s->pos += (uint64_t)r;
	s->end = bytes_to_time(s, s->pos);
	return 1;
}
