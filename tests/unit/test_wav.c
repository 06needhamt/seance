/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * WAV reader and writer, audio sources and audio sinks: round trips of
 * PCM, float and compressed formats, RF64, pipes, files other writers
 * make (16-byte fmt chunks, chunks before fmt, streaming sizes) and
 * damaged ones.
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
#include "in/audio.h"
#include "in/wav.h"
#include "media/audio.h"
#include "out/audio.h"
#include "out/wav.h"
#include "test.h"

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
	snprintf(dir, sizeof(dir), "%s/vfw-wav-XXXXXX", t && *t ? t : "/tmp");
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

static BYTE pattern(size_t i)
{
	return (BYTE)(i * 7 + (i >> 8) * 3);
}

static void fill(BYTE *buf, size_t from, size_t n)
{
	for (size_t i = 0; i < n; i++)
		buf[i] = pattern(from + i);
}

static void put32(BYTE *p, DWORD v) { memcpy(p, &v, 4); }
static DWORD get32(const BYTE *p) { DWORD v; memcpy(&v, p, 4); return v; }

/* Writes a file from a buffer. */
static void write_bytes(const char *name, const void *buf, size_t len)
{
	int fd = open(path_of(name), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0 && write(fd, buf, len) == (ssize_t)len);
	if (fd >= 0)
		close(fd);
}

/* Writes `bytes` bytes of pattern as a WAV in format wf. */
static int write_wav(const char *name, const void *wf, size_t size,
		     size_t bytes, const struct wav_writer_options *o)
{
	struct wav_writer *w = wav_writer_open(path_of(name), wf, size, o);
	BYTE buf[1000];

	if (!w)
		return -1;
	for (size_t at = 0; at < bytes; at += sizeof(buf)) {
		size_t n = bytes - at < sizeof(buf) ? bytes - at : sizeof(buf);
		fill(buf, at, n);
		if (wav_writer_write(w, buf, n)) {
			wav_writer_close(w);
			return -1;
		}
	}
	return wav_writer_close(w);
}

/* Reads all the data, checking it against the pattern. */
static size_t read_all(struct wav_reader *r, unsigned *bad)
{
	BYTE buf[777];
	size_t at = 0;
	long n;

	while ((n = wav_read(r, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++)
			*bad += buf[i] != pattern(at + i);
		at += n;
	}
	if (n < 0)
		++*bad;
	return at;
}

static void test_pcm(void)
{
	WAVEFORMATEXTENSIBLE x;
	size_t size = audio_pcm_format(AUDIO_S16, 2, 44100, 0, AUDIO_FORM_USUAL, &x);
	unsigned bad = 0;
	void *file;
	size_t len;

	CHECK_EQ(write_wav("s16.wav", &x, size, 40000, NULL), 0);
	CHECK_EQ(vfw_read_file(path_of("s16.wav"), &file, &len), 0);
	BYTE *b = file;
	/* RIFF, the JUNK chunk RF64 would use, a 16-byte fmt, data. */
	CHECK_EQ(len, 12 + 36 + 24 + 8 + 40000);
	CHECK(!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4));
	CHECK_EQ(get32(b + 4), len - 8);
	CHECK(!memcmp(b + 12, "JUNK", 4));
	CHECK_EQ(get32(b + 16), 28);
	CHECK(!memcmp(b + 48, "fmt ", 4));
	CHECK_EQ(get32(b + 52), 16);
	CHECK(!memcmp(b + 72, "data", 4));
	CHECK_EQ(get32(b + 76), 40000);
	free(file);

	struct wav_reader *r = wav_open(path_of("s16.wav"));
	CHECK(r);
	if (!r)
		return;
	size_t fsize;
	const WAVEFORMATEX *wf = wav_format(r, &fsize);
	CHECK_EQ(fsize, 18);
	CHECK(!memcmp(wf, &x, 18));
	CHECK_EQ(wav_data_size(r), 40000);
	CHECK_EQ(wav_fact_samples(r), -1);
	CHECK_EQ(wav_is_rf64(r), 0);
	CHECK_EQ(read_all(r, &bad), 40000);
	CHECK_EQ(bad, 0);
	CHECK_EQ(wav_truncated(r), 0);
	CHECK_EQ(wav_read(r, &bad, 1), 0);
	wav_close(r);

	/* Without the JUNK chunk, the classic 44-byte header. */
	struct wav_writer_options never = { WAV_RF64_NEVER };
	CHECK_EQ(write_wav("s16_44.wav", &x, size, 400, &never), 0);
	struct stat st;
	CHECK(!stat(path_of("s16_44.wav"), &st) && st.st_size == 444);
	cleanup("s16_44.wav");
}

