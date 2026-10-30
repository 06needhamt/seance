/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The VfW layer: the codec registry, the installable-driver lifecycle, the
 * ICM API and the winmm / msvfw32 built-ins, driven against testcodec.dll.
 */
#include <sys/stat.h>

#include <fcntl.h>

#include "avi/avi.h"
#include "commands.h"
#include "common/error.h"
#include "common/unicode.h"
#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "vfw/driver.h"
#include "vfw/icm.h"
#include "vfw/registry.h"
#include "win32/k32.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)
#define TST0 FCC('T', 'S', 'T', '0')

static char root[4096];

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	if (!f || fputs(text, f) < 0) {
		perror(path);
		exit(1);
	}
	fclose(f);
}

/* A throwaway VFW_ROOT whose vfw.ini registers the codec paths given. */
static void setup_root(const char *codec)
{
	char ini[16384], path[4200];
	const char *t = getenv("TMPDIR");

	snprintf(root, sizeof(root), "%s/vfw-icm-XXXXXX", t && *t ? t : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		exit(1);
	}
	setenv("VFW_ROOT", root, 1);
	unsetenv("VFW_INI");
	snprintf(ini, sizeof(ini),
		 "; codecs for test_vfw\n"
		 "[other]\n"
		 "vidc.OTHR=other.dll\n"
		 "[Drivers32]\n"
		 "  vidc.TST0 = %s\n"
		 "vidc.XXXX=\"%s\"\n"
		 "vidc.NONE=nosuch.dll\n"
		 "vidc.REL=sub/rel.dll\n"
		 "msacm.imaadpcm=imaadp32.acm\n"
		 "bogus=1\n",
		 codec, codec);
	snprintf(path, sizeof(path), "%s/vfw.ini", root);
	write_file(path, ini);
	vfw_registry_reload();
}

static void test_registry(void)
{
	FOURCC type, handler;
	char expect[4200];

	CHECK_EQ(vfw_parse_fourcc("TST0", &type, &handler), 0);
	CHECK_EQ(type, ICTYPE_VIDEO);
	CHECK_EQ(handler, TST0);
	CHECK_EQ(vfw_parse_fourcc("vidc.cvid", &type, &handler), 0);
	CHECK_EQ(handler, FCC('c', 'v', 'i', 'd'));
	CHECK_EQ(vfw_parse_fourcc("vidc.x", &type, &handler), 0);
	CHECK_EQ(handler, FCC('x', ' ', ' ', ' '));
	/* ACM entries ("msacm.imaadpcm") are not VfW codecs. */
	CHECK_EQ(vfw_parse_fourcc("msacm.x", &type, &handler), -1);
	CHECK_EQ(vfw_parse_fourcc("TOOLONG", &type, &handler), -1);
	CHECK_EQ(vfw_parse_fourcc("", &type, &handler), -1);
	CHECK_EQ(vfw_parse_fourcc("vid.TST0", &type, &handler), -1);

	snprintf(expect, sizeof(expect), "%s/vfw.ini", root);
	CHECK_STR(vfw_ini_path(), expect);

	/* Only [drivers32] counts; keys are matched case-insensitively. */
	const struct vfw_codec *c = vfw_find(ICTYPE_VIDEO, FCC('t', 's', 't', '0'));
	CHECK(c && c->from_ini && c->handler == TST0);
	CHECK(vfw_find(FCC('V', 'I', 'D', 'C'), TST0) == c);
	CHECK(!vfw_find(ICTYPE_VIDEO, FCC('O', 'T', 'H', 'R')));
	c = vfw_find(ICTYPE_VIDEO, FCC('X', 'X', 'X', 'X'));
	CHECK(c && c->path[0] == '/');	/* quotes removed */
	/* A bare name that isn't anywhere stays bare, for the loader. */
	c = vfw_find(ICTYPE_VIDEO, FCC('N', 'O', 'N', 'E'));
	CHECK(c && !strcmp(c->path, "nosuch.dll"));
	/* A relative path is taken from the ini's directory. */
	snprintf(expect, sizeof(expect), "%s/sub/rel.dll", root);
	c = vfw_find(ICTYPE_VIDEO, FCC('R', 'E', 'L', ' '));
	CHECK(c && !strcmp(c->path, expect));

	/* Enumeration by type. */
	unsigned n = 0;
	while (vfw_enum(ICTYPE_VIDEO, n))
		n++;
	CHECK_EQ(n, 4);
	n = 0;
	while (vfw_enum(0, n))
		n++;
	CHECK_EQ(n, 4);

	/* The ACM driver is in a table of its own. */
	char *alias = NULL, *apath = NULL;
	CHECK_EQ(vfw_acm_get(0, &alias, &apath), 0);
	CHECK(alias && !strcmp(alias, "msacm.imaadpcm"));
	CHECK(apath && !strcmp(apath, "imaadp32.acm"));
	free(alias);
	free(apath);
	CHECK_EQ(vfw_acm_get(1, &alias, &apath), -1);
	CHECK(vfw_is_acm_alias("MSACM.x") && !vfw_is_acm_alias("msacm.") &&
	      !vfw_is_acm_alias("vidc.TST0"));
	CHECK_EQ(vfw_acm_register_value("vidc.ABCD", "x.acm"), -1);
	CHECK_EQ(vfw_acm_register_value("msacm.extra", "/x/extra.acm"), 0);
	CHECK_EQ(vfw_acm_register_value("MSACM.EXTRA", "/x/again.acm"), 0);
	CHECK_EQ(vfw_acm_get(1, &alias, &apath), 0);
	CHECK(alias && !strcmp(alias, "msacm.extra"));
	CHECK(apath && !strcmp(apath, "/x/again.acm"));	/* replaced */
	free(alias);
	free(apath);
	CHECK_EQ(vfw_acm_unregister("msacm.Extra"), 0);
	CHECK_EQ(vfw_acm_unregister("msacm.extra"), -1);
	CHECK_EQ(vfw_acm_get(1, &alias, &apath), -1);

	/* Run-time entries. */
	CHECK_EQ(vfw_register(ICTYPE_VIDEO, FCC('R', 'U', 'N', '1'), "/x/run.dll"), 0);
	c = vfw_find(ICTYPE_VIDEO, FCC('r', 'u', 'n', '1'));
	CHECK(c && !c->from_ini && !strcmp(c->path, "/x/run.dll"));
	CHECK(vfw_find_path("/other/dir/RUN.DLL") == c);	/* by name */
	CHECK_EQ(vfw_unregister(ICTYPE_VIDEO, FCC('R', 'U', 'N', '1')), 0);
	CHECK_EQ(vfw_unregister(ICTYPE_VIDEO, FCC('R', 'U', 'N', '1')), -1);
	CHECK(!vfw_find(ICTYPE_VIDEO, FCC('R', 'U', 'N', '1')));
	/* VFW_INI overrides the location. */
	char other[4200];
	snprintf(other, sizeof(other), "%s/other.ini", root);
	write_file(other, "[drivers32]\nvidc.ONLY=only.dll\n");
	setenv("VFW_INI", other, 1);
	vfw_registry_reload();
	CHECK(vfw_find(ICTYPE_VIDEO, FCC('O', 'N', 'L', 'Y')) != NULL);
	CHECK(!vfw_find(ICTYPE_VIDEO, TST0));
	unsetenv("VFW_INI");
	unlink(other);
	vfw_registry_reload();
	CHECK(vfw_find(ICTYPE_VIDEO, TST0) != NULL);
}

static void test_names(void)
{
	char buf[24];
	struct vfw_format f;

	CHECK_STR(drv_msg_name(DRV_OPEN, buf), "DRV_OPEN");
	CHECK_STR(drv_msg_name(ICM_DECOMPRESS, buf), "ICM_DECOMPRESS");
	CHECK_STR(drv_msg_name(DRV_USER + 0x999, buf), "DRV_USER+0x999");
	CHECK_STR(drv_msg_name(0x77, buf), "0x77");
	CHECK_STR(icerr_name(ICERR_BADFORMAT, buf), "ICERR_BADFORMAT");
	CHECK_STR(icerr_name(ICERR_CUSTOM - 3, buf), "ICERR_CUSTOM-3");
	CHECK_STR(icerr_name(1234, buf), "1234");

	/* The standard formats, and format sizes. */
	CHECK_STR(vfw_std_format(0, 33, 17, &f), "I420");
	CHECK_EQ(f.bih.biSizeImage, 33 * 17 + 2 * 17 * 9);
	CHECK_STR(vfw_std_format(2, 33, 17, &f), "YUY2");
	CHECK_EQ(f.bih.biSizeImage, 17 * 4 * 17);
	CHECK_STR(vfw_std_format(5, 33, 17, &f), "BGR24");
	CHECK_EQ(f.bih.biSizeImage, 100 * 17);
	CHECK_EQ(vfw_format_size(&f.bih), 40);
	CHECK_STR(vfw_std_format(6, 33, 17, &f), "RGB565");
	CHECK_EQ(vfw_format_size(&f.bih), 52);
	CHECK_EQ(((DWORD *)f.colors)[0], 0xf800);
	CHECK_STR(vfw_std_format(8, 33, 17, &f), "PAL8");
	CHECK_EQ(vfw_format_size(&f.bih), 40 + 1024);
	CHECK(vfw_std_format(9, 33, 17, &f) == NULL);
}

/* A driver procedure written in C, for drv_open_proc / ICOpenFunction. */
static UINT proc_log[32];
static int proc_nlog;
static int refuse_load;

static LRESULT WINAPI c_proc(DWORD_PTR id, HDRVR h, UINT msg, LPARAM l1,
			     LPARAM l2)
{
	if (proc_nlog < 32)
		proc_log[proc_nlog++] = msg;
	switch (msg) {
	case DRV_LOAD:
		return !refuse_load;
	case DRV_OPEN:
		return 0x1234;
	case ICM_GETSTATE:
		return id == 0x1234 ? 42 : -1;
	default:
		return msg < DRV_USER ? 1 : ICERR_UNSUPPORTED;
	}
}

