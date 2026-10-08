/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AVI reader and writer: round trips through every index kind, OpenDML
 * files with several RIFF chunks, audio streams beside video and alone,
 * and damaged files.
 *
 * With VFW_KEEP_TEST_FILES=<dir> the files are written there and kept,
 * for tests/validate_media.sh to check with ffprobe.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>

#include "avi/avi.h"
#include "common/error.h"
#include "common/fourcc.h"
#include "media/audio.h"
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
	snprintf(dir, sizeof(dir), "%s/vfw-avi-XXXXXX", t && *t ? t : "/tmp");
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

/* Deterministic frame contents: frame n, byte i. */
static BYTE pattern(unsigned n, size_t i)
{
	return (BYTE)(n * 31 + i * 7 + (i >> 8));
}

static void fill(BYTE *buf, unsigned n, size_t size)
{
	for (size_t i = 0; i < size; i++)
		buf[i] = pattern(n, i);
}

static int frame_ok(const BYTE *buf, unsigned n, size_t size)
{
	for (size_t i = 0; i < size; i++)
		if (buf[i] != pattern(n, i))
			return 0;
	return 1;
}

static BITMAPINFOHEADER rgb_format(int w, int h, int bits)
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

/* Writes n frames of `size` bytes, keyframes every `gop`. */
static int write_file(const char *name, const BITMAPINFOHEADER *bih,
		      unsigned n, DWORD size, unsigned gop,
		      const struct avi_writer_options *opts)
{
	struct avi_writer *w = avi_writer_open(path_of(name), bih, sizeof(*bih),
					       0, 25, 1, opts);
	BYTE *buf = malloc(size ? size : 1);

	if (!w || !buf) {
		fprintf(stderr, "writer: %s\n", vfw_error());
		free(buf);
		return -1;
	}
	for (unsigned i = 0; i < n; i++) {
		fill(buf, i, size);
		if (avi_writer_add(w, buf, size, i % gop == 0)) {
			fprintf(stderr, "add: %s\n", vfw_error());
			avi_writer_close(w);
			free(buf);
			return -1;
		}
	}
	free(buf);
	return avi_writer_close(w);
}

/* Checks every frame of stream 0 against the pattern. */
static void check_frames(struct avi_file *avi, unsigned n, DWORD size,
			 unsigned gop, int keyflags)
{
	struct avi_stream *s = &avi->streams[0];
	BYTE *buf = malloc(size ? size : 1);
	unsigned bad = 0, badkey = 0;

	CHECK_EQ(s->nchunks, n);
	CHECK_EQ(s->frames, n);
	for (size_t i = 0; i < s->nchunks && i < n; i++) {
		struct avi_chunk *c = &s->chunks[i];
		if (c->size != size || avi_read_chunk(avi, c, buf) ||
		    !frame_ok(buf, i, size))
			bad++;
		int key = (c->flags & AVIIF_KEYFRAME) != 0;
		if (keyflags ? key != (i % gop == 0) : !key)
			badkey++;
	}
	CHECK_EQ(bad, 0);
	CHECK_EQ(badkey, 0);
	free(buf);
}

