/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance encode <codec> <input> -o <out.avi>: compress video with a
 * VfW codec, DirectShow filter or DMO into an AVI.
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "avi/avi.h"
#include "cmd_audio.h"
#include "cmd_util.h"
#include "commands.h"
#include "common/error.h"
#include "common/fourcc.h"
#include "common/unicode.h"
#include "dshow/discover.h"
#include "dshow/dshow.h"
#include "in/source.h"
#include "loader/pe.h"
#include "out/image.h"
#include "runtime/runtime.h"
#include "vfw/icm.h"
#include "win32/codepage.h"
#include "win32/ole.h"
#include "xform.h"

static int usage(void)
{
	fprintf(stderr,
		"usage: seance encode <codec.dll | fourcc> <input> -o <out.avi> "
		"[options]\n"
		"\n"
		"Video:\n"
		"  input                .y4m (4:2:0 or mono; \"-\" for stdin), an\n"
		"                       uncompressed .avi, a .bmp file or numbered\n"
		"                       files (frame%%04d.bmp), or raw frames with\n"
		"                       --input-format and --size\n"
		"  --fourcc <XXXX>      the handler to open a DLL as\n"
		"  --clsid <{GUID}>     the filter or DMO when a DLL has several\n"
		"  --quality <q>        0 to 10000 (default: the codec's)\n"
		"  --keyint <n>         a keyframe every n frames; 0 for only the\n"
		"                       first (default: the codec's)\n"
		"  --bitrate <kbit/s>   data rate for codecs that can aim for one\n"
		"  --rate <fps>         frame rate: 25, 30000/1001, 29.97 (default:\n"
		"                       the input's, else 25)\n"
		"  --input-format <f>   raw input: I420, YV12, YUY2, UYVY, BGR32,\n"
		"                       BGR24 (top-down), RGB565, RGB555\n"
		"  --size <WxH>         raw input: the frame size\n"
		"  --stream <n>         AVI input: the stream\n"
		"  --codec-input <f>    convert frames to this format for the codec\n"
		"                       (default: as they are if it takes them, else\n"
		"                       the first standard format it takes)\n"
		"  --pad <n>            pad the picture to a multiple of n (2, 4,\n"
		"                       ... 256) wide and high, repeating its last\n"
		"                       row and column, for codecs that want that\n"
		"  --crop <n>           crop it to a multiple of n instead\n"
		"  --start <pos>        first input frame, or a time (2.5s)\n"
		"  --frames <n>         number of frames (default: all)\n"
		"  --duration <len>     how much to encode, as frames or a time\n"
		"  --state <file>       codec state to set first (ICM_SETSTATE)\n"
		"  --save-state <file>  write the codec's state afterwards\n"
		"  --odml               write OpenDML (AVI 2.0) indexes\n"
		"\n"
		"Audio, with a DirectShow filter, DMO or ACM driver (taken for a\n"
		".wav input, raw PCM, an AVI without video, a .wav or .pcm output,\n"
		"or --audio):\n"
		"  input                .wav (\"-\" for stdin), an AVI's PCM stream,\n"
		"                       or raw PCM with --raw\n"
		"  -o <out>             .avi, .wav, or .pcm (the packets alone)\n"
		"  --audio              the first audio stream of an AVI\n"
		"  --format-tag <tag>   the format to make (0x55), for an encoder\n"
		"                       of several\n"
		"  --clsid, --quality, --bitrate, --stream, --state, --save-state\n"
		"                       as for video (an ACM driver takes only\n"
		"                       --bitrate)\n"
		"  --start <pos>        where to begin: a sample number or a time\n"
		"  --duration <len>     how much: samples or a time\n"
		"  --raw <f> --sample-rate <hz> --channels <n>\n"
		"                       raw PCM input: u8, s16, s24, s32, f32, f64\n");
	return 2;
}

static int parse_clsid(const char *s, CLSID *clsid)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	int bad = !w || ole_guid_parse(w, clsid);
	free(w);
	return bad;
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
	/* ICERR_OK or the size used on success, an ICERR_* on failure. */
	LRESULT got = ic_send(hic, ICM_GETSTATE, (DWORD_PTR)data, (DWORD_PTR)n);
	if (got < 0) {
		char err[24];
		vfw_set_error("the codec didn't give its state (ICM_GETSTATE: %s)",
			      icerr_name(got, err));
		free(data);
		return -1;
	}
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