static void test_driver_proc(void)
{
	proc_nlog = 0;
	HDRVR h = drv_open_proc(c_proc, NULL, 0);
	CHECK(h && drv_valid(h));
	CHECK_EQ(drv_send(h, ICM_GETSTATE, 0, 0), 42);
	CHECK(drv_module(h) == NULL);
	CHECK_EQ(drv_close(h, 0, 0), 1);
	CHECK(!drv_valid(h));
	CHECK_EQ(drv_send(h, ICM_GETSTATE, 0, 0), 0);
	static const UINT want[] = { DRV_LOAD, DRV_ENABLE, DRV_OPEN, ICM_GETSTATE,
				     DRV_CLOSE, DRV_DISABLE, DRV_FREE };
	CHECK_EQ(proc_nlog, 7);
	CHECK(!memcmp(proc_log, want, sizeof(want)));

	/* A refused DRV_LOAD: nothing else is sent. */
	proc_nlog = 0;
	refuse_load = 1;
	CHECK(!drv_open_proc(c_proc, NULL, 0));
	CHECK(strstr(vfw_error(), "DRV_LOAD"));
	CHECK_EQ(proc_nlog, 1);
	refuse_load = 0;
	CHECK_EQ(drv_open_count(), 0);

	/* Bad handles. */
	CHECK(!drv_valid(NULL));
	CHECK(!drv_valid((HDRVR)0x1234));
	CHECK(!drv_valid((HDRVR)0x5d000000));
	CHECK(!drv_valid((HDRVR)0x5d00ffff));
	CHECK_EQ(drv_send((HDRVR)0x5d000001, DRV_USER, 0, 0), 0);
	CHECK_EQ(drv_close((HDRVR)0x5d000001, 0, 0), 0);

	/* ICOpenFunction and ICInstall(ICINSTALL_FUNCTION). */
	HIC hic = ic_open_function(ICTYPE_VIDEO, FCC('F', 'U', 'N', 'C'),
				   ICMODE_QUERY, (FARPROC)(void *)c_proc);
	CHECK(hic && ic_valid(hic));
	CHECK_EQ(ic_send(hic, ICM_GETSTATE, 0, 0), 42);
	CHECK_STR(ic_driver_name(hic), "");
	CHECK_EQ(ic_close(hic), ICERR_OK);
	CHECK_EQ(ic_close(hic), ICERR_BADHANDLE);
	CHECK_EQ(ic_send(hic, ICM_GETSTATE, 0, 0), ICERR_BADHANDLE);

	CHECK(ic_install(ICTYPE_VIDEO, FCC('F', 'U', 'N', 'C'), (LPARAM)c_proc,
			 NULL, ICINSTALL_FUNCTION));
	hic = ic_open(ICTYPE_VIDEO, FCC('F', 'U', 'N', 'C'), ICMODE_QUERY);
	CHECK(hic != NULL);
	CHECK_EQ(ic_send(hic, ICM_GETSTATE, 0, 0), 42);
	ic_close(hic);
	CHECK(ic_remove(ICTYPE_VIDEO, FCC('F', 'U', 'N', 'C'), 0));
	CHECK(!ic_open(ICTYPE_VIDEO, FCC('F', 'U', 'N', 'C'), ICMODE_QUERY));
	CHECK(strstr(vfw_error(), "no codec is registered"));
}

typedef int (*stats_fn)(DWORD *out, int n, int reset);
typedef LONG (*count_fn)(void);
typedef int (*selftest_fn)(void);

static stats_fn codec_stats;
static count_fn codec_opens;
static DWORD (*codec_quality)(void);
static count_fn codec_module_misses;

static int log_is(const UINT *want, int n)
{
	DWORD got[64];
	int count = codec_stats(got, 64, 1);

	if (count != n) {
		fprintf(stderr, "logged %d messages, wanted %d\n", count, n);
		return 0;
	}
	for (int i = 0; i < n; i++)
		if (got[i] != want[i]) {
			fprintf(stderr, "message %d is 0x%lx, wanted 0x%x\n", i,
				(unsigned long)got[i], want[i]);
			return 0;
		}
	return 1;
}

static void test_driver_lifecycle(const char *codec)
{
	codec_stats(NULL, 0, 1);

	/* Two instances share one DRV_LOAD / DRV_FREE. */
	HDRVR a = drv_open(codec, NULL, 0);
	HDRVR b = drv_open(codec, NULL, 0);
	CHECK(a && b && a != b);
	CHECK_EQ(drv_open_count(), 2);
	CHECK_EQ(codec_opens(), 2);
	CHECK(drv_module(a) && drv_module(a) == drv_module(b));
	/* The driver found its module while loading and opening. */
	CHECK_EQ(codec_module_misses(), 0);
	ICINFO info;
	CHECK_EQ(drv_send(a, ICM_GETINFO, (LPARAM)&info, sizeof(info)),
		 sizeof(info));
	CHECK_EQ(info.fccHandler, TST0);
	CHECK_EQ(drv_close(a, 0, 0), 1);
	CHECK_EQ(drv_close(b, 0, 0), 1);
	CHECK_EQ(drv_open_count(), 0);
	CHECK_EQ(codec_opens(), 0);
	static const UINT full[] = { DRV_LOAD, DRV_ENABLE, DRV_OPEN, DRV_OPEN,
				     DRV_CLOSE, DRV_CLOSE, DRV_DISABLE, DRV_FREE };
	CHECK(log_is(full, 8));

	/* A refused DRV_OPEN on the first instance undoes the load. */
	ICOPEN op = { sizeof(op), FCC('a', 'u', 'd', 'c'), 0, ICVERSION,
		      ICMODE_QUERY, 0, NULL, NULL, 0 };
	CHECK(!drv_open(codec, NULL, (LPARAM)&op));
	CHECK(strstr(vfw_error(), "DRV_OPEN"));
	CHECK_EQ(op.dwError, ICERR_UNSUPPORTED);
	static const UINT refused[] = { DRV_LOAD, DRV_ENABLE, DRV_OPEN,
					DRV_DISABLE, DRV_FREE };
	CHECK(log_is(refused, 5));

	/* A DLL that isn't a driver, and one that isn't there. */
	CHECK(!drv_open(fixture("pe_basic.dll"), NULL, 0));
	CHECK(strstr(vfw_error(), "no DriverProc"));
	CHECK(!drv_open("/nonexistent/codec.dll", NULL, 0));
	CHECK_EQ(drv_open_count(), 0);
}

static BITMAPINFOHEADER rgb(LONG w, LONG h, WORD bits)
{
	BITMAPINFOHEADER b = { 0 };
	b.biSize = sizeof(b);
	b.biWidth = w;
	b.biHeight = h;
	b.biPlanes = 1;
	b.biBitCount = bits;
	b.biCompression = BI_RGB;
	b.biSizeImage = ((w * bits + 31) / 32 * 4) * (h < 0 ? -h : h);
	return b;
}

/* Frame n of a moving test picture, 24-bit bottom-up. */
static void picture(BYTE *p, LONG w, LONG h, int n)
{
	size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
	memset(p, 0, stride * h);
	for (LONG y = 0; y < h; y++)
		for (LONG x = 0; x < w; x++) {
			BYTE *q = p + y * stride + 3 * x;
			q[0] = (BYTE)(x * 7 + n * 3);
			q[1] = (BYTE)(y * 5);
			q[2] = (BYTE)((x + y + n) / 4 * 16);
		}
	/* Frames 4 and 5 are the same picture. */
}

static char avi_path[4200];

