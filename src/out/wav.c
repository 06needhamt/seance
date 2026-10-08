/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * WAV writer.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/error.h"
#include "media/audio.h"
#include "out/wav.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)
#define DS64_BYTES 28		/* riff, data and sample sizes, table length */
#define LIMIT 0xffffffffu

struct wav_writer {
	int fd, own_fd, seekable, failed;
	struct wav_writer_options opts;
	WORD block;
	int pcm, has_fact;
	uint64_t pos;			/* bytes written */
	uint64_t junk_pos, fact_pos, data_pos;
	uint64_t bytes;
	uint64_t samples;
	int samples_set;
};

static void wr32(BYTE *p, DWORD v) { memcpy(p, &v, 4); }
static void wr64(BYTE *p, uint64_t v) { memcpy(p, &v, 8); }

static int fail_errno(struct wav_writer *w, const char *what)
{
	vfw_set_error("%s: %s", what, strerror(errno));
	w->failed = 1;
	return -1;
}

static int append(struct wav_writer *w, const void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t r = write(w->fd, (const BYTE *)buf + done, len - done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return fail_errno(w, "write error");
		done += r;
	}
	w->pos += len;
	return 0;
}

static int put_at(struct wav_writer *w, uint64_t pos, const void *buf,
		  size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t r = pwrite(w->fd, (const BYTE *)buf + done, len - done,
				   (off_t)(pos + done));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return fail_errno(w, "write error");
		done += r;
	}
	return 0;
}

static int put_chunk(struct wav_writer *w, FOURCC id, const void *data,
		     DWORD size)
{
	BYTE hdr[8], pad = 0;

	wr32(hdr, id);
	wr32(hdr + 4, size);
	if (append(w, hdr, 8) || (size && append(w, data, size)) ||
	    ((size & 1) && append(w, &pad, 1)))
		return -1;
	return 0;
}

struct wav_writer *wav_writer_open(const char *path, const WAVEFORMATEX *wf,
				   size_t size,
				   const struct wav_writer_options *opts)
{
	struct wav_writer *w;
	struct stat st;
	BYTE fmt[sizeof(WAVEFORMATEX) + 0xffff];
	size_t fmt_size;

	if (!wf || size < 16) {
		vfw_set_error("bad audio format");
		return NULL;
	}
	if (!(w = calloc(1, sizeof(*w)))) {
		vfw_set_error("out of memory");
		return NULL;
	}
	if (opts)
		w->opts = *opts;

	/* The format as written: 16 bytes for plain PCM. */
	memset(fmt, 0, sizeof(WAVEFORMATEX));
	memcpy(fmt, wf, size < sizeof(fmt) ? size : sizeof(fmt));
	WAVEFORMATEX *f = (WAVEFORMATEX *)fmt;
	if (size < sizeof(*f))
		f->cbSize = 0;
	else if (f->cbSize > size - sizeof(*f))
		f->cbSize = (WORD)(size - sizeof(*f));
	fmt_size = sizeof(*f) + f->cbSize;
	if (f->wFormatTag == WAVE_FORMAT_PCM && !f->cbSize)
		fmt_size = 16;
	w->block = f->nBlockAlign;
	w->pcm = audio_format_sample(f, sizeof(*f) + f->cbSize) != AUDIO_NONE;
	w->has_fact = audio_format_tag(f, sizeof(*f) + f->cbSize) !=
		      WAVE_FORMAT_PCM;

	if (!strcmp(path, "-")) {
		w->fd = 1;
	} else if ((w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
				 0644)) < 0) {
		vfw_set_error("%s: %s", path, strerror(errno));
		free(w);
		return NULL;
	} else {
		w->own_fd = 1;
	}
	w->seekable = !fstat(w->fd, &st) && S_ISREG(st.st_mode) &&
		      lseek(w->fd, 0, SEEK_CUR) == 0;

	BYTE hdr[12], zero[DS64_BYTES] = { 0 };
	wr32(hdr, FCC('R', 'I', 'F', 'F'));
	wr32(hdr + 4, LIMIT);
	wr32(hdr + 8, FCC('W', 'A', 'V', 'E'));
	append(w, hdr, 12);
	if (w->opts.rf64 != WAV_RF64_NEVER && w->seekable) {
		w->junk_pos = w->pos;
		put_chunk(w, FCC('J', 'U', 'N', 'K'), zero, DS64_BYTES);
	}
	put_chunk(w, FCC('f', 'm', 't', ' '), fmt, (DWORD)fmt_size);
	if (w->has_fact) {
		/* A placeholder until the count is known. */
		w->fact_pos = w->pos;
		put_chunk(w, w->seekable ? FCC('f', 'a', 'c', 't') :
			  FCC('J', 'U', 'N', 'K'), zero, 4);
	}
	/* Sizes not patched later (on a pipe) say "to the end". */
	w->data_pos = w->pos;
	wr32(hdr, FCC('d', 'a', 't', 'a'));
	wr32(hdr + 4, LIMIT);
	append(w, hdr, 8);
	if (w->failed) {
		if (w->own_fd)
			close(w->fd);
		free(w);
		return NULL;
	}
	return w;
}

