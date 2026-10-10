/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance convert <in> -o <out>: without a codec, write an AVI's
 * uncompressed video as Y4M, BMP files or raw frames, or audio from an AVI,
 * a WAV or raw PCM as WAV, raw PCM or an AVI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "avi/avi.h"
#include "cmd_util.h"
#include "commands.h"
#include "common/error.h"
#include "in/audio.h"
#include "media/audio.h"
#include "out/audio.h"
#include "out/image.h"
#include "out/output.h"

static int usage(void)
{
	fprintf(stderr,
		"usage: seance convert <in> -o <out> [options]\n"
		"\n"
		"Video, from an AVI:\n"
		"  -o <out>           .y4m, .yuv (I420), .bgr (BGR24), .raw (frames\n"
		"                     as stored) or \"-\" for stdout; BMP files with\n"
		"                     a frame number: out%%05d.bmp\n"
		"  --format <fmt>     y4m, bmp, yuv, bgr or raw, instead of the\n"
		"                     extension\n"
		"  --stream <n>       the stream to use (default: the first video)\n"
		"  --start <pos>      first frame to write: a frame number or a\n"
		"                     time (2.5s, 1500ms, 1:02.5)\n"
		"  --frames <n>       number of frames to write (default: all)\n"
		"  --duration <len>   how much to write, as frames or a time\n"
		"\n"
		"Audio, from an AVI, a WAV, or raw PCM (taken when the input has\n"
		"no video, the output is .wav, .pcm or .avi, or --audio is given):\n"
		"  -o <out>           .wav, .pcm (the data alone) or .avi; \"-\"\n"
		"                     writes a WAV (or with --format pcm, raw) to\n"
		"                     stdout\n"
		"  --format <fmt>     wav, pcm or avi, instead of the extension\n"
		"  --audio            the first audio stream of an AVI\n"
		"  --stream <n>       the stream to use\n"
		"  --start <pos>      where to begin: a sample number or a time\n"
		"                     (compressed audio: to the packet)\n"
		"  --duration <len>   how much to write, as samples or a time\n"
		"  --sample-format <f>  write PCM as u8, s16, s24, s32, f32 or f64\n"
		"  --raw <f> --rate <hz> --channels <n>\n"
		"                     the input is raw PCM in this sample format\n");
	return 2;
}

static void warn_truncated(const char *in, const char *what)
{
	fprintf(stderr, "seance: warning: %s is truncated or damaged; some "
		"%s missing\n", in, what);
}

/* The audio path: packets from an audio source to an audio sink. */
struct audio_job {
	const char *in, *out, *format;
	struct audio_source_options src;
	struct cli_position start, duration;
	int has_start, has_duration;
	enum audio_sample sample;
};