static void test_icm(void)
{
	ICINFO info;
	HIC hic;

	/* Opening through the registry. */
	hic = ic_open(ICTYPE_VIDEO, FCC('t', 's', 't', '0'), ICMODE_QUERY);
	CHECK(hic && ic_valid(hic));
	if (!hic)
		return;
	memset(&info, 0xcc, sizeof(info));
	CHECK_EQ(ic_get_info(hic, &info, sizeof(info)), sizeof(info));
	CHECK_EQ(info.fccType, ICTYPE_VIDEO);
	CHECK_EQ(info.fccHandler, TST0);
	CHECK_EQ(info.dwVersionICM, ICVERSION);
	CHECK(info.szName[0] == 'T' && info.szName[4] == 0);
	/* The codec leaves szDriver empty; ICGetInfo fills in the file name. */
	CHECK(info.szDriver[0] == 't' && info.szDriver[9] == '.');
	CHECK_EQ(ic_get_info(hic, NULL, sizeof(info)), 0);
	ic_close(hic);

	CHECK(!ic_open(ICTYPE_VIDEO, FCC('X', 'X', 'X', 'X'), ICMODE_QUERY));
	CHECK(strstr(vfw_error(), "DRV_OPEN"));	/* the codec said no */
	CHECK(!ic_open(ICTYPE_VIDEO, FCC('N', 'O', 'N', 'E'), ICMODE_QUERY));
	CHECK(strstr(vfw_error(), "nosuch.dll"));
	CHECK(!ic_open(ICTYPE_VIDEO, FCC('Z', 'Z', 'Z', 'Z'), ICMODE_QUERY));

	/* ICInfo: by index, and by FOURCC. */
	CHECK(ic_info(ICTYPE_VIDEO, 0, &info));
	CHECK_EQ(info.fccHandler, TST0);
	CHECK_EQ(info.dwSize, sizeof(info));
	CHECK(ic_info(ICTYPE_VIDEO, 3, &info));
	CHECK(!ic_info(ICTYPE_VIDEO, 4, &info));
	CHECK(ic_info(ICTYPE_VIDEO, FCC('N', 'O', 'N', 'E'), &info));
	CHECK(info.szDriver[0] == 'n');
	CHECK(!ic_info(ICTYPE_VIDEO, FCC('Z', 'Z', 'Z', 'Z'), &info));

	/* ICLocate: by input format, trying every registered codec. */
	const LONG w = 33, h = 17;
	BITMAPINFOHEADER in = rgb(w, h, 24);
	hic = ic_locate(ICTYPE_VIDEO, 0, &in, NULL, ICMODE_COMPRESS);
	CHECK(hic != NULL);
	LRESULT err;
	BITMAPINFOHEADER *cfmt = hic ? ic_compress_get_format(hic, &in, &err) : NULL;
	CHECK(cfmt && cfmt->biCompression == TST0 && cfmt->biBitCount == 24);
	if (hic)
		ic_close(hic);
	if (!cfmt)
		return;
	HIC dec = ic_locate(ICTYPE_VIDEO, 0, cfmt, NULL, ICMODE_DECOMPRESS);
	CHECK(dec != NULL);
	if (dec)
		ic_close(dec);
	BITMAPINFOHEADER mjpg = rgb(w, h, 24);
	mjpg.biCompression = FCC('M', 'J', 'P', 'G');
	CHECK(!ic_locate(ICTYPE_VIDEO, 0, &mjpg, NULL, ICMODE_DECOMPRESS));
	CHECK(!ic_locate(ICTYPE_VIDEO, 0, &in, NULL, 77));

	/* State, through a compressor. */
	HIC enc = ic_open(ICTYPE_VIDEO, TST0, ICMODE_COMPRESS);
	CHECK(enc != NULL);
	if (!enc) {
		free(cfmt);
		return;
	}
	DWORD st[2] = { 0, 0 };
	CHECK_EQ(ic_send(enc, ICM_GETSTATE, 0, 0), 8);
	CHECK_EQ(ic_send(enc, ICM_GETSTATE, (DWORD_PTR)st, sizeof(st)), 8);
	CHECK(st[0] == 7500 && st[1] == 15);
	DWORD newst[2] = { 1234, 3 };
	CHECK_EQ(ic_send(enc, ICM_SETSTATE, (DWORD_PTR)newst, sizeof(newst)), 8);
	CHECK_EQ(ic_send(enc, ICM_GETSTATE, (DWORD_PTR)st, sizeof(st)), 8);
	CHECK(st[0] == 1234 && st[1] == 3);
	DWORD q = 0;
	CHECK_EQ(ic_send(enc, ICM_GETDEFAULTQUALITY, (DWORD_PTR)&q, 0), ICERR_OK);
	CHECK_EQ(q, 7500);

	/* Compress 8 frames; keyframes every 3 from the state. */
	const int frames = 8;
	LRESULT max = ic_send(enc, ICM_COMPRESS_GET_SIZE, (DWORD_PTR)&in,
			      (DWORD_PTR)cfmt);
	CHECK(max >= (LRESULT)in.biSizeImage);
	BYTE *src = malloc(in.biSizeImage), *out[8];
	DWORD sizes[8], flags[8];
	CHECK_EQ(ic_send(enc, ICM_COMPRESS_BEGIN, (DWORD_PTR)&in, (DWORD_PTR)cfmt),
		 ICERR_OK);
	for (int i = 0; i < frames; i++) {
		DWORD ckid = 0;
		out[i] = malloc(max);
		picture(src, w, h, i == 5 ? 4 : i);
		flags[i] = 0;
		CHECK_EQ(ic_compress(enc, 0, cfmt, out[i], &in, src, &ckid, &flags[i],
				     i, 0, 1234, NULL, NULL), ICERR_OK);
		sizes[i] = cfmt->biSizeImage;
		CHECK(sizes[i] > 4 && sizes[i] <= (DWORD)max);
		CHECK_EQ(ckid, FCC('d', 'c', 0, 0));
		CHECK_EQ(flags[i] == AVIIF_KEYFRAME, i % 3 == 0);
	}
	CHECK_EQ(ic_send(enc, ICM_COMPRESS_END, 0, 0), ICERR_OK);

	/* Keep the frames as an AVI for test_probe_cli. */
	snprintf(avi_path, sizeof(avi_path), "%s/tst0.avi", root);
	struct avi_writer *aw = avi_writer_open(avi_path, cfmt, sizeof(*cfmt),
						TST0, 25, 1, NULL);
	CHECK(aw != NULL);
	for (int i = 0; aw && i < frames; i++)
		CHECK_EQ(avi_writer_add(aw, out[i], sizes[i],
					flags[i] == AVIIF_KEYFRAME), 0);
	if (aw)
		CHECK_EQ(avi_writer_close(aw), 0);
	/* An unchanged frame's delta is tiny. */
	CHECK(sizes[5] < 64);
	ic_close(enc);

	/* Decompress into BGR24 bottom-up (exact) and BGR32 top-down. */
	dec = ic_open(ICTYPE_VIDEO, TST0, ICMODE_DECOMPRESS);
	CHECK(dec != NULL);
	if (dec) {
		BITMAPINFOHEADER *nego = ic_negotiate_output(dec, cfmt, &err);
		CHECK(nego && nego->biBitCount == 24 && nego->biHeight == h &&
		      nego->biCompression == BI_RGB);
		free(nego);
		BITMAPINFOHEADER disp;
		CHECK(ic_get_display_format(dec, cfmt, &disp, 0, 0, 0) == dec);
		CHECK_EQ(disp.biBitCount, 32);

		BITMAPINFOHEADER o24 = rgb(w, h, 24), o32 = rgb(w, -h, 32);
		/* Zeroed: the codec leaves the row padding alone. */
		BYTE *d24 = calloc(1, o24.biSizeImage), *d32 = calloc(1, o32.biSizeImage);
		BITMAPINFOHEADER cin = *cfmt;

		CHECK_EQ(ic_send(dec, ICM_DECOMPRESS_BEGIN, (DWORD_PTR)cfmt,
				 (DWORD_PTR)&o24), ICERR_OK);
		/* A delta before any keyframe. */
		cin.biSizeImage = sizes[1];
		CHECK_EQ(ic_decompress(dec, 0, &cin, out[1], &o24, d24),
			 (DWORD)ICERR_GOTOKEYFRAME);
		int bad = 0;
		for (int i = 0; i < frames; i++) {
			cin.biSizeImage = sizes[i];
			if (ic_decompress(dec, i % 3 ? ICDECOMPRESS_NOTKEYFRAME : 0,
					  &cin, out[i], &o24, d24) != ICERR_OK) {
				bad++;
				continue;
			}
			picture(src, w, h, i == 5 ? 4 : i);
			bad += memcmp(src, d24, o24.biSizeImage) != 0;
		}
		CHECK_EQ(bad, 0);
		/* Truncated data is refused. */
		cin.biSizeImage = sizes[0] / 2;
		CHECK_EQ(ic_decompress(dec, 0, &cin, out[0], &o24, d24),
			 (DWORD)ICERR_BADFORMAT);
		CHECK_EQ(ic_send(dec, ICM_DECOMPRESS_END, 0, 0), ICERR_OK);

		/* Top-down 32-bit: the same pixels, flipped, with a pad byte. */
		CHECK_EQ(ic_send(dec, ICM_DECOMPRESS_BEGIN, (DWORD_PTR)cfmt,
				 (DWORD_PTR)&o32), ICERR_OK);
		cin.biSizeImage = sizes[0];
		CHECK_EQ(ic_decompress(dec, 0, &cin, out[0], &o32, d32), ICERR_OK);
		picture(src, w, h, 0);
		size_t s24 = ((size_t)w * 3 + 3) & ~(size_t)3;
		bad = 0;
		for (LONG y = 0; y < h; y++)
			for (LONG x = 0; x < w; x++)
				bad += memcmp(d32 + (size_t)y * w * 4 + 4 * x,
					      src + (h - 1 - y) * s24 + 3 * x, 3) != 0;
		CHECK_EQ(bad, 0);
		ic_send(dec, ICM_DECOMPRESS_END, 0, 0);

		/* Formats it refuses. */
		BITMAPINFOHEADER yuy2 = rgb(w, h, 16);
		yuy2.biCompression = FCC('Y', 'U', 'Y', '2');
		CHECK_EQ(ic_send(dec, ICM_DECOMPRESS_QUERY, (DWORD_PTR)cfmt,
				 (DWORD_PTR)&yuy2), ICERR_BADFORMAT);
		BITMAPINFOHEADER small = rgb(w - 1, h, 24);
		CHECK_EQ(ic_send(dec, ICM_DECOMPRESS_QUERY, (DWORD_PTR)cfmt,
				 (DWORD_PTR)&small), ICERR_BADFORMAT);
		free(d24);
		free(d32);
		ic_close(dec);
	}
	for (int i = 0; i < frames; i++)
		free(out[i]);
	free(src);
	free(cfmt);
	CHECK_EQ(codec_opens(), 0);
}

static void test_builtins(struct module *codec)
{
	struct module *mm = mod_load("winmm.dll", NULL);
	CHECK(mm && mm->builtin);
	if (mm) {
		DRIVERPROC def = mod_get_proc(mm, "DefDriverProc");
		CHECK(def != NULL);
		if (def) {
			CHECK_EQ(def(0, NULL, DRV_LOAD, 0, 0), 1);
			CHECK_EQ(def(0, NULL, DRV_CLOSE, 0, 0), 1);
			CHECK_EQ(def(0, NULL, DRV_QUERYCONFIGURE, 0, 0), 0);
			CHECK_EQ(def(0, NULL, DRV_CONFIGURE, 0, 0), DRVCNF_OK);
			CHECK_EQ(def(0, NULL, DRV_REMOVE, 0, 0), DRVCNF_RESTART);
			CHECK_EQ(def(0, NULL, ICM_GETINFO, 0, 0), 0);
		}
		DWORD (WINAPI *tgt)(void) = mod_get_proc(mm, "timeGetTime");
		CHECK(tgt && tgt() > 0);
		mod_free(mm);
	}

	/* The codec calls winmm and msvfw32 itself. */
	selftest_fn selftest = mod_get_proc(codec, "testcodec_selftest");
	CHECK(selftest != NULL);
	if (selftest)
		CHECK_EQ(selftest(), 0);
	CHECK_EQ(drv_open_count(), 0);
	CHECK_EQ(codec_opens(), 0);
}

/* ICImageCompress / ICImageDecompress, through msvfw32's export table. */
static void test_image(void)
{
	struct module *vfw = mod_load("msvfw32.dll", NULL);
	struct module *k32 = mod_load("kernel32.dll", NULL);
	CHECK(vfw && k32);
	if (!vfw || !k32)
		return;
	HANDLE (WINAPI *compress)(HIC, UINT, BITMAPINFO *, void *, BITMAPINFO *,
				  LONG, LONG *) = mod_get_proc(vfw, "ICImageCompress");
	HANDLE (WINAPI *decompress)(HIC, UINT, BITMAPINFO *, void *,
				    BITMAPINFO *) = mod_get_proc(vfw, "ICImageDecompress");
	LPVOID (WINAPI *lock)(HGLOBAL) = mod_get_proc(k32, "GlobalLock");
	BOOL (WINAPI *unlock)(HGLOBAL) = mod_get_proc(k32, "GlobalUnlock");
	HGLOBAL (WINAPI *gfree)(HGLOBAL) = mod_get_proc(k32, "GlobalFree");
	SIZE_T (WINAPI *gsize)(HGLOBAL) = mod_get_proc(k32, "GlobalSize");
	CHECK(compress && decompress && lock && unlock && gfree && gsize);
	if (!compress || !decompress || !lock || !unlock || !gfree || !gsize)
		return;

	const LONG w = 33, h = 17;
	BITMAPINFOHEADER in = rgb(w, h, 24);
	BYTE *src = malloc(in.biSizeImage);
	picture(src, w, h, 2);

	/* A compressor found by format, its default output, a quality. */
	LONG size = 0;
	HANDLE c = compress(NULL, 0, (BITMAPINFO *)&in, src, NULL, 6000, &size);
	CHECK(c != NULL);
	if (!c) {
		free(src);
		return;
	}
	CHECK_EQ(codec_quality(), 6000);
	BITMAPINFOHEADER *ch = lock(c);
	size_t chdr = vfw_format_size(ch);
	BYTE *cbits = (BYTE *)ch + chdr;
	CHECK(ch->biCompression == TST0 && ch->biWidth == w && ch->biHeight == h);
	CHECK(size > 0 && ch->biSizeImage == (DWORD)size);
	CHECK(gsize(c) >= chdr + (SIZE_T)size);

	/* Back through a decompressor found by format, to its default. */
	HANDLE d = decompress(NULL, 0, (BITMAPINFO *)ch, cbits, NULL);
	CHECK(d != NULL);
	if (d) {
		BITMAPINFOHEADER *dh = lock(d);
		CHECK(dh->biCompression == BI_RGB && dh->biBitCount == 24 &&
		      dh->biWidth == w && dh->biHeight == h);
		CHECK_EQ(dh->biSizeImage, in.biSizeImage);
		CHECK(!memcmp((BYTE *)dh + dh->biSize, src, in.biSizeImage));
		unlock(d);
		CHECK(gfree(d) == NULL);
	}

	/* The caller's decompressor, to BGR32 top-down; it stays open. */
	HIC dec = ic_open(ICTYPE_VIDEO, TST0, ICMODE_DECOMPRESS);
	BITMAPINFOHEADER o32 = rgb(w, -h, 32);
	d = decompress(dec, 0, (BITMAPINFO *)ch, cbits, (BITMAPINFO *)&o32);
	CHECK(d != NULL && ic_valid(dec));
	if (d) {
		BITMAPINFOHEADER *dh = lock(d);
		const BYTE *px = (BYTE *)dh + sizeof(*dh);
		size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
		int bad = 0;
		CHECK(dh->biBitCount == 32 && dh->biHeight == -h);
		for (LONG y = 0; y < h; y++)
			for (LONG x = 0; x < w; x++)
				bad |= memcmp(px + ((size_t)y * w + x) * 4,
					      src + (h - 1 - y) * stride + 3 * x, 3);
		CHECK(!bad);
		unlock(d);
		gfree(d);
	}
	ic_close(dec);

	/* The caller's compressor and format; the size goes to the codec. */
	HIC enc = ic_open(ICTYPE_VIDEO, TST0, ICMODE_COMPRESS);
	LRESULT err;
	BITMAPINFOHEADER *cfmt = enc ? ic_compress_get_format(enc, &in, &err) : NULL;
	CHECK(cfmt != NULL);
	if (cfmt) {
		LONG size2 = 100000;
		HANDLE c2 = compress(enc, 0, (BITMAPINFO *)&in, src,
				     (BITMAPINFO *)cfmt, ICQUALITY_DEFAULT, &size2);
		CHECK(c2 != NULL && size2 == size && ic_valid(enc));
		CHECK_EQ(codec_quality(), (DWORD)ICQUALITY_DEFAULT);
		gfree(c2);
		free(cfmt);
	}
	ic_close(enc);

	/* Failures, with nothing left open. */
	BITMAPINFOHEADER mjpg = *ch, bad_out = rgb(w, h, 24);
	mjpg.biCompression = FCC('M', 'J', 'P', 'G');
	bad_out.biSize = 20;
	CHECK(!compress(NULL, 1, (BITMAPINFO *)&in, src, NULL, 5000, &size));
	CHECK(!compress(NULL, 0, (BITMAPINFO *)&in, NULL, NULL, 5000, &size));
	CHECK(!decompress(NULL, 0, (BITMAPINFO *)&mjpg, cbits, NULL));
	CHECK(!decompress(NULL, 0, (BITMAPINFO *)ch, cbits,
			  (BITMAPINFO *)&bad_out));
	CHECK(!decompress(NULL, 0, (BITMAPINFO *)ch, NULL, NULL));
	CHECK_EQ(codec_opens(), 0);

	unlock(c);
	gfree(c);
	free(src);
	mod_free(k32);
	mod_free(vfw);
}

