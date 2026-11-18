/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RealMedia files, made here a byte at a time: the headers, video frames
 * put back together from whole-frame, split and several-frame packets,
 * audio packets as they come (Int0) and de-interleaved (genr), the sipr
 * reordering; then RealPlayer codecs (the pe_realcodec and pe_realaudio
 * fixtures) decoding them through the host.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/error.h"
#include "in/rm.h"
#include "real/real.h"
#include "test.h"

/* A growing buffer, written big-endian as RealMedia is */
struct buf {
	BYTE *p;
	size_t len, cap;
};

static void put(struct buf *b, const void *data, size_t n)
{
	if (b->len + n > b->cap) {
		b->cap = (b->len + n) * 2 + 64;
		b->p = realloc(b->p, b->cap);
		if (!b->p)
			exit(1);
	}
	memcpy(b->p + b->len, data, n);
	b->len += n;
}
static void put8(struct buf *b, unsigned v) { BYTE x = (BYTE)v; put(b, &x, 1); }
static void put16(struct buf *b, unsigned v) { put8(b, v >> 8); put8(b, v); }
static void put32(struct buf *b, uint32_t v) { put16(b, v >> 16); put16(b, v); }
static void put_str8(struct buf *b, const char *s)
{
	put8(b, (unsigned)strlen(s));
	put(b, s, strlen(s));
}
static void set32(struct buf *b, size_t at, uint32_t v)
{
	b->p[at] = (BYTE)(v >> 24);
	b->p[at + 1] = (BYTE)(v >> 16);
	b->p[at + 2] = (BYTE)(v >> 8);
	b->p[at + 3] = (BYTE)v;
}

/* Starts a chunk; end_chunk fills in its size */
static size_t chunk(struct buf *b, const char *id)
{
	size_t at = b->len;
	put(b, id, 4);
	put32(b, 0);
	put16(b, 0);
	return at;
}
static void end_chunk(struct buf *b, size_t at)
{
	set32(b, at + 4, (uint32_t)(b->len - at));
}

static void mdpr(struct buf *b, unsigned number, const char *mime,
		 const struct buf *type)
{
	size_t c = chunk(b, "MDPR");
	put16(b, number);
	for (int i = 0; i < 7; i++)
		put32(b, 1000);		/* rates, sizes, times */
	put_str8(b, "a stream");
	put_str8(b, mime);
	put32(b, (uint32_t)type->len);
	put(b, type->p, type->len);
	end_chunk(b, c);
}

/* A .ra4 header */
static void ra4(struct buf *t, unsigned flavor, unsigned cfs, unsigned h,
		unsigned w, unsigned sps, unsigned rate, unsigned ch,
		const char *interleaver, const char *fourcc)
{
	put(t, ".ra\xfd", 4);
	put16(t, 4);
	put16(t, 0);
	put(t, ".ra4", 4);
	put32(t, 0);
	put16(t, 4);
	put32(t, 0);
	put16(t, flavor);
	put32(t, cfs);
	for (int i = 0; i < 3; i++)
		put32(t, 0);
	put16(t, h);
	put16(t, w);
	put16(t, sps);
	put16(t, 0);
	put16(t, rate);
	put16(t, 0);
	put16(t, 16);
	put16(t, ch);
	put_str8(t, interleaver);
	put_str8(t, fourcc);
}

static void packet(struct buf *b, unsigned stream, uint32_t time, int key,
		   const struct buf *payload)
{
	put16(b, 0);
	put16(b, (unsigned)(12 + payload->len));
	put16(b, stream);
	put32(b, time);
	put8(b, 0);
	put8(b, key ? 2 : 0);
	put(b, payload->p, payload->len);
}

static char dir[4096], file[4200];

static void write_file(const struct buf *b)
{
	FILE *f = fopen(file, "wb");
	CHECK(f != NULL);
	if (!f)
		exit(1);
	fwrite(b->p, 1, b->len, f);
	fclose(f);
}

/*
 * Stream 0: RV20, 16x8, frames:
 *   0 ms   whole (type 1)                 50 01 02 03
 *   40 ms  in two parts (types 0 and 2)   60 09 09 | 70 08 08
 *   80 ms, 120 ms  in one packet (type 3) 80 01 | 90 02
 *   160 ms whole, starting 00 (the fixture gives no picture for it)
 * Stream 1: 8 kHz mono, Int0, flavor 1: two packets.
 * Stream 2: genr, h 4, w 8, sub-packets of 4: four packets.
 */