static int convert_audio(const struct audio_job *j)
{
	struct audio_source *src = NULL;
	struct audio_output *o = NULL;
	struct audio_output_options oo = { .convert = j->sample };
	enum audio_output_kind kind;
	char desc[AUDIO_DESC_SIZE], odesc[AUDIO_DESC_SIZE];
	int status = 1;

	if (j->format ? audio_output_kind_from_name(j->format, &kind) :
	    !strcmp(j->out, "-") ? (kind = AUDIO_OUT_WAV, 0) :
	    audio_output_kind_from_path(j->out, &kind)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 2;
	}
	if (!(src = audio_source_open(j->in, &j->src))) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}

	size_t size;
	const WAVEFORMATEX *wf = audio_source_format(src, &size);
	enum audio_sample sample = audio_format_sample(wf, size);
	DWORD rate = wf->nSamplesPerSec;
	WORD block = wf->nBlockAlign;

	/* A compressed AVI stream keeps its timing in an AVI. */
	const AVISTREAMHEADER *h = audio_source_avi_header(src);
	if (h && sample == AUDIO_NONE && kind == AUDIO_OUT_AVI) {
		oo.rate = h->dwRate;
		oo.scale = h->dwScale;
		oo.sample_size = h->dwSampleSize;
	}
	if (!(o = audio_output_open(j->out, kind, wf, size, &oo))) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}

	/* Where to start and stop: samples for PCM, else 100 ns ticks. */
	uint64_t first = 0, last = UINT64_MAX;
	if (sample != AUDIO_NONE) {
		if (j->has_start)
			first = cli_position_units(&j->start, rate, 1);
		if (j->has_duration)
			last = first + cli_position_units(&j->duration, rate, 1);
	} else {
		if (j->has_start)
			first = audio_ticks(cli_position_units(&j->start, rate, 1),
					    1, rate);
		if (j->has_start && j->start.is_time)
			first = (uint64_t)(j->start.seconds * 1e7 + 0.5);
		if (j->has_duration)
			last = first + (j->duration.is_time ?
					(uint64_t)(j->duration.seconds * 1e7 + 0.5) :
					(uint64_t)audio_ticks(j->duration.count,
							      1, rate));
	}

	const BYTE *data;
	size_t len;
	int64_t time;
	uint64_t at = 0;	/* PCM: the sample the packet starts at */
	int r, untimed = 0;

	while ((r = audio_source_read(src, &data, &len, &time)) > 0) {
		if (sample != AUDIO_NONE) {
			uint64_t n = len / block, from = at, to = at + n;
			at = to;
			if (to <= first)
				continue;
			if (from >= last)
				break;
			uint64_t skip = first > from ? first - from : 0;
			uint64_t end = last < to ? last - from : n;
			data += skip * block;
			len = (size_t)((end - skip) * block);
		} else if (time < 0) {
			untimed = 1;
		} else if ((uint64_t)time < first) {
			continue;
		} else if ((uint64_t)time >= last) {
			break;
		}
		if (audio_output_write(o, data, len)) {
			fprintf(stderr, "seance: %s: %s\n", j->out, vfw_error());
			goto done;
		}
	}
	if (r < 0) {
		fprintf(stderr, "seance: %s: %s\n", j->in, vfw_error());
		goto done;
	}
	if (untimed && (j->has_start || j->has_duration))
		fprintf(stderr, "seance: warning: %s has no timing to trim "
			"by; all of it was written\n", j->in);
	if (sample == AUDIO_NONE && !j->has_start && !j->has_duration &&
	    audio_source_duration(src) > 0)
		audio_output_set_samples(o, (uint64_t)audio_source_duration(src) *
					 rate / 10000000u);

	size_t osize;
	const WAVEFORMATEX *owf = audio_output_format(o, &osize);
	uint64_t bytes = audio_output_bytes(o);
	WORD oblock = owf->nBlockAlign;
	audio_format_desc(wf, size, desc);
	audio_format_desc(owf, osize, odesc);
	r = audio_output_close(o);
	o = NULL;
	if (r) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	if (sample != AUDIO_NONE)
		fprintf(stderr, "%s: %llu samples (%.3f s), %s -> %s",
			j->out, (unsigned long long)(bytes / oblock),
			(double)(bytes / oblock) / rate,
			desc, audio_output_kind_name(kind));
	else
		fprintf(stderr, "%s: %llu bytes, %s -> %s", j->out,
			(unsigned long long)bytes, desc,
			audio_output_kind_name(kind));
	if (strcmp(desc, odesc))
		fprintf(stderr, " %s", odesc);
	fprintf(stderr, "\n");
	if (audio_source_truncated(src))
		warn_truncated(j->in, "audio was");
	status = 0;

done:
	if (o)
		audio_output_close(o);
	audio_source_close(src);
	return status;
}

