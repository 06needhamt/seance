/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance probe <dll|fourcc> [in.avi]: open a codec and report what it
 * says about itself and which formats it accepts.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acm/acm.h"
#include "avi/avi.h"
#include "cmd_util.h"
#include "commands.h"
#include "common/error.h"
#include "common/fourcc.h"
#include "common/unicode.h"
#include "dmo/discover.h"
#include "dshow/discover.h"
#include "dshow/dshow.h"
#include "loader/pe.h"
#include "runtime/runtime.h"
#include "vfw/icm.h"
#include "win32/codepage.h"
#include "win32/msacm32.h"
#include "win32/ole.h"
#include "in/audio.h"
#include "media/audio.h"
#include "xform.h"

static int usage(void)
{
	fprintf(stderr,
		"usage: seance probe <codec.dll | fourcc> [in.avi | in.wav] "
		"[options]\n"
		"\n"
		"  With a file, a decoder shows the outputs it takes for it (for an\n"
		"  audio decoder, of the file's audio), and an audio encoder the\n"
		"  format it makes of the file's PCM.\n"
		"\n"
		"  --fourcc <XXXX>    the handler to open a DLL as (default: from\n"
		"                     vfw.ini, else 0, then what the codec reports)\n"
		"  --clsid <{GUID}>   the filter or DMO when a DLL has several\n"
		"  --size <WxH>       frame size for the format checks (default\n"
		"                     320x240, or the AVI's)\n");
	return 2;
}

static int parse_clsid(const char *s, CLSID *clsid)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	int bad = !w || ole_guid_parse(w, clsid);
	free(w);
	return bad;
}

/*
 * The standard output formats a decoder (filter or DMO) connects with for
 * an AVI's video stream.
 */
static void probe_connections(const char *path, const struct xform_class *c,
			      const char *avi_path)
{
	struct avi_file *avi = avi_open(avi_path);
	if (!avi) {
		printf("    connection check: cannot open %s (%s)\n", avi_path,
		       vfw_error());
		return;
	}
	int sn = avi_video_stream(avi);
	const struct avi_stream *s = sn >= 0 ? &avi->streams[sn] : NULL;
	const BITMAPINFOHEADER *bih = s ? avi_stream_bih(s) : NULL;
	if (!bih) {
		printf("    connection check: no video stream in %s\n", avi_path);
		avi_close(avi);
		return;
	}
	DWORD rate = s->header.dwRate ? s->header.dwRate : 25;
	DWORD scale = s->header.dwScale ? s->header.dwScale : 1;
	AM_MEDIA_TYPE input = {0};
	HRESULT hr = S_OK;
	printf("    outputs accepted for %s:", avi_path);
	int any = 0;
	LONG h = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
	/* The first way of naming the input format that the filter takes. */
	for (unsigned variant = 0; !any; variant++) {
		dshow_free_media_type(&input);
		hr = dshow_input_variant(bih, s->format_size, rate, scale, variant, &input);
		if (hr == S_FALSE)
			break;
		if (FAILED(hr))
			continue;
		struct vfw_format f;
		const char *name;
		for (unsigned i = 0; (name = vfw_std_format(i, bih->biWidth, h, &f)); i++) {
			struct xform *x = NULL;
			AM_MEDIA_TYPE output = {0};
			/* A fresh object each time: a refusal may leave it upset. */
			hr = xform_create(path, c, &x);
			if (SUCCEEDED(hr))
				hr = dshow_bih_to_mt(&f.bih, sizeof(f), rate, scale,
						     0, &output);
			if (SUCCEEDED(hr))
				hr = xform_connect(x, &input, &output, NULL, NULL);
			if (SUCCEEDED(hr)) {
				printf(" %s", name);
				any = 1;
			}
			xform_close(x);
			dshow_free_media_type(&output);
		}
	}
	printf("%s\n", any ? "" : " none of the standard ones");
	dshow_free_media_type(&input);
	avi_close(avi);
}

/*
 * For an audio decoder, the PCM types it takes for a file's audio (a WAV,
 * or an AVI's audio stream); for an audio encoder, the format it makes of
 * the file's PCM.
 */
