/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance decode <codec> <in.avi> -o <out>: decompress an AVI's video
 * stream with a VfW codec and write the frames as Y4M, BMP files or raw.
 */
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
#include "common/unicode.h"
#include "dshow/discover.h"
#include "dshow/dshow.h"
#include "loader/pe.h"
#include "out/image.h"
#include "out/output.h"
#include "runtime/runtime.h"
#include "vfw/icm.h"
#include "win32/codepage.h"
#include "win32/ole.h"
#include "win32/path.h"

static int usage(void)
{
	fprintf(stderr,
		"usage: seance decode <codec.dll | fourcc | auto> <in.avi> "
		"-o <out> [options]\n"
		"\n"
		"  -o <out>             output: .y4m, .yuv (I420), .bgr (BGR24),\n"
		"                       .raw (the codec's output as is) or \"-\";\n"
		"                       BMP files with a frame number: out%%05d.bmp\n"
		"  --format <fmt>       y4m, bmp, yuv, bgr or raw, instead of the\n"
		"                       extension\n"
		"  --fourcc <XXXX>      the handler to open a DLL as\n"
		"  --clsid <{GUID}>     DirectShow class when a server has several\n"
		"  --output-format <f>  what the codec should decompress to: I420,\n"
		"                       YV12, YUY2, UYVY, BGR32, BGR24, RGB565,\n"
		"                       RGB555 or PAL8 (default: negotiated)\n"
		"  --stream <n>         the video stream (default: the first)\n"
		"  --start <n>          first frame to write; decoding starts at the\n"
		"                       keyframe before it (default 0)\n"
		"  --frames <n>         number of frames to write (default: all)\n"
		"  --state <file>       codec state to set first (ICM_SETSTATE)\n"
		"  --keep-going         on a frame the codec rejects, repeat the last\n"
		"                       frame and carry on\n"
		"\n"
		"\"auto\" picks a registered codec that accepts the stream.\n");
	return 2;
}

static int parse_clsid(const char *s, CLSID *clsid)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	int bad = !w || ole_guid_parse(w, clsid);
	free(w);
	return bad;
}

/* Reads a whole (small) file. NULL with vfw_error(). */
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

/* A standard format by name, or -1. */
static int std_format(const char *name, LONG w, LONG h, struct vfw_format *f)
{
	const char *n;

	for (unsigned i = 0; (n = vfw_std_format(i, w, h, f)); i++)
		if (!strcasecmp(n, name))
			return (int)i;
	return -1;
}

/*
 * The output format: the one asked for, or one that suits the output kind
 * (I420 for Y4M, BGR24 for BMP) if the codec takes it, else what it
 * suggests or the first standard one it accepts.
 */
static BITMAPINFOHEADER *choose_output(HIC hic, const BITMAPINFOHEADER *in,
				       const char *forced, enum output_kind kind)
{
	static const char *const yuv[] = { "I420", "YV12", NULL };
	static const char *const rgb[] = { "BGR24", NULL };
	const char *const *prefer = kind == OUT_Y4M || kind == OUT_YUV ? yuv :
				    kind == OUT_BMP || kind == OUT_BGR ? rgb : NULL;
	LONG w = in->biWidth, h = in->biHeight < 0 ? -in->biHeight : in->biHeight;
	BITMAPINFOHEADER *out;
	struct vfw_format f;
	char err[24];
	LRESULT r;

	if (forced) {
		if (std_format(forced, w, h, &f) < 0) {
			vfw_set_error("unknown output format '%s'", forced);
			return NULL;
		}
		r = ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in, (DWORD_PTR)&f.bih);
		if (r != ICERR_OK) {
			vfw_set_error("the codec won't decompress to %s (%s)", forced,
				      icerr_name(r, err));
			return NULL;
		}
		goto copy;
	}
	for (size_t i = 0; prefer && prefer[i]; i++) {
		std_format(prefer[i], w, h, &f);
		if (ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in,
			    (DWORD_PTR)&f.bih) == ICERR_OK)
			goto copy;
	}
	out = ic_negotiate_output(hic, in, &r);
	if (!out)
		vfw_set_error("no output format the codec accepts (%s)",
			      icerr_name(r, err));
	return out;

copy:
	out = malloc(sizeof(f));
	if (!out) {
		vfw_set_error("out of memory");
		return NULL;
	}
	memcpy(out, &f, sizeof(f));
	return out;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

struct dshow_decode_output {
	struct output *output;
	struct image_conv *conv;
};

