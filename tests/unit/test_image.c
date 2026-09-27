/*
 * DIB conversion and the output writers.
 *
 * With VFW_KEEP_TEST_FILES=<dir> the Y4M and BMP files are kept there for
 * tests/validate_media.sh.
 */
#include <stdint.h>
#include <sys/stat.h>

#include "common/error.h"
#include "out/image.h"
#include "out/output.h"
#include "test.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)

static char dir[4096];
static int keep;

static const char *path_of(const char *name)
{
	static char p[4][4200];
	static int next;
	char *s = p[next++ % 4];

	snprintf(s, sizeof(p[0]), "%s/%s", dir, name);
	return s;
}

static void setup_dir(void)
{
	const char *k = getenv("VFW_KEEP_TEST_FILES");

	if (k && *k) {
		snprintf(dir, sizeof(dir), "%s", k);
		mkdir(dir, 0755);
		keep = 1;
		return;
	}
	const char *t = getenv("TMPDIR");
	snprintf(dir, sizeof(dir), "%s/vfw-image-XXXXXX", t && *t ? t : "/tmp");
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		exit(1);
	}
}

static void cleanup(const char *name)
{
	if (!keep)
		unlink(path_of(name));
}

/* A format block: header, then up to 256 palette entries or masks. */
struct fmt {
	BITMAPINFOHEADER h;
	DWORD extra[256];
};

static struct fmt make_fmt(int w, int h, int bits, DWORD comp)
{
	struct fmt f;

	memset(&f, 0, sizeof(f));
	f.h.biSize = sizeof(f.h);
	f.h.biWidth = w;
	f.h.biHeight = h;
	f.h.biPlanes = 1;
	f.h.biBitCount = bits;
	f.h.biCompression = comp;
	return f;
}

static struct image_conv *conv_for(struct fmt *f)
{
	struct image_conv *c = image_conv_new(&f->h, sizeof(*f));
	if (!c)
		fprintf(stderr, "image_conv_new: %s\n", vfw_error());
	return c;
}

/* Pixel (x, y) of a BGR24 frame, as 0xRRGGBB. */
static DWORD px(const BYTE *bgr, int w, int x, int y)
{
	const BYTE *p = bgr + ((size_t)y * w + x) * 3;
	return (DWORD)p[2] << 16 | p[1] << 8 | p[0];
}

static void test_colours(void)
{
	/* BT.601 limited range, the reference values. */
	static const struct { BYTE r, g, b, y, u, v; } ref[] = {
		{ 0, 0, 0, 16, 128, 128 },
		{ 255, 255, 255, 235, 128, 128 },
		{ 255, 0, 0, 82, 90, 240 },
		{ 0, 255, 0, 144, 54, 34 },
		{ 0, 0, 255, 41, 240, 110 },
		{ 128, 128, 128, 126, 128, 128 },
	};
	for (size_t i = 0; i < sizeof(ref) / sizeof(ref[0]); i++) {
		BYTE bgr[3 * 4], yuv[6], back[3 * 4];
		for (int k = 0; k < 4; k++) {
			bgr[3 * k] = ref[i].b;
			bgr[3 * k + 1] = ref[i].g;
			bgr[3 * k + 2] = ref[i].r;
		}
		image_bgr24_to_i420(bgr, 2, 2, yuv);
		CHECK_EQ(yuv[0], ref[i].y);
		CHECK_EQ(yuv[3], ref[i].y);
		CHECK_EQ(yuv[4], ref[i].u);
		CHECK_EQ(yuv[5], ref[i].v);
		image_i420_to_bgr24(yuv, 2, 2, back);
		for (int k = 0; k < 3; k++)
			CHECK(abs(back[k] - bgr[k]) <= 2);
	}

	/* Chroma is the average of its 2x2 block; odd sizes round up. */
	BYTE bgr[3 * 3 * 3] = { 0 }, yuv[9 + 2 * 4];
	for (int y = 0; y < 3; y++)
		for (int x = 0; x < 3; x++)
			if (x < 2 && y < 2 && (x + y) % 2 == 0)
				bgr[(y * 3 + x) * 3 + 2] = 255;	/* red */
	CHECK_EQ(image_i420_size(3, 3), 17);
	image_bgr24_to_i420(bgr, 3, 3, yuv);
	CHECK_EQ(yuv[0], 82);
	CHECK_EQ(yuv[1], 16);
	/* Block (0,0) averages to R=128: V = ((112*128 + 128) >> 8) + 128. */
	CHECK_EQ(yuv[9 + 4], 184);
	CHECK_EQ(yuv[9 + 1], 128);	/* the 1x2 right column: black */
	CHECK_EQ(yuv[9 + 3], 128);	/* the lone corner */
}