static void probe_audio(const char *path, const struct xform_class *c,
			int encoder, const char *input)
{
	struct audio_source *src = audio_source_open(input, NULL);
	char desc[AUDIO_DESC_SIZE], name[24];

	if (!src) {
		printf("    connection check: %s\n", vfw_error());
		return;
	}
	size_t size;
	const WAVEFORMATEX *wf = audio_source_format(src, &size);
	AM_MEDIA_TYPE in = {0};
	HRESULT hr = audio_wfx_to_mt(wf, size, &in);

	if (encoder) {
		struct xform *x = NULL;
		AM_MEDIA_TYPE out = {0};
		WAVEFORMATEX *made = NULL;
		size_t made_size = 0;
		out.majortype = MEDIATYPE_Audio;
		out.subtype = (GUID)WAVE_FORMAT_SUBTYPE_INIT(c->compression);
		if (SUCCEEDED(hr))
			hr = xform_create(path, c, &x);
		if (SUCCEEDED(hr))
			hr = xform_connect(x, &in, &out, NULL, NULL);
		if (SUCCEEDED(hr))
			hr = audio_mt_to_wfx(xform_output_type(x), &made,
					     &made_size);
		if (SUCCEEDED(hr))
			printf("    makes of %s: %s\n", input,
			       audio_format_desc(made, made_size, desc));
		else
			printf("    connection check: it takes no %s (%s)\n",
			       audio_format_desc(wf, size, desc),
			       dshow_hr_name(hr, name));
		free(made);
		xform_close(x);
	} else if (audio_format_sample(wf, size) != AUDIO_NONE) {
		printf("    connection check: %s's audio is already %s\n", input,
		       audio_format_desc(wf, size, desc));
	} else {
		WAVEFORMATEXTENSIBLE choices[16];
		size_t n = audio_output_preferences(wf, size, AUDIO_NONE,
						    choices, 16);
		int any = 0;
		printf("    outputs accepted for %s:", input);
		/* The first way of naming the input that it takes. */
		for (unsigned v = 1; SUCCEEDED(hr); v++) {
			struct xform *x = NULL;
			AM_MEDIA_TYPE out = {0};
			size_t csize = choices[0].Format.cbSize ?
				       sizeof(choices[0]) : sizeof(WAVEFORMATEX);
			int taken = SUCCEEDED(xform_create(path, c, &x)) &&
				    SUCCEEDED(audio_wfx_to_mt(&choices[0].Format,
							      csize, &out)) &&
				    SUCCEEDED(xform_connect(x, &in, &out, NULL,
							    NULL));
			xform_close(x);
			dshow_free_media_type(&out);
			if (taken)
				break;
			AM_MEDIA_TYPE next;
			if (audio_input_variant(wf, size, v, &next) != S_OK)
				break;
			dshow_free_media_type(&in);
			in = next;
		}
		for (size_t i = 0; SUCCEEDED(hr) && i < n; i++) {
			struct xform *x = NULL;
			AM_MEDIA_TYPE out = {0};
			size_t csize = choices[i].Format.cbSize ?
				       sizeof(choices[i]) : sizeof(WAVEFORMATEX);
			/* A fresh object each time: a refusal may leave it upset. */
			if (SUCCEEDED(xform_create(path, c, &x)) &&
			    SUCCEEDED(audio_wfx_to_mt(&choices[i].Format, csize,
						      &out)) &&
			    SUCCEEDED(xform_connect(x, &in, &out, NULL, NULL))) {
				printf(" %s%s", audio_sample_name(
					audio_format_sample(&choices[i].Format,
							    csize)),
				       csize > sizeof(WAVEFORMATEX) ?
				       " (extensible)" : "");
				any = 1;
			}
			xform_close(x);
			dshow_free_media_type(&out);
		}
		printf("%s\n", any ? "" : " none of the usual PCM ones");
	}
	dshow_free_media_type(&in);
	audio_source_close(src);
}