static HRESULT dshow_write_sample(void *opaque, IMediaSample *sample,
				  const AM_MEDIA_TYPE *dynamic)
{
	struct dshow_decode_output *d = opaque;
	BYTE *data = NULL;
	LONG size = sample->lpVtbl->GetActualDataLength(sample);
	if (dynamic || size < 0 || FAILED(sample->lpVtbl->GetPointer(sample, &data)))
		return VFW_E_INVALIDMEDIATYPE;
	if (sample->lpVtbl->IsPreroll(sample) == S_OK)
		return S_OK;
	if (output_write(d->output, d->conv, data, (size_t)size)) {
		return E_FAIL;
	}
	return S_OK;
}

static char *registered_decoder(const BITMAPINFOHEADER *input, CLSID *clsid)
{
	struct dshow_registered *filters = NULL;
	size_t count = dshow_registered_filters(&filters);
	GUID subtype = FOURCC_SUBTYPE_INIT(input->biCompression);
	const struct dshow_registered *best = NULL;
	GUID null_guid = {0};

	for (size_t i = 0; i < count; i++) {
		const struct qz_filter *f = &filters[i].f;
		if (!filters[i].path || dshow_role(f) != DSHOW_DECODER)
			continue;
		int accepts = 0;
		for (size_t p = 0; p < f->npins && !accepts; p++) {
			if (f->pins[p].flags & REG_PINFLAG_B_OUTPUT)
				continue;
			for (size_t t = 0; t < f->pins[p].ntypes; t++) {
				const struct qz_type *type = &f->pins[p].types[t];
				int video = !memcmp(&type->major, &MEDIATYPE_Video,
						    sizeof(GUID)) ||
					    !memcmp(&type->major, &null_guid, sizeof(GUID));
				int matches = !memcmp(&type->minor, &subtype,
						      sizeof(GUID)) ||
					      !memcmp(&type->minor, &null_guid, sizeof(GUID));
				if (video && matches) {
					accepts = 1;
					break;
				}
			}
		}
		if (accepts && (!best || f->merit > best->f.merit))
			best = &filters[i];
	}
	char *path = NULL;
	if (best) {
		path = path_to_unix(best->path);
		*clsid = best->f.clsid;
	}
	dshow_registered_free(filters, count);
	return path;
}