/* Whether the codec takes any standard format at w x h. */
static int takes_size(HIC hic, LONG w, LONG h)
{
	struct vfw_format t;

	for (unsigned i = 0; vfw_std_format(i, w, h, &t); i++)
		if (ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)&t.bih, 0) ==
		    ICERR_OK)
			return 1;
	return 0;
}

/*
 * For a codec that takes nothing at w x h: the sizes it does take, as
 * advice. Most want a multiple of 16 or so (360 lines aren't one): the
 * smallest such multiple padded or cropped to, as --pad or --crop give,
 * else a common size to resize to. "" if it takes none of them.
 */
static void size_hint(HIC hic, LONG w, LONG h, char *out, size_t size)
{
	static const LONG common[][2] = {
		{ 320, 240 }, { 352, 288 }, { 640, 480 }, { 720, 480 },
		{ 720, 576 }, { 1280, 720 }, { 1920, 1080 },
	};

	out[0] = 0;
	for (LONG n = 2; n <= 64; n *= 2) {
		LONG pw = (w + n - 1) / n * n, ph = (h + n - 1) / n * n;
		LONG cw = w / n * n, ch = h / n * n;
		if (pw == w && ph == h)
			continue;	/* the size itself: refused */
		int pad = takes_size(hic, pw, ph);
		int crop = cw && ch && takes_size(hic, cw, ch);
		if (pad && crop)
			snprintf(out, size, ", but takes %ldx%ld and %ldx%ld: "
				 "add --pad %ld or --crop %ld", (long)pw, (long)ph,
				 (long)cw, (long)ch, (long)n, (long)n);
		else if (pad || crop)
			snprintf(out, size, ", but takes %ldx%ld: add --%s %ld",
				 pad ? (long)pw : (long)cw, pad ? (long)ph : (long)ch,
				 pad ? "pad" : "crop", (long)n);
		if (pad || crop)
			return;
	}
	/* The smallest that holds the picture, else the largest that doesn't. */
	size_t n = sizeof(common) / sizeof(common[0]);
	for (int fits = 1; fits >= 0; fits--)
		for (size_t k = 0; k < n; k++) {
			size_t i = fits ? k : n - 1 - k;
			LONG cw = common[i][0], ch = common[i][1];
			if ((cw >= w && ch >= h) != fits || (cw == w && ch == h) ||
			    !takes_size(hic, cw, ch))
				continue;
			snprintf(out, size, ", but takes %ldx%ld: resize the input "
				 "to that", (long)cw, (long)ch);
			return;
		}
}

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
	/* Whether every query was ICERR_UNSUPPORTED: a decompressor only. */
	int unsupported = 1;
	LRESULT r;

	if (!forced) {
		r = ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)native, 0);
		if (r == ICERR_OK) {
			memcpy(&f->fmt, native, n);
			f->fmt_size = n;
			f->name = source_format_name(src);
			return 0;
		}
		unsupported = r == ICERR_UNSUPPORTED;
	}
	for (unsigned i = 0; (name = vfw_std_format(i, w, h, &t)); i++) {
		/*
		 * 8 bits (the last tried): a colour cube's palette if the codec
		 * takes it, else the grey ramp (pvwv220.dll takes only that).
		 */
		if (t.bih.biBitCount == 8) {
			struct vfw_format cube = t;
			image_cube_palette(cube.colors);
			if (ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)&cube.bih,
				    0) == ICERR_OK)
				t = cube;
		}
		if (forced ? strcasecmp(name, forced) != 0 :
		    (r = ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)&t.bih,
				 0)) != ICERR_OK) {
			if (!forced && r != ICERR_UNSUPPORTED)
				unsupported = 0;
			continue;
		}
		if (forced) {
			r = ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)&t.bih, 0);
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
	else if (unsupported)
		vfw_set_error("the codec doesn't compress: it answers ICM_COMPRESS_QUERY "
			      "with ICERR_UNSUPPORTED for %s and every standard "
			      "format, so it only decompresses",
			      source_format_name(src));
	else {
		char hint[200];
		size_hint(hic, w, h, hint, sizeof(hint));
		vfw_set_error("the codec takes neither %s nor any standard format "
			      "at %ldx%ld%s", source_format_name(src), (long)w,
			      (long)h, hint);
	}
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

