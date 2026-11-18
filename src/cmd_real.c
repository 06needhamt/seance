// SPDX-License-Identifier: GPL-2.0-only
/*
 * `seance decode` of a RealMedia file: a stream of it through one of
 * RealPlayer's codec DLLs (see real/real.h). The video goes to Y4M (or
 * any video output), the audio to a WAV (or raw PCM).
 */
#include "cmd_real.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "in/rm.h"
#include "out/audio.h"
#include "out/image.h"
#include "out/output.h"
#include "real/real.h"

static unsigned gcd(unsigned a, unsigned b)
{
	while (b) {
		unsigned t = a % b;
		a = b;
		b = t;
	}
	return a;
}

/* An I420 picture's header, to convert it for the output with. */
static struct image_conv *i420_conv(int w, int h)
{
	BITMAPINFOHEADER b = {
		.biSize = sizeof(b), .biWidth = w, .biHeight = h,
		.biPlanes = 1, .biBitCount = 12,
		.biCompression = mmioFOURCC('I', '4', '2', '0'),
		.biSizeImage = (DWORD)image_i420_size(w, h),
	};
	return image_conv_new(&b, sizeof(b));
}

static int decode_video(const char *dll, struct rm_file *rm, unsigned sn,
			const struct real_decode_options *o)
{
	const struct rm_stream *st = rm_stream(rm, sn);
	struct real_video *v = real_video_open(dll, st);
	struct output *out = NULL;
	struct image_conv *conv = NULL;
	enum output_kind kind;
	unsigned long n = 0, bad = 0;
	int status = 1, ow = 0, oh = 0;

	if (!v) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}
	if (o->format ? output_kind_from_name(o->format, &kind) :
			output_kind_from_path(o->out, &kind)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		status = 2;
		goto done;
	}
	if (kind == OUT_RAW) {
		fprintf(stderr, "seance: raw output is for VfW decoders; "
			"give .y4m or .yuv\n");
		status = 2;
		goto done;
	}
	/* 16.16 frames a second, as a fraction */
	DWORD rate = st->fps ? st->fps : 25 << 16, scale = 65536;
	unsigned g = gcd(rate, scale);
	rate /= g;
	scale /= g;
	for (;;) {
		struct rm_frame f;
		const BYTE *pic;
		int w, h;
		if (n >= o->frames)
			break;
		int rc = rm_read_frame(rm, sn, &f);
		if (rc < 0) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		if (!rc)
			break;
		rc = real_video_decode(v, &f, &pic, &w, &h);
		if (rc < 0) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			if (!o->keep_going)
				goto done;
			bad++;
			continue;
		}
		if (!rc)
			continue;
		if (!out) {
			if (!(out = output_open(o->out, kind, w, h, rate, scale,
						o->frames == 1)) ||
			    !(conv = i420_conv(w, h))) {
				fprintf(stderr, "seance: %s\n", vfw_error());
				goto done;
			}
			ow = w;
			oh = h;
		} else if (w != ow || h != oh) {
			fprintf(stderr, "seance: the picture changes size, "
				"%dx%d to %dx%d, at %u ms\n", ow, oh, w, h,
				f.time);
			goto done;
		}
		if (output_write(out, conv, pic, image_i420_size(w, h))) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		n++;
	}
	if (!out) {
		fprintf(stderr, "seance: the decoder gave no pictures\n");
		goto done;
	}
	fprintf(stderr, "decoded %lu frames, %dx%d '%.4s'%s\n", n, ow, oh,
		(const char *)&st->fourcc, bad ? " (some refused)" : "");
	status = 0;
done:
	if (out && output_close(out) && !status) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		status = 1;
	}
	image_conv_free(conv);
	real_video_close(v);
	return status;
}

static int decode_sound(const char *dll, struct rm_file *rm, unsigned sn,
			const struct real_decode_options *o)
{
	const struct rm_stream *st = rm_stream(rm, sn);
	struct real_audio *a = real_audio_open(dll, st);
	struct audio_output *out = NULL;
	enum audio_output_kind kind;
	unsigned long blocks = 0, bad = 0;
	uint64_t bytes = 0;
	int status = 1;

	if (!a) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}
	if (o->frames != (unsigned long)-1) {
		fprintf(stderr, "seance: --frames is for a video stream; "
			"a RealAudio one is decoded whole\n");
		status = 2;
		goto done;
	}
	if (o->format ? audio_output_kind_from_name(o->format, &kind) :
			audio_output_kind_from_path(o->out, &kind)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		status = 2;
		goto done;
	}
	const WAVEFORMATEX *wf = real_audio_format(a);
	if (!(out = audio_output_open(o->out, kind, wf, sizeof(*wf), NULL))) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	for (;;) {
		const BYTE *in, *pcm;
		size_t len, plen;
		uint32_t time;
		int rc = rm_read_audio(rm, sn, &in, &len, &time);
		if (rc < 0) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		if (!rc)
			break;
		if (real_audio_decode(a, in, len, &pcm, &plen)) {
			fprintf(stderr, "seance: at %u ms: %s\n", time,
				vfw_error());
			if (!o->keep_going)
				goto done;
			bad++;
			continue;
		}
		blocks++;
		if (plen && audio_output_write(out, pcm, plen)) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		bytes += plen;
	}
	if (!bytes) {
		fprintf(stderr, "seance: the decoder gave no PCM for %lu "
			"blocks\n", blocks);
		goto done;
	}
	fprintf(stderr, "decoded %lu blocks of '%.4s' to %llu samples, %lu Hz "
		"%u-channel%s\n", blocks, (const char *)&st->fourcc,
		(unsigned long long)(bytes / wf->nBlockAlign),
		(unsigned long)wf->nSamplesPerSec, wf->nChannels,
		bad ? " (some refused)" : "");
	status = 0;
done:
	if (out && audio_output_close(out) && !status) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		status = 1;
	}
	real_audio_close(a);
	return status;
}

int real_decode(const char *dll, const char *path,
		const struct real_decode_options *o)
{
	const char *entry;
	enum real_kind k = real_kind(dll, &entry);

	if (k == REAL_NONE) {
		fprintf(stderr, "seance: %s\n", vfw_error()[0] ? vfw_error() :
			"not a RealPlayer codec: RealMedia files are decoded "
			"with one");
		return 1;
	}
	if (k == REAL_PNCODEC || k == REAL_HELIX || k == REAL_RENDERER) {
		fprintf(stderr, "seance: %s: a %s, which seance doesn't host "
			"yet\n", dll, real_kind_name(k));
		return 1;
	}
	struct rm_file *rm = rm_open(path);
	if (!rm) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}
	int sn = o->stream >= 0 ? o->stream :
		 k == REAL_VIDEO ? rm_video_stream(rm) : rm_audio_stream(rm);
	const struct rm_stream *st = sn >= 0 ? rm_stream(rm, (unsigned)sn) :
				     NULL;
	int status;
	if (!st || (k == REAL_VIDEO ? !st->video : !st->audio)) {
		fprintf(stderr, "seance: %s: %s %s stream for a %s\n", path,
			o->stream >= 0 ? "stream" : "no",
			o->stream >= 0 ? "isn't the" : k == REAL_VIDEO ?
			"video" : "audio", real_kind_name(k));
		status = 1;
	} else if (k == REAL_VIDEO) {
		status = decode_video(dll, rm, (unsigned)sn, o);
	} else {
		status = decode_sound(dll, rm, (unsigned)sn, o);
	}
	rm_close(rm);
	return status;
}