/*
 * The optional interfaces a decoder or encoder (filter or DMO) has, as the
 * codec's own probe says what it can do.
 */
static void probe_interfaces(const char *path, const struct xform_class *c)
{
	struct xform *x = NULL;
	char names[160];

	if (FAILED(xform_create(path, c, &x)))
		return;
	if (dshow_filter_interfaces(xform_object(x), names, sizeof(names)))
		printf("      interfaces: %s\n", names);
	xform_close(x);
}

static int probe_dshow(const char *path, const char *selected,
		       const char *avi_path)
{
	CLSID wanted;
	if (selected && parse_clsid(selected, &wanted)) {
		fprintf(stderr, "seance: --clsid wants a GUID, not '%s'\n", selected);
		return 2;
	}

	struct dshow_server_found found;
	HRESULT hr;
	if (dshow_server_discover(path, &found, &hr)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}
	struct qz_filter *filters = found.filters;
	size_t count = found.nfilters;
	if (FAILED(hr) && !count && !found.ndmos) {
		char name[24];
		fprintf(stderr, "seance: %s: DllRegisterServer failed (%s)\n",
			path, dshow_hr_name(hr, name));
		return 1;
	}

	/* What the file given has to check against: video, audio or both. */
	int video_input = 0, audio_input = 0;
	if (avi_path) {
		struct avi_file *avi = avi_open(avi_path);
		struct audio_source *a = audio_source_open(avi_path, NULL);
		video_input = avi && avi_video_stream(avi) >= 0;
		audio_input = a != NULL;
		avi_close(avi);
		audio_source_close(a);
		if (!video_input && !audio_input)
			printf("(%s has neither video nor audio to check)\n",
			       avi_path);
	}

	size_t shown = 0;
	printf("%s server: %s\n", count || !found.ndmos ? "DirectShow" : "DMO",
	       path);
	for (size_t i = 0; i < count; i++) {
		if (selected && memcmp(&filters[i].clsid, &wanted, sizeof(wanted)))
			continue;
		struct xform_class c = { .clsid = filters[i].clsid };
		enum dshow_role role = dshow_role(&filters[i]);
		dshow_print_filter(&filters[i], NULL, 1);
		if (role != DSHOW_OTHER)
			probe_interfaces(path, &c);
		if (video_input && role == DSHOW_VIDEO_DECODER)
			probe_connections(path, &c, avi_path);
		if (audio_input && (role == DSHOW_AUDIO_DECODER ||
				 role == DSHOW_AUDIO_ENCODER)) {
			struct xform_class ac;
			int encoder = role == DSHOW_AUDIO_ENCODER;
			if (SUCCEEDED(xform_find(path, &filters[i].clsid, 1,
						 encoder, &ac)))
				probe_audio(path, &ac, encoder, avi_path);
		}
		shown++;
	}
	for (size_t i = 0; i < found.ndmos; i++) {
		if (selected && memcmp(&found.dmos[i].clsid, &wanted,
				       sizeof(wanted)))
			continue;
		struct xform_class c = { .clsid = found.dmos[i].clsid, .dmo = 1 };
		enum dmo_role role = dmo_role(&found.dmos[i]);
		dmo_print(&found.dmos[i], NULL, 1);
		if (dmo_role_is_codec(role))
			probe_interfaces(path, &c);
		if (video_input && role == DMO_ROLE_VIDEO_DECODER)
			probe_connections(path, &c, avi_path);
		if (audio_input && (role == DMO_ROLE_AUDIO_DECODER ||
				 role == DMO_ROLE_AUDIO_ENCODER)) {
			struct xform_class ac;
			int encoder = role == DMO_ROLE_AUDIO_ENCODER;
			if (SUCCEEDED(xform_find(path, &found.dmos[i].clsid, 1,
						 encoder, &ac)))
				probe_audio(path, &ac, encoder, avi_path);
		}
		shown++;
	}
	dshow_server_found_free(&found);
	if (!shown) {
		fprintf(stderr, "seance: %s: %s\n", path,
			selected ? "that CLSID is not registered by this server" :
			"no DirectShow filters or DMOs discovered");
		return 1;
	}
	return 0;
}