struct com_encode_output {
	struct avi_writer *writer;
	uint64_t bytes;
	unsigned long written, keys;
	BITMAPINFOHEADER *format;	/* what the file's stream header says */
	size_t format_size;
};

/*
 * Whether a media type a sample announces describes the format the file was
 * opened for. The stream header is written once, before the first sample, so a
 * type that adds format data (a sequence header) or changes the codec or
 * picture would leave the file describing something else than it holds.
 */
static int com_same_format(const struct com_encode_output *out,
			   const AM_MEDIA_TYPE *mt)
{
	BITMAPINFOHEADER *bih = NULL;
	size_t size = 0;
	int same = 0;

	if (out->format && SUCCEEDED(dshow_mt_to_bih(mt, &bih, &size))) {
		/* biSizeImage is an upper bound, not part of the format. */
		DWORD a = bih->biSizeImage, b = out->format->biSizeImage;
		bih->biSizeImage = out->format->biSizeImage = 0;
		same = size == out->format_size &&
		       !memcmp(bih, out->format, size);
		bih->biSizeImage = a;
		out->format->biSizeImage = b;
		free(bih);
	}
	return same;
}

static HRESULT com_write_encoded(void *opaque, const struct xform_output *o,
				 const AM_MEDIA_TYPE *dynamic)
{
	struct com_encode_output *out = opaque;
	if (!out->writer)
		return VFW_E_WRONG_STATE;
	if (dynamic && !com_same_format(out, dynamic))
		return VFW_E_INVALIDMEDIATYPE;
	/* The first frame is a keyframe, as for ICM codecs above. */
	int sync = o->sync || !out->written;
	if (avi_writer_add(out->writer, o->data, (DWORD)o->size, sync))
		return E_FAIL;
	out->written++;
	out->keys += sync;
	out->bytes += o->size;
	return S_OK;
}

static int write_file(const char *path, const void *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		vfw_set_error("%s: can't write it", path);
		return -1;
	}
	int failed = fwrite(data, 1, size, f) != size;
	if (fclose(f))
		failed = 1;
	if (failed)
		vfw_set_error("%s: can't write it", path);
	return failed ? -1 : 0;
}

/*
 * Connects the encoder with this input format and its compression (0 if
 * the encoder hasn't said: whatever it gives); the output format the
 * stream header gets (with any codec data the encoder adds) in *output.
 * 0, or -1 if the encoder won't take it.
 */
static int com_try_input(struct xform *x, const BITMAPINFOHEADER *input,
			 size_t input_size, FOURCC compression, DWORD rate,
			 DWORD scale, struct com_encode_output *sink,
			 BITMAPINFOHEADER **output, size_t *output_size)
{
	AM_MEDIA_TYPE in_mt = {0}, out_mt = {0};
	BITMAPINFOHEADER encoded = *input;
	encoded.biSize = sizeof(encoded);
	encoded.biCompression = compression;
	DWORD raw = input->biSizeImage;
	if (!raw)
		raw = (DWORD)((uint64_t)input->biWidth *
			(input->biHeight < 0 ? -input->biHeight : input->biHeight) *
			input->biBitCount / 8);
	encoded.biSizeImage = raw > (UINT32_MAX - 4096) / 2 ? UINT32_MAX :
			      raw * 2 + 4096;
	HRESULT hr = dshow_bih_to_mt(input, input_size, rate, scale, 0, &in_mt);
	if (SUCCEEDED(hr) && compression)
		hr = dshow_bih_to_mt(&encoded, sizeof(encoded), rate, scale, 0,
				     &out_mt);
	else if (SUCCEEDED(hr))
		out_mt.majortype = in_mt.majortype;	/* any subtype */
	/*
	 * The encoder's own output type of that compression first, as a
	 * graph builder connects it to a file writer that takes anything: it
	 * may carry codec data, and some encoders take nothing else (LEAD's).
	 * Failing that, the type made here from the input.
	 */
	if (SUCCEEDED(hr)) {
		AM_MEDIA_TYPE partial = {
			.majortype = out_mt.majortype, .subtype = out_mt.subtype,
		};
		hr = xform_connect(x, &in_mt, &partial, com_write_encoded, sink);
		/* (A filter may take the partial type and add no format.) */
		if (SUCCEEDED(hr))
			hr = dshow_mt_to_bih(xform_output_type(x), output,
					     output_size);
		if (FAILED(hr) && compression) {
			xform_disconnect(x);
			hr = xform_connect(x, &in_mt, &out_mt, com_write_encoded,
					   sink);
			if (SUCCEEDED(hr))
				hr = dshow_mt_to_bih(xform_output_type(x), output,
						     output_size);
		}
	}
	if (SUCCEEDED(hr)) {
		free(sink->format);
		sink->format_size = *output_size;
		if (!(sink->format = malloc(*output_size)))
			hr = E_OUTOFMEMORY;
		else
			memcpy(sink->format, *output, *output_size);
	}
	if (FAILED(hr)) {
		free(*output);
		*output = NULL;
		xform_disconnect(x);
	}
	dshow_free_media_type(&out_mt);
	dshow_free_media_type(&in_mt);
	return SUCCEEDED(hr) ? 0 : -1;
}

