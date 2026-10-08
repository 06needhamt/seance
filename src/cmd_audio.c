/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio through a DirectShow filter or a DMO: packets from an audio source
 * pushed into a decoder (or encoder), and what comes out written to an
 * audio sink.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cmd_audio.h"
#include "common/error.h"
#include "dmo/discover.h"
#include "dshow/discover.h"
#include "dshow/dshow.h"
#include "out/audio.h"
#include "runtime/runtime.h"
#include "win32/ole.h"
#include "win32/path.h"
#include "xform.h"

const char *const audio_only_kinds[] = { "wav", "pcm", NULL };

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int write_bytes(const char *path, const void *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	int failed = !f || fwrite(data, 1, size, f) != size;

	if (f && fclose(f))
		failed = 1;
	if (failed)
		vfw_set_error("%s: can't write it", path);
	return failed ? -1 : 0;
}

/* A registered audio decoder (a filter, else a DMO) of this format tag. */
static char *registered_decoder(WORD tag, CLSID *clsid)
{
	GUID subtype = WAVE_FORMAT_SUBTYPE_INIT(tag), null_guid = { 0 };
	struct dshow_registered *filters = NULL;
	size_t count = dshow_registered_filters(&filters);
	const struct dshow_registered *best = NULL;
	char *path = NULL;

	for (size_t i = 0; i < count; i++) {
		const struct qz_filter *f = &filters[i].f;
		int accepts = 0;
		if (!filters[i].path || dshow_role(f) != DSHOW_AUDIO_DECODER)
			continue;
		for (size_t p = 0; p < f->npins && !accepts; p++) {
			if (f->pins[p].flags & REG_PINFLAG_B_OUTPUT)
				continue;
			for (size_t t = 0; t < f->pins[p].ntypes; t++) {
				const struct qz_type *type = &f->pins[p].types[t];
				if ((!memcmp(&type->major, &MEDIATYPE_Audio, sizeof(GUID)) ||
				     !memcmp(&type->major, &null_guid, sizeof(GUID))) &&
				    (!memcmp(&type->minor, &subtype, sizeof(GUID)) ||
				     !memcmp(&type->minor, &null_guid, sizeof(GUID))))
					accepts = 1;
			}
		}
		if (accepts && (!best || f->merit > best->f.merit))
			best = &filters[i];
	}
	if (best) {
		path = path_to_unix(best->path);
		*clsid = best->f.clsid;
	}
	dshow_registered_free(filters, count);
	if (path)
		return path;

	struct dmo_registered *dmos = NULL;
	count = dmo_registered(&dmos);
	for (size_t i = 0; i < count && !path; i++) {
		const struct dmo_reg *d = &dmos[i].d;
		if (!dmos[i].path || dmo_role(d) != DMO_ROLE_AUDIO_DECODER)
			continue;
		for (size_t t = 0; t < d->nin && !path; t++)
			if (!memcmp(&d->in[t].subtype, &subtype, sizeof(GUID)) ||
			    !memcmp(&d->in[t].subtype, &null_guid, sizeof(GUID))) {
				path = path_to_unix(dmos[i].path);
				*clsid = d->clsid;
			}
	}
	dmo_registered_free(dmos, count);
	return path;
}

static int load_state(struct xform *x, const char *path)
{
	void *data = NULL;
	size_t n;
	char name[24];

	if (vfw_read_file(path, &data, &n)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return -1;
	}
	HRESULT hr = dshow_filter_load_state(xform_object(x), data, n);
	free(data);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: %s: the %s refused this state (%s)\n",
			path, xform_kind_name(x), dshow_hr_name(hr, name));
		return -1;
	}
	return 0;
}

static int save_state(struct xform *x, const char *path)
{
	void *data = NULL;
	size_t n = 0;
	char name[24];
	HRESULT hr = dshow_filter_save_state(xform_object(x), &data, &n);
	int r = FAILED(hr) ? -1 : write_bytes(path, data, n);

	if (FAILED(hr))
		vfw_set_error("the %s has no state to save (%s)",
			      xform_kind_name(x), dshow_hr_name(hr, name));
	if (r)
		fprintf(stderr, "seance: %s\n", vfw_error());
	free(data);
	return r;
}