static void test_float_and_rf64(void)
{
	WAVEFORMATEXTENSIBLE x;
	size_t size = audio_pcm_format(AUDIO_F32, 6, 48000, 0, AUDIO_FORM_USUAL, &x);
	unsigned bad = 0;

	/* Float gets a fact chunk with its sample count. */
	CHECK_EQ(write_wav("f32.wav", &x, size, 24 * 1000, NULL), 0);
	struct wav_reader *r = wav_open(path_of("f32.wav"));
	CHECK(r);
	if (r) {
		size_t fsize;
		const WAVEFORMATEX *wf = wav_format(r, &fsize);
		CHECK_EQ(fsize, 40);
		CHECK(!memcmp(wf, &x, 40));
		CHECK_EQ(wav_fact_samples(r), 1000);
		CHECK_EQ(read_all(r, &bad), 24000);
		wav_close(r);
	}

	/* RF64, forced: sizes in ds64, 0xffffffff in the 32-bit fields. */
	struct wav_writer_options always = { WAV_RF64_ALWAYS };
	CHECK_EQ(write_wav("rf64.wav", &x, size, 24 * 500, &always), 0);
	void *file;
	size_t len;
	CHECK_EQ(vfw_read_file(path_of("rf64.wav"), &file, &len), 0);
	BYTE *b = file;
	CHECK(!memcmp(b, "RF64", 4));
	CHECK_EQ(get32(b + 4), 0xffffffff);
	CHECK(!memcmp(b + 12, "ds64", 4));
	CHECK_EQ(get32(b + 20), len - 8);
	CHECK_EQ(get32(b + 28), 24 * 500);
	CHECK_EQ(get32(b + 36), 500);
	free(file);
	r = wav_open(path_of("rf64.wav"));
	CHECK(r);
	if (r) {
		CHECK_EQ(wav_is_rf64(r), 1);
		CHECK_EQ(wav_data_size(r), 12000);
		CHECK_EQ(wav_fact_samples(r), 500);
		CHECK_EQ(read_all(r, &bad), 12000);
		wav_close(r);
	}
	CHECK_EQ(bad, 0);
	cleanup("f32.wav");
}

static void test_compressed(void)
{
	BYTE fmt[30] = { 0 };
	WAVEFORMATEX *wf = (WAVEFORMATEX *)fmt;
	unsigned bad = 0;

	wf->wFormatTag = WAVE_FORMAT_MPEGLAYER3;
	wf->nChannels = 2;
	wf->nSamplesPerSec = 44100;
	wf->nAvgBytesPerSec = 16000;
	wf->nBlockAlign = 1;
	wf->cbSize = 12;
	fmt[18] = 1;

	/* An odd size, padded; the count given. */
	struct wav_writer *w = wav_writer_open(path_of("mp3.wav"), wf,
					       sizeof(fmt), NULL);
	BYTE buf[1001];
	CHECK(w);
	if (!w)
		return;
	fill(buf, 0, sizeof(buf));
	CHECK_EQ(wav_writer_write(w, buf, sizeof(buf)), 0);
	CHECK_EQ(wav_writer_bytes(w), 1001);
	wav_writer_set_samples(w, 1152 * 3);
	CHECK_EQ(wav_writer_close(w), 0);
	struct stat st;
	CHECK(!stat(path_of("mp3.wav"), &st) && st.st_size % 2 == 0);

	struct wav_reader *r = wav_open(path_of("mp3.wav"));
	CHECK(r);
	if (r) {
		size_t fsize;
		CHECK(!memcmp(wav_format(r, &fsize), fmt, sizeof(fmt)));
		CHECK_EQ(fsize, 30);
		CHECK_EQ(wav_fact_samples(r), 3456);
		CHECK_EQ(read_all(r, &bad), 1001);
		wav_close(r);
	}

	/* Without a count, no fact chunk (it's left as JUNK). */
	CHECK_EQ(write_wav("mp3_nofact.wav", fmt, sizeof(fmt), 500, NULL), 0);
	r = wav_open(path_of("mp3_nofact.wav"));
	CHECK(r);
	if (r) {
		CHECK_EQ(wav_fact_samples(r), -1);
		CHECK_EQ(read_all(r, &bad), 500);
		wav_close(r);
	}
	CHECK_EQ(bad, 0);
	cleanup("mp3_nofact.wav");
}

