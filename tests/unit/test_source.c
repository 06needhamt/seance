/*
 * Frame sources for encoding: Y4M, raw frames, BMP files and uncompressed
 * AVI, including the ways each can be malformed.
 */
#include <sys/stat.h>

#include "avi/avi.h"
#include "common/error.h"
#include "in/source.h"
#include "out/image.h"
#include "out/output.h"
#include "test.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)

static char dir[4096];

static const char *path_of(const char *name)
{
	static char p[4][4200];
	static int next;
	char *s = p[next++ % 4];

	snprintf(s, sizeof(p[0]), "%s/%s", dir, name);
	return s;
}

static void write_bytes(const char *path, const void *data, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(data, 1, n, f) != n) {
		perror(path);
		exit(1);
	}
	fclose(f);
}

/* Frame n of a W x H I420 picture. */
static void yuv_frame(BYTE *p, int w, int h, int n)
{
	size_t size = image_i420_size(w, h);
	for (size_t i = 0; i < size; i++)
		p[i] = (BYTE)(i * 7 + n * 13);
}

/* A Y4M file with the given header tokens and frames. */
static void write_y4m(const char *path, const char *tokens, int w, int h,
		      int frames, int cut)
{
	FILE *f = fopen(path, "wb");
	BYTE frame[4096];

	fprintf(f, "YUV4MPEG2 %s\n", tokens);
	for (int n = 0; n < frames; n++) {
		yuv_frame(frame, w, h, n);
		fprintf(f, n == 1 ? "FRAME Ixyz\n" : "FRAME\n");
		size_t len = image_i420_size(w, h);
		if (cut && n == frames - 1)
			len /= 2;
		fwrite(frame, 1, len, f);
	}
	fclose(f);
}

static int read_all(struct source *s, int w, int h, int *frames)
{
	const BYTE *frame;
	size_t len;
	BYTE want[4096];
	int r, ok = 1;

	*frames = 0;
	while ((r = source_read(s, &frame, &len)) == 1) {
		yuv_frame(want, w, h, *frames);
		ok &= len == image_i420_size(w, h) && !memcmp(frame, want, len);
		(*frames)++;
	}
	return r < 0 ? -1 : ok;
}

static void test_y4m(void)
{
	const char *p = path_of("a.y4m");
	DWORD rate, scale;
	int frames;

	write_y4m(p, "W7 H5 F30000:1001 Ip A1:1 C420jpeg XYSCSS=420JPEG", 7, 5, 3, 0);
	struct source *s = source_open(p, NULL);
	CHECK(s != NULL);
	if (s) {
		size_t n;
		const BITMAPINFOHEADER *b = source_format(s, &n);
		CHECK_EQ(source_kind(s), SOURCE_Y4M);
		CHECK_EQ(n, sizeof(*b));
		CHECK_EQ(b->biWidth, 7);
		CHECK_EQ(b->biHeight, 5);
		CHECK_EQ(b->biBitCount, 12);
		CHECK_EQ(b->biCompression, FCC('I', '4', '2', '0'));
		CHECK_EQ(b->biSizeImage, image_i420_size(7, 5));
		CHECK_STR(source_format_name(s), "I420");
		source_rate(s, &rate, &scale);
		CHECK(rate == 30000 && scale == 1001);
		CHECK_EQ(source_frames(s), -1);
		CHECK_EQ(read_all(s, 7, 5, &frames), 1);
		CHECK_EQ(frames, 3);
		source_close(s);
	}

	/* Other 4:2:0 sitings, no C at all, and by content without .y4m. */
	static const char *const ok[] = { "W4 H4 F25:1 C420", "W4 H4 F25:1 C420paldv",
					  "W4 H4 F25:1 C420mpeg2", "W4 H4 F25:1" };
	for (size_t i = 0; i < 4; i++) {
		p = path_of(i == 3 ? "noext" : "b.y4m");
		write_y4m(p, ok[i], 4, 4, 2, 0);
		s = source_open(p, NULL);
		CHECK(s && read_all(s, 4, 4, &frames) == 1 && frames == 2);
		source_close(s);
	}

	/* Mono is 8-bit Y800. */
	p = path_of("mono.y4m");
	write_bytes(p, "YUV4MPEG2 W2 H2 F25:1 Cmono\nFRAME\nabcd", 38);
	s = source_open(p, NULL);
	CHECK(s != NULL);
	if (s) {
		const BYTE *frame;
		size_t len;
		CHECK_EQ(source_format(s, NULL)->biCompression, FCC('Y', '8', '0', '0'));
		CHECK_EQ(source_read(s, &frame, &len), 1);
		CHECK(len == 4 && !memcmp(frame, "abcd", 4));
		CHECK_EQ(source_read(s, &frame, &len), 0);
		source_close(s);
	}

	/* Refused. */
	static const char *const bad[] = { "W4 H4 C444", "W4 H4 C420p10", "W0 H4",
					   "H4", "W99999 H4" };
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		p = path_of("bad.y4m");
		write_y4m(p, bad[i], 4, 4, 1, 0);
		s = source_open(p, NULL);
		CHECK(s == NULL);
		source_close(s);
	}
	p = path_of("bad.y4m");
	write_bytes(p, "YUV4MPEG W4 H4\n", 15);
	CHECK(source_open(p, NULL) == NULL);
	CHECK(strstr(vfw_error(), "not a YUV4MPEG2") != NULL);

	/* A cut-off last frame, and a missing FRAME header. */
	p = path_of("cut.y4m");
	write_y4m(p, "W4 H4 F25:1", 4, 4, 3, 1);
	s = source_open(p, NULL);
	CHECK(s && read_all(s, 4, 4, &frames) == -1 && frames == 2);
	CHECK(strstr(vfw_error(), "cut short") != NULL);
	source_close(s);
	write_bytes(p, "YUV4MPEG2 W2 H2\nFRAMEX\n", 23);
	s = source_open(p, NULL);
	const BYTE *frame;
	size_t len;
	CHECK(s && source_read(s, &frame, &len) == -1);
	CHECK(strstr(vfw_error(), "FRAME header") != NULL);
	source_close(s);

	CHECK(source_open(path_of("nosuch.y4m"), NULL) == NULL);
}