static void test_rgb_formats(void)
{
	BYTE out[4 * 2 * 3];

	/* 24-bit bottom-up, 4x2: stride 12. */
	struct fmt f = make_fmt(4, 2, 24, BI_RGB);
	struct image_conv *c = conv_for(&f);
	CHECK(c);
	if (c) {
		BYTE src[24] = { 0 };
		src[0] = 0x11; src[1] = 0x22; src[2] = 0x33;	/* bottom left */
		src[12 + 9] = 0x44;				/* top right */
		CHECK_EQ(image_frame_size(c), 24);
		CHECK_STR(image_format_name(c), "RGB24 bottom-up");
		CHECK_EQ(image_to_bgr24(c, src, sizeof(src), out), 0);
		CHECK_EQ(px(out, 4, 0, 1), 0x332211);
		CHECK_EQ(px(out, 4, 3, 0), 0x000044);
		CHECK_EQ(image_to_bgr24(c, src, 23, out), -1);	/* too short */
		CHECK(strstr(vfw_error(), "23 bytes"));
		image_conv_free(c);
	}

	/* 24-bit top-down, 3x2: stride padded to 12. */
	f = make_fmt(3, -2, 24, BI_RGB);
	c = conv_for(&f);
	if (c) {
		BYTE src[24] = { 0 };
		src[0] = 0x11;		/* top left */
		src[12 + 6] = 0x22;	/* bottom right */
		CHECK_EQ(image_frame_size(c), 24);
		CHECK_EQ(image_to_bgr24(c, src, 24, out), 0);
		CHECK_EQ(px(out, 3, 0, 0), 0x000011);
		CHECK_EQ(px(out, 3, 2, 1), 0x000022);
		image_conv_free(c);
	}

	/* 32-bit BGRX. */
	f = make_fmt(2, 1, 32, BI_RGB);
	c = conv_for(&f);
	if (c) {
		BYTE src[8] = { 1, 2, 3, 0xff, 4, 5, 6, 0 };
		CHECK_EQ(image_to_bgr24(c, src, 8, out), 0);
		CHECK_EQ(px(out, 2, 0, 0), 0x030201);
		CHECK_EQ(px(out, 2, 1, 0), 0x060504);
		image_conv_free(c);
	}

	/* 16-bit BI_RGB is 555: bits are repeated down to 8. */
	f = make_fmt(4, 1, 16, BI_RGB);
	c = conv_for(&f);
	if (c) {
		WORD src[4] = { 0x7c00, 0x03e0, 0x001f, 0x4210 };
		CHECK_STR(image_format_name(c), "RGB555 bottom-up");
		CHECK_EQ(image_to_bgr24(c, src, 8, out), 0);
		CHECK_EQ(px(out, 4, 0, 0), 0xff0000);
		CHECK_EQ(px(out, 4, 1, 0), 0x00ff00);
		CHECK_EQ(px(out, 4, 2, 0), 0x0000ff);
		CHECK_EQ(px(out, 4, 3, 0), 0x848484);	/* 10000 -> 10000100 */
		image_conv_free(c);
	}

	/* BI_BITFIELDS 565, masks after the header. */
	f = make_fmt(2, 1, 16, BI_BITFIELDS);
	f.extra[0] = 0xf800;
	f.extra[1] = 0x07e0;
	f.extra[2] = 0x001f;
	c = conv_for(&f);
	if (c) {
		WORD src[2] = { 0x07e0, 0xf81f };
		CHECK_STR(image_format_name(c), "RGB565 bottom-up");
		CHECK_EQ(image_to_bgr24(c, src, 4, out), 0);
		CHECK_EQ(px(out, 2, 0, 0), 0x00ff00);
		CHECK_EQ(px(out, 2, 1, 0), 0xff00ff);
		image_conv_free(c);
	}

	/* BI_BITFIELDS 32-bit RGBX order. */
	f = make_fmt(1, 1, 32, BI_BITFIELDS);
	f.extra[0] = 0x000000ff;
	f.extra[1] = 0x0000ff00;
	f.extra[2] = 0x00ff0000;
	c = conv_for(&f);
	if (c) {
		BYTE src[4] = { 0x10, 0x20, 0x30, 0 };
		CHECK_EQ(image_to_bgr24(c, src, 4, out), 0);
		CHECK_EQ(px(out, 1, 0, 0), 0x102030);
		image_conv_free(c);
	}

	/* Masks that aren't contiguous, or missing, are refused. */
	f = make_fmt(1, 1, 32, BI_BITFIELDS);
	f.extra[0] = 0x00ff00ff;
	f.extra[1] = 0x0000ff00;
	f.extra[2] = 0xff000000;
	CHECK(!image_conv_new(&f.h, sizeof(f)));
	f.extra[0] = 0xff;
	CHECK(!image_conv_new(&f.h, sizeof(f.h)));
	f = make_fmt(1, 1, 24, BI_BITFIELDS);
	CHECK(!image_conv_new(&f.h, sizeof(f)));
}