/* A WAV made by hand: chunks around fmt, a 16-byte fmt, sizes as given. */
static size_t make_wav(BYTE *b, DWORD riff, DWORD data, size_t data_bytes,
		       int fmt_bytes)
{
	WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 8000, 16000, 2, 16, 0 };
	size_t n = 0;

	memcpy(b, "RIFF", 4);
	put32(b + 4, riff);
	memcpy(b + 8, "WAVE", 4);
	n = 12;
	/* A LIST chunk and an odd-sized chunk before fmt. */
	memcpy(b + n, "LIST", 4);
	put32(b + n + 4, 4);
	memcpy(b + n + 8, "INFO", 4);
	n += 12;
	memcpy(b + n, "odd ", 4);
	put32(b + n + 4, 3);
	n += 8 + 4;
	memcpy(b + n, "fmt ", 4);
	put32(b + n + 4, fmt_bytes);
	memcpy(b + n + 8, &wf, fmt_bytes < 18 ? fmt_bytes : 18);
	if (fmt_bytes > 18)
		memset(b + n + 8 + 18, 0, fmt_bytes - 18);
	n += 8 + fmt_bytes + (fmt_bytes & 1);
	memcpy(b + n, "data", 4);
	put32(b + n + 4, data);
	n += 8;
	fill(b + n, 0, data_bytes);
	return n + data_bytes;
}

static void test_other_writers(void)
{
	BYTE b[2000];
	size_t n;
	struct wav_reader *r;
	unsigned bad = 0;

	/* A 16-byte fmt, after other chunks. */
	n = make_wav(b, 0, 1000, 1000, 16);
	put32(b + 4, (DWORD)n - 8);
	write_bytes("hand.wav", b, n);
	r = wav_open(path_of("hand.wav"));
	CHECK(r);
	if (r) {
		size_t fsize;
		const WAVEFORMATEX *wf = wav_format(r, &fsize);
		CHECK_EQ(fsize, 18);
		CHECK_EQ(wf->cbSize, 0);
		CHECK_EQ(wf->nSamplesPerSec, 8000);
		CHECK_EQ(read_all(r, &bad), 1000);
		wav_close(r);
	}

	/* An odd-sized fmt with a cbSize bigger than it. */
	n = make_wav(b, 0, 10, 10, 19);
	b[12 + 12 + 12 + 8 + 16] = 9;	/* cbSize */
	write_bytes("hand.wav", b, n);
	r = wav_open(path_of("hand.wav"));
	CHECK(r);
	if (r) {
		size_t fsize;
		CHECK_EQ(wav_format(r, &fsize)->cbSize, 1);
		CHECK_EQ(fsize, 19);
		CHECK_EQ(read_all(r, &bad), 10);
		wav_close(r);
	}

	/* Streaming sizes: 0xffffffff, and 0 with data after it. */
	n = make_wav(b, 0xffffffff, 0xffffffff, 1234, 16);
	write_bytes("hand.wav", b, n);
	r = wav_open(path_of("hand.wav"));
	CHECK(r);
	if (r) {
		CHECK_EQ(wav_data_size(r), UINT64_MAX);
		CHECK_EQ(read_all(r, &bad), 1234);
		CHECK_EQ(wav_truncated(r), 0);
		wav_close(r);
	}
	n = make_wav(b, 0, 0, 100, 16);
	write_bytes("hand.wav", b, n);
	r = wav_open(path_of("hand.wav"));
	CHECK(r && read_all(r, &bad) == 100);
	wav_close(r);
	/* A real empty one. */
	n = make_wav(b, 0, 0, 0, 16);
	write_bytes("hand.wav", b, n);
	r = wav_open(path_of("hand.wav"));
	CHECK(r && wav_data_size(r) == 0 && read_all(r, &bad) == 0);
	wav_close(r);

	/* Truncated: the data says more than there is. */
	n = make_wav(b, 0, 1000, 600, 16);
	write_bytes("hand.wav", b, n);
	r = wav_open(path_of("hand.wav"));
	CHECK(r);
	if (r) {
		CHECK_EQ(wav_truncated(r), 1);
		CHECK_EQ(wav_data_size(r), 600);
		CHECK_EQ(read_all(r, &bad), 600);
		wav_close(r);
	}
	CHECK_EQ(bad, 0);
	cleanup("hand.wav");
}