static void test_plain(void)
{
	BITMAPINFOHEADER bih = rgb_format(32, 16, 24);

	CHECK_EQ(write_file("plain.avi", &bih, 20, bih.biSizeImage, 5, NULL), 0);
	struct avi_file *avi = avi_open(path_of("plain.avi"));
	CHECK(avi);
	if (!avi)
		return;
	CHECK_EQ(avi->nstreams, 1);
	CHECK_EQ(avi->riff_count, 1);
	CHECK_EQ(avi->index_source, AVI_INDEX_IDX1);
	CHECK_EQ(avi->truncated, 0);
	CHECK_EQ(avi->header.dwTotalFrames, 20);
	CHECK_EQ(avi->total_frames, 20);
	CHECK_EQ(avi->header.dwWidth, 32);
	CHECK_EQ(avi->header.dwHeight, 16);
	CHECK_EQ(avi->header.dwMicroSecPerFrame, 40000);
	CHECK(avi->header.dwFlags & AVIF_HASINDEX);
	CHECK_EQ(avi_video_stream(avi), 0);

	struct avi_stream *s = &avi->streams[0];
	CHECK_EQ(s->header.fccType, FCC('v', 'i', 'd', 's'));
	CHECK_EQ(s->header.dwRate, 25);
	CHECK_EQ(s->header.dwScale, 1);
	CHECK_EQ(s->header.dwLength, 20);
	CHECK_EQ(s->header.rcFrame.right, 32);
	CHECK_EQ(s->header.rcFrame.bottom, 16);
	const BITMAPINFOHEADER *b = avi_stream_bih(s);
	CHECK(b && !memcmp(b, &bih, sizeof(bih)));
	check_frames(avi, 20, bih.biSizeImage, 5, 1);
	/* BI_RGB frames are "db" chunks. */
	CHECK_EQ(s->chunks[0].id, FCC('0', '0', 'd', 'b'));
	avi_close(avi);
}

static void test_idx1_absolute(void)
{
	BITMAPINFOHEADER bih = rgb_format(8, 8, 24);
	struct avi_writer_options o = { .idx1_absolute = 1 };

	CHECK_EQ(write_file("idx1abs.avi", &bih, 7, bih.biSizeImage, 3, &o), 0);
	struct avi_file *avi = avi_open(path_of("idx1abs.avi"));
	CHECK(avi);
	if (!avi)
		return;
	CHECK_EQ(avi->index_source, AVI_INDEX_IDX1);
	check_frames(avi, 7, bih.biSizeImage, 3, 1);
	avi_close(avi);
	cleanup("idx1abs.avi");
}

static void test_scan(void)
{
	BITMAPINFOHEADER bih = rgb_format(8, 8, 24);
	struct avi_writer_options o = { .no_idx1 = 1 };

	CHECK_EQ(write_file("noindex.avi", &bih, 9, bih.biSizeImage, 4, &o), 0);
	struct avi_file *avi = avi_open(path_of("noindex.avi"));
	CHECK(avi);
	if (!avi)
		return;
	CHECK_EQ(avi->index_source, AVI_INDEX_SCAN);
	CHECK_EQ(avi->header.dwFlags & AVIF_HASINDEX, 0);
	/* A scan can't know keyframes: they all count as ones. */
	check_frames(avi, 9, bih.biSizeImage, 4, 0);
	avi_close(avi);
	cleanup("noindex.avi");
}

static void test_odml_small(void)
{
	BITMAPINFOHEADER bih = rgb_format(8, 8, 24);
	struct avi_writer_options o = { .force_odml = 1 };

	CHECK_EQ(write_file("odml1.avi", &bih, 12, bih.biSizeImage, 4, &o), 0);
	struct avi_file *avi = avi_open(path_of("odml1.avi"));
	CHECK(avi);
	if (!avi)
		return;
	CHECK_EQ(avi->riff_count, 1);
	CHECK_EQ(avi->index_source, AVI_INDEX_ODML);
	CHECK_EQ(avi->total_frames, 12);
	check_frames(avi, 12, bih.biSizeImage, 4, 1);
	avi_close(avi);
}

/* Replaces the first `from` FOURCC in a file with `to`. */
static int patch_fourcc(const char *path, FOURCC from, FOURCC to)
{
	void *buf;
	size_t len;

	if (vfw_read_file(path, &buf, &len))
		return -1;
	int r = -1;
	for (size_t i = 0; i + 4 <= len; i++) {
		if (!memcmp((BYTE *)buf + i, &from, 4)) {
			int fd = open(path, O_WRONLY);
			r = fd >= 0 && pwrite(fd, &to, 4, i) == 4 ? 0 : -1;
			if (fd >= 0)
				close(fd);
			break;
		}
	}
	free(buf);
	return r;
}