int wav_writer_write(struct wav_writer *w, const void *data, size_t len)
{
	if (w->failed)
		return -1;
	if (w->opts.rf64 == WAV_RF64_NEVER &&
	    w->pos + len + 1 - 8 > LIMIT) {
		vfw_set_error("WAV file would pass 4 GB");
		w->failed = 1;
		return -1;
	}
	if (append(w, data, len))
		return -1;
	w->bytes += len;
	return 0;
}

void wav_writer_set_samples(struct wav_writer *w, uint64_t samples)
{
	w->samples = samples;
	w->samples_set = 1;
}

uint64_t wav_writer_bytes(const struct wav_writer *w)
{
	return w->bytes;
}

static int finish(struct wav_writer *w)
{
	BYTE pad = 0, b[DS64_BYTES + 8];
	int have_count = w->pcm || w->samples_set;
	uint64_t samples = w->samples_set ? w->samples :
			   w->block ? w->bytes / w->block : 0;

	if ((w->bytes & 1) && append(w, &pad, 1))
		return -1;
	if (!w->seekable)
		return 0;

	uint64_t riff = w->pos - 8;
	int rf64 = w->opts.rf64 == WAV_RF64_ALWAYS ||
		   (w->opts.rf64 == WAV_RF64_AUTO &&
		    (riff > LIMIT || w->bytes > LIMIT));

	if (rf64) {
		wr32(b, FCC('d', 's', '6', '4'));
		wr32(b + 4, DS64_BYTES);
		wr64(b + 8, riff);
		wr64(b + 16, w->bytes);
		wr64(b + 24, have_count ? samples : 0);
		wr32(b + 32, 0);
		if (put_at(w, w->junk_pos, b, 8 + DS64_BYTES))
			return -1;
		wr32(b, FCC('R', 'F', '6', '4'));
		wr32(b + 4, LIMIT);
		if (put_at(w, 0, b, 8))
			return -1;
	} else {
		wr32(b, (DWORD)riff);
		if (put_at(w, 4, b, 4))
			return -1;
	}
	wr32(b, rf64 ? LIMIT : (DWORD)w->bytes);
	if (put_at(w, w->data_pos + 4, b, 4))
		return -1;

	if (w->has_fact) {
		if (have_count) {
			wr32(b, rf64 || samples > LIMIT ? LIMIT : (DWORD)samples);
			return put_at(w, w->fact_pos + 8, b, 4);
		}
		wr32(b, FCC('J', 'U', 'N', 'K'));
		return put_at(w, w->fact_pos, b, 4);
	}
	return 0;
}

int wav_writer_close(struct wav_writer *w)
{
	int r = w->failed ? -1 : finish(w);

	if (w->own_fd && close(w->fd) && !r) {
		vfw_set_error("close: %s", strerror(errno));
		r = -1;
	}
	free(w);
	return r;
}