static void test_raw(void)
{
	const char *p = path_of("frames.bgr");
	BYTE data[5 * 3 * 3 * 4];
	struct source_options o = { .raw_format = "BGR24", .width = 5, .height = 3,
				    .stream = -1 };
	const BYTE *frame;
	size_t len;

	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (BYTE)i;
	write_bytes(p, data, 45 * 3 + 7);	/* 3 frames and a partial one */
	struct source *s = source_open(p, &o);
	CHECK(s != NULL);
	if (s) {
		const BITMAPINFOHEADER *b = source_format(s, NULL);
		CHECK_EQ(source_kind(s), SOURCE_RAW);
		CHECK_EQ(b->biHeight, -3);	/* top-down */
		CHECK_EQ(b->biBitCount, 24);
		CHECK_EQ(source_frames(s), 3);
		/* 15 bytes a row in the file, padded to 16 in a DIB. */
		CHECK_EQ(b->biSizeImage, 48);
		CHECK(source_read(s, &frame, &len) == 1 && len == 48);
		CHECK(frame[0] == 0 && frame[14] == 14 && frame[15] == 0);
		CHECK(frame[16] == 15 && frame[47] == 0 && frame[46] == 44);
		CHECK(source_read(s, &frame, &len) == 1 && frame[0] == 45);
		CHECK(source_read(s, &frame, &len) == 1);
		CHECK_EQ(source_read(s, &frame, &len), -1);	/* the partial one */
		source_close(s);
	}

	o.raw_format = "I420";
	write_bytes(p, data, image_i420_size(5, 3) * 2);
	s = source_open(p, &o);
	CHECK(s && source_frames(s) == 2);
	CHECK(s && source_read(s, &frame, &len) == 1 && len == image_i420_size(5, 3));
	CHECK(s && !memcmp(frame, data, len));
	CHECK(s && source_read(s, &frame, &len) == 1);
	CHECK(s && source_read(s, &frame, &len) == 0);
	source_close(s);

	o.raw_format = "I420";
	write_bytes(p, data, image_i420_size(5, 3) + 3);
	s = source_open(p, &o);
	CHECK(s && source_read(s, &frame, &len) == 1);
	CHECK(s && source_read(s, &frame, &len) == -1);
	source_close(s);

	o.raw_format = "PAL8";
	CHECK(source_open(p, &o) == NULL);
	o.raw_format = "NOPE";
	CHECK(source_open(p, &o) == NULL);
	o.raw_format = "I420";
	o.width = 0;
	CHECK(source_open(p, &o) == NULL);
}