static void test_odml_segments(void)
{
	/* 3000-byte frames, a 64 KB RIFF limit: about 20 frames a RIFF. */
	BITMAPINFOHEADER bih = { 0 };
	bih.biSize = sizeof(bih);
	bih.biWidth = 40;
	bih.biHeight = 25;
	bih.biPlanes = 1;
	bih.biBitCount = 24;
	bih.biCompression = FCC('T', 'E', 'S', 'T');
	struct avi_writer_options o = { .riff_limit = 64 << 10 };
	const unsigned n = 150;

	CHECK_EQ(write_file("odml.avi", &bih, n, 3000, 10, &o), 0);
	struct avi_file *avi = avi_open(path_of("odml.avi"));
	CHECK(avi);
	if (!avi)
		return;
	CHECK(avi->riff_count > 5);
	CHECK_EQ(avi->index_source, AVI_INDEX_ODML);
	CHECK_EQ(avi->total_frames, n);
	CHECK(avi->header.dwTotalFrames < n);	/* the first RIFF only */
	CHECK_EQ(avi->streams[0].header.dwLength, n);
	CHECK_EQ(avi->streams[0].chunks[0].id, FCC('0', '0', 'd', 'c'));
	check_frames(avi, n, 3000, 10, 1);
	/* Every RIFF but the last is within the limit. */
	CHECK(avi->streams[0].chunks[n - 1].offset > (uint64_t)5 * (64 << 10));
	unsigned riffs = avi->riff_count;
	avi_close(avi);

	/*
	 * Without the OpenDML index, a reader falls back on idx1 for the
	 * first RIFF and scans the AVIX ones.
	 */
	char copy[4200];
	void *buf;
	size_t len;
	snprintf(copy, sizeof(copy), "%s", path_of("odml_noindx.avi"));
	CHECK_EQ(vfw_read_file(path_of("odml.avi"), &buf, &len), 0);
	int fd = open(copy, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0 && write(fd, buf, len) == (ssize_t)len);
	close(fd);
	free(buf);
	CHECK_EQ(patch_fourcc(copy, FCC('i', 'n', 'd', 'x'),
			      FCC('J', 'U', 'N', 'K')), 0);
	avi = avi_open(copy);
	CHECK(avi);
	if (avi) {
		CHECK_EQ(avi->index_source, AVI_INDEX_IDX1);
		CHECK_EQ(avi->riff_count, riffs);
		struct avi_stream *s = &avi->streams[0];
		CHECK_EQ(s->nchunks, n);
		unsigned bad = 0;
		BYTE f[3000];
		for (size_t i = 0; i < s->nchunks; i++)
			bad += s->chunks[i].size != 3000 ||
			       avi_read_chunk(avi, &s->chunks[i], f) ||
			       !frame_ok(f, i, 3000);
		CHECK_EQ(bad, 0);
		avi_close(avi);
	}
	cleanup("odml_noindx.avi");
}

/* Frames of every size from 0 up, odd ones padded. */
static void test_sizes(void)
{
	BITMAPINFOHEADER bih = rgb_format(4, 4, 24);
	bih.biCompression = FCC('T', 'E', 'S', 'T');
	struct avi_writer *w = avi_writer_open(path_of("sizes.avi"), &bih,
					       sizeof(bih), FCC('t', 'e', 's', 't'),
					       30000, 1001, NULL);
	BYTE buf[64];

	CHECK(w);
	if (!w)
		return;
	for (unsigned i = 0; i < 40; i++) {
		fill(buf, i, i);
		CHECK_EQ(avi_writer_add(w, buf, i, i == 0), 0);
	}
	CHECK_EQ(avi_writer_close(w), 0);

	struct avi_file *avi = avi_open(path_of("sizes.avi"));
	CHECK(avi);
	if (!avi)
		return;
	struct avi_stream *s = &avi->streams[0];
	CHECK_EQ(s->header.fccHandler, FCC('t', 'e', 's', 't'));
	CHECK_EQ(s->header.dwRate, 30000);
	CHECK_EQ(s->header.dwScale, 1001);
	CHECK_EQ(avi->header.dwMicroSecPerFrame, 33366);
	CHECK_EQ(s->nchunks, 40);
	unsigned bad = 0;
	for (size_t i = 0; i < s->nchunks; i++)
		bad += s->chunks[i].size != i ||
		       avi_read_chunk(avi, &s->chunks[i], buf) ||
		       !frame_ok(buf, i, i) ||
		       ((s->chunks[i].flags & AVIIF_KEYFRAME) != 0) != (i == 0);
	CHECK_EQ(bad, 0);
	avi_close(avi);
	cleanup("sizes.avi");
}