static void make_file(void)
{
	struct buf b = { 0 }, t = { 0 }, p = { 0 };

	size_t c = chunk(&b, ".RMF");
	put32(&b, 0);
	put32(&b, 5);
	end_chunk(&b, c);
	c = chunk(&b, "PROP");
	for (int i = 0; i < 10; i++)
		put32(&b, 0);
	put16(&b, 3);
	put16(&b, 0);
	end_chunk(&b, c);

	put32(&t, 34);
	put(&t, "VIDORV20", 8);
	put16(&t, 16);
	put16(&t, 8);
	put16(&t, 12);
	put32(&t, 0);
	put32(&t, 25 << 16);
	put32(&t, 8);			/* subformat */
	put32(&t, 0x20103001);		/* version */
	mdpr(&b, 0, "video/x-pn-realvideo", &t);
	t.len = 0;
	ra4(&t, 1, 4, 1, 4, 0, 8000, 1, "Int0", "TEST");
	mdpr(&b, 1, "audio/x-pn-realaudio", &t);
	t.len = 0;
	ra4(&t, 1, 8, 4, 8, 4, 8000, 1, "genr", "TEST");
	mdpr(&b, 2, "audio/x-pn-realaudio", &t);
	c = chunk(&b, "CONT");
	put32(&b, 0);
	end_chunk(&b, c);

	size_t data = chunk(&b, "DATA");
	put32(&b, 0);
	put32(&b, 0);
	/* 0 ms: a whole frame */
	p.len = 0;
	put(&p, "\x40\x01\x50\x01\x02\x03", 6);
	packet(&b, 0, 0, 1, &p);
	/* audio, Int0 */
	p.len = 0;
	put(&p, "\x80\x81\x7f\xff", 4);
	packet(&b, 1, 0, 1, &p);
	/* 40 ms: part 1 of 2, frame 6 bytes, at 0; frame number 2 */
	p.len = 0;
	put(&p, "\x00\x01\x40\x06\x40\x00\x02\x60\x09\x09", 10);
	packet(&b, 0, 40, 0, &p);
	/* audio, genr: packet y has bytes 16y + 0..7 */
	for (unsigned y = 0; y < 4; y++) {
		p.len = 0;
		for (unsigned i = 0; i < 8; i++)
			put8(&p, 16 * y + i);
		packet(&b, 2, 0, 1, &p);
	}
	/* 40 ms: the last part, 3 bytes */
	p.len = 0;
	put(&p, "\x80\x02\x40\x06\x40\x03\x02\x70\x08\x08", 10);
	packet(&b, 0, 40, 0, &p);
	/* audio, Int0 again */
	p.len = 0;
	put(&p, "\x00\x40", 2);
	packet(&b, 1, 40, 0, &p);
	/* 80 and 120 ms in one packet */
	p.len = 0;
	put(&p, "\xc0\x40\x02\x40\x50\x03\x80\x01", 8);
	put(&p, "\xc0\x40\x02\x40\x78\x04\x90\x02", 8);
	packet(&b, 0, 80, 0, &p);
	/* 160 ms: nothing to show */
	p.len = 0;
	put(&p, "\x40\x05\x00\x07", 4);
	packet(&b, 0, 160, 0, &p);
	end_chunk(&b, data);
	write_file(&b);
	free(b.p);
	free(t.p);
	free(p.p);
}

static void test_headers(void)
{
	struct rm_file *rm = rm_open(file);

	CHECK(rm_sniff(file));
	CHECK(rm != NULL);
	if (!rm)
		return;
	CHECK_EQ(rm_nstreams(rm), 3u);
	CHECK_EQ(rm_video_stream(rm), 0);
	CHECK_EQ(rm_audio_stream(rm), 1);
	const struct rm_stream *v = rm_stream(rm, 0);
	CHECK(v->video);
	CHECK_EQ(v->fourcc, mmioFOURCC('R', 'V', '2', '0'));
	CHECK_EQ(v->width, 16u);
	CHECK_EQ(v->height, 8u);
	CHECK_EQ(v->fps, 25u << 16);
	CHECK_EQ(v->extra_len, (size_t)8);
	const struct rm_stream *a = rm_stream(rm, 2);
	CHECK(a->audio);
	CHECK_EQ(a->version, 4u);
	CHECK_EQ(a->fourcc, mmioFOURCC('T', 'E', 'S', 'T'));
	CHECK_EQ(a->interleaver, mmioFOURCC('g', 'e', 'n', 'r'));
	CHECK_EQ(a->rate, 8000u);
	CHECK_EQ(a->channels, 1u);
	CHECK_EQ(a->sub_packet_h, 4u);
	CHECK_EQ(a->frame_size, 8u);
	CHECK_EQ(a->sub_packet_size, 4u);
	CHECK(rm_stream(rm, 3) == NULL);
	rm_close(rm);
}