/* A bounded UTF-16 field (ICINFO strings) as UTF-8. */
/* An ACM driver: its details, tags and roles, and with an input, its use. */
static int probe_acm(const char *path, const char *input)
{
	HACMDRIVERID id;
	HACMDRIVER had;
	ACMDRIVERDETAILSW d = { .cbStruct = sizeof(d) };
	char text[600], flags[100];

	if (msacm_add_path(path, &id) || msacm_acmDriverDetailsW(id, &d, 0) ||
	    msacm_acmDriverOpen(&had, id, 0))
		return -1;
	printf("ACM driver: %s\n", path);
	static const struct { size_t offset, chars; const char *label; } fields[] = {
		{ offsetof(ACMDRIVERDETAILSW, szShortName),
		  ACMDRIVERDETAILS_SHORTNAME_CHARS, "name" },
		{ offsetof(ACMDRIVERDETAILSW, szLongName),
		  ACMDRIVERDETAILS_LONGNAME_CHARS, "long name" },
		{ offsetof(ACMDRIVERDETAILSW, szCopyright),
		  ACMDRIVERDETAILS_COPYRIGHT_CHARS, "copyright" },
		{ offsetof(ACMDRIVERDETAILSW, szLicensing),
		  ACMDRIVERDETAILS_LICENSING_CHARS, "licensing" },
		{ offsetof(ACMDRIVERDETAILSW, szFeatures),
		  ACMDRIVERDETAILS_FEATURES_CHARS, "features" },
	};
	for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
		WCHAR *w = (WCHAR *)((BYTE *)&d + fields[i].offset);
		w[fields[i].chars - 1] = 0;
		utf16_to_utf8(w, (size_t)-1, text, sizeof(text));
		if (text[0])
			printf("  %-10s %s\n", fields[i].label, text);
	}
	printf("  %-10s %s\n", "support",
	       acm_support_names(d.fdwSupport, flags, sizeof(flags)));
	printf("  %-10s %u.%02u.%u (for ACM %u.%02u)\n", "version",
	       (unsigned)(d.vdwDriver >> 24), (unsigned)(d.vdwDriver >> 16 & 0xff),
	       (unsigned)(d.vdwDriver & 0xffff), (unsigned)(d.vdwACM >> 24),
	       (unsigned)(d.vdwACM >> 16 & 0xff));

	printf("  format tags (%u):\n", (unsigned)d.cFormatTags);
	for (DWORD i = 0; i < d.cFormatTags; i++) {
		ACMFORMATTAGDETAILSW t = { .cbStruct = sizeof(t) };
		WORD tag = 0;
		t.dwFormatTagIndex = i;
		if (msacm_acmFormatTagDetailsW(had, &t,
					       ACM_FORMATTAGDETAILSF_INDEX))
			continue;
		t.szFormatTag[ACMFORMATTAGDETAILS_FORMATTAG_CHARS - 1] = 0;
		utf16_to_utf8(t.szFormatTag, (size_t)-1, text, sizeof(text));
		printf("    0x%04x %s: %u standard format%s, up to %u bytes",
		       (unsigned)t.dwFormatTag, text,
		       (unsigned)t.cStandardFormats,
		       t.cStandardFormats == 1 ? "" : "s",
		       (unsigned)t.cbFormatSize);
		if (t.dwFormatTag != WAVE_FORMAT_PCM && t.dwFormatTag <= 0xffff) {
			int dec = SUCCEEDED(acm_find(path, 0, (WORD)t.dwFormatTag,
						     &tag));
			int enc = SUCCEEDED(acm_find(path, 1, (WORD)t.dwFormatTag,
						     &tag));
			printf("; %s%s", dec && enc ? "decodes and encodes" :
			       dec ? "decodes to PCM" : enc ? "encodes from PCM" :
			       "no PCM either way",
			       dec && !t.cStandardFormats ?
			       " (none listed to check decoding with)" : "");
		}
		printf("\n");
	}
	if (d.cFilterTags)
		printf("  filter tags: %u (not used by seance's commands)\n",
		       (unsigned)d.cFilterTags);
	msacm_acmDriverClose(had, 0);

	/* PCM goes to an encoder, anything else to a decoder. */
	struct audio_source *src = input ? audio_source_open(input, NULL) : NULL;
	if (input && !src)
		printf("  connection check: %s\n", vfw_error());
	if (src) {
		size_t size;
		const WAVEFORMATEX *wf = audio_source_format(src, &size);
		int encoder = audio_format_sample(wf, size) != AUDIO_NONE;
		struct xform_class c = { .acm = 1 };
		WORD tag;
		HRESULT hr = acm_find(path, encoder, 0, &tag);
		audio_source_close(src);
		if (encoder && hr == VFW_E_DUPLICATE_NAME)
			printf("  connection check: it encodes to several tags; "
			       "encode chooses with --format-tag\n");
		else if (FAILED(hr))
			printf("  connection check: it has no %s\n",
			       encoder ? "encoder of PCM" : "decoder to PCM");
		c.compression = encoder ? tag : 0;
		if (SUCCEEDED(hr))
			probe_audio(path, &c, encoder, input);
	}
	return 0;
}