static void test_palette(void)
{
	BYTE out[8 * 2 * 3];

	/* 1 bit, 8x2 bottom-up, two colours. */
	struct fmt f = make_fmt(8, 2, 1, BI_RGB);
	f.extra[0] = 0x00102030;	/* RGBQUAD: B=30 G=20 R=10 */
	f.extra[1] = 0x00ffffff;
	f.h.biClrUsed = 2;
	struct image_conv *c = image_conv_new(&f.h, sizeof(f.h) + 8);
	CHECK(c);
	if (c) {
		BYTE src[8] = { 0x80, 0, 0, 0, 0x01, 0, 0, 0 };	/* stride 4 */
		CHECK_EQ(image_frame_size(c), 8);
		CHECK_EQ(image_to_bgr24(c, src, 8, out), 0);
		CHECK_EQ(px(out, 8, 0, 1), 0xffffff);	/* bottom row, MSB */
		CHECK_EQ(px(out, 8, 1, 1), 0x102030);
		CHECK_EQ(px(out, 8, 7, 0), 0xffffff);
		image_conv_free(c);
	}

	/* 4 bit. */
	f = make_fmt(3, 1, 4, BI_RGB);
	for (int i = 0; i < 16; i++)
		f.extra[i] = i * 0x10101;
	c = conv_for(&f);
	if (c) {
		BYTE src[4] = { 0x5a, 0xf0, 0, 0 };
		CHECK_EQ(image_to_bgr24(c, src, 4, out), 0);
		CHECK_EQ(px(out, 3, 0, 0), 0x050505);
		CHECK_EQ(px(out, 3, 1, 0), 0x0a0a0a);
		CHECK_EQ(px(out, 3, 2, 0), 0x0f0f0f);
		image_conv_free(c);
	}

	/* 8 bit, then an AVI palette change. */
	f = make_fmt(2, 1, 8, BI_RGB);
	for (int i = 0; i < 256; i++)
		f.extra[i] = i;		/* shades of blue */
	c = conv_for(&f);
	if (c) {
		BYTE src[4] = { 7, 200, 0, 0 };
		CHECK_EQ(image_to_bgr24(c, src, 4, out), 0);
		CHECK_EQ(px(out, 2, 0, 0), 7);
		CHECK_EQ(px(out, 2, 1, 0), 200);
		/* AVIPALCHANGE: first 200, one entry, PALETTEENTRY R G B. */
		BYTE pc[8] = { 200, 1, 0, 0, 0x11, 0x22, 0x33, 0 };
		CHECK_EQ(image_palette_change(c, pc, sizeof(pc)), 0);
		CHECK_EQ(image_to_bgr24(c, src, 4, out), 0);
		CHECK_EQ(px(out, 2, 1, 0), 0x112233);
		CHECK_EQ(px(out, 2, 0, 0), 7);
		/* Past the end of the palette, or short. */
		BYTE bad[8] = { 255, 2, 0, 0, 0, 0, 0, 0 };
		CHECK_EQ(image_palette_change(c, bad, sizeof(bad)), -1);
		CHECK_EQ(image_palette_change(c, pc, 6), -1);
		image_conv_free(c);
	}

	/* No palette at all: a grey ramp. */
	f = make_fmt(2, 1, 1, BI_RGB);
	c = image_conv_new(&f.h, sizeof(f.h));
	if (c) {
		BYTE src[4] = { 0x40, 0, 0, 0 };
		CHECK_EQ(image_to_bgr24(c, src, 4, out), 0);
		CHECK_EQ(px(out, 2, 0, 0), 0);
		CHECK_EQ(px(out, 2, 1, 0), 0xffffff);
		image_conv_free(c);
	}
}