/* A read gave rc 1 and want bytes, so what it gave can be looked at. */
static int read_ok(int rc, size_t len, size_t want)
{
	CHECK_EQ(rc, 1);
	if (rc != 1)
		return 0;
	CHECK_EQ(len, want);
	return len == want;
}

static void test_video(void)
{
	struct rm_file *rm = rm_open(file);
	struct rm_frame f;

	if (!rm)
		return;
	int rc = rm_read_frame(rm, 0, &f);
	if (read_ok(rc, f.len, 4)) {
		CHECK_EQ(f.nslices, 1u);
		CHECK_EQ(f.time, 0u);
		CHECK(f.key);
		CHECK(!memcmp(f.data, "\x50\x01\x02\x03", 4));
	}

	rc = rm_read_frame(rm, 0, &f);
	if (read_ok(rc, f.len, 6)) {
		CHECK_EQ(f.nslices, 2u);
		CHECK_EQ(f.offsets[0], 0u);
		CHECK_EQ(f.offsets[1], 3u);
		CHECK_EQ(f.time, 40u);
		CHECK(!f.key);
		CHECK(!memcmp(f.data, "\x60\x09\x09\x70\x08\x08", 6));
	}

	rc = rm_read_frame(rm, 0, &f);
	if (read_ok(rc, f.len, 2)) {
		CHECK_EQ(f.time, 80u);
		CHECK_EQ(f.data[0], 0x80);
	}
	CHECK_EQ(rc = rm_read_frame(rm, 0, &f), 1);
	if (rc == 1 && f.len) {
		CHECK_EQ(f.time, 120u);
		CHECK_EQ(f.data[0], 0x90);
	}

	CHECK_EQ(rm_read_frame(rm, 0, &f), 1);
	CHECK_EQ(f.time, 160u);
	CHECK_EQ(rm_read_frame(rm, 0, &f), 0);
	CHECK_EQ(rm_read_frame(rm, 1, &f), -1);	/* audio */
	rm_close(rm);
}

static void test_audio(void)
{
	struct rm_file *rm = rm_open(file);
	const BYTE *d;
	size_t len;
	uint32_t time;

	if (!rm)
		return;
	/* Int0: packets as they are */
	int rc = rm_read_audio(rm, 1, &d, &len, &time);
	if (read_ok(rc, len, 4))
		CHECK(!memcmp(d, "\x80\x81\x7f\xff", 4));
	rc = rm_read_audio(rm, 1, &d, &len, &time);
	if (read_ok(rc, len, 2))
		CHECK_EQ(time, 40u);
	CHECK_EQ(rm_read_audio(rm, 1, &d, &len, &time), 0);
	rm_close(rm);

	/*
	 * genr, h 4, w 8, sub-packets of 4: packet y's sub-packet x goes to
	 * sub-packet 4x + 2(y & 1) + y / 2 of the group, so the group is
	 * p0s0 p2s0 p1s0 p3s0 p0s1 p2s1 p1s1 p3s1, given in blocks of w.
	 */
	static const BYTE want[32] = {
		0, 1, 2, 3, 32, 33, 34, 35, 16, 17, 18, 19, 48, 49, 50, 51,
		4, 5, 6, 7, 36, 37, 38, 39, 20, 21, 22, 23, 52, 53, 54, 55,
	};
	if (!(rm = rm_open(file)))
		return;
	for (int k = 0; k < 4; k++) {
		rc = rm_read_audio(rm, 2, &d, &len, &time);
		if (read_ok(rc, len, 8))
			CHECK(!memcmp(d, want + 8 * k, 8));
	}
	CHECK_EQ(rm_read_audio(rm, 2, &d, &len, &time), 0);
	rm_close(rm);
}