/* The BGR of pixel (x, y), counted from the top, of a 24-bit DIB. */
static DWORD bgr24_at(const BYTE *p, LONG w, LONG h, LONG x, LONG y)
{
	size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
	const BYTE *q = p + (size_t)(h > 0 ? h - 1 - y : y) * stride + 3 * x;
	return (DWORD)q[2] << 16 | q[1] << 8 | q[0];
}

/* The same of a top-down 32-bit DIB, w wide. */
static DWORD bgr32_at(const BYTE *p, LONG w, LONG x, LONG y)
{
	const BYTE *q = p + ((size_t)y * w + x) * 4;
	return (DWORD)q[2] << 16 | q[1] << 8 | q[0];
}

/*
 * Whether the dw x dh rectangle at (dx, dy) of the 32-bit target shows
 * the w x h source, stretched nearest-neighbour.
 */
static int shows(const BYTE *t, LONG tw, LONG dx, LONG dy, LONG dw, LONG dh,
		 const BYTE *src, LONG w, LONG h)
{
	for (LONG y = 0; y < dh; y++)
		for (LONG x = 0; x < dw; x++)
			if (bgr32_at(t, tw, dx + x, dy + y) !=
			    bgr24_at(src, w, h, x * w / dw, y * h / dh))
				return 0;
	return 1;
}

/* DrawDib* and ICDrawBegin/ICDraw, through msvfw32's exports. */
/* StretchDIB: nearest-pixel scaling between DIBs of one depth (qdv.dll). */
static void test_stretch_dib(void)
{
	struct module *vfw = mod_load("msvfw32.dll", NULL);
	CHECK(vfw != NULL);
	if (!vfw)
		return;
	void (WINAPI *stretch)(BITMAPINFOHEADER *, void *, int, int, int, int,
			       BITMAPINFOHEADER *, void *, int, int, int, int) =
		mod_get_proc(vfw, "StretchDIB");
	CHECK(stretch != NULL);
	if (!stretch)
		return;
	BITMAPINFOHEADER s = {
		.biSize = sizeof(s), .biWidth = 2, .biHeight = 2, .biPlanes = 1,
		.biBitCount = 24,
	};
	BITMAPINFOHEADER d = s;
	d.biWidth = 5;			/* rows of 16 bytes, padded */
	d.biHeight = 4;
	/* Rows of 8 bytes: pixels 1, 2 then 3, 4 (each pixel n is n,n,n). */
	BYTE src[16] = { 1, 1, 1, 2, 2, 2, 0, 0, 3, 3, 3, 4, 4, 4, 0, 0 };
	BYTE dst[64];

	memset(dst, 0xee, sizeof(dst));
	stretch(&d, dst, 0, 0, 4, 4, &s, src, 0, 0, 2, 2);
	static const BYTE want[4][4] = {
		{ 1, 1, 2, 2 }, { 1, 1, 2, 2 }, { 3, 3, 4, 4 }, { 3, 3, 4, 4 },
	};
	int ok = 1;
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++)
			for (int c = 0; c < 3; c++)
				ok &= dst[y * 16 + x * 3 + c] == want[y][x];
		ok &= dst[y * 16 + 12] == 0xee;	/* the fifth column untouched */
	}
	CHECK(ok);

	/* A part of the source, at an offset, clipped to the target. */
	memset(dst, 0xee, sizeof(dst));
	stretch(&d, dst, 3, 3, 4, 4, &s, src, 1, 1, 1, 1);
	CHECK(dst[3 * 16 + 9] == 4 && dst[3 * 16 + 12] == 4);
	CHECK(dst[3 * 16 + 6] == 0xee && dst[2 * 16 + 9] == 0xee);

	/* Depths that differ, and empty rectangles, draw nothing. */
	memset(dst, 0xee, sizeof(dst));
	d.biBitCount = 32;
	stretch(&d, dst, 0, 0, 4, 4, &s, src, 0, 0, 2, 2);
	d.biBitCount = 24;
	stretch(&d, dst, 0, 0, 0, 4, &s, src, 0, 0, 2, 2);
	CHECK(dst[0] == 0xee && dst[63] == 0xee);
	mod_free(vfw);
}