/* Each damaged file fails with the message given. */
static void expect_bad(const BYTE *b, size_t n, const char *msg)
{
	write_bytes("bad.wav", b, n);
	struct wav_reader *r = wav_open(path_of("bad.wav"));
	CHECK(!r);
	if (r) {
		wav_close(r);
		return;
	}
	if (!strstr(vfw_error(), msg)) {
		fprintf(stderr, "wanted \"%s\", got \"%s\"\n", msg, vfw_error());
		CHECK(0);
	}
}

static void test_bad(void)
{
	BYTE b[2000];
	size_t n = make_wav(b, 0, 100, 100, 16);
	size_t fmt = 12 + 12 + 12;	/* where the fmt chunk is */

	CHECK(!wav_open(path_of("missing.wav")));
	CHECK(strstr(vfw_error(), "missing.wav") != NULL);
	expect_bad(b, 0, "not a WAV");
	expect_bad(b, 11, "not a WAV");
	memcpy(b + 8, "AVI ", 4);
	expect_bad(b, n, "not a WAV");
	memcpy(b + 8, "WAVE", 4);
	expect_bad(b, fmt, "no fmt chunk");
	expect_bad(b, fmt + 8 + 16, "no data chunk");
	expect_bad(b, fmt + 8 + 10, "fmt chunk");
	put32(b + fmt + 4, 14);
	expect_bad(b, n, "fmt chunk of 14 bytes");
	put32(b + fmt + 4, 16);
	b[fmt + 8 + 2] = 0;	/* no channels */
	expect_bad(b, n, "channels");
	b[fmt + 8 + 2] = 1;
	memcpy(b + fmt, "fmX ", 4);
	expect_bad(b, n, "data chunk before the fmt");
	memcpy(b + fmt, "fmt ", 4);
	memcpy(b, "RF64", 4);
	expect_bad(b, n, "ds64");

	/* Two fmt chunks. */
	BYTE two[200];
	memcpy(two, b, fmt);
	memcpy(two, "RIFF", 4);
	memcpy(two + fmt, b + fmt, 24);
	memcpy(two + fmt + 24, b + fmt, 24);
	expect_bad(two, fmt + 48, "two fmt");
	unlink(path_of("bad.wav"));
}

