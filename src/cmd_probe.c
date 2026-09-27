/*
 * vfw-harness probe <dll|fourcc> [in.avi]: open a codec and report what it
 * says about itself and which formats it accepts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "avi/avi.h"
#include "commands.h"
#include "common/error.h"
#include "common/fourcc.h"
#include "common/unicode.h"
#include "runtime/runtime.h"
#include "vfw/icm.h"
#include "vfw/registry.h"

static int usage(void)
{
	fprintf(stderr,
		"usage: vfw-harness probe <codec.dll | fourcc> [in.avi] [options]\n"
		"\n"
		"  --fourcc <XXXX>    the handler to open a DLL as (default: from\n"
		"                     vfw.ini, else 0, then what the codec reports)\n"
		"  --size <WxH>       frame size for the format checks (default\n"
		"                     320x240, or the AVI's)\n");
	return 2;
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

static const char *format_desc(const BITMAPINFOHEADER *b, char *buf,
			       size_t size)
{
	char fcc[FOURCC_STR_SIZE], comp[32];

	switch (b->biCompression) {
	case BI_RGB:       snprintf(comp, sizeof(comp), "BI_RGB"); break;
	case BI_BITFIELDS: snprintf(comp, sizeof(comp), "BI_BITFIELDS"); break;
	case BI_RLE8:      snprintf(comp, sizeof(comp), "BI_RLE8"); break;
	case BI_RLE4:      snprintf(comp, sizeof(comp), "BI_RLE4"); break;
	default:
		snprintf(comp, sizeof(comp), "'%s'",
			 fourcc_str(b->biCompression, fcc));
	}
	int n = snprintf(buf, size, "%ldx%ld, %u bits, %s, image %lu bytes",
			 (long)b->biWidth, (long)b->biHeight, b->biBitCount, comp,
			 (unsigned long)b->biSizeImage);
	if (b->biSize > sizeof(*b) && n > 0 && (size_t)n < size)
		snprintf(buf + n, size - n, ", %lu bytes of extra data",
			 (unsigned long)(b->biSize - sizeof(*b)));
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

static int looks_like_dll(const char *s)
{
	size_t n = strlen(s);
	return strchr(s, '/') || !access(s, F_OK) ||
	       (n > 4 && (!strcasecmp(s + n - 4, ".dll") ||
			  !strcasecmp(s + n - 4, ".drv") ||
			  !strcasecmp(s + n - 3, ".ax")));
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
		printf("    suggested output: %s\n", format_desc(out, buf, sizeof(buf)));
	else
		printf("    suggested output: none (%s)\n", icerr_name(r, err));
	free(out);
	print_outputs(hic, in);
	out = ic_negotiate_output(hic, in, &r);
	if (out)
		printf("    negotiated output: %s\n", format_desc(out, buf, sizeof(buf)));
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
		printf("    output format: %s\n", format_desc(out, buf, sizeof(buf)));
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

/* Opens the codec, trying the query mode first, then decompress, compress. */
static HIC open_codec(const char *target, int is_dll, FOURCC handler)
{
	static const UINT modes[] = { ICMODE_QUERY, ICMODE_DECOMPRESS,
				      ICMODE_COMPRESS };
	HIC hic = NULL;

	for (size_t i = 0; i < 3 && !hic; i++)
		hic = is_dll ? ic_open_dll(target, ICTYPE_VIDEO, handler, modes[i]) :
			       ic_open(ICTYPE_VIDEO, handler, modes[i]);
	return hic;
}

int cmd_probe(int argc, char **argv)
{
	const char *target = NULL, *avi_path = NULL, *fourcc = NULL;
	LONG w = 320, h = 240;
	int have_size = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--fourcc") && i + 1 < argc) {
			fourcc = argv[++i];
		} else if (!strcmp(a, "--size") && i + 1 < argc) {
			long x, y;
			char end;
			if (sscanf(argv[++i], "%ldx%ld%c", &x, &y, &end) != 2 ||
			    x <= 0 || y <= 0 || x > 16384 || y > 16384) {
				fprintf(stderr, "vfw-harness: --size wants WxH\n");
				return 2;
			}
			w = x;
			h = y;
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
	/* An existing file named without a directory is a path, not a name. */
	char local[4096];
	if (!strchr(target, '/') && !access(target, F_OK)) {
		snprintf(local, sizeof(local), "./%s", target);
		target = local;
	}
	if (runtime_enter()) {
		fprintf(stderr, "vfw-harness: %s\n", vfw_error());
		return 1;
	}

	FOURCC type = ICTYPE_VIDEO, handler = 0;
	int is_dll = looks_like_dll(target);
	if (fourcc && vfw_parse_fourcc(fourcc, &type, &handler)) {
		fprintf(stderr, "vfw-harness: bad FOURCC '%s'\n", fourcc);
		return 2;
	}
	if (!is_dll && vfw_parse_fourcc(target, &type, &handler)) {
		fprintf(stderr, "vfw-harness: '%s' is neither a DLL nor a FOURCC\n",
			target);
		return 2;
	}
	if (is_dll && !fourcc) {
		const struct vfw_codec *c = vfw_find_path(target);
		if (c)
			handler = c->handler;
	}

	struct avi_file *avi = NULL;
	const BITMAPINFOHEADER *in = NULL;
	if (avi_path) {
		if (!(avi = avi_open(avi_path))) {
			fprintf(stderr, "vfw-harness: %s\n", vfw_error());
			return 1;
		}
		int s = avi_video_stream(avi);
		if (s < 0 || !(in = avi_stream_bih(&avi->streams[s]))) {
			fprintf(stderr, "vfw-harness: %s: no video stream\n", avi_path);
			avi_close(avi);
			return 1;
		}
	}

	HIC hic = open_codec(target, is_dll, handler);
	if (!hic) {
		char fcc[FOURCC_STR_SIZE];
		fprintf(stderr, "vfw-harness: %s\n", vfw_error());
		if (is_dll && !handler && strstr(vfw_error(), "DRV_OPEN"))
			fprintf(stderr, "vfw-harness: some codecs only open under their "
				"FOURCC; try --fourcc\n");
		else if (!is_dll && strstr(vfw_error(), "no codec is registered"))
			fprintf(stderr, "vfw-harness: codecs are registered in %s, "
				"as vidc.%s=<path to the DLL>\n", vfw_ini_path(),
				fourcc_str(handler, fcc));
		avi_close(avi);
		return 1;
	}

	ICINFO info;
	memset(&info, 0, sizeof(info));
	LRESULT got = ic_get_info(hic, &info, sizeof(info));

	/* Reopen under the codec's own FOURCC if it was opened without one. */
	if (is_dll && got && !handler && info.fccHandler) {
		handler = info.fccHandler;
		HIC again = open_codec(target, 1, handler);
		if (again) {
			ic_close(hic);
			hic = again;
		}
	}
	if (is_dll && handler && !vfw_find(ICTYPE_VIDEO, handler))
		vfw_register(ICTYPE_VIDEO, handler, target);

	char a[FOURCC_STR_SIZE], b[FOURCC_STR_SIZE], s[512];
	printf("Codec: %s\n", is_dll ? target : ic_driver_name(hic));
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
		printf("  input: %s\n", format_desc(in, s, sizeof(s)));
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
		fprintf(stderr, "vfw-harness: the codec did not answer ICM_GETINFO (%s)\n",
			icerr_name(got, err));
	return got ? 0 : 1;
}