static WAVEFORMATEX pcm_format(WORD channels, DWORD rate)
{
	WAVEFORMATEXTENSIBLE x;
	audio_pcm_format(AUDIO_S16, channels, rate, 0, AUDIO_FORM_PLAIN, &x);
	return x.Format;
}

/*
 * Video at 25 frames per second and 16-bit stereo PCM at 44100 Hz,
 * interleaved: a frame, then its 1764 samples in chunks of `per` samples.
 */
static int write_av(const char *name, unsigned frames, unsigned per,
		    const struct avi_writer_options *opts)
{
	BITMAPINFOHEADER bih = rgb_format(32, 16, 24);
	WAVEFORMATEX wf = pcm_format(2, 44100);
	struct avi_writer *w = avi_writer_create(path_of(name), opts);
	BYTE frame[32 * 16 * 3], audio[4 * 1764];
	unsigned sample = 0;
	int v, a;

	if (!w)
		return -1;
	v = avi_writer_add_video(w, &bih, sizeof(bih), 0, 25, 1);
	a = avi_writer_add_audio(w, &wf, sizeof(wf), 0, 0, 0);
	if (v != 0 || a != 1) {
		avi_writer_close(w);
		return -1;
	}
	for (unsigned f = 0; f < frames; f++) {
		fill(frame, f, sizeof(frame));
		if (avi_writer_write(w, v, frame, sizeof(frame), f % 5 == 0))
			break;
		for (unsigned done = 0; done < 1764; done += per) {
			unsigned n = 1764 - done < per ? 1764 - done : per;
			for (unsigned i = 0; i < 4 * n; i++)
				audio[i] = pattern(1000, 4 * (size_t)sample + i);
			sample += n;
			if (avi_writer_write(w, a, audio, 4 * n, 0))
				break;
		}
	}
	return avi_writer_close(w);
}

/* Checks the audio stream of a write_av file: every sample, in order. */
static void check_av_audio(struct avi_file *avi, unsigned frames)
{
	struct avi_stream *s = &avi->streams[1];
	uint64_t at = 0;
	unsigned bad = 0, badstart = 0;
	BYTE buf[4 * 1764];

	CHECK_EQ(s->length, 1764ull * frames);
	for (size_t c = 0; c < s->nchunks; c++) {
		struct avi_chunk *k = &s->chunks[c];
		badstart += k->start != at / 4 ||
			    (k->flags & AVIIF_KEYFRAME) == 0 ||
			    k->id != FCC('0', '1', 'w', 'b');
		if (k->size > sizeof(buf) || avi_read_chunk(avi, k, buf)) {
			bad++;
			continue;
		}
		for (unsigned i = 0; i < k->size; i++)
			bad += buf[i] != pattern(1000, at + i);
		at += k->size;
	}
	CHECK_EQ(at, 4 * 1764ull * frames);
	CHECK_EQ(bad, 0);
	CHECK_EQ(badstart, 0);
}