/* Reads a WAV from standard input, and writes one to standard output. */
static void test_pipes(void)
{
	int p[2], saved;
	BYTE b[2000], got[4000];
	size_t n = make_wav(b, 0, 0xffffffff, 1500, 16);
	unsigned bad = 0;

	/* In: the whole file fits in the pipe's buffer. */
	CHECK_EQ(pipe(p), 0);
	CHECK(write(p[1], b, n) == (ssize_t)n);
	close(p[1]);
	saved = dup(0);
	dup2(p[0], 0);
	close(p[0]);
	struct wav_reader *r = wav_open("-");
	CHECK(r);
	if (r) {
		CHECK_EQ(read_all(r, &bad), 1500);
		wav_close(r);
	}
	dup2(saved, 0);
	close(saved);

	/* Out: sizes can't be patched, and say "to the end". */
	WAVEFORMATEX wf = { WAVE_FORMAT_IEEE_FLOAT, 1, 8000, 32000, 4, 32, 0 };
	CHECK_EQ(pipe(p), 0);
	fflush(stdout);
	saved = dup(1);
	dup2(p[1], 1);
	close(p[1]);
	struct wav_writer *w = wav_writer_open("-", &wf, sizeof(wf), NULL);
	CHECK(w);
	if (w) {
		fill(b, 0, 400);
		CHECK_EQ(wav_writer_write(w, b, 400), 0);
		CHECK_EQ(wav_writer_close(w), 0);
	}
	dup2(saved, 1);
	close(saved);
	ssize_t got_n = read(p[0], got, sizeof(got));
	close(p[0]);
	CHECK(got_n > 400);
	if (got_n > 400) {
		CHECK_EQ(get32(got + 4), 0xffffffff);
		/* No JUNK; the fact placeholder is JUNK too. */
		CHECK(!memcmp(got + 12, "fmt ", 4));
		CHECK(!memcmp(got + 12 + 8 + 18, "JUNK", 4));
		BYTE *d = got + got_n - 400 - 8;
		CHECK(!memcmp(d, "data", 4));
		CHECK_EQ(get32(d + 4), 0xffffffff);
		write_bytes("piped.wav", got, (size_t)got_n);
		r = wav_open(path_of("piped.wav"));
		CHECK(r && read_all(r, &bad) == 400);
		wav_close(r);
	}
	CHECK_EQ(bad, 0);
	cleanup("piped.wav");
}