/* The sample frames --start and --duration ask for, at rate. */
static void trim(const struct audio_codec_job *j, DWORD rate, uint64_t *first,
		 uint64_t *last)
{
	*first = j->has_start ? cli_position_units(&j->start, rate, 1) : 0;
	*last = j->has_duration ?
		*first + cli_position_units(&j->duration, rate, 1) : UINT64_MAX;
}

/* Where frames [at, at + n) and [first, last) meet: *skip and *count. */
static void overlap(uint64_t at, uint64_t n, uint64_t first, uint64_t last,
		    uint64_t *skip, uint64_t *count)
{
	uint64_t from = at > first ? at : first;
	uint64_t to = at + n < last ? at + n : last;

	*skip = from - at;
	*count = to > from ? to - from : 0;
}

static int reserve(BYTE **buf, size_t *cap, size_t n)
{
	if (n <= *cap)
		return 0;
	BYTE *b = realloc(*buf, n);
	if (!b) {
		vfw_set_error("out of memory");
		return -1;
	}
	*buf = b;
	*cap = n;
	return 0;
}

/* Decoding. */

struct decode_sink {
	struct audio_output *out;
	enum audio_sample want;		/* the PCM the output was opened for */
	enum audio_sample from;		/* the PCM the decoder gives now */
	WORD channels, block;		/* block: of what it gives now */
	DWORD rate;
	BYTE *buf;
	size_t cap;
	uint64_t at, first, last;	/* sample frames */
	unsigned changes;
	char error[256];
};

/*
 * A new output type: more PCM of the same channels and rate can be
 * converted to what the file holds; anything else a file can't follow.
 */
static HRESULT decode_change(struct decode_sink *d, const AM_MEDIA_TYPE *mt)
{
	WAVEFORMATEX *wf = NULL;
	size_t size = 0;
	char desc[AUDIO_DESC_SIZE];
	HRESULT hr = audio_mt_to_wfx(mt, &wf, &size);
	enum audio_sample s = SUCCEEDED(hr) ? audio_format_sample(wf, size) :
			      AUDIO_NONE;

	if (s == AUDIO_NONE || wf->nChannels != d->channels ||
	    wf->nSamplesPerSec != d->rate) {
		snprintf(d->error, sizeof(d->error), "the decoder changed its "
			 "output to %s mid-stream, which the file can't follow",
			 wf ? audio_format_desc(wf, size, desc) :
			 "something not audio");
		free(wf);
		return VFW_E_TYPE_NOT_ACCEPTED;
	}
	d->from = s;
	d->block = wf->nBlockAlign;
	d->changes++;
	free(wf);
	return S_OK;
}

static HRESULT decode_output(void *opaque, const struct xform_output *o,
			     const AM_MEDIA_TYPE *dynamic)
{
	struct decode_sink *d = opaque;
	uint64_t skip, n;

	if (dynamic) {
		HRESULT hr = decode_change(d, dynamic);
		if (FAILED(hr))
			return hr;
	}
	uint64_t frames = o->size / d->block;
	overlap(d->at, frames, d->first, d->last, &skip, &n);
	d->at += frames;
	if (!n)
		return S_OK;
	const BYTE *data = o->data + skip * d->block;
	size_t len = (size_t)n * d->block;
	if (d->from != d->want) {
		size_t want = (size_t)n * d->channels *
			      audio_sample_bytes(d->want);
		if (reserve(&d->buf, &d->cap, want)) {
			snprintf(d->error, sizeof(d->error), "%s", vfw_error());
			return E_OUTOFMEMORY;
		}
		audio_convert(d->from, data, d->want, d->buf,
			      (size_t)n * d->channels);
		data = d->buf;
		len = want;
	}
	if (audio_output_write(d->out, data, len)) {
		snprintf(d->error, sizeof(d->error), "%s", vfw_error());
		return E_FAIL;
	}
	return S_OK;
}

/*
 * Connects the decoder with each PCM type in turn; the type it took in
 * *out (malloc()ed). S_OK, or the last failure.
 */