static void test_av(void)
{
	CHECK_EQ(write_av("av.avi", 10, 600, NULL), 0);
	struct avi_file *avi = avi_open(path_of("av.avi"));
	CHECK(avi);
	if (!avi)
		return;
	CHECK_EQ(avi->nstreams, 2);
	CHECK_EQ(avi->index_source, AVI_INDEX_IDX1);
	CHECK_EQ(avi_video_stream(avi), 0);
	CHECK_EQ(avi_audio_stream(avi), 1);
	CHECK_EQ(avi->header.dwStreams, 2);
	CHECK_EQ(avi->header.dwTotalFrames, 10);
	CHECK_EQ(avi->header.dwMicroSecPerFrame, 40000);
	CHECK_EQ(avi->header.dwMaxBytesPerSec, 1536 * 25 + 176400);

	struct avi_stream *v = &avi->streams[0], *a = &avi->streams[1];
	CHECK_EQ(v->nchunks, 10);
	CHECK_EQ(v->length, 10);
	CHECK_EQ(v->chunks[3].start, 3);
	CHECK_EQ(avi_stream_time(v, 3), 1200000);
	CHECK(avi_stream_wfx(v) == NULL);
	CHECK(avi_stream_bih(a) == NULL);

	CHECK_EQ(a->header.fccType, FCC('a', 'u', 'd', 's'));
	CHECK_EQ(a->header.dwScale, 1);
	CHECK_EQ(a->header.dwRate, 44100);
	CHECK_EQ(a->header.dwSampleSize, 4);
	CHECK_EQ(a->header.dwLength, 17640);
	CHECK_EQ(a->header.dwSuggestedBufferSize, 2400);
	CHECK_EQ(a->nchunks, 30);
	const WAVEFORMATEX *wf = avi_stream_wfx(a);
	WAVEFORMATEX want = pcm_format(2, 44100);
	CHECK(wf && a->format_size == sizeof(want) &&
	      !memcmp(wf, &want, sizeof(want)));
	/* 600, 600 and 564 samples a frame. */
	CHECK_EQ(a->chunks[2].size, 564 * 4);
	CHECK_EQ(a->chunks[4].start, 1764 + 600);
	CHECK_EQ(avi_stream_time(a, a->chunks[3].start), 400000);
	check_av_audio(avi, 10);
	avi_close(avi);

	/* The same, indexed by a scan of the movi list. */
	struct avi_writer_options o = { .no_idx1 = 1 };
	CHECK_EQ(write_av("av_scan.avi", 4, 1764, &o), 0);
	avi = avi_open(path_of("av_scan.avi"));
	CHECK(avi);
	if (avi) {
		CHECK_EQ(avi->index_source, AVI_INDEX_SCAN);
		CHECK_EQ(avi->streams[1].nchunks, 4);
		check_av_audio(avi, 4);
		avi_close(avi);
	}
	cleanup("av_scan.avi");
}

/* Video and audio over several RIFF chunks, and with the idx1 only. */
static void test_av_odml(void)
{
	struct avi_writer_options o = { .riff_limit = 64 << 10 };
	unsigned n = 60;

	CHECK_EQ(write_av("av_odml.avi", n, 1000, &o), 0);
	struct avi_file *avi = avi_open(path_of("av_odml.avi"));
	CHECK(avi);
	if (!avi)
		return;
	CHECK(avi->riff_count > 3);
	CHECK_EQ(avi->index_source, AVI_INDEX_ODML);
	CHECK_EQ(avi->total_frames, n);
	CHECK_EQ(avi->streams[0].nchunks, n);
	CHECK_EQ(avi->streams[0].chunks[0].id, FCC('0', '0', 'd', 'b'));
	CHECK_EQ(avi->streams[1].nchunks, 2 * n);
	CHECK_EQ(avi->streams[1].header.dwLength, 1764 * n);
	check_av_audio(avi, n);
	int riffs = (int)avi->riff_count;
	avi_close(avi);

	/* Without the OpenDML indexes: idx1, then a scan of the AVIX parts. */
	void *buf;
	size_t len;
	const char *copy = path_of("av_odml_idx1.avi");
	CHECK_EQ(vfw_read_file(path_of("av_odml.avi"), &buf, &len), 0);
	int fd = open(copy, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0 && write(fd, buf, len) == (ssize_t)len);
	close(fd);
	free(buf);
	CHECK_EQ(patch_fourcc(copy, FCC('i', 'n', 'd', 'x'),
			      FCC('J', 'U', 'N', 'K')), 0);
	avi = avi_open(copy);
	CHECK(avi);
	if (avi) {
		CHECK_EQ(avi->index_source, AVI_INDEX_IDX1);
		CHECK_EQ(avi->riff_count, riffs);
		CHECK_EQ(avi->streams[0].nchunks, n);
		check_av_audio(avi, n);
		avi_close(avi);
	}
	unlink(copy);
}