static void test_rle(void)
{
	/* RLE8, 6x3 (bottom-up), palette index = grey level. */
	struct fmt f = make_fmt(6, 3, 8, BI_RLE8);
	for (int i = 0; i < 256; i++)
		f.extra[i] = i * 0x10101;
	struct image_conv *c = conv_for(&f);
	BYTE out[6 * 3 * 3], yuv[6 * 3 + 2 * 3 * 2];

	CHECK(c);
	if (!c)
		return;
	CHECK(image_is_delta(c));
	CHECK_EQ(image_frame_size(c), 0);
	static const BYTE key[] = {
		3, 1, 0, 3, 2, 3, 4, 0,	/* row 0: 1 1 1, absolute 2 3 4 */
		0, 0,			/* end of line */
		6, 9,			/* row 1: 9 x 6 */
		0, 0,
		2, 5, 0, 2, 3, 0, 1, 6,	/* row 2: 5 5, delta +3, 6 */
		0, 1,			/* end of bitmap */
	};
	CHECK_EQ(image_to_bgr24(c, key, sizeof(key), out), 0);
	/* Output is top-down: row 2 of the RLE is the top. */
	CHECK_EQ(px(out, 6, 0, 2), 0x010101);
	CHECK_EQ(px(out, 6, 3, 2), 0x020202);
	CHECK_EQ(px(out, 6, 5, 2), 0x040404);
	CHECK_EQ(px(out, 6, 5, 1), 0x090909);
	CHECK_EQ(px(out, 6, 1, 0), 0x050505);
	CHECK_EQ(px(out, 6, 2, 0), 0);		/* skipped by the delta */
	CHECK_EQ(px(out, 6, 5, 0), 0x060606);

	/* A delta frame: move to (4, 1), write one pixel; the rest stays. */
	static const BYTE delta[] = { 0, 2, 4, 1, 1, 77, 0, 1 };
	CHECK_EQ(image_to_bgr24(c, delta, sizeof(delta), out), 0);
	CHECK_EQ(px(out, 6, 4, 1), 0x4d4d4d);
	CHECK_EQ(px(out, 6, 3, 1), 0x090909);
	CHECK_EQ(px(out, 6, 0, 2), 0x010101);

	/* Garbage runs off the picture without writing outside it. */
	static const BYTE junk[] = { 255, 1, 0, 2, 255, 255, 0, 200, 1 };
	CHECK_EQ(image_to_i420(c, junk, sizeof(junk), yuv), 0);
	image_feed(c, delta, sizeof(delta));
	image_conv_free(c);

	/* RLE4: runs alternate the two nibbles. */
	f = make_fmt(5, 1, 4, BI_RLE4);
	for (int i = 0; i < 16; i++)
		f.extra[i] = i * 0x10101;
	c = conv_for(&f);
	if (c) {
		static const BYTE rle4[] = { 3, 0x12, 0, 3, 0x45, 0x60, 0, 1 };
		CHECK_EQ(image_to_bgr24(c, rle4, sizeof(rle4), out), 0);
		CHECK_EQ(px(out, 5, 0, 0), 0x010101);
		CHECK_EQ(px(out, 5, 1, 0), 0x020202);
		CHECK_EQ(px(out, 5, 2, 0), 0x010101);
		CHECK_EQ(px(out, 5, 3, 0), 0x040404);
		CHECK_EQ(px(out, 5, 4, 0), 0x050505);
		image_conv_free(c);
	}

	/* Top-down RLE doesn't exist. */
	f = make_fmt(4, -4, 8, BI_RLE8);
	CHECK(!image_conv_new(&f.h, sizeof(f)));
}