/* A BMP file of w x h at bits (24 or 8), pixel values from seed. */
static void write_bmp(const char *path, int w, int h, int bits, int seed)
{
	size_t stride = ((size_t)w * bits + 31) / 32 * 4, colors = bits == 8 ? 256 : 0;
	size_t off = 14 + 40 + 4 * colors, size = off + stride * h;
	BYTE *b = calloc(1, size);

	b[0] = 'B';
	b[1] = 'M';
	memcpy(b + 2, &(DWORD){ (DWORD)size }, 4);
	memcpy(b + 10, &(DWORD){ (DWORD)off }, 4);
	BITMAPINFOHEADER h0 = { .biSize = 40, .biWidth = w, .biHeight = h,
				.biPlanes = 1, .biBitCount = (WORD)bits };
	memcpy(b + 14, &h0, 40);
	for (size_t i = 0; i < colors; i++)
		b[54 + 4 * i] = (BYTE)i;
	for (size_t i = off; i < size; i++)
		b[i] = (BYTE)(i + seed);
	write_bytes(path, b, size);
	free(b);
}

static void test_bmp(void)
{
	const BYTE *frame;
	size_t len;
	int n = 0;

	/* A sequence from 1, as FFmpeg numbers them. */
	for (int i = 1; i <= 3; i++) {
		char name[32];
		snprintf(name, sizeof(name), "s%03d.bmp", i);
		write_bmp(path_of(name), 5, 3, 24, i);
	}
	struct source *s = source_open(path_of("s%03d.bmp"), NULL);
	CHECK(s != NULL);
	if (s) {
		CHECK_EQ(source_kind(s), SOURCE_BMP);
		CHECK_EQ(source_format(s, NULL)->biHeight, 3);
		CHECK_STR(source_format_name(s), "RGB24 bottom-up");
		while (source_read(s, &frame, &len) == 1) {
			n++;
			CHECK_EQ(len, 48);
			CHECK_EQ(frame[0], (BYTE)(54 + n));
		}
		CHECK_EQ(n, 3);
		source_close(s);
	}

	/* One file, with a palette. */
	write_bmp(path_of("one.bmp"), 4, 2, 8, 0);
	s = source_open(path_of("one.bmp"), NULL);
	CHECK(s != NULL);
	if (s) {
		size_t fs;
		const BITMAPINFOHEADER *b = source_format(s, &fs);
		CHECK_EQ(fs, 40 + 1024);
		CHECK_EQ(b->biClrUsed, 256);
		CHECK_EQ(((const BYTE *)b)[40 + 4 * 9], 9);
		CHECK(source_read(s, &frame, &len) == 1 && len == 8);
		CHECK_EQ(source_read(s, &frame, &len), 0);
		source_close(s);
	}

	/* A file in the sequence with another format. */
	write_bmp(path_of("s002.bmp"), 5, 4, 24, 2);
	s = source_open(path_of("s%03d.bmp"), NULL);
	CHECK(s && source_read(s, &frame, &len) == 1);
	CHECK(s && source_read(s, &frame, &len) == -1);
	CHECK(strstr(vfw_error(), "different format") != NULL);
	source_close(s);

	CHECK(source_open(path_of("z%03d.bmp"), NULL) == NULL);
	CHECK(source_open(path_of("s%d%d.bmp"), NULL) == NULL);
	write_bytes(path_of("junk.bmp"), "BMxxxxxxxxxxxxxxxxxxxxxxx", 25);
	CHECK(source_open(path_of("junk.bmp"), NULL) == NULL);
}