static HRESULT decode_connect(struct xform *x, const WAVEFORMATEX *wf,
			      size_t size, enum audio_sample want,
			      struct decode_sink *sink, WAVEFORMATEX **out,
			      size_t *out_size)
{
	WAVEFORMATEXTENSIBLE choices[16];
	AM_MEDIA_TYPE in = { 0 };
	HRESULT hr = audio_wfx_to_mt(wf, size, &in);
	size_t n = audio_output_preferences(wf, size, want, choices, 16);

	if (FAILED(hr))
		return hr;
	hr = VFW_E_NO_ACCEPTABLE_TYPES;
	for (size_t i = 0; i < n; i++) {
		AM_MEDIA_TYPE mt;
		size_t csize = choices[i].Format.cbSize ? sizeof(choices[i]) :
			       sizeof(WAVEFORMATEX);
		if (FAILED(audio_wfx_to_mt(&choices[i].Format, csize, &mt)))
			continue;
		hr = xform_connect(x, &in, &mt, decode_output, sink);
		dshow_free_media_type(&mt);
		if (SUCCEEDED(hr)) {
			hr = audio_mt_to_wfx(xform_output_type(x), out, out_size);
			break;
		}
	}
	dshow_free_media_type(&in);
	return hr;
}

int decode_audio(const struct audio_codec_job *j)
{
	struct audio_source *src = NULL;
	struct xform *x = NULL;
	struct decode_sink sink = { 0 };
	WAVEFORMATEX *connected = NULL;
	size_t connected_size = 0;
	char *found_path = NULL;
	CLSID found_clsid;
	const CLSID *clsid = j->clsid;
	const char *codec = j->codec;
	char name[24], in_desc[AUDIO_DESC_SIZE], out_desc[AUDIO_DESC_SIZE];
	enum audio_output_kind kind;
	int status = 1;
	HRESULT hr;

	if (j->format ? audio_output_kind_from_name(j->format, &kind) :
	    !strcmp(j->out, "-") ? (kind = AUDIO_OUT_WAV, 0) :
	    audio_output_kind_from_path(j->out, &kind)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 2;
	}
	if (!(src = audio_source_open(j->in, &j->source))) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}
	size_t size;
	const WAVEFORMATEX *wf = audio_source_format(src, &size);
	audio_format_desc(wf, size, in_desc);
	if (audio_format_sample(wf, size) != AUDIO_NONE) {
		fprintf(stderr, "seance: %s is already %s; see convert\n",
			j->in, in_desc);
		goto done;
	}

	if (!strcmp(codec, "auto")) {
		if (runtime_enter()) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		WORD tag = audio_format_tag(wf, size);
		if (!(found_path = registered_decoder(tag, &found_clsid))) {
			fprintf(stderr, "seance: no registered decoder of %s\n",
				in_desc);
			goto done;
		}
		codec = found_path;
		clsid = &found_clsid;
	}
	hr = xform_open(codec, clsid, 1, 0, &x, NULL);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: %s has no %s audio decoder (%s)\n",
			codec, hr == VFW_E_DUPLICATE_NAME ? "single" : "",
			dshow_hr_name(hr, name));
		goto done;
	}
	if (j->state && load_state(x, j->state))
		goto done;

	/* The PCM asked for, else what suits the decoder, converted. */
	sink.channels = wf->nChannels;
	sink.rate = wf->nSamplesPerSec;
	enum audio_sample convert = AUDIO_NONE;
	hr = decode_connect(x, wf, size, j->sample, &sink, &connected,
			    &connected_size);
	if (FAILED(hr) && j->sample != AUDIO_NONE) {
		convert = j->sample;
		hr = decode_connect(x, wf, size, AUDIO_NONE, &sink, &connected,
				    &connected_size);
	}
	if (FAILED(hr)) {
		fprintf(stderr, "seance: the %s takes no PCM output for %s "
			"(%s)\n", xform_kind_name(x), in_desc,
			dshow_hr_name(hr, name));
		goto done;
	}
	sink.want = sink.from = audio_format_sample(connected, connected_size);
	sink.block = connected->nBlockAlign;
	if (sink.want == AUDIO_NONE || !sink.block) {
		fprintf(stderr, "seance: the %s's output (%s) isn't PCM seance "
			"can write\n", xform_kind_name(x),
			audio_format_desc(connected, connected_size, out_desc));
		goto done;
	}
	trim(j, sink.rate, &sink.first, &sink.last);
	struct audio_output_options oo = { .convert = convert };
	if (!(sink.out = audio_output_open(j->out, kind, connected,
					   connected_size, &oo))) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}

	const BYTE *data;
	size_t len;
	int64_t time, end = 0;
	unsigned long packets = 0, errors = 0;
	double busy = 0;
	int r = 0;
	while (sink.at < sink.last &&
	       (r = audio_source_read(src, &data, &len, &time)) > 0) {
		if (time < 0)
			time = end;
		end = audio_source_packet_end(src);
		if (end < time)
			end = time;
		DWORD flags = AM_SAMPLE_SPLICEPOINT |
			      (!packets ? AM_SAMPLE_DATADISCONTINUITY : 0);
		double t = now();
		hr = xform_push(x, data, len, time, end, flags);
		busy += now() - t;
		packets++;
		if (FAILED(hr)) {
			errors++;
			fprintf(stderr, "seance: packet %lu: %s%s%s\n",
				packets - 1, sink.error[0] ? sink.error : "",
				sink.error[0] ? ": " : "", dshow_hr_name(hr, name));
			if (!j->keep_going || sink.error[0])
				goto done;
		}
	}
	if (r < 0) {
		fprintf(stderr, "seance: %s: %s\n", j->in, vfw_error());
		goto done;
	}
	hr = xform_end(x);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: end of stream: %s%s%s\n", sink.error,
			sink.error[0] ? ": " : "", dshow_hr_name(hr, name));
		goto done;
	}

	size_t osize;
	const WAVEFORMATEX *owf = audio_output_format(sink.out, &osize);
	uint64_t frames = audio_output_bytes(sink.out) / owf->nBlockAlign;
	audio_format_desc(owf, osize, out_desc);
	r = audio_output_close(sink.out);
	sink.out = NULL;
	if (r) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	fprintf(stderr, "%s: %llu samples (%.3f s), %s -> %s -> %s",
		j->out, (unsigned long long)frames,
		(double)frames / sink.rate, in_desc, out_desc,
		audio_output_kind_name(kind));
	if (packets)
		fprintf(stderr, ", %.3f ms/packet", busy * 1000 / packets);
	fprintf(stderr, "\n");
	if (sink.changes)
		fprintf(stderr, "seance: the decoder changed its output format "
			"%u time%s; converted back\n", sink.changes,
			sink.changes == 1 ? "" : "s");
	if (errors)
		fprintf(stderr, "seance: %lu packet%s failed to decode\n",
			errors, errors == 1 ? "" : "s");
	if (audio_source_truncated(src))
		fprintf(stderr, "seance: warning: %s is truncated or damaged\n",
			j->in);
	status = 0;