/* A 4x2 YUV frame whose samples encode their position. */
static void test_yuv_formats(void)
{
	/* The I420 every format below should give. */
	static const BYTE want[8 + 2 + 2] = {
		10, 11, 12, 13, 20, 21, 22, 23,	/* Y */
		100, 101,			/* U */
		200, 201,			/* V */
	};
	static const struct {
		FOURCC fcc;
		BYTE data[16];
		size_t len;
	} fmts[] = {
		{ FCC('Y', 'U', 'Y', '2'), { 10, 99, 11, 199, 12, 101, 13, 201,
					     20, 101, 21, 201, 22, 101, 23, 201 }, 16 },
		{ FCC('U', 'Y', 'V', 'Y'), { 99, 10, 199, 11, 101, 12, 201, 13,
					     101, 20, 201, 21, 101, 22, 201, 23 }, 16 },
		{ FCC('Y', 'V', 'Y', 'U'), { 10, 199, 11, 99, 12, 201, 13, 101,
					     20, 201, 21, 101, 22, 201, 23, 101 }, 16 },
		{ FCC('I', '4', '2', '0'), { 10, 11, 12, 13, 20, 21, 22, 23,
					     100, 101, 200, 201 }, 12 },
		{ FCC('I', 'Y', 'U', 'V'), { 10, 11, 12, 13, 20, 21, 22, 23,
					     100, 101, 200, 201 }, 12 },
		{ FCC('Y', 'V', '1', '2'), { 10, 11, 12, 13, 20, 21, 22, 23,
					     200, 201, 100, 101 }, 12 },
		{ FCC('N', 'V', '1', '2'), { 10, 11, 12, 13, 20, 21, 22, 23,
					     100, 200, 101, 201 }, 12 },
		{ FCC('N', 'V', '2', '1'), { 10, 11, 12, 13, 20, 21, 22, 23,
					     200, 100, 201, 101 }, 12 },
		{ FCC('y', 'u', 'y', '2'), { 10, 99, 11, 199, 12, 101, 13, 201,
					     20, 101, 21, 201, 22, 101, 23, 201 }, 16 },
	};
	BYTE out[12];

	for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
		struct fmt f = make_fmt(4, 2, 16, fmts[i].fcc);
		struct image_conv *c = conv_for(&f);
		CHECK(c);
		if (!c)
			continue;
		CHECK_EQ(image_frame_size(c), fmts[i].len);
		memset(out, 0, sizeof(out));
		CHECK_EQ(image_to_i420(c, fmts[i].data, fmts[i].len, out), 0);
		if (memcmp(out, want, sizeof(want))) {
			fprintf(stderr, "format %zu (%s) converted wrongly\n", i,
				image_format_name(c));
			CHECK(0);
		}
		CHECK_EQ(image_to_i420(c, fmts[i].data, fmts[i].len - 1, out), -1);
		image_conv_free(c);
	}

	/* Y800: luma only. */
	struct fmt f = make_fmt(2, 2, 8, FCC('Y', '8', '0', '0'));
	struct image_conv *c = conv_for(&f);
	if (c) {
		BYTE src[4] = { 16, 50, 100, 235 };
		CHECK_EQ(image_to_i420(c, src, 4, out), 0);
		CHECK(!memcmp(out, src, 4));
		CHECK_EQ(out[4], 128);
		CHECK_EQ(out[5], 128);
		BYTE bgr[12];
		CHECK_EQ(image_to_bgr24(c, src, 4, bgr), 0);
		CHECK_EQ(px(bgr, 2, 0, 0), 0);
		CHECK_EQ(px(bgr, 2, 1, 1), 0xffffff);
		image_conv_free(c);
	}

	/* Odd sizes: 3x3 I420 has 2x2 chroma. */
	f = make_fmt(3, 3, 12, FCC('I', '4', '2', '0'));
	c = conv_for(&f);
	if (c) {
		CHECK_EQ(image_frame_size(c), 9 + 8);
		image_conv_free(c);
	}
	/* 3x1 YUY2: groups of two pixels, so 8 bytes a row. */
	f = make_fmt(3, 1, 16, FCC('Y', 'U', 'Y', '2'));
	c = conv_for(&f);
	if (c) {
		BYTE src[8] = { 1, 50, 2, 60, 3, 70, 4, 80 };
		BYTE o[3 + 2 + 2];
		CHECK_EQ(image_frame_size(c), 8);
		CHECK_EQ(image_to_i420(c, src, 8, o), 0);
		CHECK_EQ(o[2], 3);
		CHECK_EQ(o[4], 70);
		CHECK_EQ(o[6], 80);
		image_conv_free(c);
	}
}

static void test_refused(void)
{
	struct fmt f = make_fmt(16, 16, 24, FCC('M', 'J', 'P', 'G'));

	CHECK(!image_conv_new(&f.h, sizeof(f)));
	CHECK(strstr(vfw_error(), "MJPG"));
	CHECK(strstr(vfw_error(), "codec"));
	f = make_fmt(16, 16, 24, BI_JPEG);
	CHECK(!image_conv_new(&f.h, sizeof(f)));
	f = make_fmt(0, 16, 24, BI_RGB);
	CHECK(!image_conv_new(&f.h, sizeof(f)));
	f = make_fmt(16, 0, 24, BI_RGB);
	CHECK(!image_conv_new(&f.h, sizeof(f)));
	f = make_fmt(100000, 16, 24, BI_RGB);
	CHECK(!image_conv_new(&f.h, sizeof(f)));
	f = make_fmt(16, 16, 12, BI_RGB);
	CHECK(!image_conv_new(&f.h, sizeof(f)));
	f = make_fmt(16, 16, 24, BI_RGB);
	CHECK(!image_conv_new(&f.h, 39));
	f.h.biSize = sizeof(f) + 1;
	CHECK(!image_conv_new(&f.h, sizeof(f)));
	/* Other names for BI_RGB. */
	f = make_fmt(16, 16, 24, FCC('D', 'I', 'B', ' '));
	struct image_conv *c = conv_for(&f);
	CHECK(c);
	image_conv_free(c);
}

