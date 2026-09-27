/*
 * The VfW layer: the codec registry, the installable-driver lifecycle, the
 * ICM API and the winmm / msvfw32 built-ins, driven against testcodec.dll.
 */
#include <sys/stat.h>

#include <fcntl.h>

#include "avi/avi.h"
#include "commands.h"
#include "common/error.h"
#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "vfw/driver.h"
#include "vfw/icm.h"
#include "vfw/registry.h"

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
	CHECK(strstr(out, "description: vfw-harness test codec"));
	CHECK(strstr(out, "flags: 0x0024 TEMPORAL FASTTEMPORALC"));
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
	CHECK(strstr(out, "TST0: vfw-harness test codec"));
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

int main(void)
{
	char codec[4096];

	/* The fixture's path is needed for the ini, but test it later. */
	const char *dir = getenv("VFW_FIXTURES");
	snprintf(codec, sizeof(codec), "%s/testcodec.dll", dir ? dir : ".");
	setup_root(codec);
	CHECK_EQ(runtime_enter(), 0);

	test_registry();
	test_names();
	test_driver_proc();

	struct module *m = mod_load(fixture("testcodec.dll"), NULL);
	CHECK(m != NULL);
	if (!m) {
		fprintf(stderr, "  %s\n", vfw_error());
		return test_summary("test_vfw");
	}
	codec_stats = mod_get_proc(m, "testcodec_stats");
	codec_opens = mod_get_proc(m, "testcodec_open_count");
	CHECK(codec_stats && codec_opens);
	if (codec_stats && codec_opens) {
		test_driver_lifecycle(codec);
		test_icm();
		test_builtins(m);
		test_probe_cli(codec);
		test_seq();
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