static void test_drawdib(void)
{
	struct module *vfw = mod_load("msvfw32.dll", NULL);
	struct module *gdi = mod_load("gdi32.dll", NULL);
	CHECK(vfw && gdi);
	if (!vfw || !gdi)
		return;
#define P(mod, name) mod_get_proc(mod, #name)
	HDRAWDIB (WINAPI *open)(void) = P(vfw, DrawDibOpen);
	BOOL (WINAPI *close)(HDRAWDIB) = P(vfw, DrawDibClose);
	BOOL (WINAPI *begin)(HDRAWDIB, HDC, int, int, BITMAPINFOHEADER *, int,
			     int, UINT) = P(vfw, DrawDibBegin);
	BOOL (WINAPI *draw)(HDRAWDIB, HDC, int, int, int, int,
			    BITMAPINFOHEADER *, void *, int, int, int, int,
			    UINT) = P(vfw, DrawDibDraw);
	BOOL (WINAPI *end)(HDRAWDIB) = P(vfw, DrawDibEnd);
	void *(WINAPI *get_buffer)(HDRAWDIB, BITMAPINFOHEADER *, DWORD,
				   DWORD) = P(vfw, DrawDibGetBuffer);
	HPALETTE (WINAPI *get_palette)(HDRAWDIB) = P(vfw, DrawDibGetPalette);
	UINT (WINAPI *realize)(HDRAWDIB, HDC, BOOL) = P(vfw, DrawDibRealize);
	BOOL (WINAPI *change_palette)(HDRAWDIB, int, int, PALETTEENTRY *) =
		P(vfw, DrawDibChangePalette);
	BOOL (WINAPI *start)(HDRAWDIB, DWORD) = P(vfw, DrawDibStart);
	BOOL (WINAPI *stop)(HDRAWDIB) = P(vfw, DrawDibStop);
	BOOL (WINAPI *timing)(HDRAWDIB, DRAWDIBTIME *) = P(vfw, DrawDibTime);
	LRESULT (WINAPI *profile)(BITMAPINFOHEADER *) =
		P(vfw, DrawDibProfileDisplay);
	DWORD (__attribute__((cdecl)) *draw_begin)(HIC, DWORD, HPALETTE, HWND,
		HDC, int, int, int, int, BITMAPINFOHEADER *, int, int, int,
		int, DWORD, DWORD) = P(vfw, ICDrawBegin);
	DWORD (__attribute__((cdecl)) *ic_draw)(HIC, DWORD, void *, void *,
		DWORD, LONG) = P(vfw, ICDraw);
	HDC (WINAPI *create_dc)(HDC) = P(gdi, CreateCompatibleDC);
	BOOL (WINAPI *delete_dc)(HDC) = P(gdi, DeleteDC);
	HBITMAP (WINAPI *create_dib)(HDC, const BITMAPINFO *, UINT, void **,
				     HANDLE, DWORD) = P(gdi, CreateDIBSection);
	HGDIOBJ (WINAPI *select)(HDC, HGDIOBJ) = P(gdi, SelectObject);
	BOOL (WINAPI *delete_obj)(HGDIOBJ) = P(gdi, DeleteObject);
#undef P
	CHECK(open && close && begin && draw && end && get_buffer &&
	      get_palette && realize && change_palette && start && stop &&
	      timing && profile && draw_begin && ic_draw && create_dc &&
	      delete_dc && create_dib && select && delete_obj);
	if (!open || !close || !begin || !draw || !end || !get_buffer ||
	    !get_palette || !realize || !change_palette || !start || !stop ||
	    !timing || !profile || !draw_begin || !ic_draw || !create_dc ||
	    !delete_dc || !create_dib || !select || !delete_obj)
		return;

	/* The target: a 32-bit top-down DIB section in a memory DC. */
	const LONG tw = 80, th = 60, w = 21, h = 13;
	BITMAPINFOHEADER tfmt = rgb(tw, -th, 32);
	BYTE *t = NULL;
	HDC hdc = create_dc(NULL);
	HBITMAP tbm = create_dib(hdc, (BITMAPINFO *)&tfmt, DIB_RGB_COLORS,
				 (void **)&t, NULL, 0);
	CHECK(hdc && tbm && t);
	if (!hdc || !tbm || !t)
		return;
	HGDIOBJ old = select(hdc, tbm);

	BITMAPINFOHEADER in = rgb(w, h, 24);
	BYTE *src = malloc(in.biSizeImage), *src2 = malloc(in.biSizeImage);
	picture(src, w, h, 0);
	picture(src2, w, h, 9);

	HDRAWDIB hdd = open();
	CHECK(hdd != NULL);
	CHECK(profile(&in) & PD_CAN_DRAW_DIB);
	CHECK(start(hdd, 1000) && stop(hdd));
	DRAWDIBTIME tm;
	CHECK(!timing(hdd, &tm));
	CHECK(!get_buffer(hdd, NULL, 0, 0));	/* not begun */
	CHECK(!realize(hdd, hdc, FALSE));

	/* Uncompressed, 1:1, without DrawDibBegin; -1 sizes are the DIB's. */
	CHECK(draw(hdd, hdc, 3, 4, -1, -1, &in, src, 0, 0, -1, -1, 0));
	CHECK(shows(t, tw, 3, 4, w, h, src, w, h));
	CHECK(bgr32_at(t, tw, 2, 4) == 0 && bgr32_at(t, tw, 3 + w, 4) == 0);
	BITMAPINFOHEADER buf_fmt;
	memset(&buf_fmt, 0xcc, sizeof(buf_fmt));
	BYTE *buf = get_buffer(hdd, &buf_fmt, sizeof(buf_fmt), 0);
	CHECK(buf && !memcmp(buf, src, in.biSizeImage));
	CHECK(buf_fmt.biBitCount == 24 && buf_fmt.biWidth == w);

	/* The bits after a packed DIB's header when none are given. */
	BYTE *packed = malloc(sizeof(in) + in.biSizeImage);
	memcpy(packed, &in, sizeof(in));
	memcpy(packed + sizeof(in), src2, in.biSizeImage);
	CHECK(draw(hdd, hdc, 3, 4, w, h, (BITMAPINFOHEADER *)packed, NULL, 0,
		   0, w, h, 0));
	CHECK(shows(t, tw, 3, 4, w, h, src2, w, h));
	free(packed);

	/* Decompressed without drawing, then DrawDibUpdate shows it. */
	CHECK(draw(hdd, hdc, 3, 4, w, h, &in, src, 0, 0, w, h, DDF_DONTDRAW));
	CHECK(shows(t, tw, 3, 4, w, h, src2, w, h));
	CHECK(draw(hdd, hdc, 3, 4, 0, 0, NULL, NULL, 0, 0, 0, 0, DDF_UPDATE));
	CHECK(shows(t, tw, 3, 4, w, h, src, w, h));
	/* An update onto another DC, which doesn't begin again. */
	BYTE *t2 = NULL;
	HDC hdc2 = create_dc(NULL);
	HBITMAP tbm2 = create_dib(hdc2, (BITMAPINFO *)&tfmt, DIB_RGB_COLORS,
				  (void **)&t2, NULL, 0);
	HGDIOBJ old2 = select(hdc2, tbm2);
	CHECK(t2 && draw(hdd, hdc2, 7, 8, 0, 0, NULL, NULL, 0, 0, 0, 0,
			 DDF_UPDATE));
	CHECK(t2 && shows(t2, tw, 7, 8, w, h, src, w, h));
	CHECK(draw(hdd, hdc, 3, 4, 0, 0, NULL, NULL, 0, 0, 0, 0, DDF_UPDATE));
	select(hdc2, old2);
	delete_obj(tbm2);
	delete_dc(hdc2);

	/* A top-down source. */
	BITMAPINFOHEADER td = rgb(w, -h, 24);
	BYTE *flip = malloc(in.biSizeImage);
	size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
	for (LONG y = 0; y < h; y++)
		memcpy(flip + y * stride, src2 + (h - 1 - y) * stride, stride);
	CHECK(draw(hdd, hdc, 40, 30, w, h, &td, flip, 0, 0, w, h, 0));
	CHECK(shows(t, tw, 40, 30, w, h, src2, w, h));
	free(flip);

	/* Compressed: through a TST0 decompressor it opens, stretched 2x. */
	LONG csize = 0;
	HANDLE c = ic_image_compress(NULL, 0, (BITMAPINFO *)&in, src, NULL,
				     ICQUALITY_DEFAULT, &csize);
	CHECK(c != NULL);
	BITMAPINFOHEADER *cfmt = c ? global_lock(c) : NULL;
	if (cfmt) {
		void *cbits = (BYTE *)cfmt + vfw_format_size(cfmt);
		memset(t, 0, (size_t)tw * th * 4);
		CHECK(begin(hdd, hdc, 2 * w, 2 * h, cfmt, w, h, 0));
		CHECK_EQ(codec_opens(), 1);
		CHECK(draw(hdd, hdc, 1, 2, 2 * w, 2 * h, cfmt, cbits, 0, 0, w,
			   h, 0));
		CHECK(shows(t, tw, 1, 2, 2 * w, 2 * h, src, w, h));
		buf = get_buffer(hdd, &buf_fmt, sizeof(buf_fmt), 0);
		CHECK(buf && buf_fmt.biCompression == BI_RGB &&
		      !memcmp(buf, src, in.biSizeImage));
		/* Back to uncompressed: the decompressor is closed. */
		CHECK(draw(hdd, hdc, 1, 2, w, h, &in, src2, 0, 0, w, h, 0));
		CHECK_EQ(codec_opens(), 0);
		CHECK(begin(hdd, hdc, w, h, cfmt, w, h, 0));
		CHECK(end(hdd));
		CHECK_EQ(codec_opens(), 0);

		/* A format nothing decompresses. */
		BITMAPINFOHEADER mjpg = *cfmt;
		mjpg.biCompression = FCC('M', 'J', 'P', 'G');
		CHECK(!begin(hdd, hdc, w, h, &mjpg, w, h, 0));
		CHECK(!draw(hdd, hdc, 0, 0, w, h, &mjpg, cbits, 0, 0, w, h, 0));
		CHECK(!get_buffer(hdd, NULL, 0, 0));

		/* A codec drawing for itself: ICDrawBegin, ICDraw. */
		HIC hic = ic_open(ICTYPE_VIDEO, TST0, ICMODE_DRAW);
		CHECK(hic != NULL);
		CHECK_EQ((LONG)draw_begin(hic, ICDRAW_QUERY, NULL, NULL, NULL,
					  0, 0, w, h, cfmt, 0, 0, w, h, 25, 1),
			 ICERR_OK);
		CHECK_EQ((LONG)draw_begin(hic, ICDRAW_QUERY, NULL, NULL, NULL,
					  0, 0, w, h, &mjpg, 0, 0, w, h, 25, 1),
			 ICERR_BADFORMAT);
		memset(t, 0, (size_t)tw * th * 4);
		CHECK_EQ((LONG)draw_begin(hic, ICDRAW_HDC, NULL, NULL, hdc, 5,
					  6, 3 * w, 3 * h, cfmt, 0, 0, w, h,
					  25, 1), ICERR_OK);
		CHECK_EQ((LONG)ic_draw(hic, 0, cfmt, cbits, (DWORD)csize, 0),
			 ICERR_OK);
		CHECK(shows(t, tw, 5, 6, 3 * w, 3 * h, src, w, h));
		memset(t, 0, (size_t)tw * th * 4);
		CHECK_EQ((LONG)ic_draw(hic, ICDRAW_UPDATE, cfmt, NULL, 0, 0),
			 ICERR_OK);
		CHECK(shows(t, tw, 5, 6, 3 * w, 3 * h, src, w, h));
		CHECK_EQ(ic_send(hic, ICM_DRAW_END, 0, 0), ICERR_OK);
		ic_close(hic);
		global_unlock(c);
	}
	if (c)
		global_free(c);

	/*
	 * A keyframe then a delta, each with its own biSizeImage as a player
	 * sets it: the same stream, so the decompressor carries on.
	 */
	COMPVARS cv;
	memset(&cv, 0, sizeof(cv));
	cv.cbSize = sizeof(cv);
	cv.hic = ic_open(ICTYPE_VIDEO, TST0, ICMODE_COMPRESS);
	cv.lKey = 15;
	cv.lQ = ICQUALITY_DEFAULT;
	CHECK(ic_seq_start(&cv, (BITMAPINFO *)&in));
	if (cv.lpbiOut) {
		BITMAPINFOHEADER sf = cv.lpbiOut->bmiHeader;
		BOOL key0 = FALSE, key1 = TRUE;
		LONG n0 = 0, n1 = 0;
		BYTE *f = ic_seq_frame(&cv, 0, src, &key0, &n0);
		BYTE *k0 = f ? malloc(n0) : NULL;
		if (k0)
			memcpy(k0, f, n0);
		BYTE *k1 = ic_seq_frame(&cv, 0, src2, &key1, &n1);
		CHECK(k0 && k1 && key0 && !key1 && n0 != n1);
		if (k0 && k1) {
			sf.biSizeImage = (DWORD)n0;
			CHECK(draw(hdd, hdc, 0, 0, w, h, &sf, k0, 0, 0, w, h, 0));
			CHECK(shows(t, tw, 0, 0, w, h, src, w, h));
			sf.biSizeImage = (DWORD)n1;
			CHECK(draw(hdd, hdc, 0, 0, w, h, &sf, k1, 0, 0, w, h,
				   DDF_NOTKEYFRAME));
			CHECK(shows(t, tw, 0, 0, w, h, src2, w, h));
		}
		free(k0);
		ic_seq_end(&cv);
	}
	ic_compressor_free(&cv);

	/* Palettised: two colours, then colour 0 changed. */
	struct {
		BITMAPINFOHEADER h;
		RGBQUAD pal[2];
	} p8 = { rgb(4, 2, 8), { { 0, 0, 0xff, 0 }, { 0xff, 0, 0, 0 } } };
	p8.h.biClrUsed = 2;
	BYTE idx[8] = { 0, 1, 0, 1, 1, 1, 1, 1 };	/* bottom row first */
	memset(t, 0, (size_t)tw * th * 4);
	CHECK(draw(hdd, hdc, 0, 0, 4, 2, &p8.h, idx, 0, 0, 4, 2, 0));
	CHECK_EQ(bgr32_at(t, tw, 0, 1), 0xff0000);
	CHECK_EQ(bgr32_at(t, tw, 1, 1), 0x0000ff);
	PALETTEENTRY green = { 0, 0xff, 0, 0 };
	CHECK(change_palette(hdd, 0, 1, &green));
	CHECK(draw(hdd, hdc, 0, 0, 4, 2, &p8.h, idx, 0, 0, 4, 2,
		   DDF_SAME_HDC | DDF_SAME_DRAW));
	CHECK_EQ(bgr32_at(t, tw, 0, 1), 0x00ff00);
	CHECK_EQ(bgr32_at(t, tw, 1, 1), 0x0000ff);
	/* A new colour table is a new format, though the header is the same. */
	p8.pal[1] = (RGBQUAD){ 0, 0xff, 0xff, 0 };
	CHECK(draw(hdd, hdc, 0, 0, 4, 2, &p8.h, idx, 0, 0, 4, 2, 0));
	CHECK_EQ(bgr32_at(t, tw, 0, 1), 0xff0000);
	CHECK_EQ(bgr32_at(t, tw, 1, 1), 0xffff00);

	/* A V5 header holds its masks: the bits follow it directly. */
	BYTE v5[124 + 8 * 4];
	memset(v5, 0, sizeof(v5));
	BITMAPINFOHEADER *vh = (BITMAPINFOHEADER *)v5;
	*vh = rgb(4, -2, 32);
	vh->biSize = 124;
	vh->biCompression = BI_BITFIELDS;
	const DWORD masks[3] = { 0xff0000, 0xff00, 0xff };
	memcpy(v5 + 40, masks, sizeof(masks));
	for (DWORD i = 0; i < 8; i++) {
		DWORD v = 0x010203 * (i + 1);
		memcpy(v5 + 124 + 4 * i, &v, 4);
	}
	memset(t, 0, (size_t)tw * th * 4);
	CHECK(draw(hdd, hdc, 0, 0, 4, 2, vh, NULL, 0, 0, 4, 2, 0));
	CHECK_EQ(bgr32_at(t, tw, 0, 0), 0x010203);
	CHECK_EQ(bgr32_at(t, tw, 3, 1), 0x081018);
	/* A halftone palette, which the true-colour display doesn't need. */
	CHECK_EQ(realize(hdd, hdc, FALSE), 0);
	CHECK(get_palette(hdd) != NULL);

	CHECK(close(hdd));
	CHECK(!close(hdd));
	CHECK(!draw(hdd, hdc, 0, 0, w, h, &in, src, 0, 0, w, h, 0));
	CHECK(!begin(hdd, hdc, w, h, &in, w, h, 0));
	CHECK_EQ(codec_opens(), 0);

	free(src);
	free(src2);
	select(hdc, old);
	delete_obj(tbm);
	delete_dc(hdc);
	mod_free(gdi);
	mod_free(vfw);
}