/* Encoding with a DirectShow filter or a DMO. */
static int encode_com(const char *target, const CLSID *selected,
			struct source *src, const char *out_path, const char *forced,
			const char *state_path, const char *save_path,
			unsigned long start, unsigned long count,
			unsigned long quality, unsigned long keyint,
			unsigned long bitrate, DWORD rate, DWORD scale,
			const struct avi_writer_options *writer_options)
{
	struct xform *x = NULL;
	int connected = 0;
	struct feed feed = {0};
	BITMAPINFOHEADER *output_format = NULL;
	size_t output_size = 0;
	struct com_encode_output sink = {0};
	void *state = NULL, *saved = NULL;
	size_t saved_size = 0;
	int status = 1;
	char error[24], fcc[FOURCC_STR_SIZE], desc[160];
	struct xform_class found;
	FOURCC compression;

	HRESULT hr = xform_find(target, selected, 0, 1, &found);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: %s: no unique DirectShow or DMO video "
			"encoder\n", target);
		goto done;
	}
	compression = found.compression;
	hr = xform_create(target, &found, &x);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: cannot open encoder in %s (%s)\n", target,
			dshow_hr_name(hr, error));
		goto done;
	}
	if (!rate)
		source_rate(src, &rate, &scale);
	if (!rate || !scale) {
		rate = 25;
		scale = 1;
	}

	if (state_path) {
		size_t n;
		if (!(state = read_file(state_path, &n))) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		hr = dshow_filter_load_state(xform_object(x), state, n);
		if (FAILED(hr)) {
			fprintf(stderr, "seance: %s: the %s refused this state "
				"(%s)\n", state_path, xform_kind_name(x),
				dshow_hr_name(hr, error));
			goto done;
		}
	}
	struct dshow_encoder_options options = {
		.key_interval = keyint == (unsigned long)-1 ? -1 :
				keyint > INT32_MAX ? INT32_MAX : (LONG)keyint,
		.quality = quality == (unsigned long)-1 ? -1.0 : quality / 10000.0,
		.bitrate = bitrate ? (LONGLONG)bitrate * 1000 : -1,
	};
	if (options.key_interval >= 0 || options.quality >= 0.0 ||
	    options.bitrate >= 0) {
		hr = dshow_encoder_configure(xform_object(x), &options);
		if (FAILED(hr)) {
			fprintf(stderr, "seance: encoder options were refused (%s)\n",
				dshow_hr_name(hr, error));
			goto done;
		}
	}

	size_t native_size;
	const BITMAPINFOHEADER *native = source_format(src, &native_size);
	LONG width = native->biWidth;
	LONG height = native->biHeight < 0 ? -native->biHeight : native->biHeight;
	if (!forced && !com_try_input(x, native, native_size, compression,
			rate, scale, &sink, &output_format, &output_size)) {
		connected = 1;
		memcpy(&feed.fmt, native, native_size);
		feed.fmt_size = native_size;
		feed.name = source_format_name(src);
	} else {
		struct vfw_format candidate;
		const char *name;
		for (unsigned i = 0; !connected &&
		     (name = vfw_std_format(i, width, height, &candidate)); i++) {
			if (candidate.bih.biBitCount == 8 ||
			    (forced && strcasecmp(forced, name)))
				continue;
			if (com_try_input(x, &candidate.bih,
					vfw_format_size(&candidate.bih), compression,
					rate, scale, &sink, &output_format,
					&output_size))
				continue;
			connected = 1;
			feed.fmt = candidate;
			feed.fmt_size = vfw_format_size(&candidate.bih);
			feed.name = name;
			feed.from = image_conv_new(native, native_size);
			feed.to = image_conv_new(&candidate.bih, feed.fmt_size);
			if (!feed.from || !feed.to)
				goto conversion_error;
			size_t mid = image_pack_wants_i420(feed.to) ?
				image_i420_size(width, height) :
				image_bgr24_size(width, height);
			feed.tmp = malloc(mid);
			feed.packed = calloc(1, image_frame_size(feed.to));
			if (!feed.tmp || !feed.packed) {
				vfw_set_error("out of memory");
				goto conversion_error;
			}
		}
	}
	if (!connected) {
		fprintf(stderr, "seance: encoder takes neither %s nor %s standard "
			"input format\n", source_format_name(src),
			forced ? "the requested" : "any");
		goto done;
	}
	/* An encoder that registered no compression says it now. */
	if (!compression)
		compression = output_format->biCompression;
	if (!compression || compression == BI_BITFIELDS) {
		fprintf(stderr, "seance: the encoder's output isn't compressed\n");
		goto done;
	}

	sink.writer = avi_writer_open(out_path, output_format, (DWORD)output_size,
		compression, rate, scale, writer_options);
	if (!sink.writer) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	cli_format_desc(output_format, desc, sizeof(desc));
	unsigned long read = 0, pushed = 0;
	double busy = 0;
	for (;;) {
		if (count != (unsigned long)-1 && pushed >= count)
			break;
		const BYTE *frame;
		size_t len;
		int got = source_read(src, &frame, &len);
		if (got < 0) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		if (!got)
			break;
		if (read++ < start) {
			if (feed.from && image_is_delta(feed.from))
				image_feed(feed.from, frame, len);
			continue;
		}
		void *bits = convert(&feed, frame, len);
		if (!bits) {
			fprintf(stderr, "seance: frame %lu: %s\n", read - 1,
				vfw_error());
			goto done;
		}
		size_t input_bytes = feed.from ? image_frame_size(feed.to) : len;
		REFERENCE_TIME a = (REFERENCE_TIME)pushed * scale * 10000000 / rate;
		REFERENCE_TIME b = (REFERENCE_TIME)(pushed + 1) * scale * 10000000 / rate;
		/*
		 * Every uncompressed frame is a sync point, as a file source
		 * gives them: an encoder that sets none of its own (strmbase's
		 * copy the input's) makes every frame a keyframe, as it would
		 * on Windows, and one with keyframes of its own (asked for
		 * through IAMVideoCompression or ICodecAPI) marks the rest.
		 */
		DWORD flags = AM_SAMPLE_SPLICEPOINT;
		if (!pushed)
			flags |= AM_SAMPLE_DATADISCONTINUITY;
		double then = now();
		hr = xform_push(x, bits, input_bytes, a, b, flags);
		busy += now() - then;
		if (FAILED(hr)) {
			fprintf(stderr, "seance: frame %lu: %s\n", read - 1,
				dshow_hr_name(hr, error));
			goto done;
		}
		pushed++;
	}
	hr = xform_end(x);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: end of stream: %s\n",
			dshow_hr_name(hr, error));
		goto done;
	}
	xform_disconnect(x);
	if (save_path) {
		hr = dshow_filter_save_state(xform_object(x), &saved, &saved_size);
		if (FAILED(hr) || write_file(save_path, saved, saved_size)) {
			if (FAILED(hr))
				vfw_set_error("the %s has no state to save (%s)",
					xform_kind_name(x),
					dshow_hr_name(hr, error));
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
	}
	if (avi_writer_close(sink.writer)) {
		sink.writer = NULL;
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	sink.writer = NULL;
	fprintf(stderr, "%s: %lu frame%s (%lu key), %s -> '%s' (%s)",
		out_path, sink.written, sink.written == 1 ? "" : "s", sink.keys,
		feed.name, fourcc_str(compression, fcc), desc);
	if (sink.written) {
		double seconds = (double)sink.written * scale / rate;
		fprintf(stderr, ", %llu bytes, %.1f kbit/s, %.3f ms/frame",
			(unsigned long long)sink.bytes,
			sink.bytes * 8 / seconds / 1000, busy * 1000 / sink.written);
	}
	fprintf(stderr, "\n");
	status = 0;
	goto done;

conversion_error:
	fprintf(stderr, "seance: cannot convert input to the encoder format: %s\n",
		vfw_error());
done:
	if (sink.writer)
		avi_writer_close(sink.writer);
	xform_close(x);
	free(sink.format);
	image_conv_free(feed.from);
	image_conv_free(feed.to);
	free(feed.tmp);
	free(feed.packed);
	free(output_format);
	free(saved);
	free(state);
	return status;
}

int cmd_encode(int argc, char **argv)
{
	static const UINT modes[] = { ICMODE_COMPRESS };
	const char *target = NULL, *path = NULL, *out_path = NULL, *fourcc = NULL;
	const char *clsid_arg = NULL;
	const char *forced = NULL, *state_path = NULL, *save_path = NULL;
	unsigned long start = 0, count = (unsigned long)-1, quality = (unsigned long)-1;
	unsigned long keyint = (unsigned long)-1, bitrate = 0, stream = (unsigned long)-1;
	unsigned long channels = 0, hz = 0;
	const char *raw = NULL;
	int want_audio = 0;
	DWORD rate = 0, scale = 0;
	struct source_options so = { .stream = -1 };
	struct avi_writer_options wo = { 0 };
	struct audio_codec_job job = { .source.stream = -1, .quality = -1 };

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int v = i + 1 < argc;

		if (!strcmp(a, "-o") && v)
			out_path = argv[++i];
		else if (!strcmp(a, "--fourcc") && v)
			fourcc = argv[++i];
		else if (!strcmp(a, "--clsid") && v)
			clsid_arg = argv[++i];
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
		} else if ((!strcmp(a, "--pad") || !strcmp(a, "--crop")) && v) {
			unsigned long n;
			if (cli_parse_count(a, argv[++i], &n))
				return 2;
			if (n < 2 || n > 256 || (n & (n - 1))) {
				fprintf(stderr, "seance: %s takes 2, 4, 8, ... 256\n",
					a);
				return 2;
			}
			if (a[2] == 'p')
				so.pad = (int)n;
			else
				so.crop = (int)n;
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
			if (cli_parse_position(a, argv[++i], &job.start))
				return 2;
			job.has_start = 1;
		} else if (!strcmp(a, "--duration") && v) {
			if (cli_parse_position(a, argv[++i], &job.duration))
				return 2;
			job.has_duration = 1;
		} else if (!strcmp(a, "--audio")) {
			want_audio = 1;
		} else if (!strcmp(a, "--format-tag") && v) {
			char *end;
			const char *t = argv[++i];
			job.format_tag = strtoul(t, &end, 0);
			if (!*t || *end || !job.format_tag ||
			    job.format_tag > 0xffff) {
				fprintf(stderr, "seance: --format-tag wants a "
					"format tag (85, 0x55), not '%s'\n", t);
				return 2;
			}
		} else if (!strcmp(a, "--raw") && v) {
			raw = argv[++i];
		} else if (!strcmp(a, "--channels") && v) {
			if (cli_parse_count(a, argv[++i], &channels))
				return 2;
		} else if (!strcmp(a, "--sample-rate") && v) {
			if (cli_parse_count(a, argv[++i], &hz))
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
		fprintf(stderr, "seance: --quality is 0 to %d\n", ICQUALITY_HIGH);
		return 2;
	}
	if (so.raw_format && !so.width) {
		fprintf(stderr, "seance: --input-format needs --size\n");
		return 2;
	}
	if (stream != (unsigned long)-1)
		so.stream = stream > 1000 ? 1000 : (int)stream;

	/* Audio: a WAV, raw PCM, or an AVI's audio stream. */
	if (raw || cli_wants_audio(path, so.stream, want_audio, NULL, out_path,
				   audio_only_kinds, target, clsid_arg, 1)) {
		if (fourcc || forced || keyint != (unsigned long)-1 || rate ||
		    count != (unsigned long)-1 || so.raw_format || wo.force_odml ||
		    so.pad || so.crop) {
			fprintf(stderr, "seance: --fourcc, --codec-input, --keyint, "
				"--rate, --frames, --input-format, --odml, --pad "
				"and --crop are for video\n");
			return 2;
		}
		if (clsid_arg && parse_clsid(clsid_arg, &job.clsid_value)) {
			fprintf(stderr, "seance: --clsid wants a GUID, not '%s'\n",
				clsid_arg);
			return 2;
		}
		job.clsid = clsid_arg ? &job.clsid_value : NULL;
		job.codec = target;
		job.in = path;
		job.out = out_path;
		job.source.stream = so.stream;
		job.state = state_path;
		job.save_state = save_path;
		job.quality = quality == (unsigned long)-1 ? -1 : (long)quality;
		job.bitrate = bitrate;
		if (raw) {
			job.source.kind = AUDIO_SOURCE_RAW;
			job.source.channels = channels;
			job.source.rate = hz;
			if (audio_sample_from_name(raw, &job.source.raw_sample)) {
				fprintf(stderr, "seance: %s\n", vfw_error());
				return 2;
			}
		}
		return encode_audio(&job);
	}
	if (raw || channels || hz || job.format_tag) {
		fprintf(stderr, "seance: --raw, --channels, --sample-rate and "
			"--format-tag are for audio\n");
		return 2;
	}

	struct source *src = source_open(path, &so);
	if (!src) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}
	/* --start and --duration in frames, at the rate the frames will have. */
	{
		DWORD r = rate, sc = scale;
		if (!r)
			source_rate(src, &r, &sc);
		if (!r || !sc) {
			r = 25;
			sc = 1;
		}
		if (job.has_start) {
			unsigned long long n = cli_position_units(&job.start, r, sc);
			start = n > ULONG_MAX ? ULONG_MAX : (unsigned long)n;
		}
		if (job.has_duration) {
			unsigned long long n = cli_position_units(&job.duration, r, sc);
			if (n < count)
				count = (unsigned long)n;
		}
	}
	struct pe_image *image = pe_open(target);
	int directshow = image && dshow_is_server(image);
	pe_free(image);
	if (directshow || clsid_arg) {
		CLSID clsid, *selected = NULL;
		if (!directshow) {
			fprintf(stderr, "seance: --clsid requires a DirectShow or DMO server\n");
			source_close(src);
			return 2;
		}
		if (fourcc) {
			fprintf(stderr, "seance: --fourcc is for VfW codecs\n");
			source_close(src);
			return 2;
		}
		if (clsid_arg) {
			if (parse_clsid(clsid_arg, &clsid)) {
				fprintf(stderr, "seance: --clsid wants a GUID, not '%s'\n",
					clsid_arg);
				source_close(src);
				return 2;
			}
			selected = &clsid;
		}
		int status = encode_com(target, selected, src, out_path, forced,
			state_path, save_path, start, count, quality, keyint, bitrate,
			rate, scale, &wo);
		source_close(src);
		return status;
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
		fprintf(stderr, "seance: %s\n", vfw_error());
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
	/* Without --quality, ic_seq_start_ex asks the codec for its own. */
	cv.lQ = quality == (unsigned long)-1 ? ICQUALITY_DEFAULT : (LONG)quality;
	cv.lDataRate = (LONG)((bitrate + 7) / 8);	/* KB/s */
	if (state_path) {
		size_t n;
		if (!(state = read_file(state_path, &n))) {
			fprintf(stderr, "seance: %s\n", vfw_error());
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
		fprintf(stderr, "seance: %s\n", vfw_error());
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
		fprintf(stderr, "seance: %s\n", vfw_error());
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
			fprintf(stderr, "seance: %s\n", vfw_error());
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
			fprintf(stderr, "seance: frame %lu: %s\n", read - 1,
				vfw_error());
			goto done;
		}
		BOOL key = FALSE;
		LONG size = 0;
		double t = now();
		void *data = ic_seq_frame(&cv, 0, bits, &key, &size);
		busy += now() - t;
		if (!data) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		/*
		 * The first frame is decoded from nothing, so it is a keyframe
		 * whatever the codec says (WavCWAIP.dll and icmw_32.dll flag
		 * none, which leaves a file nothing can seek in).
		 */
		if (!written)
			key = TRUE;
		if (avi_writer_add(w, data, (DWORD)size, key)) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		written++;
		keys += key != 0;
		bytes += (uint64_t)size;
	}

	ic_seq_end(&cv);
	started = 0;
	if (save_path && save_state(codec.hic, save_path)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	int cr = avi_writer_close(w);
	w = NULL;
	if (cr) {
		fprintf(stderr, "seance: %s\n", vfw_error());
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
		fprintf(stderr, "seance: warning: no frames were written\n");
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
