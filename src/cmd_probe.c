/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance probe <dll|fourcc> [in.avi]: open a codec and report what it
 * says about itself and which formats it accepts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avi/avi.h"
#include "cmd_util.h"
#include "commands.h"
#include "common/error.h"
#include "common/fourcc.h"
#include "common/unicode.h"
#include "dshow/discover.h"
#include "dshow/dshow.h"
#include "loader/pe.h"
#include "vfw/icm.h"
#include "win32/codepage.h"
#include "win32/ole.h"

static int usage(void)
{
	fprintf(stderr,
		"usage: seance probe <codec.dll | fourcc> [in.avi] [options]\n"
		"\n"
		"  --fourcc <XXXX>    the handler to open a DLL as (default: from\n"
		"                     vfw.ini, else 0, then what the codec reports)\n"
		"  --clsid <{GUID}>   DirectShow class when a server has several\n"
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

/* Probe registration metadata without changing the persistent registry. */
static void probe_dshow_connections(const char *path, const CLSID *clsid,
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
			struct dshow_filter *server = NULL;
			IBaseFilter *filter = NULL;
			struct dshow_host *host = NULL;
			AM_MEDIA_TYPE output = {0};
			hr = dshow_filter_open(path, clsid, 0, &server, &filter, NULL);
			if (SUCCEEDED(hr) && SUCCEEDED(dshow_bih_to_mt(&f.bih, sizeof(f),
					rate, scale, 0, &output)))
				hr = dshow_host_create(filter, &input, &output, NULL, NULL,
						       &host);
			if (SUCCEEDED(hr)) {
				printf(" %s", name);
				any = 1;
			}
			dshow_host_destroy(host);
			dshow_filter_close(server, filter);
			dshow_free_media_type(&output);
		}
	}
	printf("%s\n", any ? "" : " none of the standard ones");
	dshow_free_media_type(&input);
	avi_close(avi);
}

/* The optional interfaces a decoder or encoder has, as the codec's own probe says what it can do. */
static void probe_dshow_interfaces(const char *path, const struct qz_filter *f)
{
	enum dshow_role role = dshow_role(f);
	struct dshow_filter *server = NULL;
	IBaseFilter *filter = NULL;
	char names[160];

	if (role == DSHOW_OTHER ||
	    FAILED(dshow_filter_open(path, &f->clsid, role == DSHOW_ENCODER,
				     &server, &filter, NULL)))
		return;
	if (dshow_filter_interfaces(filter, names, sizeof(names)))
		printf("      interfaces: %s\n", names);
	dshow_filter_close(server, filter);
}

static int probe_dshow(const char *path, const char *selected,
		       const char *avi_path)
{
	CLSID wanted;
	if (selected && parse_clsid(selected, &wanted)) {
		fprintf(stderr, "seance: --clsid wants a GUID, not '%s'\n", selected);
		return 2;
	}

	struct qz_filter *filters = NULL;
	size_t count = 0;
	HRESULT hr;
	if (dshow_discover(path, &filters, &count, &hr)) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}
	if (FAILED(hr) && !count) {
		char name[24];
		fprintf(stderr, "seance: %s: DllRegisterServer failed (%s)\n",
			path, dshow_hr_name(hr, name));
		return 1;
	}

	size_t shown = 0;
	printf("DirectShow server: %s\n", path);
	for (size_t i = 0; i < count; i++) {
		if (selected && memcmp(&filters[i].clsid, &wanted, sizeof(wanted)))
			continue;
		dshow_print_filter(&filters[i], NULL, 1);
		probe_dshow_interfaces(path, &filters[i]);
		if (avi_path && dshow_role(&filters[i]) == DSHOW_DECODER)
			probe_dshow_connections(path, &filters[i].clsid, avi_path);
		shown++;
	}
	qz_filters_free(filters, count);
	if (!shown) {
		fprintf(stderr, "seance: %s: %s\n", path,
			selected ? "that CLSID is not registered by this server" :
			"no DirectShow filters discovered");
		return 1;
	}
	return 0;
}

/* A bounded UTF-16 field (ICINFO strings) as UTF-8. */
static const char *wide(const WCHAR *s, size_t max, char *buf, size_t size)
{
	size_t n = 0;
	while (n < max && s[n])
		n++;
	utf16_to_utf8(s, n, buf, size);
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
	int any = 0;

	printf("\nCompression (%ldx%ld inputs):\n", (long)w, (long)h);
	for (unsigned i = 0; (name = vfw_std_format(i, w, h, &f)); i++) {
		LRESULT r = ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)&f.bih, 0);
		if (r != ICERR_OK)
			continue;
		any = 1;
		printf("  %s input: accepted\n", name);
		BITMAPINFOHEADER *out = ic_compress_get_format(hic, &f.bih, &r);
		if (!out) {
			printf("    output format: none (%s)\n", icerr_name(r, err));
			continue;
		}
		printf("    output format: %s\n", cli_format_desc(out, buf, sizeof(buf)));
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
	if (!any)
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
		int directshow = dshow_is_server(image);
		pe_free(image);
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
	if (!in || have_size)
		probe_compress(hic, w, h);
	else
		probe_compress(hic, in->biWidth,
			       in->biHeight < 0 ? -in->biHeight : in->biHeight);

	ic_close(hic);
	avi_close(avi);
	if (!got)
		fprintf(stderr, "seance: the codec did not answer ICM_GETINFO (%s)\n",
			icerr_name(got, err));
	return got ? 0 : 1;
}