/* The audio sources over WAV, raw PCM and AVI files. */
static void test_sources(void)
{
	struct audio_source *s;
	const BYTE *data;
	size_t len, total = 0;
	int64_t time;
	unsigned bad = 0, packets = 0;
	int r;

	/* WAV, found by its contents: 44100 Hz stereo s16, 40000 bytes. */
	rename(path_of("s16.wav"), path_of("s16.dat"));
	s = audio_source_open(path_of("s16.dat"), NULL);
	CHECK(s);
	if (s) {
		size_t fsize;
		CHECK_EQ(audio_source_kind(s), AUDIO_SOURCE_WAV);
		CHECK_EQ(audio_source_format(s, &fsize)->nChannels, 2);
		CHECK_EQ(audio_source_duration(s), 10000 * 10000000ll / 44100);
		while ((r = audio_source_read(s, &data, &len, &time)) > 0) {
			bad += len % 4 != 0;
			bad += time != (int64_t)(total / 4) * 10000000 / 44100;
			for (size_t i = 0; i < len; i++)
				bad += data[i] != pattern(total + i);
			total += len;
			packets++;
		}
		CHECK_EQ(r, 0);
		CHECK_EQ(total, 40000);
		CHECK_EQ(audio_source_truncated(s), 0);
		audio_source_close(s);
	}
	rename(path_of("s16.dat"), path_of("s16.wav"));

	/* Raw PCM, a part frame at the end. */
	BYTE raw[1003];
	fill(raw, 0, sizeof(raw));
	write_bytes("in.pcm", raw, sizeof(raw));
	struct audio_source_options o = {
		.kind = AUDIO_SOURCE_AUTO, .stream = -1,
		.raw_sample = AUDIO_S24, .channels = 3, .rate = 8000,
	};
	s = audio_source_open(path_of("in.pcm"), &o);
	CHECK(s);
	if (s) {
		size_t fsize;
		const WAVEFORMATEX *wf = audio_source_format(s, &fsize);
		CHECK_EQ(audio_source_kind(s), AUDIO_SOURCE_RAW);
		CHECK_EQ(fsize, 40);
		CHECK_EQ(audio_format_sample(wf, fsize), AUDIO_S24);
		CHECK_EQ(wf->nBlockAlign, 9);
		CHECK_EQ(audio_source_duration(s), 111 * 10000000ll / 8000);
		CHECK_EQ(audio_source_read(s, &data, &len, &time), 1);
		CHECK_EQ(len, 999);
		CHECK_EQ(time, 0);
		CHECK_EQ(audio_source_read(s, &data, &len, &time), 0);
		CHECK_EQ(audio_source_truncated(s), 1);
		audio_source_close(s);
	}
	o.channels = 0;
	CHECK(!audio_source_open(path_of("in.pcm"), &o));
	CHECK(strstr(vfw_error(), "channel count") != NULL);
	o.kind = AUDIO_SOURCE_AUTO;
	CHECK(!audio_source_open(path_of("in.dat"), NULL));
	cleanup("in.pcm");

	/* AVI: the audio stream of an AVI with video first. */
	BITMAPINFOHEADER bih = { sizeof(bih), 2, 2, 1, 24, BI_RGB, 12, 0, 0, 0, 0 };
	WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 1000, 2000, 2, 16, 0 };
	struct avi_writer *w = avi_writer_create(path_of("src.avi"), NULL);
	CHECK(w && avi_writer_add_video(w, &bih, sizeof(bih), 0, 10, 1) == 0);
	CHECK(w && avi_writer_add_audio(w, &wf, sizeof(wf), 0, 0, 0) == 1);
	for (unsigned f = 0; w && f < 5; f++) {
		fill(raw, 200 * f, 200);
		CHECK_EQ(avi_writer_write(w, 0, raw, 12, 1), 0);
		CHECK_EQ(avi_writer_write(w, 1, raw, 200, 1), 0);
	}
	CHECK(w && avi_writer_close(w) == 0);
	s = audio_source_open(path_of("src.avi"), NULL);
	CHECK(s);
	if (s) {
		CHECK_EQ(audio_source_kind(s), AUDIO_SOURCE_AVI);
		CHECK(audio_source_avi_header(s) &&
		      audio_source_avi_header(s)->dwRate == 1000);
		CHECK_EQ(audio_source_duration(s), 5000000);
		total = 0;
		while ((r = audio_source_read(s, &data, &len, &time)) > 0) {
			bad += len != 200 || time != (int64_t)total / 2 * 10000;
			for (size_t i = 0; i < len; i++)
				bad += data[i] != pattern(total + i);
			total += len;
		}
		CHECK_EQ(total, 1000);
		audio_source_close(s);
	}
	o = (struct audio_source_options){ .stream = 0 };
	CHECK(!audio_source_open(path_of("src.avi"), &o));
	CHECK(strstr(vfw_error(), "not audio") != NULL);
	o.stream = 5;
	CHECK(!audio_source_open(path_of("src.avi"), &o));
	CHECK(strstr(vfw_error(), "no such stream") != NULL);
	CHECK_EQ(bad, 0);
	CHECK(packets > 0);
	cleanup("src.avi");

	CHECK_EQ(audio_source_kind_of_path("a/b.WAV"), AUDIO_SOURCE_WAV);
	CHECK_EQ(audio_source_kind_of_path("x.raw"), AUDIO_SOURCE_RAW);
	CHECK_EQ(audio_source_kind_of_path("x.d/y"), AUDIO_SOURCE_AUTO);
}