done:
	if (sink.out)
		audio_output_close(sink.out);
	xform_close(x);
	audio_source_close(src);
	free(connected);
	free(sink.buf);
	free(found_path);
	return status;
}

/* Encoding. */

struct encode_sink {
	const char *path;
	enum audio_output_kind kind;
	struct audio_output *out;
	WAVEFORMATEX *format;		/* the encoder's output format */
	size_t format_size;
	DWORD rate;
	unsigned long packets;
	uint64_t bytes;
	char error[256];
};

static int same_format(const struct encode_sink *e, const AM_MEDIA_TYPE *mt)
{
	WAVEFORMATEX *wf = NULL;
	size_t size = 0;
	int same = SUCCEEDED(audio_mt_to_wfx(mt, &wf, &size)) &&
		   size == e->format_size && !memcmp(wf, e->format, size);

	free(wf);
	return same;
}

/*
 * The file opens with the first packet: an AVI stream counts packets of the
 * length the first one has (variable-rate, as MP3 in AVI is), or blocks of
 * the format when the encoder doesn't time its packets.
 */
static int encode_open(struct encode_sink *e, const struct xform_output *o)
{
	struct audio_output_options oo = { 0 };

	if (o && o->timed && o->stop > o->start) {
		uint64_t frames = (uint64_t)((o->stop - o->start) *
					     (double)e->rate / 1e7 + 0.5);
		if (frames && frames <= 0xffffffffu) {
			oo.rate = e->rate;
			oo.scale = (DWORD)frames;
		}
	}
	if (!(e->out = audio_output_open(e->path, e->kind, e->format,
					 e->format_size, &oo))) {
		snprintf(e->error, sizeof(e->error), "%s", vfw_error());
		return -1;
	}
	return 0;
}