int cmd_convert(int argc, char **argv)
{
	const char *in = NULL, *out = NULL, *format = NULL;
	const char *raw = NULL;
	unsigned long count = (unsigned long)-1, stream = (unsigned long)-1;
	unsigned long channels = 0, hz = 0;
	struct audio_job job = { .src.stream = -1 };
	int want_audio = 0;
	enum output_kind kind;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int has_value = i + 1 < argc;

		if (!strcmp(a, "-o") && has_value)
			out = argv[++i];
		else if (!strcmp(a, "--format") && has_value)
			format = argv[++i];
		else if (!strcmp(a, "--start") && has_value) {
			if (cli_parse_position(a, argv[++i], &job.start))
				return 2;
			job.has_start = 1;
		} else if (!strcmp(a, "--frames") && has_value) {
			if (cli_parse_count(a, argv[++i], &count))
				return 2;
		} else if (!strcmp(a, "--duration") && has_value) {
			if (cli_parse_position(a, argv[++i], &job.duration))
				return 2;
			job.has_duration = 1;
		} else if (!strcmp(a, "--stream") && has_value) {
			if (cli_parse_count(a, argv[++i], &stream))
				return 2;
		} else if (!strcmp(a, "--audio")) {
			want_audio = 1;
		} else if (!strcmp(a, "--sample-format") && has_value) {
			if (audio_sample_from_name(argv[++i], &job.sample)) {
				fprintf(stderr, "seance: %s\n", vfw_error());
				return 2;
			}
		} else if (!strcmp(a, "--raw") && has_value) {
			raw = argv[++i];
		} else if (!strcmp(a, "--rate") && has_value) {
			if (cli_parse_count(a, argv[++i], &hz))
				return 2;
		} else if (!strcmp(a, "--channels") && has_value) {
			if (cli_parse_count(a, argv[++i], &channels))
				return 2;
		} else if (!strcmp(a, "-") && !in)
			in = a;
		else if (a[0] == '-' || in)
			return usage();
		else
			in = a;
	}
	if (!in || !out)
		return usage();

	/* Audio, or video? */
	static const char *const audio_kinds[] = { "wav", "pcm", "avi", NULL };
	int audio = raw || job.sample != AUDIO_NONE || !strcmp(in, "-") ||
		    cli_wants_audio(in, stream == (unsigned long)-1 ? -1 :
				    stream > 99 ? 99 : (long)stream,
				    want_audio, format, out, audio_kinds,
				    NULL, NULL, 0);
	if (audio) {
		job.in = in;
		job.out = out;
		job.format = format;
		if (stream != (unsigned long)-1)
			job.src.stream = stream > 99 ? 99 : (int)stream;
		if (raw) {
			job.src.kind = AUDIO_SOURCE_RAW;
			job.src.channels = channels;
			job.src.rate = hz;
			if (audio_sample_from_name(raw, &job.src.raw_sample)) {
				fprintf(stderr, "seance: %s\n", vfw_error());
				return 2;
			}
		} else if (channels || hz) {
			fprintf(stderr, "seance: --rate and --channels describe "
				"raw PCM, with --raw\n");
			return 2;
		}
		if (count != (unsigned long)-1) {
			fprintf(stderr, "seance: --frames is for video; use "
				"--duration\n");
			return 2;
		}
		return convert_audio(&job);
	}

	if (format ? output_kind_from_name(format, &kind) :
		     output_kind_from_path(out, &kind)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 2;
	}

	struct avi_file *avi = avi_open(in);
	if (!avi) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}

	int status = 1;
	struct image_conv *conv = NULL;
	struct output *o = NULL;
	BYTE *buf = NULL;

	int sn = stream == (unsigned long)-1 ? avi_video_stream(avi) :
		 stream < avi->nstreams ? (int)stream : -1;
	if (sn < 0) {
		fprintf(stderr, "seance: %s: %s\n", in,
			stream == (unsigned long)-1 ? "no video stream" :
			"no such stream");
		goto done;
	}
	const struct avi_stream *s = &avi->streams[sn];
	const BITMAPINFOHEADER *bih = avi_stream_bih(s);
	if (!bih) {
		fprintf(stderr, "seance: %s: stream %d is not video\n", in, sn);
		goto done;
	}
	if (!(conv = image_conv_new(bih, s->format_size))) {
		fprintf(stderr, "seance: %s: %s\n", in, vfw_error());
		goto done;
	}

	DWORD rate = s->header.dwRate, scale = s->header.dwScale;
	if (!rate || !scale) {
		/* Fall back on the main header's frame duration. */
		rate = 1000000;
		scale = avi->header.dwMicroSecPerFrame ?
			avi->header.dwMicroSecPerFrame : 40000;
	}
	unsigned long long start = job.has_start ?
				   cli_position_units(&job.start, rate, scale) : 0;
	if (job.has_duration) {
		unsigned long long n = cli_position_units(&job.duration, rate,
							  scale);
		if (n < count)
			count = (unsigned long)n;
	}
	size_t want = s->frames > start ? s->frames - start : 0;
	if (count < want)
		want = count;

	o = output_open(out, kind, image_width(conv), image_height(conv),
			rate, scale, want == 1);
	if (!o) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}

	size_t cap = 0;
	unsigned long frame = 0;
	for (size_t c = 0; c < s->nchunks && output_frames(o) < want; c++) {
		const struct avi_chunk *k = &s->chunks[c];
		int is_frame = avi_chunk_is_frame(k);
		/* Earlier frames matter only to delta formats. */
		int needed = !is_frame || frame >= start || image_is_delta(conv);

		if (is_frame && !needed) {
			frame++;
			continue;
		}
		if (k->size > cap) {
			BYTE *b = realloc(buf, k->size);
			if (!b) {
				fprintf(stderr, "seance: out of memory\n");
				goto done;
			}
			buf = b;
			cap = k->size;
		}
		if (k->size && avi_read_chunk(avi, k, buf)) {
			fprintf(stderr, "seance: %s: %s\n", in, vfw_error());
			goto done;
		}
		if (!is_frame) {
			if (image_palette_change(conv, buf, k->size))
				fprintf(stderr, "seance: %s: chunk %zu: %s\n",
					in, c, vfw_error());
			continue;
		}

		int r;
		if (frame < start)
			r = image_feed(conv, buf, k->size);
		else if (!k->size && kind != OUT_RAW)
			r = output_repeat(o);	/* a dropped frame */
		else
			r = output_write(o, conv, buf, k->size);
		if (r) {
			fprintf(stderr, "seance: frame %lu: %s\n", frame,
				vfw_error());
			goto done;
		}
		frame++;
	}

	unsigned long written = output_frames(o);
	int r = output_close(o);
	o = NULL;
	if (r) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	fprintf(stderr, "%s: %lu frame%s, %dx%d %s -> %s\n", out, written,
		written == 1 ? "" : "s", image_width(conv), image_height(conv),
		image_format_name(conv), output_kind_name(kind));
	if (avi->truncated)
		warn_truncated(in, "frames were");
	status = 0;

done:
	if (o)
		output_close(o);
	free(buf);
	image_conv_free(conv);
	avi_close(avi);
	return status;
}