/* The audio sinks, with sample conversion. */
static void test_outputs(void)
{
	WAVEFORMATEXTENSIBLE x;
	size_t size = audio_pcm_format(AUDIO_S16, 2, 8000, 0, AUDIO_FORM_USUAL, &x);
	struct audio_output_options oo = { .convert = AUDIO_F32 };
	int16_t s16[8] = { 0, 16384, -16384, 32767, -32768, 1, 2, 3 };
	enum audio_output_kind k;
	struct audio_output *o;

	CHECK_EQ(audio_output_kind_from_path("x/y.Wav", &k), 0);
	CHECK_EQ(k, AUDIO_OUT_WAV);
	CHECK_EQ(audio_output_kind_from_path("y.raw", &k), 0);
	CHECK_EQ(k, AUDIO_OUT_PCM);
	CHECK_EQ(audio_output_kind_from_path("y.mp3", &k), -1);
	CHECK_EQ(audio_output_kind_from_name("avi", &k), 0);
	CHECK_EQ(k, AUDIO_OUT_AVI);
	CHECK_EQ(audio_output_kind_from_name("ogg", &k), -1);

	o = audio_output_open(path_of("conv.wav"), AUDIO_OUT_WAV, &x.Format,
			      size, &oo);
	CHECK(o);
	if (o) {
		size_t osize;
		const WAVEFORMATEX *owf = audio_output_format(o, &osize);
		CHECK_EQ(audio_format_sample(owf, osize), AUDIO_F32);
		CHECK_EQ(audio_output_write(o, s16, sizeof(s16)), 0);
		/* Part samples are refused, and nothing is written. */
		CHECK_EQ(audio_output_write(o, s16, 3), -1);
		CHECK_EQ(audio_output_bytes(o), 32);
		CHECK_EQ(audio_output_close(o), 0);
	}
	o = audio_output_open(path_of("conv.wav"), AUDIO_OUT_WAV, &x.Format,
			      size, &oo);
	CHECK(o && audio_output_write(o, s16, sizeof(s16)) == 0);
	CHECK(o && audio_output_close(o) == 0);
	struct wav_reader *r = wav_open(path_of("conv.wav"));
	CHECK(r);
	if (r) {
		float f[8];
		CHECK_EQ(wav_read(r, f, sizeof(f)), 32);
		CHECK(f[1] == 0.5f && f[4] == -1.0f);
		CHECK_EQ(wav_fact_samples(r), 4);
		wav_close(r);
	}

	/* Raw, as it is; AVI. */
	o = audio_output_open(path_of("out.pcm"), AUDIO_OUT_PCM, &x.Format,
			      size, NULL);
	CHECK(o && audio_output_write(o, s16, sizeof(s16)) == 0);
	CHECK(o && audio_output_close(o) == 0);
	void *buf;
	size_t len;
	CHECK(!vfw_read_file(path_of("out.pcm"), &buf, &len) &&
	      len == sizeof(s16) && !memcmp(buf, s16, len));
	free(buf);
	o = audio_output_open(path_of("out.avi"), AUDIO_OUT_AVI, &x.Format,
			      size, NULL);
	CHECK(o && audio_output_write(o, s16, sizeof(s16)) == 0);
	CHECK(o && audio_output_write(o, s16, 8) == 0);
	CHECK(o && audio_output_close(o) == 0);
	struct avi_file *avi = avi_open(path_of("out.avi"));
	CHECK(avi && avi->streams[0].nchunks == 2 &&
	      avi->streams[0].length == 6);
	avi_close(avi);

	/* Converting a compressed format is refused, and errors are kept. */
	WAVEFORMATEX mp3 = { WAVE_FORMAT_MPEGLAYER3, 2, 44100, 16000, 1, 0, 0 };
	CHECK(!audio_output_open(path_of("x.wav"), AUDIO_OUT_WAV, &mp3,
				 sizeof(mp3), &oo));
	CHECK(strstr(vfw_error(), "only PCM") != NULL);
	CHECK(!audio_output_open(path_of("no/such/dir.avi"), AUDIO_OUT_AVI,
				 &mp3, sizeof(mp3), NULL));
	CHECK(strstr(vfw_error(), "no/such/dir.avi") != NULL);
	CHECK(!audio_output_open("-", AUDIO_OUT_AVI, &mp3, sizeof(mp3), NULL));
	cleanup("conv.wav");
	cleanup("out.pcm");
	cleanup("out.avi");
}

int main(void)
{
	setup_dir();
	test_pcm();
	test_float_and_rf64();
	test_compressed();
	test_other_writers();
	test_bad();
	test_pipes();
	test_sources();
	test_outputs();
	cleanup("s16.wav");
	cleanup("rf64.wav");
	cleanup("mp3.wav");
	if (!keep)
		rmdir(dir);
	return test_summary("test_wav");
}