/* Audio alone, constant rate (compressed blocks) and variable rate. */
static void test_audio_only(void)
{
	/* An IMA ADPCM-like format: 256-byte blocks of 505 samples. */
	BYTE fmt[20] = { 0 };
	WAVEFORMATEX *wf = (WAVEFORMATEX *)fmt;
	wf->wFormatTag = WAVE_FORMAT_IMA_ADPCM;
	wf->nChannels = 1;
	wf->nSamplesPerSec = 22050;
	wf->nAvgBytesPerSec = 11155;
	wf->nBlockAlign = 256;
	wf->wBitsPerSample = 4;
	wf->cbSize = 2;
	fmt[18] = 0xf9;	/* 505 samples a block */
	fmt[19] = 0x01;
	BYTE block[512];

	struct avi_writer *w = avi_writer_create(path_of("audio.avi"), NULL);
	CHECK(w);
	if (!w)
		return;
	CHECK_EQ(avi_writer_add_audio(w, fmt, sizeof(fmt), 0, 0, 0), 0);
	for (unsigned i = 0; i < 6; i++) {
		fill(block, i, sizeof(block));
		/* One block, then two. */
		CHECK_EQ(avi_writer_write(w, 0, block, 256 * (1 + i % 2), 0), 0);
	}
	/* Streams can't be added once chunks are written. */
	CHECK_EQ(avi_writer_add_audio(w, fmt, sizeof(fmt), 0, 0, 0), -1);
	CHECK_EQ(avi_writer_write(w, 1, block, 4, 0), -1);
	CHECK_EQ(avi_writer_close(w), 0);

	struct avi_file *avi = avi_open(path_of("audio.avi"));
	CHECK(avi);
	if (!avi)
		return;
	CHECK_EQ(avi_video_stream(avi), -1);
	CHECK_EQ(avi_audio_stream(avi), 0);
	CHECK_EQ(avi->header.dwTotalFrames, 0);
	CHECK_EQ(avi->header.dwMicroSecPerFrame, 0);
	struct avi_stream *s = &avi->streams[0];
	CHECK_EQ(s->header.dwScale, 256);
	CHECK_EQ(s->header.dwRate, 11155);
	CHECK_EQ(s->header.dwSampleSize, 256);
	CHECK_EQ(s->header.dwLength, 9);
	CHECK_EQ(s->length, 9);
	CHECK_EQ(s->chunks[0].id, FCC('0', '0', 'w', 'b'));
	CHECK_EQ(s->chunks[3].start, 4);
	CHECK_EQ(avi_stream_time(s, 4), 4ll * 256 * 10000000 / 11155);
	CHECK_EQ(s->format_size, sizeof(fmt));
	CHECK(!memcmp(s->format, fmt, sizeof(fmt)));
	avi_close(avi);

	/* Variable rate: one 1152-sample frame a chunk. */
	WAVEFORMATEX mp3 = { WAVE_FORMAT_MPEGLAYER3, 2, 44100, 16000, 1152, 0, 0 };
	w = avi_writer_create(path_of("vbr.avi"), NULL);
	CHECK(w);
	if (!w)
		return;
	CHECK_EQ(avi_writer_add_audio(w, &mp3, sizeof(mp3), 44100, 1152, 0), 0);
	for (unsigned i = 0; i < 20; i++) {
		fill(block, i, 100 + i);
		CHECK_EQ(avi_writer_write(w, 0, block, 100 + i, 0), 0);
	}
	CHECK_EQ(avi_writer_close(w), 0);
	avi = avi_open(path_of("vbr.avi"));
	CHECK(avi);
	if (!avi)
		return;
	s = &avi->streams[0];
	CHECK_EQ(s->header.dwSampleSize, 0);
	CHECK_EQ(s->header.dwLength, 20);
	CHECK_EQ(s->length, 20);
	CHECK_EQ(s->chunks[7].start, 7);
	CHECK_EQ(s->chunks[7].size, 107);
	CHECK_EQ(avi_stream_time(s, 7), 7ll * 1152 * 10000000 / 44100);
	avi_close(avi);
}