/* ICCompressorChoose without its dialog, through msvfw32's exports. */
static void test_choose(void)
{
	struct module *vfw = mod_load("msvfw32.dll", NULL);
	CHECK(vfw != NULL);
	if (!vfw)
		return;
	BOOL (WINAPI *choose)(HWND, UINT, LPVOID, LPVOID, COMPVARS *, LPSTR) =
		mod_get_proc(vfw, "ICCompressorChoose");
	void (WINAPI *cfree)(COMPVARS *) = mod_get_proc(vfw, "ICCompressorFree");
	BOOL (WINAPI *start)(COMPVARS *, BITMAPINFO *) =
		mod_get_proc(vfw, "ICSeqCompressFrameStart");
	LPVOID (WINAPI *frame)(COMPVARS *, UINT, LPVOID, BOOL *, LONG *) =
		mod_get_proc(vfw, "ICSeqCompressFrame");
	void (WINAPI *end)(COMPVARS *) = mod_get_proc(vfw, "ICSeqCompressFrameEnd");
	CHECK(choose && cfree && start && frame && end);
	if (!choose || !cfree || !start || !frame || !end) {
		mod_free(vfw);
		return;
	}
	const LONG w = 33, h = 17;
	BITMAPINFOHEADER in = rgb(w, h, 24), yuy2 = rgb(w, h, 16);
	yuy2.biCompression = FCC('Y', 'U', 'Y', '2');
	char title[] = "Video Compression";
	COMPVARS cv;

	memset(&cv, 0, sizeof(cv));
	CHECK(!choose(NULL, 0, &in, NULL, &cv, title));	/* no cbSize */

	/* The initial selection of a fresh COMPVARS: Full Frames. */
	unsetenv("VFW_COMPRESSOR");
	cv.cbSize = sizeof(cv);
	cv.fccHandler = TST0;	/* ignored without ICMF_COMPVARS_VALID */
	CHECK(choose(NULL, 0, &in, NULL, &cv, title));
	CHECK(cv.dwFlags & ICMF_COMPVARS_VALID);
	CHECK(!cv.hic && cv.fccType == ICTYPE_VIDEO && cv.fccHandler == comptypeDIB);
	CHECK(cv.lQ == ICQUALITY_DEFAULT && cv.lKey == -1 && cv.lDataRate == 300);
	cfree(&cv);

	/* Chosen by VFW_COMPRESSOR: opened, with the codec's defaults. */
	setenv("VFW_COMPRESSOR", "vidc.tst0", 1);
	CHECK(choose(NULL, 0, &in, NULL, &cv, title));
	CHECK(cv.hic && ic_valid(cv.hic) && cv.fccHandler == TST0);
	CHECK(cv.lQ == 7500 && cv.lKey == 15);
	CHECK_EQ(codec_opens(), 1);

	/* Choosing again keeps the choice and its state; one codec open. */
	unsetenv("VFW_COMPRESSOR");
	DWORD st[2] = { 1234, 3 }, got[2] = { 0, 0 };
	cv.lpState = st;
	cv.cbState = sizeof(st);
	CHECK(choose(NULL, 0, &in, NULL, &cv, title));
	CHECK(cv.hic && cv.fccHandler == TST0);
	CHECK_EQ(codec_opens(), 1);
	CHECK_EQ(ic_send(cv.hic, ICM_GETSTATE, (DWORD_PTR)got, sizeof(got)), 8);
	CHECK(got[0] == 1234 && got[1] == 3);
	cv.lpState = NULL;
	cv.cbState = 0;

	/* The choice works for a sequence. */
	BYTE *src = malloc(in.biSizeImage);
	picture(src, w, h, 1);
	BOOL key = FALSE;
	LONG size = 0;
	CHECK(start(&cv, (BITMAPINFO *)&in));
	CHECK(frame(&cv, 0, src, &key, &size) && key && size > 0);
	end(&cv);
	free(src);

	/* A compressor that won't take the input isn't listed. */
	setenv("VFW_COMPRESSOR", "TST0", 1);
	HIC before = cv.hic;
	CHECK(!choose(NULL, 0, &yuy2, NULL, &cv, title));
	CHECK(cv.hic == before && ic_valid(before));	/* unchanged */
	CHECK(choose(NULL, ICMF_CHOOSE_ALLCOMPRESSORS, &yuy2, NULL, &cv, title));
	CHECK(cv.hic && cv.fccHandler == TST0);
	CHECK_EQ(codec_opens(), 1);
	unsetenv("VFW_COMPRESSOR");
	CHECK(choose(NULL, 0, &yuy2, NULL, &cv, title));
	CHECK(!cv.hic && cv.fccHandler == comptypeDIB);	/* fell back */
	CHECK_EQ(codec_opens(), 0);

	/* Cancelled, unknown, or not a FOURCC: FALSE, cv untouched. */
	COMPVARS saved = cv;
	setenv("VFW_COMPRESSOR", "Cancel", 1);
	CHECK(!choose(NULL, 0, &in, NULL, &cv, title));
	setenv("VFW_COMPRESSOR", "ZZZZ", 1);
	CHECK(!choose(NULL, 0, &in, NULL, &cv, title));
	setenv("VFW_COMPRESSOR", "much too long", 1);
	CHECK(!choose(NULL, 0, &in, NULL, &cv, NULL));
	CHECK(!memcmp(&saved, &cv, sizeof(cv)));
	setenv("VFW_COMPRESSOR", "TST0", 1);
	CHECK(choose(NULL, 0, &in, NULL, &cv, title));
	/* Another compressor's state doesn't follow the choice. */
	cv.lpState = st;
	cv.cbState = sizeof(st);
	setenv("VFW_COMPRESSOR", "uncompressed", 1);
	CHECK(choose(NULL, 0, &in, NULL, &cv, title));
	CHECK(!cv.hic && cv.fccHandler == comptypeDIB);
	CHECK(!cv.lpState && !cv.cbState);
	CHECK_EQ(codec_opens(), 0);
	cv.lpState = st;
	cv.cbState = sizeof(st);
	setenv("VFW_COMPRESSOR", "TST0", 1);
	CHECK(choose(NULL, 0, &in, NULL, &cv, title));
	CHECK(cv.hic && !cv.lpState && !cv.cbState);
	unsetenv("VFW_COMPRESSOR");

	cfree(&cv);
	mod_free(vfw);
}

/* Runs a command with stdout captured; returns its status and output. */
static int run_captured(int (*cmd)(int, char **), int argc, char **argv,
			char *out, size_t size)
{
	char path[4200];
	snprintf(path, sizeof(path), "%s/stdout.txt", root);
	fflush(stdout);
	int saved = dup(1);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	dup2(fd, 1);
	close(fd);
	int status = cmd(argc, argv);
	fflush(stdout);
	dup2(saved, 1);
	close(saved);

	FILE *f = fopen(path, "r");
	size_t n = f ? fread(out, 1, size - 1, f) : 0;
	out[n] = '\0';
	if (f)
		fclose(f);
	unlink(path);
	return status;
}

static void test_probe_cli(const char *codec)
{
	static char out[65536];
	char *args[4];

	args[0] = "probe";
	args[1] = (char *)codec;
	args[2] = avi_path;
	CHECK_EQ(run_captured(cmd_probe, 3, args, out, sizeof(out)), 0);
	CHECK(strstr(out, "handler 'TST0'"));
	CHECK(strstr(out, "description: seance test codec"));
	CHECK(strstr(out, "flags: 0x0034 TEMPORAL DRAW FASTTEMPORALC"));
	CHECK(strstr(out, "state: 8 bytes"));
	CHECK(strstr(out, "Decompression of"));
	CHECK(strstr(out, "input: 33x17, 24 bits, 'TST0'"));
	CHECK(strstr(out, "outputs accepted: BGR32 BGR24\n"));
	CHECK(strstr(out, "negotiated output: 33x17, 24 bits, BI_RGB"));
	CHECK(strstr(out, "BGR24 input: accepted"));
	CHECK(!strstr(out, "YUY2 input"));

	/* By FOURCC, through vfw.ini. */
	args[1] = "TST0";
	CHECK_EQ(run_captured(cmd_probe, 2, args, out, sizeof(out)), 0);
	CHECK(strstr(out, "Decompression of 'TST0' at 320x240"));

	args[0] = "list";
	CHECK_EQ(run_captured(cmd_list, 1, args, out, sizeof(out)), 0);
	CHECK(strstr(out, "TST0: seance test codec"));
	CHECK(strstr(out, "not a driver") == NULL);
	CHECK(strstr(out, "vidc.NONE  nosuch.dll\n      can't open"));

	/* Failures. */
	args[0] = "probe";
	args[1] = "ZZZZ";
	CHECK_EQ(run_captured(cmd_probe, 2, args, out, sizeof(out)), 1);
	args[1] = "XXXX";
	CHECK_EQ(run_captured(cmd_probe, 2, args, out, sizeof(out)), 1);
	args[1] = (char *)codec;
	args[2] = "--size";
	args[3] = "0x5";
	CHECK_EQ(run_captured(cmd_probe, 4, args, out, sizeof(out)), 2);
	CHECK_EQ(drv_open_count(), 0);
	CHECK_EQ(codec_opens(), 0);
}

static char seq_avi[4200];

/* A codec that answers the format size query with 0. */
static LRESULT WINAPI zero_size_proc(DWORD_PTR id, HDRVR h, UINT msg,
				     LPARAM l1, LPARAM l2)
{
	switch (msg) {
	case DRV_LOAD: case DRV_ENABLE: case DRV_DISABLE: case DRV_FREE:
	case DRV_CLOSE:
		return 1;
	case DRV_OPEN:
		return 1;
	case ICM_COMPRESS_GET_FORMAT:
		if (!l2)
			return 0;
		*(BITMAPINFOHEADER *)l2 = *(const BITMAPINFOHEADER *)l1;
		((BITMAPINFOHEADER *)l2)->biCompression = FCC('Z', 'E', 'R', 'O');
		return ICERR_OK;
	}
	return ICERR_UNSUPPORTED;
}

static void test_format_size(void)
{
	HIC hic = ic_open_function(ICTYPE_VIDEO, FCC('Z', 'E', 'R', 'O'),
				   ICMODE_COMPRESS, (FARPROC)(void *)zero_size_proc);
	CHECK(hic != NULL);
	if (!hic)
		return;
	BITMAPINFOHEADER in = rgb(8, 8, 24);
	size_t size = 0;
	LRESULT err;
	BITMAPINFOHEADER *out = ic_get_format(hic, ICM_COMPRESS_GET_FORMAT, &in,
					      &size, &err);
	CHECK(out && out->biCompression == FCC('Z', 'E', 'R', 'O'));
	/* Just the header: not the room it was given to fill in. */
	CHECK_EQ(size, sizeof(BITMAPINFOHEADER));
	free(out);
	/* A codec's own answer counts when it is more than the header. */
	out = ic_get_format(hic, ICM_COMPRESS_GET_FORMAT, &in, NULL, &err);
	CHECK(out != NULL);
	free(out);
	ic_close(hic);
}