static HRESULT encode_output(void *opaque, const struct xform_output *o,
			     const AM_MEDIA_TYPE *dynamic)
{
	struct encode_sink *e = opaque;

	if (dynamic && !e->out) {
		/* Before the file is opened, its format can still change. */
		WAVEFORMATEX *wf = NULL;
		size_t size = 0;
		HRESULT hr = audio_mt_to_wfx(dynamic, &wf, &size);
		if (FAILED(hr))
			return hr;
		free(e->format);
		e->format = wf;
		e->format_size = size;
	} else if (dynamic && !same_format(e, dynamic)) {
		snprintf(e->error, sizeof(e->error), "the encoder changed its "
			 "format after the file was begun");
		return VFW_E_INVALIDMEDIATYPE;
	}
	if (!e->out && encode_open(e, o))
		return E_FAIL;
	if (audio_output_write(e->out, o->data, o->size)) {
		snprintf(e->error, sizeof(e->error), "%s", vfw_error());
		return E_FAIL;
	}
	e->packets++;
	e->bytes += o->size;
	return S_OK;
}

/*
 * Connects the encoder with PCM in format wf, its output left for it to
 * fill in (a major type and the tag's subtype). 0, or -1.
 */
static int encode_connect(struct xform *x, const WAVEFORMATEX *wf,
			  size_t size, WORD tag, struct encode_sink *e)
{
	AM_MEDIA_TYPE in = { 0 }, out = { 0 };
	HRESULT hr = audio_wfx_to_mt(wf, size, &in);

	out.majortype = MEDIATYPE_Audio;
	out.subtype = (GUID)WAVE_FORMAT_SUBTYPE_INIT(tag);
	if (SUCCEEDED(hr))
		hr = xform_connect(x, &in, &out, encode_output, e);
	if (SUCCEEDED(hr)) {
		free(e->format);
		e->format = NULL;
		hr = audio_mt_to_wfx(xform_output_type(x), &e->format,
				     &e->format_size);
		if (FAILED(hr))
			xform_disconnect(x);
	}
	dshow_free_media_type(&in);
	return SUCCEEDED(hr) ? 0 : -1;
}

