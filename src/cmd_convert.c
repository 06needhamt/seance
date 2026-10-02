/*
 * seance convert <in.avi> -o <out>: write an AVI's uncompressed video
 * as Y4M, BMP files or raw frames, without a codec.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avi/avi.h"
#include "cmd_util.h"
#include "commands.h"
#include "common/error.h"
#include "out/image.h"
#include "out/output.h"

static int usage(void)
{
	fprintf(stderr,
		"usage: seance convert <in.avi> -o <out> [options]\n"
		"\n"
		"  -o <out>           output: .y4m, .yuv (I420), .bgr (BGR24),\n"
		"                     .raw (frames as stored) or \"-\" for stdout;\n"
		"                     BMP files with a frame number: out%%05d.bmp\n"
		"  --format <fmt>     y4m, bmp, yuv, bgr or raw, instead of the\n"
		"                     extension\n"
		"  --stream <n>       the video stream to use (default: the first)\n"
		"  --start <n>        first frame to write (default 0)\n"
		"  --frames <n>       number of frames to write (default: all)\n");
	return 2;
}

int cmd_convert(int argc, char **argv)
{
	const char *in = NULL, *out = NULL, *format = NULL;
	unsigned long start = 0, count = (unsigned long)-1, stream = (unsigned long)-1;
	enum output_kind kind;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int has_value = i + 1 < argc;

		if (!strcmp(a, "-o") && has_value)
			out = argv[++i];
		else if (!strcmp(a, "--format") && has_value)
			format = argv[++i];
		else if (!strcmp(a, "--start") && has_value) {
			if (cli_parse_count(a, argv[++i], &start))
				return 2;
		} else if (!strcmp(a, "--frames") && has_value) {
			if (cli_parse_count(a, argv[++i], &count))
				return 2;
		} else if (!strcmp(a, "--stream") && has_value) {
			if (cli_parse_count(a, argv[++i], &stream))
				return 2;
		} else if (a[0] == '-' || in)
			return usage();
		else
			in = a;
	}
	if (!in || !out)
		return usage();
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
		fprintf(stderr, "seance: warning: %s is truncated or "
			"damaged; some frames were missing\n", in);
	status = 0;

done:
	if (o)
		output_close(o);
	free(buf);
	image_conv_free(conv);
	avi_close(avi);
	return status;
}