static int decode_dshow(const char *target, const CLSID *selected,
			struct avi_file *avi, const struct avi_stream *s,
			BITMAPINFOHEADER *in, size_t in_size, const char *out_path,
			enum output_kind kind, const char *forced,
			const char *state_path, unsigned long start,
			unsigned long count, int keep_going)
{
	struct dshow_filter *server = NULL;
	IBaseFilter *filter = NULL;
	struct dshow_host *host = NULL;
	BITMAPINFOHEADER *out = NULL;
	struct image_conv *conv = NULL;
	struct output *output = NULL;
	BYTE *buf = NULL;
	void *state = NULL;
	AM_MEDIA_TYPE in_mt = {0}, out_mt = {0};
	struct dshow_decode_output sink = {0};
	int status = 1;
	char desc[160], name[24], fcc[FOURCC_STR_SIZE];

	DWORD rate = s->header.dwRate, scale = s->header.dwScale;
	if (!rate || !scale) {
		rate = 1000000;
		scale = avi->header.dwMicroSecPerFrame ?
			avi->header.dwMicroSecPerFrame : 40000;
	}
	DWORD largest = in->biSizeImage;
	for (size_t i = 0; i < s->nchunks; i++)
		if (avi_chunk_is_frame(&s->chunks[i]) && s->chunks[i].size > largest)
			largest = s->chunks[i].size;
	in->biSizeImage = largest;
	HRESULT hr = dshow_filter_open(target, selected, 0, &server, &filter, NULL);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: cannot open a unique decoder in %s (%s)\n",
			target, dshow_hr_name(hr, name));
		goto done;
	}
	if (state_path) {
		size_t n;
		if (!(state = read_file(state_path, &n))) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		hr = dshow_filter_load_state(filter, state, n);
		if (FAILED(hr)) {
			fprintf(stderr, "seance: %s: filter refused this state (%s)\n",
				state_path, dshow_hr_name(hr, name));
			goto done;
		}
	}
	if (FAILED(hr = dshow_bih_to_mt(in, in_size, rate, scale, 0, &in_mt)))
		goto media_error;

	LONG height = in->biHeight < 0 ? -in->biHeight : in->biHeight;
	struct vfw_format fmt;
	const char *format_name;
	int passes = forced || kind == OUT_RAW ? 1 : 2;
	for (int pass = 0; pass < passes && !host; pass++) {
		for (unsigned i = 0; (format_name = vfw_std_format(i, in->biWidth,
							     height, &fmt)); i++) {
			if (forced && strcasecmp(forced, format_name))
				continue;
			int preferred =
				((kind == OUT_Y4M || kind == OUT_YUV) &&
				 (!strcasecmp(format_name, "I420") ||
				  !strcasecmp(format_name, "YV12"))) ||
				((kind == OUT_BMP || kind == OUT_BGR) &&
				 !strcasecmp(format_name, "BGR24"));
			if (!forced && kind != OUT_RAW && preferred != !pass)
				continue;
			if (FAILED(dshow_bih_to_mt(&fmt.bih, sizeof(fmt), rate, scale,
						       0, &out_mt)))
				continue;
			hr = dshow_host_create(filter, &in_mt, &out_mt,
					       dshow_write_sample, &sink, &host);
			if (SUCCEEDED(hr)) {
				out = malloc(sizeof(fmt));
				if (out)
					memcpy(out, &fmt, sizeof(fmt));
				break;
			}
			dshow_free_media_type(&out_mt);
		}
	}
	if (!host || !out) {
		if (forced)
			fprintf(stderr, "seance: filter won't connect to output format %s\n",
				forced);
		else
			fprintf(stderr, "seance: no suitable DirectShow output format (%s)\n",
				dshow_hr_name(hr, name));
		goto done;
	}
	conv = image_conv_new(out, vfw_format_size(out));
	if (!conv && kind != OUT_RAW) {
		fprintf(stderr, "seance: filter output (%s) cannot be converted\n",
			cli_format_desc(out, desc, sizeof(desc)));
		goto done;
	}

	int has_keys = 0;
	for (size_t c = 0; c < s->nchunks && !has_keys; c++)
		has_keys = !!(s->chunks[c].flags & AVIIF_KEYFRAME);
	size_t first_chunk = 0;
	unsigned long first_frame = 0, frame = 0;
	for (size_t c = 0; c < s->nchunks && frame <= start; c++) {
		if (!avi_chunk_is_frame(&s->chunks[c]))
			continue;
		if (!has_keys || (s->chunks[c].flags & AVIIF_KEYFRAME)) {
			first_chunk = c;
			first_frame = frame;
		}
		frame++;
	}
	if (start) {
		REFERENCE_TIME segment_start =
			(REFERENCE_TIME)first_frame * scale * 10000000 / rate;
		REFERENCE_TIME segment_stop =
			(REFERENCE_TIME)s->frames * scale * 10000000 / rate;
		hr = dshow_host_seek(host, segment_start, segment_stop);
		if (FAILED(hr)) {
			fprintf(stderr, "seance: seek to frame %lu: %s\n", start,
				dshow_hr_name(hr, name));
			goto done;
		}
	}
	size_t want = s->frames > start ? s->frames - start : 0;
	if (count < want)
		want = count;
	if (!(output = output_open(out_path, kind, out->biWidth,
			out->biHeight < 0 ? -out->biHeight : out->biHeight,
			rate, scale, want == 1))) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	sink.output = output;
	sink.conv = conv;

	size_t cap = 0;
	unsigned long decoded = 0, errors = 0;
	double busy = 0;
	frame = first_frame;
	for (size_t c = first_chunk; c < s->nchunks && output_frames(output) < want;
	     c++) {
		const struct avi_chunk *chunk = &s->chunks[c];
		if (!avi_chunk_is_frame(chunk))
			continue;
		int show = frame >= start;
		if (!chunk->size) {
			if (show && output_repeat(output)) goto write_error;
			frame++;
			continue;
		}
		if (chunk->size > cap) {
			BYTE *next = realloc(buf, chunk->size);
			if (!next) {
				fprintf(stderr, "seance: out of memory\n");
				goto done;
			}
			buf = next;
			cap = chunk->size;
		}
		if (avi_read_chunk(avi, chunk, buf)) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		REFERENCE_TIME a = (REFERENCE_TIME)frame * scale * 10000000 / rate;
		REFERENCE_TIME b = (REFERENCE_TIME)(frame + 1) * scale * 10000000 / rate;
		DWORD flags = (!has_keys || (chunk->flags & AVIIF_KEYFRAME)) ?
			AM_SAMPLE_SPLICEPOINT : 0;
		if (!show) flags |= AM_SAMPLE_PREROLL;
		double t = now();
		hr = dshow_host_push(host, buf, chunk->size, a, b, flags);
		busy += now() - t;
		decoded++;
		if (FAILED(hr)) {
			errors++;
			fprintf(stderr, "seance: frame %lu: %s\n", frame,
				dshow_hr_name(hr, name));
			if (!keep_going) goto done;
			if (show && output_repeat(output)) goto write_error;
		}
		frame++;
	}
	hr = dshow_host_end(host, 0);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: end of stream: %s\n", dshow_hr_name(hr, name));
		goto done;
	}
	unsigned long written = output_frames(output);
	if (output_close(output)) {
		output = NULL;
		goto write_error;
	}
	output = NULL;
	fprintf(stderr, "%s: %lu frame%s, %ldx%ld '%s' -> %s -> %s",
		out_path, written, written == 1 ? "" : "s", (long)in->biWidth,
		(long)height, fourcc_str(in->biCompression, fcc),
		conv ? image_format_name(conv) : "(as is)", output_kind_name(kind));
	if (decoded)
		fprintf(stderr, ", %.3f ms/frame", busy * 1000 / decoded);
	fprintf(stderr, "\n");
	if (errors) fprintf(stderr, "seance: %lu frame%s failed to decode\n",
		errors, errors == 1 ? "" : "s");
	status = 0;
	goto done;

