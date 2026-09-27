/*
 * vfw-harness encode <codec> <input> -o <out.avi>: compress video with a
 * VfW codec into an AVI, through the ICSeqCompressFrame* sequence API.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "avi/avi.h"
#include "cmd_util.h"
#include "commands.h"
#include "common/error.h"
#include "common/fourcc.h"
#include "in/source.h"
#include "out/image.h"
#include "vfw/icm.h"

static int usage(void)
{
	fprintf(stderr,
		"usage: vfw-harness encode <codec.dll | fourcc> <input> -o <out.avi> "
		"[options]\n"
		"\n"
		"  input                .y4m (4:2:0 or mono; \"-\" for stdin), an\n"
		"                       uncompressed .avi, a .bmp file or numbered\n"
		"                       files (frame%%04d.bmp), or raw frames with\n"
		"                       --input-format and --size\n"
		"  --fourcc <XXXX>      the handler to open a DLL as\n"
		"  --quality <q>        0 to 10000 (default: the codec's)\n"
		"  --keyint <n>         a keyframe every n frames; 0 for only the\n"
		"                       first (default: the codec's)\n"
		"  --bitrate <kbit/s>   data rate for codecs that can aim for one\n"
		"  --rate <fps>         frame rate: 25, 30000/1001, 29.97 (default:\n"
		"                       the input's, else 25)\n"
		"  --input-format <f>   raw input: I420, YV12, YUY2, UYVY, BGR32,\n"
		"                       BGR24 (top-down), RGB565, RGB555\n"
		"  --size <WxH>         raw input: the frame size\n"
		"  --stream <n>         AVI input: the video stream\n"
		"  --codec-input <f>    convert frames to this format for the codec\n"
		"                       (default: as they are if it takes them, else\n"
		"                       the first standard format it takes)\n"
		"  --start <n>          first input frame (default 0)\n"
		"  --frames <n>         number of frames (default: all)\n"
		"  --state <file>       codec state to set first (ICM_SETSTATE)\n"
		"  --save-state <file>  write the codec's state afterwards\n"
		"  --odml               write OpenDML (AVI 2.0) indexes\n");
	return 2;
}

static void *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	void *data = NULL;
	long n;

	if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > (64L << 20) ||
	    fseek(f, 0, SEEK_SET) || !(data = malloc(n ? (size_t)n : 1)) ||
	    fread(data, 1, (size_t)n, f) != (size_t)n) {
		vfw_set_error("%s: can't read it", path);
		free(data);
		data = NULL;
	} else {
		*len = (size_t)n;
	}
	if (f)
		fclose(f);
	return data;
}

static int save_state(HIC hic, const char *path)
{
	LRESULT n = ic_send(hic, ICM_GETSTATE, 0, 0);
	void *data;
	FILE *f;

	if (n <= 0 || n > (64L << 20)) {
		vfw_set_error("the codec has no state to save");
		return -1;
	}
	if (!(data = calloc(1, (size_t)n))) {
		vfw_set_error("out of memory");
		return -1;
	}
	LRESULT got = ic_send(hic, ICM_GETSTATE, (DWORD_PTR)data, (DWORD_PTR)n);
	if (got > 0 && got < n)
		n = got;
	int r = -1;
	if (!(f = fopen(path, "wb")) || fwrite(data, 1, (size_t)n, f) != (size_t)n)
		vfw_set_error("%s: can't write it", path);
	else
		r = 0;
	if (f && fclose(f) && !r) {
		vfw_set_error("%s: can't write it", path);
		r = -1;
	}
	free(data);
	return r;
}

/* The input format the codec gets: the source's, or a standard one. */
struct feed {
	struct vfw_format fmt;
	size_t fmt_size;
	const char *name;
	struct image_conv *from;	/* NULL when frames go as they are */
	struct image_conv *to;
	BYTE *tmp, *packed;
};

static int choose_input(HIC hic, const struct source *src, const char *forced,
			struct feed *f)
{
	size_t n;
	const BITMAPINFOHEADER *native = source_format(src, &n);
	LONG w = native->biWidth;
	LONG h = native->biHeight < 0 ? -native->biHeight : native->biHeight;
	struct vfw_format t;
	const char *name;
	char err[24];

	if (!forced && ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)native, 0) ==
	    ICERR_OK) {
		memcpy(&f->fmt, native, n);
		f->fmt_size = n;
		f->name = source_format_name(src);
		return 0;
	}
	for (unsigned i = 0; (name = vfw_std_format(i, w, h, &t)); i++) {
		if (t.bih.biBitCount == 8)
			continue;	/* no palettes */
		if (forced ? strcasecmp(name, forced) != 0 :
		    ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)&t.bih, 0) != ICERR_OK)
			continue;
		if (forced) {
			LRESULT r = ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)&t.bih, 0);
			if (r != ICERR_OK) {
				vfw_set_error("the codec won't compress %s (%s)", name,
					      icerr_name(r, err));
				return -1;
			}
		}
		f->fmt = t;
		f->fmt_size = vfw_format_size(&t.bih);
		f->name = name;
		if (!(f->from = image_conv_new(native, n)) ||
		    !(f->to = image_conv_new(&t.bih, f->fmt_size)))
			return -1;
		size_t mid = image_pack_wants_i420(f->to) ? image_i420_size(w, h) :
			     image_bgr24_size(w, h);
		if (!(f->tmp = malloc(mid)) ||
		    !(f->packed = calloc(1, image_frame_size(f->to)))) {
			vfw_set_error("out of memory");
			return -1;
		}
		return 0;
	}
	if (forced)
		vfw_set_error("unknown codec input format '%s'", forced);
	else
		vfw_set_error("the codec takes neither %s nor any standard format "
			      "at %ldx%ld", source_format_name(src), (long)w, (long)h);
	return -1;
}

