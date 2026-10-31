/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance decode <codec> <in.avi> -o <out>: decompress an AVI's video
 * stream with a VfW codec, a DirectShow filter or a DMO and write the
 * frames as Y4M, BMP files or raw.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "avi/avi.h"
#include "common/padding.h"
#include "cmd_audio.h"
#include "cmd_util.h"
#include "commands.h"
#include "common/error.h"
#include "common/log.h"
#include "common/fourcc.h"
#include "common/unicode.h"
#include "dmo/discover.h"
#include "dshow/discover.h"
#include "dshow/dshow.h"
#include "loader/pe.h"
#include "out/image.h"
#include "out/output.h"
#include "runtime/guard.h"
#include "runtime/crash.h"
#include "runtime/runtime.h"
#include "vfw/icm.h"
#include "win32/codepage.h"
#include "win32/ole.h"
#include "win32/path.h"
#include "xform.h"


/*
 * The buffer a compressed frame is read into: room for size bytes and the
 * zeroed padding after them (common/padding.h). -1 if out of memory.
 */
static int input_buffer(BYTE **buf, size_t *cap, size_t size)
{
	if (size + VFW_INPUT_PADDING > *cap) {
		BYTE *b = realloc(*buf, size + VFW_INPUT_PADDING);
		if (!b)
			return -1;
		*buf = b;
		*cap = size + VFW_INPUT_PADDING;
	}
	memset(*buf + size, 0, VFW_INPUT_PADDING);
	return 0;
}
static int usage(void)
{
	fprintf(stderr,
		"usage: seance decode <codec.dll | fourcc | auto> <input> "
		"-o <out> [options]\n"
		"\n"
		"Video, from an AVI:\n"
		"  -o <out>             output: .y4m, .yuv (I420), .bgr (BGR24),\n"
		"                       .raw (the codec's output as is) or \"-\";\n"
		"                       BMP files with a frame number: out%%05d.bmp\n"
		"  --format <fmt>       y4m, bmp, yuv, bgr or raw, instead of the\n"
		"                       extension\n"
		"  --fourcc <XXXX>      the handler to open a DLL as\n"
		"  --clsid <{GUID}>     the filter or DMO when a DLL has several\n"
		"  --output-format <f>  what the codec should decompress to: I420,\n"
		"                       YV12, YUY2, UYVY, BGR32, BGR24, RGB565,\n"
		"                       RGB555 or PAL8 (default: negotiated)\n"
		"  --stream <n>         the stream (default: the first video)\n"
		"  --start <pos>        first frame to write, or a time (2.5s);\n"
		"                       decoding starts at the keyframe before it\n"
		"  --frames <n>         number of frames to write (default: all)\n"
		"  --duration <len>     how much to write, as frames or a time\n"
		"  --state <file>       codec state to set first (ICM_SETSTATE)\n"
		"  --keep-going         on a frame the codec rejects, repeat the last\n"
		"                       frame and carry on\n"
		"\n"
		"Audio, with a DirectShow filter, DMO or ACM driver, from a WAV or\n"
		"an AVI's audio stream (taken for a .wav input, an AVI without\n"
		"video, a .wav or .pcm output, or --audio):\n"
		"  -o <out>             .wav, .pcm (the samples alone), or \"-\"\n"
		"  --format <fmt>       wav or pcm, instead of the extension\n"
		"  --audio              the first audio stream of an AVI\n"
		"  --sample-format <f>  the PCM to write: u8, s16, s24, s32, f32 or\n"
		"                       f64 (default: what the decoder gives)\n"
		"  --start <pos>        first sample to write, or a time\n"
		"  --duration <len>     how much to write, as samples or a time\n"
		"  --clsid, --stream, --state, --keep-going  as for video\n"
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
 * (YUV for Y4M, planar 4:2:0 first, then packed 4:2:2, which spares the
 * codec's own conversion to RGB, as the ASUS ASV decoders' is a poor one;
 * BGR24 for BMP) if the codec takes it, else what it suggests or the first
 * standard one it accepts.
 */
static BITMAPINFOHEADER *choose_output(HIC hic, const BITMAPINFOHEADER *in,
				       const char *forced, enum output_kind kind)
{
	static const char *const yuv[] = { "I420", "YV12", "YUY2", "UYVY", NULL };
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

struct com_decode_output {
	struct output *output;
	struct image_conv **conv;	/* replaced when the format changes */
	REFERENCE_TIME first_time;	/* earlier samples are before --start */
	unsigned long limit;		/* frames to write */
};

/*
 * A sample carrying a new media type changes the format of it and of every
 * sample after it (the filter has first asked QueryAccept). Picture size
 * and layout are fixed once the output file is open, so a change must keep
 * them; what may change is the pixel format, which gets a new converter.
 */
static HRESULT com_format_change(struct com_decode_output *d,
				   const AM_MEDIA_TYPE *mt)
{
	BITMAPINFOHEADER *bih = NULL;
	size_t size = 0;
	HRESULT hr = dshow_mt_to_bih(mt, &bih, &size);

	if (FAILED(hr))
		return hr;
	LONG height = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
	struct image_conv *next = NULL;
	if (bih->biWidth == image_width(*d->conv) &&
	    height == image_height(*d->conv))
		next = image_conv_new(bih, size);
	free(bih);
	if (!next)
		return VFW_E_INVALIDMEDIATYPE;
	image_conv_free(*d->conv);
	*d->conv = next;
	return S_OK;
}

static HRESULT com_write_output(void *opaque, const struct xform_output *o,
				const AM_MEDIA_TYPE *dynamic)
{
	struct com_decode_output *d = opaque;
	if (dynamic && *d->conv) {
		HRESULT hr = com_format_change(d, dynamic);
		if (FAILED(hr))
			return hr;
	}
	if (dynamic && !*d->conv)
		return VFW_E_INVALIDMEDIATYPE;
	/*
	 * Decoders needn't pass the preroll flag on, and may hold back frames
	 * (to reorder them) that come out after the ones asked for, so the
	 * time stamps and the count decide too.
	 */
	if (o->preroll || (d->first_time && o->timed &&
			   o->start < d->first_time) ||
	    output_frames(d->output) >= d->limit)
		return S_OK;
	if (output_write(d->output, *d->conv, o->data, o->size))
		return E_FAIL;
	return S_OK;
}

/*
 * The format frames come in when the filter or DMO completed the type:
 * the one it connected with (which carries its palette, which way up it
 * is, and may differ from what was asked in more than that), if it
 * describes the picture offered. malloc()ed, at least a struct
 * vfw_format; NULL if it doesn't.
 */
static BITMAPINFOHEADER *connected_format(struct xform *x,
					  const struct vfw_format *offered)
{
	const AM_MEDIA_TYPE *mt = xform_output_type(x);
	BITMAPINFOHEADER *bih = NULL, *out = NULL;
	size_t size = 0;
	char desc[160];

	if (!mt || FAILED(dshow_mt_to_bih(mt, &bih, &size)))
		return NULL;
	int same = bih->biWidth == offered->bih.biWidth &&
		   labs(bih->biHeight) == labs(offered->bih.biHeight);
	TRACE("decode: the decoder's own type: %s%s",
	      cli_format_desc(bih, desc, sizeof(desc)),
	      same ? "" : ", a different picture");
	if (same &&
	    (out = calloc(1, size > sizeof(*offered) ? size : sizeof(*offered))))
		memcpy(out, bih, size);
	free(bih);
	return out;
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
		if (!filters[i].path || dshow_role(f) != DSHOW_VIDEO_DECODER)
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
	if (path)
		return path;

	/* No filter: a DMO registered as a video decoder of the stream. */
	struct dmo_registered *dmos = NULL;
	count = dmo_registered(&dmos);
	for (size_t i = 0; i < count && !path; i++) {
		const struct dmo_reg *d = &dmos[i].d;
		if (!dmos[i].path || dmo_role(d) != DMO_ROLE_VIDEO_DECODER)
			continue;
		for (size_t t = 0; t < d->nin && !path; t++) {
			const DMO_PARTIAL_MEDIATYPE *type = &d->in[t];
			int video = !memcmp(&type->type, &MEDIATYPE_Video,
					    sizeof(GUID)) ||
				    !memcmp(&type->type, &null_guid, sizeof(GUID));
			int matches = !memcmp(&type->subtype, &subtype,
					      sizeof(GUID)) ||
				      !memcmp(&type->subtype, &null_guid,
					      sizeof(GUID));
			if (video && matches) {
				path = path_to_unix(dmos[i].path);
				*clsid = d->clsid;
			}
		}
	}
	dmo_registered_free(dmos, count);
	return path;
}

/* Decoding with a DirectShow filter or a DMO. */
static int decode_com(const char *target, const CLSID *selected,
			struct avi_file *avi, const struct avi_stream *s,
			BITMAPINFOHEADER *in, size_t in_size, const char *out_path,
			enum output_kind kind, const char *forced,
			const char *state_path, unsigned long start,
			unsigned long count, int keep_going)
{
	struct xform *x = NULL;
	int connected = 0;
	BITMAPINFOHEADER *out = NULL;
	struct image_conv *conv = NULL;
	struct output *output = NULL;
	BYTE *buf = NULL;
	void *state = NULL;
	AM_MEDIA_TYPE in_mt = {0}, out_mt = {0};
	struct com_decode_output sink = {0};
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
	HRESULT hr = xform_open(target, selected, 0, 0, &x, NULL);
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
		hr = dshow_filter_load_state(xform_object(x), state, n);
		if (FAILED(hr)) {
			fprintf(stderr, "seance: %s: the %s refused this state "
				"(%s)\n", state_path, xform_kind_name(x),
				dshow_hr_name(hr, name));
			goto done;
		}
	}
	LONG height = in->biHeight < 0 ? -in->biHeight : in->biHeight;
	struct vfw_format fmt;
	const char *format_name;
	int passes = forced || kind == OUT_RAW ? 1 : 2;
	HRESULT connect_hr = E_FAIL;
	/* The input type a filter takes depends on how it names its formats. */
	for (unsigned variant = 0; !connected; variant++) {
		dshow_free_media_type(&in_mt);
		hr = dshow_input_variant(in, in_size, rate, scale, variant, &in_mt);
		if (hr == S_FALSE)
			break;
		if (FAILED(hr)) {
			if (!variant)
				goto media_error;
			continue;
		}
		for (int pass = 0; pass < passes && !connected; pass++) {
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
				if (FAILED(dshow_bih_to_mt(&fmt.bih, sizeof(fmt), rate,
							   scale, 0, &out_mt)))
					continue;
				/*
				 * The decoder's own type of that subtype first, as a
				 * renderer takes it in a graph: it carries the
				 * decoder's palette (LEAD's) and the layout it
				 * writes. Failing that, the type made here.
				 */
				AM_MEDIA_TYPE partial = {
					.majortype = out_mt.majortype,
					.subtype = out_mt.subtype,
				};
				connect_hr = xform_connect(x, &in_mt, &partial,
							   com_write_output, &sink);
				if (SUCCEEDED(connect_hr) &&
				    (out = connected_format(x, &fmt))) {
					connected = 1;
					break;
				}
				if (SUCCEEDED(connect_hr))
					xform_disconnect(x);
				connect_hr = xform_connect(x, &in_mt, &out_mt,
							   com_write_output, &sink);
				if (SUCCEEDED(connect_hr)) {
					connected = 1;
					out = malloc(sizeof(fmt));
					if (out)
						memcpy(out, &fmt, sizeof(fmt));
					break;
				}
				dshow_free_media_type(&out_mt);
			}
		}
	}
	hr = connect_hr;
	if (!connected || !out) {
		if (forced)
			fprintf(stderr, "seance: the %s won't decode to output "
				"format %s\n", xform_kind_name(x), forced);
		else
			fprintf(stderr, "seance: no suitable %s output format (%s)\n",
				xform_kind_name(x), dshow_hr_name(hr, name));
		goto done;
	}
	conv = image_conv_new(out, vfw_format_size(out));
	if (!conv && kind != OUT_RAW) {
		fprintf(stderr, "seance: the %s's output (%s) cannot be "
			"converted\n", xform_kind_name(x),
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
		hr = xform_seek(x, segment_start, segment_stop);
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
	sink.conv = &conv;
	sink.limit = want;
	sink.first_time = start ?
		(REFERENCE_TIME)start * scale * 10000000 / rate : 0;

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
		if (input_buffer(&buf, &cap, chunk->size)) {
			fprintf(stderr, "seance: out of memory\n");
			goto done;
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
		hr = xform_push(x, buf, chunk->size, a, b, flags);
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
	hr = xform_end(x);
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
	xform_close(x);
	dshow_free_media_type(&out_mt);
	dshow_free_media_type(&in_mt);
	image_conv_free(conv);
	free(out);
	free(state);
	free(buf);
	return status;
}

/*
 * Some codecs write YUV bottom-up, as if it were RGB (vp4vfw.dll's I420),
 * where YUV is top-down. Checked on the first picture against the same
 * frame decoded to BGR24, whose orientation is known, by comparing luma.
 */

/* Whether b is a YUV layout these know: planar 4:2:0 or packed 4:2:2. */
static int yuv_kind(const BITMAPINFOHEADER *b)
{
	switch (b->biCompression) {
	case mmioFOURCC('I', '4', '2', '0'):
	case mmioFOURCC('I', 'Y', 'U', 'V'):
	case mmioFOURCC('Y', 'V', '1', '2'):
		return 1;
	case mmioFOURCC('Y', 'U', 'Y', '2'):
	case mmioFOURCC('Y', 'U', 'Y', 'V'):
	case mmioFOURCC('Y', 'V', 'Y', 'U'):
		return 2;
	case mmioFOURCC('U', 'Y', 'V', 'Y'):
		return 3;
	}
	return 0;
}

/* The luma at x of memory row y. */
static int luma_at(const BITMAPINFOHEADER *b, const BYTE *p, LONG x, LONG y)
{
	LONG w = b->biWidth;

	switch (yuv_kind(b)) {
	case 1:
		return p[y * w + x];
	case 2:
		return p[y * w * 2 + x * 2];
	case 3:
		return p[y * w * 2 + x * 2 + 1];
	}
	/* BGR24, to limited-range luma as YUV has it (BT.601). */
	const BYTE *q = p + y * ((w * 3 + 3) & ~3) + x * 3;
	return ((q[0] * 25 + q[1] * 129 + q[2] * 66 + 128) >> 8) + 16;
}

/* Turns a YUV picture of yuv_kind over. */
static void flip_yuv(const BITMAPINFOHEADER *b, BYTE *p)
{
	LONG w = b->biWidth, h = b->biHeight < 0 ? -b->biHeight : b->biHeight;
	BYTE tmp[16384];
	struct { BYTE *base; LONG stride, rows; } planes[3];
	int n = 1;

	if (yuv_kind(b) == 1) {
		planes[0].base = p;
		planes[0].stride = w;
		planes[0].rows = h;
		/* Chroma rounded up, as image_i420_size has it. */
		LONG cw = (w + 1) / 2, ch = (h + 1) / 2;
		planes[1].base = p + w * h;
		planes[2].base = planes[1].base + cw * ch;
		planes[1].stride = planes[2].stride = cw;
		planes[1].rows = planes[2].rows = ch;
		n = 3;
	} else {
		planes[0].base = p;
		planes[0].stride = w * 2;
		planes[0].rows = h;
	}
	for (int k = 0; k < n; k++) {
		LONG s = planes[k].stride;
		if (s > (LONG)sizeof(tmp))
			return;
		for (LONG y = 0; y < planes[k].rows / 2; y++) {
			BYTE *a = planes[k].base + y * s;
			BYTE *z = planes[k].base + (planes[k].rows - 1 - y) * s;
			memcpy(tmp, a, s);
			memcpy(a, z, s);
			memcpy(z, tmp, s);
		}
	}
}

/*
 * Whether the codec writes its YUV output out upside down: frame buf,
 * which decodes on its own, decoded to out and to BGR24 by another
 * instance matches only when one is turned over.
 */
struct orientation {
	const struct cli_codec *codec;
	BITMAPINFOHEADER *in;
	const void *buf;
	const BITMAPINFOHEADER *out;
	int upside_down;
};

static int yuv_upside_down(const struct cli_codec *codec,
			   BITMAPINFOHEADER *in, const void *buf,
			   const BITMAPINFOHEADER *out);

static void check_orientation(void *arg)
{
	struct orientation *o = arg;

	o->upside_down = yuv_upside_down(o->codec, o->in, o->buf, o->out);
}

static int yuv_upside_down(const struct cli_codec *codec,
			   BITMAPINFOHEADER *in, const void *buf,
			   const BITMAPINFOHEADER *out)
{
	LONG w = out->biWidth, h = out->biHeight;
	/* Packed 4:2:2 pairs pixels; planar 4:2:0 rounds its chroma up. */
	if (!yuv_kind(out) || h <= 1 || w <= 0 ||
	    (yuv_kind(out) != 1 && (w & 1)) || !codec->target[0])
		return 0;
	BITMAPINFOHEADER rgb = {
		.biSize = sizeof(rgb), .biWidth = w, .biHeight = h,
		.biPlanes = 1, .biBitCount = 24, .biCompression = BI_RGB,
		.biSizeImage = (DWORD)(((w * 3 + 3) & ~3) * h),
	};
	const BITMAPINFOHEADER *as[2] = { out, &rgb };
	BYTE *pic[2] = { calloc(1, out->biSizeImage + (size_t)w * h * 2),
			 malloc(rgb.biSizeImage) };
	int ok = pic[0] && pic[1];

	for (int k = 0; k < 2 && ok; k++) {
		HIC hic = cli_reopen_codec(codec, ICMODE_DECOMPRESS);
		ok = hic && ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in,
				    (DWORD_PTR)as[k]) == ICERR_OK &&
		     ic_decompress_begin(hic, in, as[k]) == ICERR_OK;
		/*
		 * Closed without ICM_DECOMPRESS_END, as programs often do: one
		 * that ends badly (WavCWAIP.dll) is then spared it once more.
		 */
		if (ok)
			ok = (LONG)ic_decompress(hic, 0, in, (void *)buf,
						 (BITMAPINFOHEADER *)as[k],
						 pic[k]) >= 0;
		if (hic)
			ic_close(hic);
	}
	unsigned long same = 0, flipped = 0;
	if (ok) {
		/* BGR24's memory rows run from the bottom. */
		for (LONG y = 0; y < h; y += 4)
			for (LONG x = 0; x < w; x += 4) {
				int v = luma_at(out, pic[0], x, y);
				same += abs(v - luma_at(&rgb, pic[1], x, h - 1 - y));
				flipped += abs(v - luma_at(&rgb, pic[1], x, y));
			}
	}
	free(pic[0]);
	free(pic[1]);
	return ok && flipped * 2 < same;
}

/* The first frame of the stream that decodes on its own, read into *buf. */
static int first_key_frame(struct avi_file *avi, const struct avi_stream *s,
			   void **buf, size_t *size)
{
	int has_keys = 0;
	for (size_t c = 0; c < s->nchunks && !has_keys; c++)
		has_keys = (s->chunks[c].flags & AVIIF_KEYFRAME) != 0;
	for (size_t c = 0; c < s->nchunks; c++) {
		const struct avi_chunk *k = &s->chunks[c];
		if (!avi_chunk_is_frame(k) || !k->size ||
		    (has_keys && !(k->flags & AVIIF_KEYFRAME)))
			continue;
		/* With the zeros after it that decoding gives every frame. */
		if (!(*buf = calloc(1, k->size + VFW_INPUT_PADDING)) ||
		    avi_read_chunk(avi, k, *buf)) {
			free(*buf);
			*buf = NULL;
			return -1;
		}
		*size = k->size;
		return 0;
	}
	return -1;
}

/* Whether the codec decompresses the format ctx. */
static int decompresses(HIC hic, void *ctx)
{
	return ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)ctx, 0) == ICERR_OK;
}

int cmd_decode(int argc, char **argv)
{
	static const UINT modes[] = { ICMODE_DECOMPRESS };
	const char *target = NULL, *path = NULL, *out_path = NULL, *format = NULL;
	const char *fourcc = NULL, *clsid_arg = NULL, *forced = NULL;
	const char *state_path = NULL;
	unsigned long start = 0, count = (unsigned long)-1, stream = (unsigned long)-1;
	int keep_going = 0, want_audio = 0;
	enum output_kind kind;
	struct audio_codec_job job = { .source.stream = -1, .quality = -1 };

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
			if (cli_parse_position(a, argv[++i], &job.start))
				return 2;
			job.has_start = 1;
		} else if (!strcmp(a, "--duration") && v) {
			if (cli_parse_position(a, argv[++i], &job.duration))
				return 2;
			job.has_duration = 1;
		} else if (!strcmp(a, "--audio")) {
			want_audio = 1;
		} else if (!strcmp(a, "--sample-format") && v) {
			if (audio_sample_from_name(argv[++i], &job.sample)) {
				fprintf(stderr, "seance: %s\n", vfw_error());
				return 2;
			}
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

	/* Audio: a WAV, an AVI's audio stream, or a .wav or .pcm output. */
	if (job.sample != AUDIO_NONE || !strcmp(path, "-") ||
	    cli_wants_audio(path, stream == (unsigned long)-1 ? -1 :
			    stream > 99 ? 99 : (long)stream, want_audio, format,
			    out_path, audio_only_kinds,
			    !format && output_is_null(out_path) ? target : NULL,
			    clsid_arg, 0)) {
		if (fourcc || forced || count != (unsigned long)-1) {
			fprintf(stderr, "seance: --fourcc, --output-format and "
				"--frames are for video\n");
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
		job.format = format;
		if (stream != (unsigned long)-1)
			job.source.stream = stream > 99 ? 99 : (int)stream;
		job.state = state_path;
		job.keep_going = keep_going;
		return decode_audio(&job);
	}
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

	int status = 1, begun = 0, flip = 0;
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
	/* --start and --duration in frames. */
	{
		DWORD r = s->header.dwRate, sc = s->header.dwScale;
		if (!r || !sc) {
			r = 1000000;
			sc = avi->header.dwMicroSecPerFrame ?
			     avi->header.dwMicroSecPerFrame : 40000;
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
			fprintf(stderr, "seance: --clsid requires a DirectShow or "
				"DMO server\n");
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
		status = decode_com(target, selected, avi, s, in, in_size,
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
				status = decode_com(server, &clsid, avi, s, in, in_size,
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
	if (r != ICERR_OK &&
	    cli_codec_try_fourccs(&codec, decompresses, (void *)in))
		r = ICERR_OK;
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
	/*
	 * YUV the codec writes upside down: RGB instead, which it gets right
	 * (vp4vfw.dll, whose I420 is also a row out), else turned over. Not
	 * for .raw, which is the codec's output exactly as it gives it.
	 */
	void *key_buf = NULL;
	size_t key_size;
	if (kind != OUT_RAW && yuv_kind(out) &&
	    !first_key_frame(avi, s, &key_buf, &key_size)) {
		DWORD was = in->biSizeImage;
		in->biSizeImage = (DWORD)key_size;
		/* A codec that faults here (in a second instance) isn't checked. */
		struct orientation chk = { &codec, in, key_buf, out, 0 };
		int upside = !crash_try(check_orientation, &chk) &&
			     chk.upside_down;
		in->biSizeImage = was;
		free(key_buf);
		BITMAPINFOHEADER *rgb = upside && !forced ?
			choose_output(codec.hic, in, "BGR24", kind) : NULL;
		if (rgb) {
			fprintf(stderr, "seance: the codec writes %s upside down; "
				"decoding to BGR24 instead\n",
				fourcc_str(out->biCompression, fcc));
			free(out);
			out = rgb;
		} else if (upside) {
			fprintf(stderr, "seance: the codec writes %s upside down; "
				"turned over\n", fourcc_str(out->biCompression, fcc));
			flip = 1;
		}
	}
	r = ic_decompress_begin(codec.hic, in, out);
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
	if (!pic_size || !(pic = guard_alloc(pic_size, "output buffer"))) {
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
		if (input_buffer(&buf, &cap, k->size)) {
			fprintf(stderr, "seance: out of memory\n");
			goto done;
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
		if (flip && dr >= 0 && dr != ICERR_DONTDRAW)
			flip_yuv(out, pic);

		/* A FOURCC found in it is kept once a frame has decoded. */
		if (dr >= 0 && dr != ICERR_GOTOKEYFRAME)
			cli_remember_codec(&codec);
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
		ic_end(codec.hic, ICM_DECOMPRESS_END);
	cli_close_codec(&codec);
	image_conv_free(conv);
	free(state);
	free(buf);
	guard_free(pic);
	free(out);
	free(in);
	avi_close(avi);
	return status;
}