static void test_sipr(void)
{
	/* 12 packets of 8 bytes: 96 blocks of a byte (two nibbles) each */
	BYTE b[96];

	for (int i = 0; i < 96; i++)
		b[i] = (BYTE)i;
	rm_sipr_reorder(b, 12, 8);
	CHECK_EQ(b[0], 63);
	CHECK_EQ(b[63], 0);
	CHECK_EQ(b[77], 80);
	CHECK_EQ(b[80], 77);
	CHECK_EQ(b[4], 4);		/* not one of the pairs */
	/* Swapping pairs back puts it as it was */
	rm_sipr_reorder(b, 12, 8);
	for (int i = 0; i < 96; i++)
		CHECK_EQ(b[i], i);
}

/*
 * Damage the reader turns away: a last part longer than what's left of
 * its packet, a frame cut off at the end, Int4 of one row, genr rows of
 * part sub-packets.
 */
static void test_malformed(void)
{
	struct buf b = { 0 }, t = { 0 }, p = { 0 };
	struct rm_frame f;
	const BYTE *d;
	size_t len;
	uint32_t time;

	size_t c = chunk(&b, ".RMF");
	put32(&b, 0);
	put32(&b, 4);
	end_chunk(&b, c);
	put32(&t, 34);
	put(&t, "VIDORV20", 8);
	put16(&t, 16);
	put16(&t, 8);
	put16(&t, 12);
	put32(&t, 0);
	put32(&t, 25 << 16);
	put32(&t, 8);
	put32(&t, 0x20103001);
	mdpr(&b, 0, "video/x-pn-realvideo", &t);
	t.len = 0;
	ra4(&t, 1, 4, 1, 8, 0, 8000, 1, "Int4", "TEST");
	mdpr(&b, 1, "audio/x-pn-realaudio", &t);
	t.len = 0;
	ra4(&t, 1, 8, 4, 8, 3, 8000, 1, "genr", "TEST");
	mdpr(&b, 2, "audio/x-pn-realaudio", &t);
	size_t data = chunk(&b, "DATA");
	put32(&b, 0);
	put32(&b, 0);
	/* Frame 2: part 1 of 6 bytes, then a last part claiming 9 */
	put(&p, "\x00\x01\x40\x06\x40\x00\x02\x60\x09\x09", 10);
	packet(&b, 0, 0, 1, &p);
	p.len = 0;
	put(&p, "\x80\x02\x40\x06\x40\x09\x02\x70\x08\x08", 10);
	packet(&b, 0, 0, 1, &p);
	/* Frame 3: the first half, then the file ends */
	p.len = 0;
	put(&p, "\x00\x01\x40\x06\x40\x00\x03\x60\x09\x09", 10);
	packet(&b, 0, 40, 0, &p);
	p.len = 0;
	for (unsigned i = 0; i < 8; i++)
		put8(&p, i);
	packet(&b, 1, 0, 1, &p);
	packet(&b, 2, 0, 1, &p);
	end_chunk(&b, data);
	write_file(&b);

	struct rm_file *rm = rm_open(file);
	CHECK(rm != NULL);
	if (rm) {
		/* Neither frame is whole: none comes out */
		CHECK_EQ(rm_read_frame(rm, 0, &f), 0);
		rm_close(rm);
	}
	if ((rm = rm_open(file))) {
		CHECK_EQ(rm_read_audio(rm, 1, &d, &len, &time), -1);
		CHECK(strstr(vfw_error(), "'Int4' interleaving") != NULL);
		CHECK_EQ(rm_read_audio(rm, 2, &d, &len, &time), -1);
		CHECK(strstr(vfw_error(), "'genr' interleaving") != NULL);
		rm_close(rm);
	}
	free(b.p);
	free(t.p);
	free(p.p);
}

static void test_garbage(void)
{
	struct buf b = { 0 };

	/* A whole head, but a size smaller than one */
	put(&b, ".RMF", 4);
	put32(&b, 4);
	put16(&b, 0);
	write_file(&b);
	CHECK(rm_open(file) == NULL);
	CHECK(strstr(vfw_error(), "a .RMF chunk of 4 bytes") != NULL);
	/* The same in a later chunk, after a good .RMF */
	b.len = 0;
	put(&b, ".RMF", 4);
	put32(&b, 18);
	put16(&b, 0);
	put32(&b, 0);
	put32(&b, 0);
	put(&b, "PROP", 4);
	put32(&b, 4);
	put16(&b, 0);
	write_file(&b);
	CHECK(rm_open(file) == NULL);
	CHECK(strstr(vfw_error(), "a chunk of 4 bytes") != NULL);
	b.len = 0;
	put(&b, "RIFF....WAVE", 12);
	write_file(&b);
	CHECK(!rm_sniff(file));
	CHECK(rm_open(file) == NULL);
	free(b.p);
}