media_error:
	fprintf(stderr, "seance: invalid DirectShow media type (%s)\n",
		dshow_hr_name(hr, name));
	goto done;
write_error:
	fprintf(stderr, "seance: %s\n", vfw_error());
done:
	if (output) output_close(output);
	dshow_host_destroy(host);
	dshow_filter_close(server, filter);
	dshow_free_media_type(&out_mt);
	dshow_free_media_type(&in_mt);
	image_conv_free(conv);
	free(out);
	free(state);
	free(buf);
	return status;
}

int cmd_decode(int argc, char **argv)
{
	static const UINT modes[] = { ICMODE_DECOMPRESS };
	const char *target = NULL, *path = NULL, *out_path = NULL, *format = NULL;
	const char *fourcc = NULL, *clsid_arg = NULL, *forced = NULL;
	const char *state_path = NULL;
	unsigned long start = 0, count = (unsigned long)-1, stream = (unsigned long)-1;
	int keep_going = 0;
	enum output_kind kind;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int v = i + 1 < argc;

		if (!strcmp(a, "-o") && v)
			out_path = argv[++i];
		else if (!strcmp(a, "--format") && v)
			format = argv[++i];
		else if (!strcmp(a, "--fourcc") && v)
			fourcc = argv[++i];
		else if (!strcmp(a, "--clsid") && v)
			clsid_arg = argv[++i];
		else if (!strcmp(a, "--output-format") && v)
			forced = argv[++i];
		else if (!strcmp(a, "--state") && v)
			state_path = argv[++i];
		else if (!strcmp(a, "--keep-going"))
			keep_going = 1;
		else if (!strcmp(a, "--start") && v) {
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
	if (format ? output_kind_from_name(format, &kind) :
		     output_kind_from_path(out_path, &kind)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 2;
	}

	struct avi_file *avi = avi_open(path);
	if (!avi) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}

	int status = 1, begun = 0;
	struct cli_codec codec = { 0 };
	BITMAPINFOHEADER *in = NULL, *out = NULL;
	struct image_conv *conv = NULL;
	struct output *o = NULL;
	BYTE *buf = NULL, *pic = NULL;
	void *state = NULL;
	char err[24], desc[160], fcc[FOURCC_STR_SIZE];

	int sn = stream == (unsigned long)-1 ? avi_video_stream(avi) :
		 stream < avi->nstreams ? (int)stream : -1;
	const struct avi_stream *s = sn >= 0 ? &avi->streams[sn] : NULL;
	const BITMAPINFOHEADER *bih = s ? avi_stream_bih(s) : NULL;
	if (!bih) {
		fprintf(stderr, "seance: %s: %s\n", path,
			stream == (unsigned long)-1 ? "no video stream" :
			"no such video stream");
		goto done;
	}
	/* The codec gets its own copy, with biSizeImage set per frame. */
	size_t in_size = s->format_size > sizeof(*bih) ? s->format_size :
			 sizeof(*bih);
	if (!(in = calloc(1, in_size))) {
		fprintf(stderr, "seance: out of memory\n");
		goto done;
	}
	memcpy(in, s->format, s->format_size);

	struct pe_image *image = strcmp(target, "auto") ? pe_open(target) : NULL;
	int directshow = image && dshow_is_server(image);
	pe_free(image);
	if (directshow || clsid_arg) {
		CLSID clsid, *selected = NULL;
		if (!directshow) {
			fprintf(stderr, "seance: --clsid requires a DirectShow server\n");
			goto done;
		}
		if (fourcc) {
			fprintf(stderr, "seance: --fourcc is for VfW codecs\n");
			status = 2; goto done;
		}
		if (clsid_arg) {
			if (parse_clsid(clsid_arg, &clsid)) {
				fprintf(stderr, "seance: --clsid wants a GUID, not '%s'\n",
					clsid_arg);
				status = 2; goto done;
			}
			selected = &clsid;
		}
		status = decode_dshow(target, selected, avi, s, in, in_size,
			out_path, kind, forced, state_path, start, count, keep_going);
		goto done;
	}

	/* The codec. */
	FOURCC guess = s->header.fccHandler ? s->header.fccHandler :
		       bih->biCompression;
	if (!strcmp(target, "auto")) {
		if (runtime_enter()) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		codec.hic = ic_locate(ICTYPE_VIDEO, guess, in, NULL,
				      ICMODE_DECOMPRESS);
		if (!codec.hic) {
			CLSID clsid;
			char *server = registered_decoder(in, &clsid);
			if (server) {
				status = decode_dshow(server, &clsid, avi, s, in, in_size,
					out_path, kind, forced, state_path, start, count,
					keep_going);
				free(server);
				goto done;
			}
			fprintf(stderr, "seance: no registered codec decompresses "
				"%s ('%s')\n", path,
				fourcc_str(bih->biCompression, fcc));
			goto done;
		}
	} else {
		int r = cli_open_codec(target, fourcc, guess, modes, 1, &codec);
		if (r) {
			status = r;
			goto done;
		}
	}
	LRESULT r = ic_send(codec.hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in, 0);
	if (r != ICERR_OK) {
		fprintf(stderr, "seance: the codec doesn't decompress %s (%s)\n",
			cli_format_desc(in, desc, sizeof(desc)), icerr_name(r, err));
		goto done;
	}
	if (state_path) {
		size_t n;
		if (!(state = read_file(state_path, &n))) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			goto done;
		}
		/* The bytes of state used; 0 or an ICERR_* if refused. */
		LRESULT used = ic_send(codec.hic, ICM_SETSTATE, (DWORD_PTR)state,
				       (DWORD_PTR)n);
		if (used <= 0) {
			fprintf(stderr, "seance: %s: the codec refused this state "
				"(ICM_SETSTATE: %s)\n", state_path, icerr_name(used, err));
			goto done;
		}
	}

	/* The output format and its converter. */
	if (!(out = choose_output(codec.hic, in, forced, kind))) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	r = ic_send(codec.hic, ICM_DECOMPRESS_BEGIN, (DWORD_PTR)in, (DWORD_PTR)out);
	if (r != ICERR_OK) {
		fprintf(stderr, "seance: ICM_DECOMPRESS_BEGIN: %s\n",
			icerr_name(r, err));
		goto done;
	}
	begun = 1;
	if (out->biBitCount <= 8 && out->biCompression == BI_RGB)
		ic_send(codec.hic, ICM_DECOMPRESS_GET_PALETTE, (DWORD_PTR)in,
			(DWORD_PTR)out);
	conv = image_conv_new(out, vfw_format_size(out));
	if (!conv && kind != OUT_RAW) {
		fprintf(stderr, "seance: the codec's output (%s) can't be "
			"converted; try --output-format or --format raw\n",
			cli_format_desc(out, desc, sizeof(desc)));
		goto done;
	}
	size_t pic_size = conv ? image_frame_size(conv) : 0;
	if (out->biSizeImage > pic_size)
		pic_size = out->biSizeImage;
	if (!pic_size || !(pic = calloc(1, pic_size))) {
		fprintf(stderr, "seance: %s\n", pic_size ? "out of memory" :
			"the output format has no image size");
		goto done;
	}

	/* Where to start: the keyframe at or before --start. */
	int has_keys = 0;
	for (size_t c = 0; c < s->nchunks && !has_keys; c++)
		has_keys = (s->chunks[c].flags & AVIIF_KEYFRAME) != 0;
	size_t first_chunk = 0;
	unsigned long first_frame = 0, frame = 0;
	for (size_t c = 0; c < s->nchunks && frame <= start; c++) {
		const struct avi_chunk *k = &s->chunks[c];
		if (!avi_chunk_is_frame(k))
			continue;
		if (!has_keys || (k->flags & AVIIF_KEYFRAME)) {
			first_chunk = c;
			first_frame = frame;
		}
		frame++;
	}

	size_t want = s->frames > start ? s->frames - start : 0;
	if (count < want)
		want = count;
	DWORD rate = s->header.dwRate, scale = s->header.dwScale;
	if (!rate || !scale) {
		rate = 1000000;
		scale = avi->header.dwMicroSecPerFrame ?
			avi->header.dwMicroSecPerFrame : 40000;
	}
	LONG oh = out->biHeight < 0 ? -out->biHeight : out->biHeight;
	if (!(o = output_open(out_path, kind, out->biWidth, oh, rate, scale,
			      want == 1))) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}

	size_t cap = 0;
	unsigned long errors = 0, decoded = 0;
	double busy = 0;
	frame = first_frame;
	for (size_t c = first_chunk; c < s->nchunks && output_frames(o) < want; c++) {
		const struct avi_chunk *k = &s->chunks[c];
		if (!avi_chunk_is_frame(k))
			continue;	/* codecs aren't told of palette changes */
		int show = frame >= start;

		if (!k->size) {
			/* A dropped frame: the previous one again. */
			if (show && output_repeat(o))
				goto write_error;
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
		if (avi_read_chunk(avi, k, buf)) {
			fprintf(stderr, "seance: %s: %s\n", path, vfw_error());
			goto done;
		}
		in->biSizeImage = k->size;
		DWORD flags = show ? 0 : ICDECOMPRESS_PREROLL;
		int key = !has_keys || (k->flags & AVIIF_KEYFRAME);
		if (!key)
			flags |= ICDECOMPRESS_NOTKEYFRAME;

		double t = now();
		LRESULT dr = (LONG)ic_decompress(codec.hic, flags, in, buf, out, pic);
		busy += now() - t;
		decoded++;

		if (dr < 0 || dr == ICERR_GOTOKEYFRAME) {
			errors++;
			fprintf(stderr, "seance: frame %lu: %s\n", frame,
				icerr_name(dr, err));
			if (!keep_going)
				goto done;
			if (show && output_repeat(o))
				goto write_error;
		} else if (show) {
			/* ICERR_DONTDRAW: nothing new to show. */
			if (dr == ICERR_DONTDRAW ? output_repeat(o) :
			    output_write(o, conv, pic, pic_size))
				goto write_error;
		}
		frame++;
	}

	unsigned long written = output_frames(o);
	int cr = output_close(o);
	o = NULL;
	if (cr) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		goto done;
	}
	fprintf(stderr, "%s: %lu frame%s, %ldx%ld '%s' -> %s -> %s", out_path,
		written, written == 1 ? "" : "s", (long)bih->biWidth,
		(long)(bih->biHeight < 0 ? -bih->biHeight : bih->biHeight),
		fourcc_str(bih->biCompression, fcc),
		conv ? image_format_name(conv) : "(as is)", output_kind_name(kind));
	if (decoded)
		fprintf(stderr, ", %.3f ms/frame", busy * 1000 / decoded);
	if (decoded > written)
		fprintf(stderr, ", %lu decoded to get there", decoded - written);
	fprintf(stderr, "\n");
	if (errors)
		fprintf(stderr, "seance: %lu frame%s failed to decode and "
			"%s repeated\n", errors, errors == 1 ? "" : "s",
			errors == 1 ? "was" : "were");
	if (avi->truncated)
		fprintf(stderr, "seance: warning: %s is truncated or "
			"damaged; some frames were missing\n", path);
	status = 0;
	goto done;

write_error:
	fprintf(stderr, "seance: frame %lu: %s\n", frame, vfw_error());
done:
	if (o)
		output_close(o);
	if (begun)
		ic_send(codec.hic, ICM_DECOMPRESS_END, 0, 0);
	cli_close_codec(&codec);
	image_conv_free(conv);
	free(state);
	free(buf);
	free(pic);
	free(out);
	free(in);
	avi_close(avi);
	return status;
}