/* ICSeqCompressFrame*: keyframes, forced keyframes, and the output. */
static void test_seq(void)
{
	const LONG w = 33, h = 17;
	BITMAPINFOHEADER in = rgb(w, h, 24);
	BYTE *src = malloc(in.biSizeImage), *dst = malloc(in.biSizeImage);
	COMPVARS cv;
	BOOL key = 7;
	LONG size = 0;

	memset(&cv, 0, sizeof(cv));
	picture(src, w, h, 0);
	CHECK(!ic_seq_frame(&cv, 0, src, &key, &size));
	CHECK(!ic_seq_start(&cv, (BITMAPINFO *)&in));	/* no cbSize, no codec */
	cv.cbSize = sizeof(cv);
	CHECK(!ic_seq_start(&cv, (BITMAPINFO *)&in));
	cv.hic = ic_open(ICTYPE_VIDEO, TST0, ICMODE_COMPRESS);
	CHECK(cv.hic != NULL);
	if (!cv.hic)
		goto done;
	HIC hic = cv.hic;
	BITMAPINFOHEADER yuy2 = rgb(w, h, 16);
	yuy2.biCompression = FCC('Y', 'U', 'Y', '2');
	CHECK(!ic_seq_start(&cv, (BITMAPINFO *)&yuy2));
	CHECK(strstr(vfw_error(), "ICERR_BADFORMAT") != NULL);
	CHECK(!cv.lpbiIn && !cv.lpbiOut);

	cv.lKey = 3;
	cv.lQ = ICQUALITY_DEFAULT;
	CHECK(ic_seq_start(&cv, (BITMAPINFO *)&in));
	CHECK(cv.lpbiIn && cv.lpbiOut && cv.lpBitsOut && !cv.lpBitsPrev);
	CHECK_EQ(cv.lpbiOut->bmiHeader.biCompression, TST0);
	CHECK_EQ(ic_seq_format_size(&cv), sizeof(BITMAPINFOHEADER));

	/* Decompress each frame again as it comes. */
	BITMAPINFOHEADER cf = cv.lpbiOut->bmiHeader, of = rgb(w, h, 24);
	HIC dec = ic_open(ICTYPE_VIDEO, TST0, ICMODE_DECOMPRESS);
	CHECK(dec && ic_send(dec, ICM_DECOMPRESS_BEGIN, (DWORD_PTR)&cf,
			     (DWORD_PTR)&of) == ICERR_OK);
	snprintf(seq_avi, sizeof(seq_avi), "%s/seq.avi", root);
	struct avi_writer *aw = avi_writer_open(seq_avi, &cf, sizeof(cf), TST0,
						25, 1, NULL);
	CHECK(aw != NULL);
	/*
	 * Keyframes every 3rd frame, and frame 4 is asked to be one. The codec
	 * also counts from its own start (ICM_COMPRESS_FRAMES_INFO gave it the
	 * rate), so it makes frame 6 a keyframe too, and 7 then isn't.
	 */
	static const int want[8] = { 1, 0, 0, 1, 1, 0, 1, 0 };
	for (int i = 0; i < 8; i++) {
		picture(src, w, h, i);
		BYTE *data = ic_seq_frame(&cv, i == 4 ? ICCOMPRESS_KEYFRAME : 0, src,
					  &key, &size);
		CHECK(data && size > 4 && size <= (LONG)in.biSizeImage + 4 + 64);
		if (!data)
			break;
		CHECK_EQ(!!key, want[i]);
		CHECK_EQ(cv.lFrame, i + 1);
		cf.biSizeImage = (DWORD)size;
		memset(dst, 0, in.biSizeImage);
		CHECK_EQ(ic_decompress(dec, key ? 0 : ICDECOMPRESS_NOTKEYFRAME, &cf,
				       data, &of, dst), ICERR_OK);
		CHECK(!memcmp(dst, src, in.biSizeImage));
		/* For --keep-going: frame 2 is damaged in the file. */
		if (i == 2)
			data[0] = 'X';
		if (aw)
			avi_writer_add(aw, data, (DWORD)size, key);
	}
	CHECK(aw && avi_writer_close(aw) == 0);
	if (dec) {
		ic_send(dec, ICM_DECOMPRESS_END, 0, 0);
		ic_close(dec);
	}

	ic_seq_end(&cv);
	CHECK(!cv.lpbiIn && !cv.lpbiOut && !cv.lpBitsOut);
	CHECK(!ic_seq_frame(&cv, 0, src, &key, &size));
	CHECK_EQ(ic_seq_format_size(&cv), 0);
	ic_seq_end(&cv);	/* twice is harmless */
	ic_compressor_free(&cv);
	CHECK(!cv.hic && !ic_valid(hic));

	/* The same functions are what msvfw32.dll exports. */
	struct module *mv = mod_load("MSVFW32.DLL", NULL);
	CHECK(mv && mod_get_proc(mv, "ICSeqCompressFrame") == (void *)ic_seq_frame);
	CHECK(mv && mod_get_proc(mv, "ICSeqCompressFrameStart") == (void *)ic_seq_start);
	CHECK(mv && mod_get_proc(mv, "ICCompressorFree") == (void *)ic_compressor_free);
	mod_free(mv);
done:
	free(src);
	free(dst);
}

static void *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	void *p = malloc(n > 0 ? (size_t)n : 1);
	*len = p && fread(p, 1, (size_t)n, f) == (size_t)n ? (size_t)n : 0;
	fclose(f);
	return p;
}

static int same_files(const char *a, const char *b)
{
	size_t na, nb;
	void *pa = slurp(a, &na), *pb = slurp(b, &nb);
	int same = pa && pb && na == nb && na && !memcmp(pa, pb, na);
	free(pa);
	free(pb);
	return same;
}

/* Frame n of a W x H I420 clip. */
static void clip_frame(BYTE *p, int w, int h, int n)
{
	size_t luma = (size_t)w * h, size = luma + 2 * ((w + 1) / 2) * ((h + 1) / 2);
	for (size_t i = 0; i < size; i++)
		p[i] = i < luma ? (BYTE)(i % w * 5 + i / w * 3 + n * 4) : (BYTE)(100 + n + i % 7);
}

static int run(int (*cmd)(int, char **), char **argv)
{
	static char out[65536];
	int argc = 0;
	while (argv[argc])
		argc++;
	return run_captured(cmd, argc, argv, out, sizeof(out));
}

#define P(name) (snprintf(paths[np], sizeof(paths[0]), "%s/%s", root, name), paths[np++])

/* encode and decode: lossless round trips and the options. */
static void test_codec_cli(const char *codec)
{
	static char paths[24][4200];
	int np = 0;
	enum { W = 33, H = 17, N = 10 };
	size_t fsize = (size_t)W * H + 2 * ((W + 1) / 2) * ((H + 1) / 2);
	BYTE frame[W * H * 4];
	char *c = (char *)codec;

	/* A Y4M clip with the header the Y4M writer uses, and the same frames raw. */
	const char *y4m = P("in.y4m"), *yuv = P("in.yuv");
	FILE *f = fopen(y4m, "wb"), *g = fopen(yuv, "wb");
	fprintf(f, "YUV4MPEG2 W%d H%d F15:1 Ip A1:1 C420jpeg\n", W, H);
	for (int n = 0; n < N; n++) {
		clip_frame(frame, W, H, n);
		fprintf(f, "FRAME\n");
		fwrite(frame, 1, fsize, f);
		fwrite(frame, 1, fsize, g);
	}
	fclose(f);
	fclose(g);

	const char *avi = P("t.avi"), *out = P("out.y4m");
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)y4m, "-o",
					     (char *)avi, "--keyint", "4", NULL }), 0);
	struct avi_file *a = avi_open(avi);
	CHECK(a != NULL);
	if (a) {
		const struct avi_stream *s = &a->streams[0];
		const BITMAPINFOHEADER *b = avi_stream_bih(s);
		CHECK_EQ(s->frames, N);
		CHECK(b && b->biCompression == TST0 && b->biBitCount == 12);
		CHECK_EQ(s->header.fccHandler, TST0);
		CHECK(s->header.dwRate == 15 && s->header.dwScale == 1);
		for (size_t i = 0; i < s->nchunks; i++)
			CHECK_EQ(!!(s->chunks[i].flags & AVIIF_KEYFRAME), i % 4 == 0);
		avi_close(a);
	}
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)avi, "-o",
					     (char *)out, NULL }), 0);
	CHECK(same_files(y4m, out));

	/* From frame 5: decoding starts at the keyframe before it. */
	const char *part = P("part.yuv");
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)avi, "-o",
					     (char *)part, "--start", "5", "--frames",
					     "3", NULL }), 0);
	size_t n, m;
	BYTE *pp = slurp(part, &n), *all = slurp(yuv, &m);
	CHECK(pp && all && n == 3 * fsize && !memcmp(pp, all + 5 * fsize, n));
	free(pp);

	/* "auto" finds TST0 through vfw.ini; raw I420 input gives the same. */
	const char *autoyuv = P("auto.yuv"), *avi2 = P("raw.avi");
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", "auto", (char *)avi, "-o",
					     (char *)autoyuv, NULL }), 0);
	CHECK(same_files(autoyuv, yuv));
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", "TST0", (char *)yuv, "-o",
					     (char *)avi2, "--input-format", "I420",
					     "--size", "33x17", "--rate", "29.97",
					     NULL }), 0);
	a = avi_open(avi2);
	CHECK(a && a->streams[0].frames == N);
	CHECK(a && a->streams[0].header.dwRate == 30000 &&
	      a->streams[0].header.dwScale == 1001);
	/* The codec's default keyframe rate, 15: only frame 0. */
	CHECK(a && a->streams[0].chunks[0].flags & AVIIF_KEYFRAME);
	CHECK(a && !(a->streams[0].chunks[9].flags & AVIIF_KEYFRAME));
	avi_close(a);

	/* BGR24 raw frames (top-down, unpadded rows) and back, exactly. */
	const char *bgr = P("in.bgr"), *bgr_avi = P("bgr.avi"), *bgr_out = P("out.bgr");
	f = fopen(bgr, "wb");
	for (int i = 0; i < 3 * W * H * 3; i++)
		fputc((i * 31 + i / 99) & 0xff, f);
	fclose(f);
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)bgr, "-o",
					     (char *)bgr_avi, "--input-format", "BGR24",
					     "--size", "33x17", NULL }), 0);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)bgr_avi, "-o",
					     (char *)bgr_out, NULL }), 0);
	CHECK(same_files(bgr, bgr_out));

	/* Converting to a format the codec takes, and choosing its output. */
	const char *conv = P("conv.avi"), *conv_out = P("conv.yuv");
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)y4m, "-o",
					     (char *)conv, "--codec-input", "BGR32",
					     "--frames", "3", "--start", "2", NULL }), 0);
	a = avi_open(conv);
	CHECK(a && a->streams[0].frames == 3 &&
	      avi_stream_bih(&a->streams[0])->biBitCount == 32);
	avi_close(a);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)conv, "-o",
					     (char *)conv_out, "--output-format",
					     "BGR32", NULL }), 0);
	pp = slurp(conv_out, &n);
	CHECK(pp && n == 3 * fsize);
	if (pp && all && n == 3 * fsize) {
		/*
		 * I420 -> RGB -> I420 is close, not exact: on average within a
		 * level or two, more where the made-up chroma is out of gamut.
		 */
		long total = 0;
		for (size_t i = 0; i < n; i++)
			total += abs(pp[i] - all[2 * fsize + i]);
		CHECK(total <= (long)n * 2);
	}
	free(pp);
	free(all);

	/* State: the options reach the codec, and --save-state keeps them. */
	const char *st = P("state.bin"), *q = P("q.avi");
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)y4m, "-o",
					     (char *)q, "--quality", "1234", "--keyint",
					     "2", "--save-state", (char *)st, NULL }), 0);
	DWORD *state = (DWORD *)slurp(st, &n);
	CHECK(state && n == 8 && state[0] == 1234 && state[1] == 2);
	free(state);
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)y4m, "-o",
					     (char *)q, "--state", (char *)st, NULL }), 0);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)q, "-o",
					     (char *)P("q.yuv"), "--state", (char *)st,
					     NULL }), 0);
	CHECK_EQ(codec_quality(), 1234);
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)y4m, "-o",
					     (char *)q, "--quality", "321", NULL }), 0);
	CHECK_EQ(codec_quality(), 321);
	/* Without --quality, the codec's current quality, not -1. */
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)y4m, "-o",
					     (char *)q, NULL }), 0);
	CHECK_EQ(codec_quality(), 7500);

	/* A state the codec refuses stops both commands. */
	const char *bad = P("bad.bin");
	f = fopen(bad, "wb");
	fwrite("xyz", 1, 3, f);
	fclose(f);
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)y4m, "-o",
					     (char *)q, "--state", (char *)bad, NULL }), 1);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)avi, "-o",
					     (char *)P("q.yuv"), "--state", (char *)bad,
					     NULL }), 1);

	/* A damaged frame: an error, unless --keep-going repeats the last one. */
	const char *kg = P("kg.bgr");
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, seq_avi, "-o",
					     (char *)kg, NULL }), 1);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, seq_avi, "-o",
					     (char *)kg, "--keep-going", NULL }), 0);
	pp = slurp(kg, &n);
	CHECK(pp && n == 8 * (size_t)W * H * 3);
	if (pp && n == 8 * (size_t)W * H * 3)
		CHECK(!memcmp(pp + 1 * W * H * 3, pp + 2 * W * H * 3, (size_t)W * H * 3));
	free(pp);

	/* Mistakes. */
	const char *x = P("x.avi");
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)avi, "-o",
					     (char *)P("x.yuv"), "--output-format",
					     "YUY2", NULL }), 1);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)avi, "-o",
					     (char *)P("x.yuv"), "--output-format",
					     "NOPE", NULL }), 1);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)y4m, "-o",
					     (char *)P("x.yuv"), NULL }), 1);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", "XXXX", (char *)avi, "-o",
					     (char *)P("x.yuv"), NULL }), 1);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)avi, NULL }), 2);
	CHECK_EQ(run(cmd_decode, (char *[]){ "decode", c, (char *)avi, "-o",
					     "x.nope", NULL }), 2);
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)y4m, "-o",
					     (char *)x, "--quality", "20000", NULL }), 2);
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)yuv, "-o",
					     (char *)x, "--input-format", "I420", NULL }), 2);
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)yuv, "-o",
					     (char *)x, "--codec-input", "YUY2", NULL }), 1);
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)P("nosuch.y4m"),
					     "-o", (char *)x, NULL }), 1);
	CHECK_EQ(run(cmd_encode, (char *[]){ "encode", c, (char *)y4m, "-o",
					     (char *)x, "--rate", "fast", NULL }), 2);

	CHECK_EQ(drv_open_count(), 0);
	CHECK_EQ(codec_opens(), 0);
	for (int i = 0; i < np; i++)
		unlink(paths[i]);
	unlink(seq_avi);
}