/* Audio formats as files have them: short, or with a cbSize too big. */
static void test_audio_formats(void)
{
	WAVEFORMATEX wf = pcm_format(1, 8000);
	BYTE big[24];
	struct avi_writer *w;
	struct avi_file *avi;

	w = avi_writer_create(path_of("fmt16.avi"), NULL);
	CHECK(w && avi_writer_add_audio(w, &wf, 16, 0, 0, 0) == 0);
	CHECK(w && avi_writer_write(w, 0, "abcd", 4, 0) == 0);
	CHECK(w && avi_writer_close(w) == 0);
	avi = avi_open(path_of("fmt16.avi"));
	CHECK(avi);
	if (avi) {
		const WAVEFORMATEX *got = avi_stream_wfx(&avi->streams[0]);
		CHECK(got != NULL);
		CHECK_EQ(avi->streams[0].format_size, 18);
		CHECK(got && got->cbSize == 0 && got->nSamplesPerSec == 8000);
		avi_close(avi);
	}

	memset(big, 0x55, sizeof(big));
	memcpy(big, &wf, 16);
	((WAVEFORMATEX *)big)->cbSize = 40;
	w = avi_writer_create(path_of("fmtbig.avi"), NULL);
	CHECK(w && avi_writer_add_audio(w, big, sizeof(big), 0, 0, 0) == 0);
	CHECK(w && avi_writer_close(w) == 0);
	avi = avi_open(path_of("fmtbig.avi"));
	CHECK(avi);
	if (avi) {
		const WAVEFORMATEX *got = avi_stream_wfx(&avi->streams[0]);
		CHECK(got && got->cbSize == 6);
		CHECK_EQ(avi->streams[0].nchunks, 0);
		CHECK_EQ(avi->streams[0].length, 0);
		avi_close(avi);
	}
	/* A start offset counts in the times. */
	avi = avi_open(path_of("fmt16.avi"));
	if (avi) {
		avi->streams[0].header.dwStart = 8000;
		CHECK_EQ(avi_stream_time(&avi->streams[0], 4000), 15000000);
		avi_close(avi);
	}
	/* A bad audio format isn't written. */
	w = avi_writer_create(path_of("fmtbad.avi"), NULL);
	wf.nBlockAlign = 0;
	CHECK(w && avi_writer_add_audio(w, &wf, sizeof(wf), 0, 0, 0) == -1);
	/* Nor is a file without streams. */
	CHECK(w && avi_writer_close(w) == -1);
	cleanup("fmt16.avi");
	cleanup("fmtbig.avi");
	unlink(path_of("fmtbad.avi"));
}

static void test_bad_files(void)
{
	const char *p = path_of("bad.avi");
	int fd;

	CHECK(!avi_open(path_of("does-not-exist.avi")));
	CHECK(strstr(vfw_error(), "does-not-exist"));

	fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	close(fd);
	CHECK(!avi_open(p));
	CHECK(strstr(vfw_error(), "not an AVI"));

	fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(write(fd, "RIFF\x04\0\0\0WAVE", 12) == 12);
	close(fd);
	CHECK(!avi_open(p));

	/* An AVI with no header list. */
	fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(write(fd, "RIFF\x04\0\0\0AVI ", 12) == 12);
	close(fd);
	CHECK(!avi_open(p));
	CHECK(strstr(vfw_error(), "hdrl"));
	unlink(p);
}

/*
 * Every chunk the reader reports must lie inside the file and read back,
 * however the file was damaged. (Run under ASan by the asan preset.)
 */