static void test_bmp_paths(void)
{
	char b[64];

	CHECK_EQ(output_bmp_path("f%05d.bmp", 42, 0, b, sizeof(b)), 0);
	CHECK_STR(b, "f00042.bmp");
	CHECK_EQ(output_bmp_path("f%d.bmp", 7, 0, b, sizeof(b)), 0);
	CHECK_STR(b, "f7.bmp");
	CHECK_EQ(output_bmp_path("100%%-%u.bmp", 3, 0, b, sizeof(b)), 0);
	CHECK_STR(b, "100%-3.bmp");
	CHECK_EQ(output_bmp_path("%012d", 5, 0, b, sizeof(b)), 0);
	CHECK_STR(b, "000000000005");
	CHECK_EQ(output_bmp_path("one.bmp", 0, 1, b, sizeof(b)), 0);
	CHECK_STR(b, "one.bmp");

	static const char *const bad[] = {
		"one.bmp", "%s.bmp", "%x.bmp", "%5d.bmp", "%0d.bmp", "%d%d.bmp",
		"%n", "%", "a%", "%123d", "%-5d", "%ld", "%.3d", "%*d",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		if (output_bmp_path(bad[i], 1, 0, b, sizeof(b)) == 0) {
			fprintf(stderr, "pattern '%s' accepted\n", bad[i]);
			CHECK(0);
		}
	CHECK_EQ(output_bmp_path("%d%%d", 1, 0, b, sizeof(b)), 0);
	CHECK_STR(b, "1%d");
	/* Too long for the buffer. */
	CHECK_EQ(output_bmp_path("%020d", 1, 0, b, 16), -1);

	enum output_kind k;
	CHECK_EQ(output_kind_from_path("a/b.Y4M", &k), 0);
	CHECK_EQ(k, OUT_Y4M);
	CHECK_EQ(output_kind_from_path("x%05d.bmp", &k), 0);
	CHECK_EQ(k, OUT_BMP);
	CHECK_EQ(output_kind_from_path("x.yuv", &k), 0);
	CHECK_EQ(k, OUT_YUV);
	CHECK_EQ(output_kind_from_path("x.bgr", &k), 0);
	CHECK_EQ(k, OUT_BGR);
	CHECK_EQ(output_kind_from_path("x.raw", &k), 0);
	CHECK_EQ(k, OUT_RAW);
	CHECK_EQ(output_kind_from_path("x.mp4", &k), -1);
	CHECK_EQ(output_kind_from_path("dir.y4m/x", &k), -1);
	CHECK_EQ(output_kind_from_name("I420", &k), 0);
	CHECK_EQ(k, OUT_YUV);
}

/* A gradient test picture, as 24-bit bottom-up DIB bits. */
static void gradient(BYTE *dib, int w, int h, int frame)
{
	size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;

	memset(dib, 0, stride * h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			BYTE *p = dib + (size_t)(h - 1 - y) * stride + 3 * x;
			p[0] = (BYTE)(x * 255 / (w - 1));
			p[1] = (BYTE)(y * 255 / (h - 1));
			p[2] = (BYTE)(frame * 60);
		}
}

static void test_outputs(void)
{
	const int w = 33, h = 17, frames = 3;
	struct fmt f = make_fmt(w, h, 24, BI_RGB);
	struct image_conv *c = conv_for(&f);
	size_t dib_size = (((size_t)w * 3 + 3) & ~(size_t)3) * h;
	BYTE *dib = malloc(dib_size);
	void *data;
	size_t len;

	CHECK(c && dib);
	if (!c || !dib)
		return;

	struct output *y4m = output_open(path_of("out.y4m"), OUT_Y4M, w, h,
					 30000, 1001, 0);
	struct output *yuv = output_open(path_of("out.yuv"), OUT_YUV, w, h, 1, 1, 0);
	struct output *bgr = output_open(path_of("out.bgr"), OUT_BGR, w, h, 1, 1, 0);
	struct output *raw = output_open(path_of("out.raw"), OUT_RAW, w, h, 1, 1, 0);
	struct output *bmp = output_open(path_of("out%03d.bmp"), OUT_BMP, w, h,
					 1, 1, 0);
	CHECK(y4m && yuv && bgr && raw && bmp);
	if (!y4m || !yuv || !bgr || !raw || !bmp)
		return;
	for (int i = 0; i < frames; i++) {
		gradient(dib, w, h, i);
		CHECK_EQ(output_write(y4m, c, dib, dib_size), 0);
		CHECK_EQ(output_write(yuv, c, dib, dib_size), 0);
		CHECK_EQ(output_write(bgr, c, dib, dib_size), 0);
		CHECK_EQ(output_write(raw, c, dib, dib_size), 0);
		CHECK_EQ(output_write(bmp, c, dib, dib_size), 0);
	}
	CHECK_EQ(output_repeat(y4m), 0);	/* a dropped frame */
	CHECK_EQ(output_frames(y4m), frames + 1);
	CHECK_EQ(output_close(y4m), 0);
	CHECK_EQ(output_close(yuv), 0);
	CHECK_EQ(output_close(bgr), 0);
	CHECK_EQ(output_close(raw), 0);
	CHECK_EQ(output_close(bmp), 0);

	/* Y4M: header, then FRAME + I420 per frame. */
	size_t fs = image_i420_size(w, h);
	CHECK_EQ(vfw_read_file(path_of("out.y4m"), &data, &len), 0);
	const char *hdr = "YUV4MPEG2 W33 H17 F30000:1001 Ip A1:1 C420jpeg\n";
	CHECK(len > strlen(hdr) && !memcmp(data, hdr, strlen(hdr)));
	CHECK_EQ(len, strlen(hdr) + (frames + 1) * (6 + fs));
	CHECK(!memcmp((BYTE *)data + strlen(hdr), "FRAME\n", 6));
	void *planes;
	size_t plen;
	CHECK_EQ(vfw_read_file(path_of("out.yuv"), &planes, &plen), 0);
	CHECK_EQ(plen, frames * fs);
	/* The same pictures, and the repeat is the last one again. */
	const BYTE *y = (BYTE *)data + strlen(hdr);
	int same = 1;
	for (int i = 0; i < frames + 1 && same; i++)
		same = !memcmp(y + i * (6 + fs) + 6,
			       (BYTE *)planes + (i < frames ? i : frames - 1) * fs, fs);
	CHECK(same);
	free(data);
	free(planes);

	/* BGR is top-down; frame 2's top-left pixel is R=120, G=0, B=0. */
	CHECK_EQ(vfw_read_file(path_of("out.bgr"), &data, &len), 0);
	CHECK_EQ(len, frames * image_bgr24_size(w, h));
	const BYTE *last = (BYTE *)data + 2 * image_bgr24_size(w, h);
	CHECK_EQ(px(last, w, 0, 0), 0x780000);
	CHECK_EQ(px(last, w, w - 1, h - 1), 0x78ffff);
	free(data);

	CHECK_EQ(vfw_read_file(path_of("out.raw"), &data, &len), 0);
	CHECK_EQ(len, frames * dib_size);
	CHECK(!memcmp((BYTE *)data + 2 * dib_size, dib, dib_size));
	free(data);

	/* BMP: a 24-bit bottom-up DIB with rows padded to 4 bytes, like ours. */
	CHECK_EQ(vfw_read_file(path_of("out002.bmp"), &data, &len), 0);
	CHECK_EQ(len, 54 + dib_size);
	const BYTE *b = data;
	CHECK(b[0] == 'B' && b[1] == 'M');
	DWORD v;
	memcpy(&v, b + 2, 4);
	CHECK_EQ(v, len);
	memcpy(&v, b + 10, 4);
	CHECK_EQ(v, 54);
	BITMAPINFOHEADER bi;
	memcpy(&bi, b + 14, sizeof(bi));
	CHECK_EQ(bi.biWidth, w);
	CHECK_EQ(bi.biHeight, h);
	CHECK_EQ(bi.biBitCount, 24);
	CHECK(!memcmp(b + 54, dib, dib_size));
	free(data);
	CHECK_EQ(access(path_of("out000.bmp"), R_OK), 0);
	CHECK(access(path_of("out003.bmp"), R_OK) != 0);

	/* A BMP path without a number, for more than one frame. */
	CHECK(!output_open(path_of("single.bmp"), OUT_BMP, w, h, 1, 1, 0));
	struct output *one = output_open(path_of("single.bmp"), OUT_BMP, w, h,
					 1, 1, 1);
	CHECK(one);
	if (one) {
		CHECK_EQ(output_write(one, c, dib, dib_size), 0);
		CHECK_EQ(output_close(one), 0);
		CHECK_EQ(access(path_of("single.bmp"), R_OK), 0);
	}
	/* A short frame fails without writing. */
	struct output *o = output_open(path_of("short.yuv"), OUT_YUV, w, h, 1, 1, 0);
	CHECK(o);
	if (o) {
		CHECK_EQ(output_write(o, c, dib, dib_size - 1), -1);
		CHECK_EQ(output_frames(o), 0);
		output_close(o);
	}
	CHECK(!output_open("/nonexistent/dir/x.y4m", OUT_Y4M, w, h, 1, 1, 0));

	cleanup("out.yuv");
	cleanup("out.bgr");
	cleanup("out.raw");
	cleanup("out000.bmp");
	cleanup("out001.bmp");
	cleanup("single.bmp");
	cleanup("short.yuv");
	if (!keep) {
		unlink(path_of("out.y4m"));
		unlink(path_of("out002.bmp"));
	}
	free(dib);
	image_conv_free(c);
}

/* image_pack() is the inverse of the conversions, for every format it takes. */
static void test_pack(void)
{
	enum { W = 7, H = 5 };
	static const DWORD yuv[] = {
		FCC('I', '4', '2', '0'), FCC('I', 'Y', 'U', 'V'), FCC('Y', 'V', '1', '2'),
		FCC('Y', 'U', 'Y', '2'), FCC('U', 'Y', 'V', 'Y'), FCC('Y', 'V', 'Y', 'U'),
		FCC('N', 'V', '1', '2'), FCC('N', 'V', '2', '1'), FCC('y', 'u', 'y', '2'),
	};
	BYTE src[W * H * 3], back[W * H * 3], i420[W * H * 3], packed[W * H * 8];
	BYTE again[W * H * 8];
	size_t n420 = image_i420_size(W, H);

	srand(7);
	for (size_t i = 0; i < sizeof(src); i++)
		src[i] = (BYTE)rand();

	/* YUV: from I420 and back, exactly (4:2:2 repeats the chroma rows). */
	memcpy(i420, src, n420);
	for (size_t k = 0; k < sizeof(yuv) / sizeof(yuv[0]); k++) {
		int bits = yuv[k] == FCC('Y', 'U', 'Y', '2') || yuv[k] == FCC('U', 'Y', 'V', 'Y') ||
			   yuv[k] == FCC('Y', 'V', 'Y', 'U') || yuv[k] == FCC('y', 'u', 'y', '2') ?
			   16 : 12;
		struct fmt f = make_fmt(W, H, bits, yuv[k]);
		struct image_conv *c = conv_for(&f);
		CHECK(c != NULL);
		if (!c)
			continue;
		CHECK(image_can_pack(c));
		CHECK(image_pack_wants_i420(c));
		CHECK_EQ(image_pack(c, i420, packed), 0);
		CHECK_EQ(image_to_i420(c, packed, image_frame_size(c), back), 0);
		CHECK(!memcmp(back, i420, n420));
		image_conv_free(c);
	}
	{
		struct fmt f = make_fmt(W, H, 8, FCC('Y', '8', '0', '0'));
		struct image_conv *c = conv_for(&f);
		CHECK(c && image_pack(c, i420, packed) == 0);
		CHECK(c && image_to_i420(c, packed, W * H, back) == 0);
		CHECK(!memcmp(back, i420, W * H));
		CHECK(back[W * H] == 128 && back[n420 - 1] == 128);
		image_conv_free(c);
	}

	/* 24 and 32 bits, both ways up: exact. */
	for (int bits = 24; bits <= 32; bits += 8)
		for (int dir = -1; dir <= 1; dir += 2) {
			struct fmt f = make_fmt(W, dir * H, bits, BI_RGB);
			struct image_conv *c = conv_for(&f);
			CHECK(c && !image_pack_wants_i420(c));
			CHECK(c && image_pack(c, src, packed) == 0);
			CHECK(c && image_to_bgr24(c, packed, image_frame_size(c), back) == 0);
			CHECK(!memcmp(back, src, sizeof(src)));
			/* Row padding is zero. */
			if (bits == 24 && dir > 0)
				CHECK(packed[W * 3] == 0 && packed[W * 3 + 2] == 0);
			image_conv_free(c);
		}

	/* Fewer bits per channel: packing what it unpacks gives the same frame. */
	static const DWORD masks[][3] = {
		{ 0x7c00, 0x03e0, 0x001f }, { 0xf800, 0x07e0, 0x001f },
		{ 0x3ff00000, 0x000ffc00, 0x000003ff }, { 0x00ff0000, 0x0000ff00, 0x000000ff },
	};
	for (size_t k = 0; k < 5; k++) {
		struct fmt f = k == 4 ? make_fmt(W, H, 16, BI_RGB) :
			       make_fmt(W, H, k < 2 ? 16 : 32, BI_BITFIELDS);
		if (k < 4)
			memcpy(f.extra, masks[k], sizeof(masks[k]));
		struct image_conv *c = conv_for(&f);
		CHECK(c != NULL);
		if (!c)
			continue;
		size_t n = image_frame_size(c);
		CHECK_EQ(image_pack(c, src, packed), 0);
		CHECK_EQ(image_to_bgr24(c, packed, n, back), 0);
		CHECK_EQ(image_pack(c, back, again), 0);
		CHECK(!memcmp(again, packed, n));
		int worst = 0;
		for (size_t i = 0; i < sizeof(src); i++) {
			int d = abs(back[i] - src[i]);
			worst = d > worst ? d : worst;
		}
		CHECK(worst <= (k == 2 || k == 3 ? 0 : 4));
		image_conv_free(c);
	}

	/* Palettes can't be packed. */
	struct fmt f = make_fmt(W, H, 8, BI_RGB);
	struct image_conv *c = conv_for(&f);
	CHECK(c && !image_can_pack(c));
	CHECK(c && image_pack(c, src, packed) == -1);
	image_conv_free(c);
}

int main(void)
{
	setup_dir();
	test_colours();
	test_rgb_formats();
	test_palette();
	test_rle();
	test_yuv_formats();
	test_refused();
	test_bmp_paths();
	test_outputs();
	test_pack();
	if (!keep)
		rmdir(dir);
	return test_summary("test_image");
}
