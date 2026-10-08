/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * WAV reader: the chunks up to data, read in order.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/error.h"
#include "in/wav.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)
#define MAX_FMT (1u << 20)

struct wav_reader {
	int fd, own_fd;
	uint64_t file_size;	/* 0 when not a regular file */
	uint64_t pos;		/* bytes consumed */
	BYTE *format;
	size_t format_size;
	uint64_t data_size, data_left;
	int64_t fact;
	int rf64, truncated;
};

static DWORD rd32(const BYTE *p)
{
	DWORD v;
	memcpy(&v, p, 4);
	return v;
}

static uint64_t rd64(const BYTE *p)
{
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

/* Reads up to len bytes; fewer only at the end of the file. */
static long read_full(struct wav_reader *w, void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t r = read(w->fd, (BYTE *)buf + done, len - done);
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
	w->pos += done;
	return (long)done;
}

/* Reads exactly len bytes, or fails naming what was being read. */
static int read_exact(struct wav_reader *w, void *buf, size_t len,
		      const char *what)
{
	long r = read_full(w, buf, len);

	if (r < 0)
		return -1;
	if ((size_t)r < len) {
		vfw_set_error("file ends inside the %s", what);
		return -1;
	}
	return 0;
}

static int skip(struct wav_reader *w, uint64_t len)
{
	BYTE buf[4096];

	if (w->file_size && lseek(w->fd, (off_t)len, SEEK_CUR) >= 0) {
		w->pos += len;
		return 0;
	}
	while (len) {
		long r = read_full(w, buf, len < sizeof(buf) ? len : sizeof(buf));
		if (r <= 0)
			return r < 0 ? -1 : 0;	/* the end: nothing more */
		len -= r;
	}
	return 0;
}

static int read_fmt(struct wav_reader *w, DWORD size)
{
	size_t cap = size < sizeof(WAVEFORMATEX) ? sizeof(WAVEFORMATEX) : size;
	WAVEFORMATEX *wf;

	if (w->format) {
		vfw_set_error("two fmt chunks");
		return -1;
	}
	if (size < 16 || size > MAX_FMT) {
		vfw_set_error("fmt chunk of %lu bytes", (unsigned long)size);
		return -1;
	}
	if (!(w->format = calloc(1, cap))) {
		vfw_set_error("out of memory");
		return -1;
	}
	if (read_exact(w, w->format, size, "fmt chunk") ||
	    ((size & 1) && skip(w, 1)))
		return -1;
	wf = (WAVEFORMATEX *)w->format;
	if (size < sizeof(*wf))
		wf->cbSize = 0;
	else if (wf->cbSize > size - sizeof(*wf))
		wf->cbSize = (WORD)(size - sizeof(*wf));
	w->format_size = sizeof(*wf) + wf->cbSize;
	if (!wf->nChannels || !wf->nSamplesPerSec || !wf->nBlockAlign) {
		vfw_set_error("format has no %s",
			      !wf->nChannels ? "channels" :
			      !wf->nSamplesPerSec ? "sample rate" : "block size");
		return -1;
	}
	return 0;
}

/* RF64's ds64 chunk: the 64-bit sizes. */
static int read_ds64(struct wav_reader *w, uint64_t *riff, uint64_t *data)
{
	BYTE hdr[8], b[28];

	if (read_exact(w, hdr, 8, "RF64 header") ||
	    rd32(hdr) != FCC('d', 's', '6', '4') || rd32(hdr + 4) < 24) {
		vfw_set_error("RF64 file without a ds64 chunk");
		return -1;
	}
	DWORD size = rd32(hdr + 4);
	if (read_exact(w, b, 24, "ds64 chunk"))
		return -1;
	*riff = rd64(b);
	*data = rd64(b + 8);
	if (rd64(b + 16))
		w->fact = (int64_t)rd64(b + 16);
	return skip(w, size - 24 + (size & 1));
}

static int parse(struct wav_reader *w)
{
	BYTE hdr[12];
	uint64_t riff_size, ds64_data = 0;

	if (read_exact(w, hdr, 12, "RIFF header") ||
	    rd32(hdr + 8) != FCC('W', 'A', 'V', 'E') ||
	    (rd32(hdr) != FCC('R', 'I', 'F', 'F') &&
	     rd32(hdr) != FCC('R', 'F', '6', '4') &&
	     rd32(hdr) != FCC('B', 'W', '6', '4'))) {
		vfw_set_error("not a WAV file");
		return -1;
	}
	riff_size = rd32(hdr + 4);
	if (rd32(hdr) != FCC('R', 'I', 'F', 'F')) {
		w->rf64 = 1;
		if (read_ds64(w, &riff_size, &ds64_data))
			return -1;
	}
	(void)riff_size;	/* chunks are read until data, whatever it says */

	for (;;) {
		long r = read_full(w, hdr, 8);
		if (r < 0)
			return -1;
		if (r < 8) {
			vfw_set_error(w->format ? "no data chunk" : "no fmt chunk");
			return -1;
		}
		FOURCC id = rd32(hdr);
		DWORD size = rd32(hdr + 4);

		if (id == FCC('f', 'm', 't', ' ')) {
			if (read_fmt(w, size))
				return -1;
		} else if (id == FCC('f', 'a', 'c', 't') && size >= 4) {
			BYTE b[4];
			if (read_exact(w, b, 4, "fact chunk") ||
			    skip(w, size - 4 + (size & 1)))
				return -1;
			/* RF64 puts 0xffffffff here and the count in ds64. */
			if (!w->rf64 || rd32(b) != 0xffffffff)
				w->fact = rd32(b);
		} else if (id == FCC('d', 'a', 't', 'a')) {
			if (!w->format) {
				vfw_set_error("data chunk before the fmt chunk");
				return -1;
			}
			uint64_t n = size;
			if (w->rf64 && size == 0xffffffff)
				n = ds64_data;
			else if (size == 0xffffffff ||
				 (!size && (!w->file_size ||
					    w->file_size > w->pos)))
				n = UINT64_MAX;	/* a streaming writer's */
			if (n != UINT64_MAX && w->file_size &&
			    n > w->file_size - w->pos) {
				w->truncated = 1;
				n = w->file_size - w->pos;
			}
			w->data_size = w->data_left = n;
			return 0;
		} else if (skip(w, (uint64_t)size + (size & 1))) {
			return -1;
		}
	}
}

struct wav_reader *wav_open(const char *path)
{
	struct wav_reader *w = calloc(1, sizeof(*w));
	struct stat st;

	if (!w) {
		vfw_set_error("out of memory");
		return NULL;
	}
	w->fact = -1;
	if (!strcmp(path, "-")) {
		w->fd = 0;
	} else if ((w->fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
		vfw_set_error("%s: %s", path, strerror(errno));
		free(w);
		return NULL;
	} else {
		w->own_fd = 1;
	}
	if (!fstat(w->fd, &st) && S_ISREG(st.st_mode)) {
		/* Standard input may be a file already partly read. */
		off_t at = lseek(w->fd, 0, SEEK_CUR);
		if (at >= 0 && at <= st.st_size)
			w->file_size = (uint64_t)(st.st_size - at);
	}
	if (parse(w)) {
		char msg[512];
		snprintf(msg, sizeof(msg), "%s", vfw_error());
		vfw_set_error("%s: %s", path, msg);
		wav_close(w);
		return NULL;
	}
	return w;
}

void wav_close(struct wav_reader *w)
{
	if (!w)
		return;
	if (w->own_fd)
		close(w->fd);
	free(w->format);
	free(w);
}

const WAVEFORMATEX *wav_format(const struct wav_reader *w, size_t *size)
{
	if (size)
		*size = w->format_size;
	return (const WAVEFORMATEX *)w->format;
}

uint64_t wav_data_size(const struct wav_reader *w)
{
	return w->data_size;
}

int64_t wav_fact_samples(const struct wav_reader *w)
{
	return w->fact;
}

int wav_is_rf64(const struct wav_reader *w)
{
	return w->rf64;
}

long wav_read(struct wav_reader *w, void *buf, size_t len)
{
	long r;

	if (len > w->data_left)
		len = (size_t)w->data_left;
	if (len > 1u << 30)
		len = 1u << 30;
	if (!len)
		return 0;
	r = read_full(w, buf, len);
	if (r < 0)
		return -1;
	if (w->data_left != UINT64_MAX)
		w->data_left -= r;
	if ((size_t)r < len) {
		w->truncated |= w->data_size != UINT64_MAX;
		w->data_left = 0;
	}
	return r;
}

int wav_truncated(const struct wav_reader *w)
{
	return w->truncated;
}