static const char *wide(const WCHAR *s, size_t max, char *buf, size_t size)
{
	utf16_field_to_utf8(s, max, buf, size);
	return buf;
}

static void print_flags(DWORD flags)
{
	static const struct {
		DWORD bit;
		const char *name;
	} names[] = {
		{ VIDCF_QUALITY, "QUALITY" }, { VIDCF_CRUNCH, "CRUNCH" },
		{ VIDCF_TEMPORAL, "TEMPORAL" },
		{ VIDCF_COMPRESSFRAMES, "COMPRESSFRAMES" }, { VIDCF_DRAW, "DRAW" },
		{ VIDCF_FASTTEMPORALC, "FASTTEMPORALC" },
		{ VIDCF_QUALITYTIME, "QUALITYTIME" },
		{ VIDCF_FASTTEMPORALD, "FASTTEMPORALD" },
	};

	printf("0x%04lx", (unsigned long)flags);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (flags & names[i].bit)
			printf(" %s", names[i].name);
	printf("\n");
}

/* The standard formats a decompressor accepts for `in`, on one line. */
static void print_outputs(HIC hic, const BITMAPINFOHEADER *in)
{
	struct vfw_format f;
	const char *name;
	int any = 0;
	LONG h = in->biHeight < 0 ? -in->biHeight : in->biHeight;

	printf("    outputs accepted:");
	for (unsigned i = 0; (name = vfw_std_format(i, in->biWidth, h, &f)); i++)
		if (ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in,
			    (DWORD_PTR)&f.bih) == ICERR_OK) {
			printf(" %s", name);
			any = 1;
		}
	printf("%s\n", any ? "" : " none of the standard ones");
}

/* Decompression of `in`: the query, the codec's suggestion, the outputs. */
static void probe_decompress(HIC hic, const BITMAPINFOHEADER *in)
{
	char buf[160], err[24];
	LRESULT r = ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in, 0);

	printf("    decompress query: %s\n", icerr_name(r, err));
	if (r != ICERR_OK)
		return;
	BITMAPINFOHEADER *out = ic_decompress_get_format(hic, in, &r);
	if (out)
		printf("    suggested output: %s\n", cli_format_desc(out, buf, sizeof(buf)));
	else
		printf("    suggested output: none (%s)\n", icerr_name(r, err));
	free(out);
	print_outputs(hic, in);
	out = ic_negotiate_output(hic, in, &r);
	if (out)
		printf("    negotiated output: %s\n", cli_format_desc(out, buf, sizeof(buf)));
	else
		printf("    negotiated output: none (%s)\n", icerr_name(r, err));
	free(out);
}