static unsigned check_sane(const char *path, unsigned *opened)
{
	struct avi_file *avi = avi_open(path);
	unsigned bad = 0;

	if (!avi)
		return 0;
	++*opened;
	for (unsigned i = 0; i < avi->nstreams; i++) {
		struct avi_stream *s = &avi->streams[i];
		for (size_t c = 0; c < s->nchunks; c++) {
			struct avi_chunk *k = &s->chunks[c];
			BYTE *b = malloc(k->size ? k->size : 1);
			if (k->offset > avi->size ||
			    k->size > avi->size - k->offset ||
			    !b || avi_read_chunk(avi, k, b))
				bad++;
			free(b);
		}
		avi_stream_bih(s);
		const WAVEFORMATEX *wf = avi_stream_wfx(s);
		if (wf && (s->format_size < sizeof(*wf) ||
			   wf->cbSize > s->format_size - sizeof(*wf)))
			bad++;
		if (s->nchunks && s->chunks[s->nchunks - 1].start > s->length)
			bad++;
	}
	avi_close(avi);
	return bad;
}

static void test_damage(const char *name)
{
	void *orig;
	size_t len;
	const char *p = path_of("damaged.avi");
	unsigned bad = 0, opened = 0;

	if (vfw_read_file(path_of(name), &orig, &len)) {
		CHECK(0);
		return;
	}

	/* Truncated at many points, including inside the headers. */
	for (size_t cut = 0; cut < len; cut += cut < 4096 ? 13 : 1777) {
		int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (write(fd, orig, cut) != (ssize_t)cut)
			bad++;
		close(fd);
		bad += check_sane(p, &opened);
	}
	CHECK_EQ(bad, 0);
	CHECK(opened > 10);

	/* Truncated in the last RIFF: the frames before the cut survive. */
	{
		int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		CHECK(write(fd, orig, len - 20000) == (ssize_t)(len - 20000));
		close(fd);
		struct avi_file *avi = avi_open(p);
		CHECK(avi);
		if (avi) {
			CHECK(avi->truncated);
			CHECK(avi->streams[0].nchunks > 30);
			avi_close(avi);
		}
	}

	/* Random bytes changed, in the headers and anywhere. */
	uint32_t seed = 12345;
	BYTE *copy = malloc(len);
	opened = 0;
	for (int round = 0; round < 300; round++) {
		memcpy(copy, orig, len);
		for (int k = 0; k < 8; k++) {
			seed = seed * 1103515245 + 12345;
			size_t at = round & 1 ? (seed >> 8) % 4096 : (seed >> 8) % len;
			seed = seed * 1103515245 + 12345;
			copy[at] = (BYTE)(seed >> 16);
		}
		int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (write(fd, copy, len) != (ssize_t)len)
			bad++;
		close(fd);
		bad += check_sane(p, &opened);
	}
	CHECK_EQ(bad, 0);
	CHECK(opened > 100);
	free(copy);
	free(orig);
	unlink(p);
}

static void test_fourcc_str(void)
{
	char b[FOURCC_STR_SIZE];

	CHECK_STR(fourcc_str(FCC('Y', 'U', 'Y', '2'), b), "YUY2");
	CHECK_STR(fourcc_str(0, b), "\\x00\\x00\\x00\\x00");
	CHECK_STR(fourcc_str(FCC('a', '\\', '\n', 0xff), b), "a\\x5c\\x0a\\xff");
}

int main(void)
{
	setup_dir();
	test_fourcc_str();
	test_plain();
	test_idx1_absolute();
	test_scan();
	test_odml_small();
	test_odml_segments();
	test_sizes();
	test_av();
	test_av_odml();
	test_audio_only();
	test_audio_formats();
	test_bad_files();
	test_damage("odml.avi");
	test_damage("av_odml.avi");
	cleanup("plain.avi");
	cleanup("av.avi");
	cleanup("av_odml.avi");
	cleanup("audio.avi");
	cleanup("vbr.avi");
	cleanup("odml1.avi");
	cleanup("odml.avi");
	if (!keep)
		rmdir(dir);
	return test_summary("test_avi");
}