/* ICINFO strings, in UTF-16 as they should be or in ANSI as some write. */
static void test_info_text(void)
{
	WCHAR field[16];
	char out[64];

	memset(field, 0, sizeof(field));
	memcpy(field, "WavLor", 7);
	CHECK_EQ(utf16_field_to_utf8(field, 16, out, sizeof(out)), 6);
	CHECK_STR(out, "WavLor");
	CHECK_EQ(utf16_field_to_utf8(field, 16, out, 4), 3);
	CHECK_STR(out, "Wav");

	static const WCHAR name[16] = { 'T', 'S', 'T', '0', ' ', 0xe9 };
	CHECK_EQ(utf16_field_to_utf8(name, 16, out, sizeof(out)), 7);
	CHECK_STR(out, "TST0 \xc3\xa9");
	static const WCHAR one[16] = { 'A' };
	utf16_field_to_utf8(one, 16, out, sizeof(out));
	CHECK_STR(out, "A");

	/* No NUL in the field: as many units as it holds. */
	for (int i = 0; i < 16; i++)
		field[i] = 'x';
	CHECK_EQ(utf16_field_to_utf8(field, 16, out, sizeof(out)), 16);
}

/* A file's text, in a static buffer. */
static const char *text_of(const char *path)
{
	static char buf[8192];
	size_t n = 0;
	char *p = slurp(path, &n);

	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	if (p)
		memcpy(buf, p, n);
	buf[n] = 0;
	free(p);
	return buf;
}

/*
 * A FOURCC found in a DLL is kept in its own ini file's [drivers32]: an
 * entry of that key replaced, other sections and entries left as they
 * are, and a file without the section given one.
 */
static void test_ini_remember(void)
{
	char ini[4200];
	FOURCC vp70 = FCC('V', 'P', '7', '0');

	snprintf(ini, sizeof(ini), "%s/remember.ini", root);
	setenv("VFW_INI", ini, 1);
	write_file(ini, "; mine\n[drivers32]\nvidc.ABCD=a.dll\nVIDC.vp70 = old.dll\n"
		   "[other]\nvidc.VP70=keep.dll\n");
	vfw_registry_reload();
	CHECK_EQ(vfw_ini_remember(ICTYPE_VIDEO, vp70, "/x/vp7vfw.dll"), 0);
	CHECK_STR(text_of(ini), "; mine\n[drivers32]\nvidc.ABCD=a.dll\n"
		  "vidc.VP70=/x/vp7vfw.dll\n[other]\nvidc.VP70=keep.dll\n");
	vfw_registry_reload();
	const struct vfw_codec *c = vfw_find(ICTYPE_VIDEO, vp70);
	CHECK(c && !strcmp(c->path, "/x/vp7vfw.dll"));

	/* No [drivers32]: one is added. No file: one is made. */
	write_file(ini, "[other]\nx=1");
	CHECK_EQ(vfw_ini_remember(ICTYPE_VIDEO, FCC('T', 'S', 'T', ' '), "/t.dll"), 0);
	CHECK_STR(text_of(ini), "[other]\nx=1\n[drivers32]\nvidc.TST=/t.dll\n");
	unlink(ini);
	vfw_registry_reload();
	CHECK_EQ(vfw_ini_remember(ICTYPE_VIDEO, vp70, "/y.dll"), 0);
	CHECK_STR(text_of(ini), "[drivers32]\nvidc.VP70=/y.dll\n");
	unlink(ini);

	/* Not a file: nothing is written. */
	setenv("VFW_INI", "/dev/null", 1);
	vfw_registry_reload();
	CHECK_EQ(vfw_ini_remember(ICTYPE_VIDEO, vp70, "/y.dll"), -1);

	unsetenv("VFW_INI");
	vfw_registry_reload();
}

/* Where the search of a DLL for FOURCCs finds them, and in what order. */
static void test_dll_handlers(void)
{
	FOURCC found[64];
	unsigned char where[64];
	size_t n = vfw_dll_handlers(fixture("pe_fourccs.dll"), found, where, 64);
	static const struct { FOURCC f; unsigned from; } want[] = {
		{ FCC('C', 'M', 'P', 'A'), VFW_FOURCC_FROM_CMP },
		{ FCC('V', 'E', 'R', 'D'), VFW_FOURCC_FROM_VERSION },
		{ FCC('S', 'T', 'R', 'C'), VFW_FOURCC_FROM_STRING },
		{ FCC('M', 'O', 'V', 'B'), VFW_FOURCC_FROM_MOV },
	};
	size_t at[4];

	for (size_t k = 0; k < 4; k++) {
		at[k] = n;
		for (size_t i = 0; i < n; i++)
			if (found[i] == want[k].f) {
				at[k] = i;
				break;
			}
		CHECK(at[k] < n);
		if (at[k] < n)
			CHECK_EQ(where[at[k]], want[k].from);
		if (k && at[k] < n && at[k - 1] < n)
			CHECK(at[k - 1] < at[k]);	/* the likelier first */
	}
	CHECK(strstr(vfw_fourcc_from_name(VFW_FOURCC_FROM_MOV), "stored"));

	/* Raw formats a codec checks its input against come last. */
	n = vfw_dll_handlers(fixture("testcodec.dll"), found, NULL, 64);
	CHECK(n > 0 && found[0] == TST0);
	size_t i420 = n, others = 0;
	for (size_t i = 0; i < n; i++) {
		if (found[i] == FCC('I', '4', '2', '0'))
			i420 = i;
		else if (found[i] == FCC('T', 'S', 'T', 'P'))
			others = i;
	}
	CHECK(i420 < n && others < i420);
}

int main(void)
{
	char codec[4096];

	/* The fixture's path is needed for the ini, but test it later. */
	const char *dir = getenv("VFW_FIXTURES");
	snprintf(codec, sizeof(codec), "%s/testcodec.dll", dir ? dir : ".");
	setup_root(codec);
	CHECK_EQ(runtime_enter(), 0);

	test_info_text();
	test_registry();
	test_ini_remember();
	test_dll_handlers();
	test_names();
	test_stretch_dib();
	test_driver_proc();

	struct module *m = mod_load(fixture("testcodec.dll"), NULL);
	CHECK(m != NULL);
	if (!m) {
		fprintf(stderr, "  %s\n", vfw_error());
		return test_summary("test_vfw");
	}
	codec_stats = mod_get_proc(m, "testcodec_stats");
	codec_opens = mod_get_proc(m, "testcodec_open_count");
	codec_quality = mod_get_proc(m, "testcodec_last_quality");
	codec_module_misses = mod_get_proc(m, "testcodec_module_misses");
	CHECK(codec_stats && codec_opens && codec_quality &&
	      codec_module_misses);
	if (codec_stats && codec_opens && codec_quality) {
		test_driver_lifecycle(codec);
		test_icm();
		test_builtins(m);
		test_probe_cli(codec);
		test_seq();
		test_image();
		test_drawdib();
		test_choose();
		test_format_size();
		test_codec_cli(codec);
	}
	mod_free(m);

	char ini[4200];
	snprintf(ini, sizeof(ini), "%s/vfw.ini", root);
	unlink(ini);
	unlink(avi_path);
	rmdir(root);
	return test_summary("test_vfw");
}