/* Compression of each standard input format at w x h. */
static void probe_compress(HIC hic, LONG w, LONG h)
{
	struct vfw_format f;
	const char *name;
	char buf[160], err[24];
	int any = 0, unsupported = 1;

	printf("\nCompression (%ldx%ld inputs):\n", (long)w, (long)h);
	for (unsigned i = 0; (name = vfw_std_format(i, w, h, &f)); i++) {
		LRESULT r = ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)&f.bih, 0);
		if (r != ICERR_OK) {
			if (r != ICERR_UNSUPPORTED)
				unsupported = 0;
			continue;
		}
		any = 1;
		printf("  %s input: accepted\n", name);
		BITMAPINFOHEADER *out = ic_compress_get_format(hic, &f.bih, &r);
		if (!out) {
			printf("    output format: none (%s)\n", icerr_name(r, err));
			continue;
		}
		printf("    output format: %s%s\n", cli_format_desc(out, buf, sizeof(buf)),
		       out->biCompression == BI_RGB ? " (not compressed)" : "");
		/* What encode asks next, with the output; it goes on regardless. */
		r = ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)&f.bih, (DWORD_PTR)out);
		if (r != ICERR_OK)
			printf("    with that output: %s (the codec refuses its own "
			       "format; encode tries anyway)\n", icerr_name(r, err));
		LRESULT size = ic_send(hic, ICM_COMPRESS_GET_SIZE, (DWORD_PTR)&f.bih,
				       (DWORD_PTR)out);
		if (size > 0)
			printf("    largest frame: %ld bytes\n", (long)size);
		else
			printf("    largest frame: not reported (%s)\n",
			       icerr_name(size, err));
		probe_decompress(hic, out);
		free(out);
	}
	if (!any && unsupported)
		printf("  none: every query answered ICERR_UNSUPPORTED (it only "
		       "decompresses)\n");
	else if (!any)
		printf("  no standard input format accepted\n");
}