static void test_avi(void)
{
	BITMAPINFOHEADER b = { .biSize = 40, .biWidth = 5, .biHeight = 3,
			       .biPlanes = 1, .biBitCount = 24,
			       .biCompression = BI_RGB, .biSizeImage = 48 };
	BYTE frame[48];
	const BYTE *got;
	size_t len;

	struct avi_writer *w = avi_writer_open(path_of("u.avi"), &b, sizeof(b),
					       0, 12, 1, NULL);
	CHECK(w != NULL);
	for (int i = 0; i < 3 && w; i++) {
		memset(frame, 'a' + i, sizeof(frame));
		/* Frame 1 is dropped (empty). */
		CHECK_EQ(avi_writer_add(w, frame, i == 1 ? 0 : sizeof(frame), 1), 0);
	}
	CHECK(w && avi_writer_close(w) == 0);

	struct source *s = source_open(path_of("u.avi"), NULL);
	CHECK(s != NULL);
	if (s) {
		DWORD rate, scale;
		CHECK_EQ(source_kind(s), SOURCE_AVI);
		CHECK_EQ(source_frames(s), 3);
		source_rate(s, &rate, &scale);
		CHECK(rate == 12 && scale == 1);
		CHECK(source_read(s, &got, &len) == 1 && len == 48 && got[0] == 'a');
		CHECK(source_read(s, &got, &len) == 1 && got[0] == 'a');
		CHECK(source_read(s, &got, &len) == 1 && got[47] == 'c');
		CHECK_EQ(source_read(s, &got, &len), 0);
		source_close(s);
	}

	/* A compressed stream can't be encoded from. */
	b.biCompression = FCC('X', 'V', 'I', 'D');
	w = avi_writer_open(path_of("c.avi"), &b, sizeof(b), 0, 25, 1, NULL);
	CHECK(w && avi_writer_add(w, frame, 10, 1) == 0 && avi_writer_close(w) == 0);
	CHECK(source_open(path_of("c.avi"), NULL) == NULL);
	CHECK(strstr(vfw_error(), "compressed") != NULL);
	struct source_options o = { .stream = 3 };
	CHECK(source_open(path_of("u.avi"), &o) == NULL);
}

static DWORD rng_state = 0x2545f491;

static DWORD rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void *read_back(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	void *p = NULL;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	rewind(f);
	if ((p = malloc(*len)) && fread(p, 1, *len, f) != *len) {
		free(p);
		p = NULL;
	}
	fclose(f);
	return p;
}

/*
 * Damaged inputs: every truncation of small Y4M, BMP and AVI files and
 * random byte changes, read to the end. Nothing may crash or overrun (the
 * sanitizer build checks that); errors are fine.
 */
static void test_fuzz(void)
{
	static const char *const names[] = { "fz.y4m", "fz.bmp", "fz.avi" };
	BITMAPINFOHEADER b = { .biSize = 40, .biWidth = 5, .biHeight = 3,
			       .biPlanes = 1, .biBitCount = 24,
			       .biCompression = BI_RGB, .biSizeImage = 48 };
	BYTE frame[48];

	write_y4m(path_of(names[0]), "W7 H5 F25:1 C420jpeg", 7, 5, 3, 0);
	write_bmp(path_of(names[1]), 5, 3, 8, 3);
	struct avi_writer *w = avi_writer_open(path_of(names[2]), &b, sizeof(b),
					       0, 25, 1, NULL);
	for (int i = 0; w && i < 3; i++) {
		memset(frame, i, sizeof(frame));
		avi_writer_add(w, frame, sizeof(frame), 1);
	}
	CHECK(w && avi_writer_close(w) == 0);

	int opened = 0, frames = 0;
	for (size_t k = 0; k < 3; k++) {
		size_t len;
		BYTE *orig = read_back(path_of(names[k]), &len);
		CHECK(orig != NULL);
		if (!orig)
			continue;
		BYTE *buf = malloc(len);
		const char *p = path_of(k == 0 ? "cut.y4m" : k == 1 ? "cut.bmp" : "cut.avi");
		for (int iter = 0; iter < (int)len + 600; iter++) {
			size_t n = len;
			memcpy(buf, orig, len);
			if (iter < (int)len) {
				n = (size_t)iter;		/* each truncation */
			} else {
				int flips = 1 + rng() % 4;	/* then damage */
				for (int f = 0; f < flips; f++)
					buf[rng() % (len < 256 || iter % 2 ? len : 256)] = rng();
			}
			write_bytes(p, buf, n);
			struct source *s = source_open(p, NULL);
			if (!s)
				continue;
			opened++;
			const BYTE *fr;
			size_t fl;
			while (source_read(s, &fr, &fl) == 1) {
				/* Touch every byte a frame claims to have. */
				volatile BYTE sum = 0;
				for (size_t i = 0; i < fl; i++)
					sum += fr[i];
				frames++;
			}
			source_close(s);
		}
		free(buf);
		free(orig);
	}
	CHECK(opened > 0 && frames > 0);
}

int main(void)
{
	const char *t = getenv("TMPDIR");
	snprintf(dir, sizeof(dir), "%s/vfw-source-XXXXXX", t && *t ? t : "/tmp");
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	test_y4m();
	test_raw();
	test_bmp();
	test_avi();
	test_fuzz();

	char cmd[4200];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "couldn't remove %s\n", dir);
	return test_summary("test_source");
}