int encode_audio(const struct audio_codec_job *j)
{
	struct audio_source *src = NULL;
	struct xform *x = NULL;
	struct xform_class found;
	struct encode_sink sink = { 0 };
	WAVEFORMATEXTENSIBLE pcm;
	BYTE *buf = NULL;
	size_t cap = 0;
	char name[24], in_desc[AUDIO_DESC_SIZE], out_desc[AUDIO_DESC_SIZE];
	int status = 1;
	HRESULT hr;

	sink.path = j->out;
	if (j->format ? audio_output_kind_from_name(j->format, &sink.kind) :
	    audio_output_kind_from_path(j->out, &sink.kind)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 2;
	}
	if (!(src = audio_source_open(j->in, &j->source))) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}
	size_t size;
	const WAVEFORMATEX *wf = audio_source_format(src, &size);
	enum audio_sample native = audio_format_sample(wf, size);
	audio_format_desc(wf, size, in_desc);
	if (native == AUDIO_NONE) {
		fprintf(stderr, "seance: %s is %s; encoding takes PCM\n", j->in,
			in_desc);
		goto done;
	}
	sink.rate = wf->nSamplesPerSec;

	hr = xform_find(j->codec, j->clsid, 1, 1, &found);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: %s: no %s DirectShow or DMO audio "
			"encoder\n", j->codec,
			hr == VFW_E_DUPLICATE_NAME ? "single" : "");
		goto done;
	}
	if (FAILED(hr = xform_create(j->codec, &found, &x))) {
		fprintf(stderr, "seance: cannot open the encoder in %s (%s)\n",
			j->codec, dshow_hr_name(hr, name));
		goto done;
	}
	if (j->state && load_state(x, j->state))
		goto done;
	struct dshow_encoder_options options = {
		.key_interval = -1,
		.quality = j->quality < 0 ? -1.0 : j->quality / 10000.0,
		.bitrate = j->bitrate ? (LONGLONG)j->bitrate * 1000 : -1,
	};
	if (options.quality >= 0.0 || options.bitrate >= 0) {
		hr = dshow_encoder_configure(xform_object(x), &options);
		if (FAILED(hr)) {
			fprintf(stderr, "seance: encoder options were refused "
				"(%s)\n", dshow_hr_name(hr, name));
			goto done;
		}
	}

	/* The PCM as it is, else converted to a sample format it takes. */
	static const enum audio_sample others[] = {
		AUDIO_S16, AUDIO_S24, AUDIO_S32, AUDIO_F32, AUDIO_U8,
	};
	enum audio_sample feed = native;
	int connected = !encode_connect(x, wf, size, (WORD)found.compression,
					&sink);
	for (size_t i = 0; !connected && i < sizeof(others) / sizeof(*others);
	     i++) {
		DWORD mask = wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE ?
			     ((const WAVEFORMATEXTENSIBLE *)wf)->dwChannelMask : 0;
		for (int form = AUDIO_FORM_PLAIN; !connected &&
		     form <= AUDIO_FORM_EXTENSIBLE; form++) {
			size_t n = audio_pcm_format(others[i], wf->nChannels,
						    wf->nSamplesPerSec, mask, form,
						    &pcm);
			if (others[i] == native && !memcmp(&pcm, wf, n))
				continue;
			connected = !encode_connect(x, &pcm.Format, n,
						    (WORD)found.compression,
						    &sink);
			if (connected)
				feed = others[i];
		}
	}
	if (!connected) {
		fprintf(stderr, "seance: the %s takes neither %s nor any other "
			"PCM of its channels and rate\n", xform_kind_name(x),
			in_desc);
		goto done;
	}

	uint64_t first, last, at = 0, pushed = 0;
	trim(j, sink.rate, &first, &last);
	const BYTE *data;
	size_t len;
	int64_t time;
	double busy = 0;
	int r = 0;
	while (at < last && (r = audio_source_read(src, &data, &len, &time)) > 0) {
		uint64_t skip, n, frames = len / wf->nBlockAlign;
		overlap(at, frames, first, last, &skip, &n);
		at += frames;
		if (!n)
			continue;
		data += skip * wf->nBlockAlign;
		len = (size_t)n * wf->nBlockAlign;
		if (feed != native) {
			size_t want = (size_t)n * wf->nChannels *
				      audio_sample_bytes(feed);
			if (reserve(&buf, &cap, want)) {
				fprintf(stderr, "seance: %s\n", vfw_error());
				goto done;
			}
			audio_convert(native, data, feed, buf,
				      (size_t)n * wf->nChannels);
			data = buf;
			len = want;
		}
		REFERENCE_TIME a = audio_ticks(pushed, 1, sink.rate);
		REFERENCE_TIME b = audio_ticks(pushed + n, 1, sink.rate);
		double t = now();
		hr = xform_push(x, data, len, a, b, AM_SAMPLE_SPLICEPOINT |
				(!pushed ? AM_SAMPLE_DATADISCONTINUITY : 0));
		busy += now() - t;
		if (FAILED(hr)) {
			fprintf(stderr, "seance: at sample %llu: %s%s%s\n",
				(unsigned long long)pushed, sink.error,
				sink.error[0] ? ": " : "",
				dshow_hr_name(hr, name));
			goto done;
		}
		pushed += n;
	}
	if (r < 0) {
		fprintf(stderr, "seance: %s: %s\n", j->in, vfw_error());
		goto done;
	}
	hr = xform_end(x);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: end of stream: %s%s%s\n", sink.error,
			sink.error[0] ? ": " : "", dshow_hr_name(hr, name));
		goto done;
	}
	xform_disconnect(x);
	if (j->save_state && save_state(x, j->save_state))
		goto done;
	/* An encoder that gave nothing still gets its (empty) file. */
	if (!sink.out && encode_open(&sink, NULL)) {
		fprintf(stderr, "seance: %s\n", sink.error);
		goto done;
	}
	audio_output_set_samples(sink.out, pushed);
	audio_format_desc(sink.format, sink.format_size, out_desc);
	r = audio_output_close(sink.out);
	sink.out = NULL;
	if (r) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	fprintf(stderr, "%s: %lu packet%s, %llu samples (%.3f s), %s -> %s -> "
		"%s, %llu bytes", j->out, sink.packets,
		sink.packets == 1 ? "" : "s", (unsigned long long)pushed,
		(double)pushed / sink.rate, in_desc, out_desc,
		audio_output_kind_name(sink.kind),
		(unsigned long long)sink.bytes);
	if (pushed)
		fprintf(stderr, ", %.1f kbit/s",
			sink.bytes * 8.0 * sink.rate / pushed / 1000);
	if (sink.packets)
		fprintf(stderr, ", %.3f ms/packet", busy * 1000 / sink.packets);
	fprintf(stderr, "\n");
	status = 0;

done:
	if (sink.out)
		audio_output_close(sink.out);
	xform_close(x);
	audio_source_close(src);
	free(sink.format);
	free(buf);
	return status;
}