/*
 * The host, with the fixtures (dir/name; dir is $VFW_FIXTURES): skipped
 * without them, but the parser's checks above still count.
 */
static void test_host(const char *dir)
{
	char vdll[4096], adll[4096];
	const char *entry;

	snprintf(vdll, sizeof(vdll), "%s/pe_realcodec.dll", dir);
	snprintf(adll, sizeof(adll), "%s/pe_realaudio.dll", dir);
	if (access(vdll, R_OK) || access(adll, R_OK)) {
		printf("fixtures not available in %s: host checks skipped\n",
		       dir);
		return;
	}

	CHECK_EQ(real_kind(vdll, &entry), REAL_VIDEO);
	CHECK_STR(entry, "RV20toYUV420");
	CHECK_EQ(real_kind(adll, NULL), REAL_AUDIO);

	make_file();
	struct rm_file *rm = rm_open(file);
	if (!rm)
		return;
	struct real_video *v = real_video_open(vdll, rm_stream(rm, 0));
	CHECK(v != NULL);
	if (v) {
		struct rm_frame f;
		const BYTE *pic;
		int w, h, n = 0, none = 0;
		while (rm_read_frame(rm, 0, &f) == 1) {
			int rc = real_video_decode(v, &f, &pic, &w, &h);
			CHECK(rc >= 0);
			if (rc <= 0) {
				none++;
				continue;
			}
			CHECK_EQ(w, 16);
			CHECK_EQ(h, 8);
			/* Band per slice: the slice table was right */
			if (f.nslices == 2) {
				CHECK_EQ(pic[0], 0x60);
				CHECK_EQ(pic[16 * 3 + 15], 0x60);
				CHECK_EQ(pic[16 * 4], 0x70);
				CHECK_EQ(pic[16 * 7 + 15], 0x70);
			} else {
				CHECK_EQ(pic[16 * 7], f.data[0]);
			}
			CHECK_EQ(pic[16 * 8], 128);
			n++;
		}
		CHECK_EQ(n, 4);
		CHECK_EQ(none, 1);
		real_video_close(v);
	}
	rm_close(rm);

	if (!(rm = rm_open(file)))
		return;
	struct real_audio *a = real_audio_open(adll, rm_stream(rm, 1));
	CHECK(a != NULL);
	if (a) {
		const WAVEFORMATEX *wf = real_audio_format(a);
		const BYTE *in, *pcm;
		size_t len, plen;
		uint32_t time;
		CHECK_EQ(wf->nSamplesPerSec, 8000u);
		CHECK_EQ(wf->nChannels, 1);
		int rc = rm_read_audio(rm, 1, &in, &len, &time);
		CHECK_EQ(rc, 1);
		if (rc == 1) {
			rc = real_audio_decode(a, in, len, &pcm, &plen);
			CHECK_EQ(rc, 0);
		}
		if (rc == 0) {
			CHECK_EQ(plen, (size_t)8);
		}
		if (rc == 0 && plen == 8) {
			short s[4];
			memcpy(s, pcm, sizeof(s));
			CHECK_EQ(s[0], 0);
			CHECK_EQ(s[1], 256);
			CHECK_EQ(s[2], -256);
			CHECK_EQ(s[3], 127 * 256);
		}
		real_audio_close(a);
	}
	/* Each DLL refuses the other kind of stream */
	CHECK(real_video_open(adll, rm_stream(rm, 0)) == NULL);
	CHECK(real_audio_open(vdll, rm_stream(rm, 1)) == NULL);
	rm_close(rm);
}

int main(void)
{
	const char *t = getenv("TMPDIR");

	snprintf(dir, sizeof(dir), "%s/vfw-rm-XXXXXX", t && *t ? t : "/tmp");
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(file, sizeof(file), "%s/test.rm", dir);
	make_file();
	test_headers();
	test_video();
	test_audio();
	test_sipr();
	test_malformed();
	test_garbage();
	const char *fx = getenv("VFW_FIXTURES");
	if (fx && *fx)
		test_host(fx);
	else
		printf("VFW_FIXTURES not set: host checks skipped\n");
	int status = test_summary("test_rm");
	unlink(file);
	rmdir(dir);
	return status;
}