int cmd_probe(int argc, char **argv)
{
	static const UINT modes[] = { ICMODE_QUERY, ICMODE_DECOMPRESS,
				      ICMODE_COMPRESS };
	const char *target = NULL, *avi_path = NULL, *fourcc = NULL;
	const char *clsid = NULL;
	LONG w = 320, h = 240;
	int have_size = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--fourcc") && i + 1 < argc) {
			fourcc = argv[++i];
		} else if (!strcmp(a, "--clsid") && i + 1 < argc) {
			clsid = argv[++i];
		} else if (!strcmp(a, "--size") && i + 1 < argc) {
			if (cli_parse_size(a, argv[++i], &w, &h))
				return 2;
			have_size = 1;
		} else if (a[0] == '-' || (target && avi_path)) {
			return usage();
		} else if (!target) {
			target = a;
		} else {
			avi_path = a;
		}
	}
	if (!target)
		return usage();

	struct pe_image *image = pe_open(target);
	if (image) {
		struct pe_export e;
		int directshow = dshow_is_server(image);
		int driver = !pe_find_export(image, "DriverProc", 0, &e);
		pe_free(image);
		/* A driver that answers as an ACM one is one. */
		if (driver && !fourcc && !clsid && !runtime_enter() &&
		    !probe_acm(target, avi_path))
			return 0;
		if (directshow) {
			if (fourcc) {
				fprintf(stderr, "seance: --fourcc is for VfW codecs; "
					"use --clsid for DirectShow filters\n");
				return 2;
			}
			return probe_dshow(target, clsid, avi_path);
		}
	}
	if (clsid) {
		fprintf(stderr, "seance: --clsid requires a DirectShow server\n");
		return 2;
	}

	struct avi_file *avi = NULL;
	const BITMAPINFOHEADER *in = NULL;
	if (avi_path) {
		if (!(avi = avi_open(avi_path))) {
			fprintf(stderr, "seance: %s\n", vfw_error());
			return 1;
		}
		int s = avi_video_stream(avi);
		if (s < 0 || !(in = avi_stream_bih(&avi->streams[s]))) {
			fprintf(stderr, "seance: %s: no video stream\n", avi_path);
			avi_close(avi);
			return 1;
		}
	}

	struct cli_codec codec;
	int rc = cli_open_codec(target, fourcc, 0, modes, 3, &codec);
	if (rc) {
		avi_close(avi);
		return rc;
	}
	HIC hic = codec.hic;
	ICINFO info = codec.info;
	LRESULT got = codec.have_info;
	FOURCC handler = codec.handler;

	char a[FOURCC_STR_SIZE], b[FOURCC_STR_SIZE], s[512];
	printf("Codec: %s\n", cli_codec_name(&codec));
	if (!got) {
		printf("  ICM_GETINFO: no answer\n");
	} else {
		printf("  type '%s', handler '%s'\n", fourcc_str(info.fccType, a),
		       fourcc_str(info.fccHandler, b));
		printf("  name: %s\n", wide(info.szName, 16, s, sizeof(s)));
		printf("  description: %s\n",
		       wide(info.szDescription, 128, s, sizeof(s)));
		printf("  driver: %s\n", wide(info.szDriver, 128, s, sizeof(s)));
		printf("  version 0x%08lx, ICM version 0x%04lx\n",
		       (unsigned long)info.dwVersion,
		       (unsigned long)info.dwVersionICM);
		printf("  flags: ");
		print_flags(info.dwFlags);
	}

	LRESULT state = ic_send(hic, ICM_GETSTATE, 0, 0);
	DWORD quality = 0, keyrate = 0;
	char err[24];
	printf("  state: %ld bytes\n", state > 0 ? (long)state : 0L);
	if (ic_send(hic, ICM_GETDEFAULTQUALITY, (DWORD_PTR)&quality, 0) == ICERR_OK)
		printf("  default quality: %lu\n", (unsigned long)quality);
	else
		printf("  default quality: not reported\n");
	if (ic_send(hic, ICM_GETDEFAULTKEYFRAMERATE, (DWORD_PTR)&keyrate, 0) ==
	    ICERR_OK)
		printf("  default keyframe rate: %lu\n", (unsigned long)keyrate);
	else
		printf("  default keyframe rate: not reported\n");
	printf("  configure dialog: %s, about box: %s\n",
	       ic_send(hic, ICM_CONFIGURE, (DWORD_PTR)-1, ICMF_CONFIGURE_QUERY) ==
	       ICERR_OK ? "yes" : "no",
	       ic_send(hic, ICM_ABOUT, (DWORD_PTR)-1, ICMF_ABOUT_QUERY) ==
	       ICERR_OK ? "yes" : "no");

	if (in) {
		printf("\nDecompression of %s:\n", avi_path);
		printf("  input: %s\n", cli_format_desc(in, s, sizeof(s)));
		probe_decompress(hic, in);
	} else if (handler) {
		/* The codec's own FOURCC as input, without a sample. */
		if (got && info.fccHandler)
			handler = info.fccHandler;
		BITMAPINFOHEADER guess = { 0 };
		guess.biSize = sizeof(guess);
		guess.biWidth = w;
		guess.biHeight = h;
		guess.biPlanes = 1;
		guess.biBitCount = 24;
		guess.biCompression = handler;
		guess.biSizeImage = (DWORD)(w * h * 3);
		printf("\nDecompression of '%s' at %ldx%ld (no sample file; the "
		       "codec may want more):\n", fourcc_str(handler, a), (long)w,
		       (long)h);
		probe_decompress(hic, &guess);
	}
	/* Compression as encode does it: in an instance opened to compress. */
	HIC chic = cli_reopen_codec(&codec, ICMODE_COMPRESS);
	if (!chic) {
		printf("\nCompression: the codec won't open to compress "
		       "(ICMODE_COMPRESS refused): it only decompresses\n");
	} else {
		if (!in || have_size)
			probe_compress(chic, w, h);
		else
			probe_compress(chic, in->biWidth,
				       in->biHeight < 0 ? -in->biHeight : in->biHeight);
		ic_close(chic);
	}

	ic_close(hic);
	avi_close(avi);
	if (!got)
		fprintf(stderr, "seance: the codec did not answer ICM_GETINFO (%s)\n",
			icerr_name(got, err));
	return got ? 0 : 1;
}