/* A frame as the codec wants it. NULL with vfw_error(). */
static void *convert(struct feed *f, const BYTE *frame, size_t len)
{
	if (!f->from)
		return (void *)frame;
	int r = image_pack_wants_i420(f->to) ?
		image_to_i420(f->from, frame, len, f->tmp) :
		image_to_bgr24(f->from, frame, len, f->tmp);
	if (r || image_pack(f->to, f->tmp, f->packed))
		return NULL;
	return f->packed;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int cmd_encode(int argc, char **argv)
{
	static const UINT modes[] = { ICMODE_COMPRESS };
	const char *target = NULL, *path = NULL, *out_path = NULL, *fourcc = NULL;
	const char *forced = NULL, *state_path = NULL, *save_path = NULL;
	unsigned long start = 0, count = (unsigned long)-1, quality = (unsigned long)-1;
	unsigned long keyint = (unsigned long)-1, bitrate = 0, stream = (unsigned long)-1;
	DWORD rate = 0, scale = 0;
	struct source_options so = { .stream = -1 };
	struct avi_writer_options wo = { 0 };

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int v = i + 1 < argc;

		if (!strcmp(a, "-o") && v)
			out_path = argv[++i];
		else if (!strcmp(a, "--fourcc") && v)
			fourcc = argv[++i];
		else if (!strcmp(a, "--codec-input") && v)
			forced = argv[++i];
		else if (!strcmp(a, "--input-format") && v)
			so.raw_format = argv[++i];
		else if (!strcmp(a, "--state") && v)
			state_path = argv[++i];
		else if (!strcmp(a, "--save-state") && v)
			save_path = argv[++i];
		else if (!strcmp(a, "--odml"))
			wo.force_odml = 1;
		else if (!strcmp(a, "--size") && v) {
			LONG w, h;
			if (cli_parse_size(a, argv[++i], &w, &h))
				return 2;
			so.width = (int)w;
			so.height = (int)h;
		} else if (!strcmp(a, "--rate") && v) {
			if (cli_parse_rate(a, argv[++i], &rate, &scale))
				return 2;
		} else if (!strcmp(a, "--quality") && v) {
			if (cli_parse_count(a, argv[++i], &quality))
				return 2;
		} else if (!strcmp(a, "--keyint") && v) {
			if (cli_parse_count(a, argv[++i], &keyint))
				return 2;
		} else if (!strcmp(a, "--bitrate") && v) {
			if (cli_parse_count(a, argv[++i], &bitrate))
				return 2;
		} else if (!strcmp(a, "--start") && v) {
			if (cli_parse_count(a, argv[++i], &start))
				return 2;
		} else if (!strcmp(a, "--frames") && v) {
			if (cli_parse_count(a, argv[++i], &count))
				return 2;
		} else if (!strcmp(a, "--stream") && v) {
			if (cli_parse_count(a, argv[++i], &stream))
				return 2;
		} else if ((a[0] == '-' && a[1]) || (target && path))
			return usage();
		else if (!target)
			target = a;
		else
			path = a;
	}
	if (!target || !path || !out_path)
		return usage();
	if (quality != (unsigned long)-1 && quality > ICQUALITY_HIGH) {
		fprintf(stderr, "vfw-harness: --quality is 0 to %d\n", ICQUALITY_HIGH);
		return 2;
	}
	if (so.raw_format && !so.width) {
		fprintf(stderr, "vfw-harness: --input-format needs --size\n");
		return 2;
	}
	if (stream != (unsigned long)-1)
		so.stream = stream > 1000 ? 1000 : (int)stream;

	struct source *src = source_open(path, &so);
	if (!src) {
		fprintf(stderr, "vfw-harness: %s\n", vfw_error());
		return 1;
	}

	int status = 1, started = 0;
	struct cli_codec codec = { 0 };
	struct feed feed = { 0 };
	COMPVARS cv;
	struct avi_writer *w = NULL;
	void *state = NULL;
	char fcc[FOURCC_STR_SIZE];

	memset(&cv, 0, sizeof(cv));
	int r = cli_open_codec(target, fourcc, 0, modes, 1, &codec);
	if (r) {
		status = r;
		goto done;
	}
	if (choose_input(codec.hic, src, forced, &feed)) {
		fprintf(stderr, "vfw-harness: %s\n", vfw_error());
		goto done;
	}

	/* The sequence: keyframes, quality, data rate and state. */
	cv.cbSize = sizeof(cv);
	cv.hic = codec.hic;
	cv.fccType = ICTYPE_VIDEO;
	cv.fccHandler = codec.handler;
	if (keyint == (unsigned long)-1) {
		DWORD k = 0;
		ic_send(codec.hic, ICM_GETDEFAULTKEYFRAMERATE, (DWORD_PTR)&k, 0);
		keyint = k;
	}
	cv.lKey = keyint > INT32_MAX ? INT32_MAX : (LONG)keyint;
	cv.lQ = quality == (unsigned long)-1 ? ICQUALITY_DEFAULT : (LONG)quality;
	cv.lDataRate = (LONG)((bitrate + 7) / 8);	/* KB/s */
	if (state_path) {
		size_t n;
		if (!(state = read_file(state_path, &n))) {
			fprintf(stderr, "vfw-harness: %s\n", vfw_error());
			goto done;
		}
		cv.lpState = state;
		cv.cbState = (LONG)n;
		cv.dwFlags = ICMF_COMPVARS_VALID;
	}
	if (!rate)
		source_rate(src, &rate, &scale);
	if (!rate || !scale) {
		rate = 25;
		scale = 1;
	}
	long total = source_frames(src);
	long frames = total < 0 ? 0 : total > (long)start ? total - (long)start : 0;
	if (count != (unsigned long)-1 && (total < 0 || (unsigned long)frames > count))
		frames = (long)count;
	if (!ic_seq_start_ex(&cv, (BITMAPINFO *)&feed.fmt, rate, scale, frames)) {
		fprintf(stderr, "vfw-harness: %s\n", vfw_error());
		goto done;
	}
	started = 1;

	const BITMAPINFOHEADER *ofmt = &cv.lpbiOut->bmiHeader;
	FOURCC handler = codec.have_info && codec.info.fccHandler ?
			 codec.info.fccHandler : codec.handler ? codec.handler :
			 ofmt->biCompression;
	w = avi_writer_open(out_path, ofmt, (DWORD)ic_seq_format_size(&cv),
			    handler, rate, scale, &wo);
	if (!w) {
		fprintf(stderr, "vfw-harness: %s\n", vfw_error());
		goto done;
	}
	/* What to report; lpbiOut goes with the sequence. */
	char out_desc[160];
	FOURCC out_fcc = ofmt->biCompression;
	cli_format_desc(ofmt, out_desc, sizeof(out_desc));

	unsigned long read = 0, written = 0, keys = 0;
	uint64_t bytes = 0;
	double busy = 0;
	for (;;) {
		if (count != (unsigned long)-1 && written >= count)
			break;
		const BYTE *frame;
		size_t len;
		int got = source_read(src, &frame, &len);
		if (got < 0) {
			fprintf(stderr, "vfw-harness: %s\n", vfw_error());
			goto done;
		}
		if (!got)
			break;
		if (read++ < start) {
			/* Frames that build on earlier ones still need these. */
			if (feed.from && image_is_delta(feed.from))
				image_feed(feed.from, frame, len);
			continue;
		}
		void *bits = convert(&feed, frame, len);
		if (!bits) {
			fprintf(stderr, "vfw-harness: frame %lu: %s\n", read - 1,
				vfw_error());
			goto done;
		}
		BOOL key = FALSE;
		LONG size = 0;
		double t = now();
		void *data = ic_seq_frame(&cv, 0, bits, &key, &size);
		busy += now() - t;
		if (!data) {
			fprintf(stderr, "vfw-harness: %s\n", vfw_error());
			goto done;
		}
		if (avi_writer_add(w, data, (DWORD)size, key)) {
			fprintf(stderr, "vfw-harness: %s\n", vfw_error());
			goto done;
		}
		written++;
		keys += key != 0;
		bytes += (uint64_t)size;
	}

	ic_seq_end(&cv);
	started = 0;
	if (save_path && save_state(codec.hic, save_path)) {
		fprintf(stderr, "vfw-harness: %s\n", vfw_error());
		goto done;
	}
	int cr = avi_writer_close(w);
	w = NULL;
	if (cr) {
		fprintf(stderr, "vfw-harness: %s\n", vfw_error());
		goto done;
	}
	fprintf(stderr, "%s: %lu frame%s (%lu key), %s -> '%s' (%s)", out_path,
		written, written == 1 ? "" : "s", keys, feed.name,
		fourcc_str(out_fcc, fcc), out_desc);
	if (written) {
		double secs = (double)written * scale / rate;
		fprintf(stderr, ", %llu bytes, %.1f kbit/s, %.3f ms/frame",
			(unsigned long long)bytes, bytes * 8 / secs / 1000,
			busy * 1000 / written);
	}
	fprintf(stderr, "\n");
	if (!written)
		fprintf(stderr, "vfw-harness: warning: no frames were written\n");
	status = 0;

done:
	if (started)
		ic_seq_end(&cv);
	if (w)
		avi_writer_close(w);
	cli_close_codec(&codec);
	image_conv_free(feed.from);
	image_conv_free(feed.to);
	free(feed.tmp);
	free(feed.packed);
	free(state);
	source_close(src);
	return status;
}
