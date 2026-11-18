/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance sweep: every codec in a directory, tried the same way.
 *
 * Each DLL is probed, then given a test clip to encode and the result to
 * decode back, each step in a process of its own (a codec that crashes or
 * hangs takes only that step with it). Video gets a PSNR against what the
 * codec was given; audio is round-tripped through the driver. A codec
 * that names a size it takes (--pad, --crop, --scale) is tried again at
 * that size. The results go to the terminal, and to a Markdown table and
 * a TSV file when asked; a TSV from an earlier sweep can be compared
 * against, to find codecs that got worse between builds.
 */
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "avi/avi.h"
#include "cmd_util.h"
#include "commands.h"
#include "common/error.h"
#include "in/source.h"
#include "in/wav.h"
#include "out/image.h"
#include "out/wav.h"

extern char **environ;

/* What became of a codec, from best to worst (for --compare). */
enum status {
	ST_HANG,		/* a step didn't finish */
	ST_CRASH,		/* a step crashed */
	ST_NOT_CODEC,		/* loads as nothing seance can drive */
	ST_FAILS,		/* refused or failed */
	ST_DECODER,		/* decodes only: nothing here to give it */
	ST_DECODES,		/* decodes only, and a sample checks out */
	ST_ENCODES,		/* encodes, but doesn't decode its output */
	ST_ROUND_TRIP,		/* encodes and decodes */
	ST_COUNT
};

static const char *const status_names[ST_COUNT] = {
	"hang", "crash", "not-codec", "fails", "decoder", "decodes",
	"encodes",
	"round-trip",
};

struct result {
	char file[256];
	char kind[16];		/* VfW, DirectShow, DMO, ACM, "" */
	char id[64];		/* the handler, or the roles */
	int audio;
	unsigned tags[8];	/* ACM: the format tags it encodes to */
	int ntags;
	enum status status;
	double psnr;		/* video, dB; NAN if none, INFINITY if exact */
	double kbits;		/* NAN if none */
	char fix[96];		/* the options it needed: "--pad 16" */
	char note[512];
	/* From the probe: */
	char clsids[8][40];	/* its video encoders, when it has several */
	int nclsids;
	int decodes_own;	/* VfW: decompresses its own FOURCC */
	char inputs[12][8];	/* what it decodes: "XVID", "0x0055" */
	int ninputs;
};

/* A stream to give decoders, and what it should decode to. */
struct sample {
	char name[64];
	int audio;
	char tag[8];		/* FOURCC, or "0x0055" */
	char file[PATH_MAX];
	char ref[PATH_MAX];	/* Y4M or 16-bit PCM WAV */
	int pooled;		/* made in this sweep (removed after) */
};

struct sweep {
	const char *self;
	char *emu[8];
	int nemu;
	char work[PATH_MAX - 128];
	int keep;
	unsigned long timeout, frames;
	int width, height;
	const char *video_in;	/* the clip: given, or made */
	char made_video[PATH_MAX], made_audio[8][PATH_MAX];
	struct result *results;
	size_t count, cap;
	struct sample *samples;	/* --samples, then the sweep's own */
	size_t nsamples, samples_cap;
	size_t npooled;
};

static int usage(void)
{
	fprintf(stderr,
		"usage: seance sweep <dir | codec.dll>... [options]\n"
		"\n"
		"Probes each codec (*.dll, *.drv, *.ax, *.acm), encodes a test\n"
		"clip with it and decodes the result back, each step in a\n"
		"process of its own, and reports what worked: the PSNR of the\n"
		"round trip, the bit rate, and why anything failed.\n"
		"\n"
		"  -i <in.y4m|avi>      the video to encode (default: a test\n"
		"                       pattern made for it)\n"
		"  --size <WxH>         the test pattern's size (default 320x240)\n"
		"  --frames <n>         frames to encode (default 10)\n"
		"  --timeout <s>        each step's limit, as VFW_TIMEOUT (30)\n"
		"  --markdown <file>    write the results as a Markdown table\n"
		"  --tsv <file>         write them as tab-separated values\n"
		"  --compare <old.tsv>  report what changed since an earlier\n"
		"                       sweep's TSV; exit 1 if anything got worse\n"
		"  --samples <dir>      give decoders the streams in dir (made\n"
		"                       by tests/gen_samples.sh) and compare\n"
		"                       what they make with ffmpeg's decode\n"
		"  --keep <dir>         keep the files made, in dir\n"
		"\n"
		"A decoder is also given what the sweep's own encoders made, by\n"
		"FOURCC or format tag.\n");
	return 2;
}

/* Files and processes */

static int wanted(const char *name)
{
	static const char *const ext[] = { ".dll", ".drv", ".ax", ".acm" };
	size_t n = strlen(name);

	for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
		size_t e = strlen(ext[i]);
		if (n > e && !strcasecmp(name + n - e, ext[i]))
			return 1;
	}
	return 0;
}

static const char *base_name(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

/* path in the work directory, for the codec numbered n. */
static void work_path(const struct sweep *s, size_t n, const char *what,
		      char *out, size_t size)
{
	snprintf(out, size, "%s/%03zu.%.16s", s->work, n, what);
}

/*
 * Runs seance with args, its stdout and stderr to files. Returns its exit
 * status: 128 + the signal if one ended it, 124 if it outlasted the
 * timeout (and was killed), -1 if it couldn't start.
 */
static int run(const struct sweep *s, const char *const *args,
	       const char *out, const char *err)
{
	char *argv[64];
	int n = 0;
	posix_spawn_file_actions_t fa;
	pid_t pid;

	for (int i = 0; i < s->nemu; i++)
		argv[n++] = s->emu[i];
	argv[n++] = (char *)s->self;
	for (int i = 0; args[i] && n < 63; i++)
		argv[n++] = (char *)args[i];
	argv[n] = NULL;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 1, out,
					 O_WRONLY | O_CREAT | O_TRUNC, 0644);
	posix_spawn_file_actions_addopen(&fa, 2, err,
					 O_WRONLY | O_CREAT | O_TRUNC, 0644);
	/* An emulator may be named bare (qemu-i386), found on PATH. */
	int r = (s->nemu ? posix_spawnp : posix_spawn)(&pid, argv[0], &fa,
						       NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	if (r) {
		fprintf(stderr, "seance: sweep: can't run %s: %s\n", argv[0],
			strerror(r));
		return -1;
	}

	/*
	 * VFW_TIMEOUT stops a single call into a DLL that takes too long;
	 * this stops a step that takes far longer than that in all.
	 */
	struct timespec start, now, nap = { 0, 20 * 1000 * 1000 };
	clock_gettime(CLOCK_MONOTONIC, &start);
	double limit = (double)s->timeout * 4 + 30;
	for (;;) {
		int st;
		pid_t w = waitpid(pid, &st, WNOHANG);
		if (w == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) :
			       WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
		if (w < 0 && errno != EINTR)
			return -1;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec +
		    (now.tv_nsec - start.tv_nsec) / 1e9 > limit) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			return 124;
		}
		nanosleep(&nap, NULL);
	}
}

/* A file's text, NUL-terminated (at most 1 MB); NULL if unreadable. */
static char *slurp_text(const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t cap = 1 << 20, n;
	char *buf;

	if (!f)
		return NULL;
	if (!(buf = malloc(cap + 1))) {
		fclose(f);
		return NULL;
	}
	n = fread(buf, 1, cap, f);
	buf[n] = 0;
	fclose(f);
	return buf;
}

/*
 * The message a step ended with: its crash report if it crashed, else the
 * last "seance: " line; without the prefix or the codec's path.
 */
static void message_of(const char *err_path, int crashed, char *out,
		       size_t size)
{
	char *text = slurp_text(err_path), *line, *save = NULL;
	const char *pick = NULL;

	out[0] = 0;
	if (!text)
		return;
	for (line = strtok_r(text, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "seance: ", 8))
			continue;
		if (crashed && !strncmp(line, "seance: crash", 13)) {
			pick = line;
			/* and the line after it, saying where */
			char *next = strtok_r(NULL, "\n", &save);
			if (next && !strncmp(next, "seance:   ", 10)) {
				snprintf(out, size, "%s %s", line + 8,
					 next + 10);
				free(text);
				return;
			}
			break;
		}
		if (!crashed || !pick)
			pick = line;
	}
	if (pick)
		snprintf(out, size, "%s", pick + 8);
	free(text);
}

/* The option a refusal names: "add --pad 16", "add --scale 640x480". */
static int fix_from(const char *msg, char *fix, size_t size)
{
	static const char *const opts[] = { "--pad ", "--crop ", "--scale " };
	const char *add = strstr(msg, "add --");

	if (!add)
		return 0;
	add += 4;
	for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
		size_t n = strlen(opts[i]);
		if (strncmp(add, opts[i], n))
			continue;
		size_t v = strspn(add + n, "0123456789x");
		if (!v)
			return 0;
		snprintf(fix, size, "%.*s", (int)(n + v), add);
		return 1;
	}
	return 0;
}

/* Test media */

/*
 * A moving test pattern: gradients, fine checks, a moving square and some
 * noise, drawn in RGB and converted, so its colours are ones RGB codecs
 * can hold too (a YUV picture outside RGB's gamut would lose to clipping
 * in any of them).
 */
static int make_video(struct sweep *s)
{
	int w = s->width, h = s->height;
	size_t size = image_i420_size(w, h);
	BYTE *f = malloc(size), *rgb = malloc(image_bgr24_size(w, h));
	FILE *out = NULL;
	uint32_t seed = 12345;

	snprintf(s->made_video, sizeof(s->made_video), "%s/clip.y4m", s->work);
	if (!f || !rgb || !(out = fopen(s->made_video, "wb"))) {
		fprintf(stderr, "seance: sweep: %s: %s\n", s->made_video,
			strerror(errno));
		free(f);
		free(rgb);
		return -1;
	}
	fprintf(out, "YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n", w, h);
	for (unsigned long n = 0; n < s->frames; n++) {
		int sq = (int)(n * 4 % (unsigned long)(w > 32 ? w - 32 : 1));
		BYTE *p = rgb;
		for (int j = 0; j < h; j++)
			for (int i = 0; i < w; i++) {
				int r = 40 + i * 170 / w;
				int g = 40 + (j + (int)n * 2) % h * 170 / h;
				int b = 40 + (i + j) * 170 / (w + h);
				if ((i / 4 + j / 4) % 2 && j > h * 3 / 4)
					r += 40, g += 40, b += 40; /* fine checks */
				if (i >= sq && i < sq + 32 && j >= h / 3 &&
				    j < h / 3 + 32)
					r = g = b = 230;	/* the square */
				seed = seed * 1103515245u + 12345u;
				int d = (int)(seed >> 28) - 8;	/* noise */
				*p++ = (BYTE)(b + d < 0 ? 0 : b + d > 255 ? 255 : b + d);
				*p++ = (BYTE)(g + d < 0 ? 0 : g + d > 255 ? 255 : g + d);
				*p++ = (BYTE)(r + d < 0 ? 0 : r + d > 255 ? 255 : r + d);
			}
		image_bgr24_to_i420(rgb, w, h, f);
		fprintf(out, "FRAME\n");
		fwrite(f, 1, size, out);
	}
	free(f);
	free(rgb);
	if (fclose(out)) {
		fprintf(stderr, "seance: sweep: %s: write error\n",
			s->made_video);
		return -1;
	}
	s->video_in = s->made_video;
	return 0;
}

/* PCM layouts to offer an audio encoder, speech-band first. */
static const struct {
	DWORD rate;
	WORD channels;
} pcm_layouts[] = {
	{ 8000, 1 }, { 11025, 1 }, { 16000, 1 }, { 22050, 1 }, { 44100, 2 },
	{ 48000, 2 }, { 22050, 2 }, { 44100, 1 },
};
#define NLAYOUTS (sizeof(pcm_layouts) / sizeof(pcm_layouts[0]))

/*
 * Two seconds of 16-bit audio in layout k: a tone, a sweep and a little
 * noise, written once to the work directory. Its path, or NULL.
 */
static const char *test_audio(struct sweep *s, size_t k)
{
	DWORD rate = pcm_layouts[k].rate;
	WORD ch = pcm_layouts[k].channels;
	WAVEFORMATEX wf = { WAVE_FORMAT_PCM, ch, rate, rate * ch * 2,
			    (WORD)(ch * 2), 16, 0 };
	struct wav_writer *w;
	size_t frames = (size_t)rate * 2;
	short *buf;
	uint32_t seed = 777;

	snprintf(s->made_audio[k], sizeof(s->made_audio[k]), "%s/pcm%lu_%u.wav",
		 s->work, (unsigned long)rate, (unsigned)ch);
	if (access(s->made_audio[k], R_OK) == 0)
		return s->made_audio[k];
	if (!(buf = malloc(frames * ch * sizeof(short))))
		return NULL;
	for (size_t i = 0; i < frames; i++) {
		double t = (double)i / rate;
		double v = 0.3 * sin(2 * M_PI * 440 * t) +
			   0.2 * sin(2 * M_PI * (200 + 600 * t) * t);
		seed = seed * 1103515245u + 12345u;
		v += ((int)(seed >> 16 & 0xff) - 128) / 128.0 * 0.02;
		for (WORD c = 0; c < ch; c++)
			buf[i * ch + c] = (short)(v * (c ? 26000 : 32767));
	}
	int bad = !(w = wav_writer_open(s->made_audio[k], &wf, sizeof(wf),
					 NULL)) ||
		  wav_writer_write(w, buf, frames * ch * sizeof(short));
	if (w && wav_writer_close(w))
		bad = 1;
	free(buf);
	if (bad) {
		fprintf(stderr, "seance: sweep: %s: %s\n", s->made_audio[k],
			vfw_error());
		return NULL;
	}
	return s->made_audio[k];
}

/* Measuring */

/* A frame of a source as I420, into buf (image_i420_size bytes). */
static int frame_i420(struct source *src, struct image_conv *conv, BYTE *buf)
{
	const BYTE *frame;
	size_t len;
	int r = source_read(src, &frame, &len);

	if (r != 1)
		return r;
	return image_to_i420(conv, frame, len, buf) ? -1 : 1;
}

/* The squared error of a w x h plane of a against b's top-left. */
static double plane_sse(const BYTE *a, int astride, const BYTE *b,
			int bstride, int w, int h)
{
	double sum = 0;

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int d = (int)a[(size_t)y * astride + x] -
				(int)b[(size_t)y * bstride + x];
			sum += (double)(d * d);
		}
	return sum;
}

/*
 * The PSNR of dec (a Y4M) against the clip as the codec was given it (ref
 * through the same --pad, --crop or --scale), over all three planes as
 * FFmpeg's average is. NAN with a note if they can't be compared, or,
 * with whole set, if dec has fewer frames than ref.
 */
static double psnr_of(const char *ref, const struct source_options *ro,
		      const char *dec, int whole, char *note, size_t size)
{
	struct source_options dopt = { .stream = -1 };
	struct source *a = source_open(ref, ro), *b = NULL;
	struct image_conv *ca = NULL, *cb = NULL;
	BYTE *fa = NULL, *fb = NULL;
	double result = NAN, sum = 0;
	uint64_t count = 0, frames = 0;
	int ra = 0;

	if (!a || !(b = source_open(dec, &dopt))) {
		snprintf(note, size, "can't compare: %s", vfw_error());
		goto out;
	}
	size_t na, nb;
	const BITMAPINFOHEADER *ha = source_format(a, &na);
	const BITMAPINFOHEADER *hb = source_format(b, &nb);
	int w = ha->biWidth, h = ha->biHeight < 0 ? -ha->biHeight : ha->biHeight;
	int bw = hb->biWidth, bh = hb->biHeight < 0 ? -hb->biHeight : hb->biHeight;
	/*
	 * A decoder that pads to its block size (LEncMpg23: 352x240 for
	 * 320x240) is measured on the top-left of what it gives.
	 */
	if (bw < w || bh < h) {
		snprintf(note, size, "decodes at %dx%d, not %dx%d", bw, bh, w, h);
		goto out;
	}
	size_t fs = image_i420_size(w, h), bs = image_i420_size(bw, bh);
	if (!(ca = image_conv_new(ha, na)) || !(cb = image_conv_new(hb, nb)) ||
	    !(fa = malloc(fs)) || !(fb = malloc(bs))) {
		snprintf(note, size, "can't compare: %s", vfw_error());
		goto out;
	}
	for (;;) {
		int rb;
		if ((ra = frame_i420(a, ca, fa)) != 1 ||
		    (rb = frame_i420(b, cb, fb)) != 1)
			break;
		frames++;
		sum += plane_sse(fa, w, fb, bw, w, h);
		sum += plane_sse(fa + (size_t)w * h, (w + 1) / 2,
				 fb + (size_t)bw * bh, (bw + 1) / 2,
				 (w + 1) / 2, (h + 1) / 2);
		sum += plane_sse(fa + (size_t)w * h + (size_t)((w + 1) / 2) *
				 ((h + 1) / 2), (w + 1) / 2,
				 fb + (size_t)bw * bh + (size_t)((bw + 1) / 2) *
				 ((bh + 1) / 2), (bw + 1) / 2,
				 (w + 1) / 2, (h + 1) / 2);
		count += fs;
	}
	if (count && (bw != w || bh != h))
		snprintf(note, size, "decodes at %dx%d; measured on its top-left "
			 "%dx%d", bw, bh, w, h);
	if (!count) {
		snprintf(note, size, "no frames to compare");
		goto out;
	}
	if (whole && ra == 1) {
		/* The frame just read from ref, and the rest */
		uint64_t all = frames + 1;
		while (frame_i420(a, ca, fa) == 1)
			all++;
		snprintf(note, size, "decodes %llu frames of the reference's "
			 "%llu", (unsigned long long)frames,
			 (unsigned long long)all);
		goto out;
	}
	result = sum ? 10 * log10(255.0 * 255.0 * (double)count / sum) :
		 INFINITY;
out:
	free(fa);
	free(fb);
	image_conv_free(ca);
	image_conv_free(cb);
	source_close(a);
	source_close(b);
	return result;
}

/* The video stream's data rate, kbit/s; NAN if unreadable. */
static double avi_kbits(const char *path, char *fmt, size_t size)
{
	struct avi_file *avi = avi_open(path);
	double r = NAN;

	if (!avi)
		return r;
	int v = avi_video_stream(avi);
	if (v >= 0) {
		const struct avi_stream *st = &avi->streams[v];
		uint64_t bytes = 0;
		for (size_t i = 0; i < st->nchunks; i++)
			if (avi_chunk_is_frame(&st->chunks[i]))
				bytes += st->chunks[i].size;
		double secs = st->header.dwRate ? (double)st->frames *
			      st->header.dwScale / st->header.dwRate : 0;
		if (secs > 0)
			r = (double)bytes * 8 / secs / 1000;
		const BITMAPINFOHEADER *b = avi_stream_bih(st);
		if (b && fmt)
			snprintf(fmt, size, "%ldx%ld", (long)b->biWidth,
				 (long)(b->biHeight < 0 ? -b->biHeight :
					b->biHeight));
	}
	avi_close(avi);
	return r;
}

/* A WAV's format, "8000 Hz mono 0x0031", and its rate in kbit/s. */
static double wav_describe(const char *path, char *out, size_t size)
{
	struct wav_reader *w = wav_open(path);
	double r = NAN;

	out[0] = 0;
	if (!w)
		return r;
	size_t n;
	const WAVEFORMATEX *f = wav_format(w, &n);
	if (f) {
		snprintf(out, size, "%lu Hz %s, tag 0x%04x",
			 (unsigned long)f->nSamplesPerSec,
			 f->nChannels == 1 ? "mono" :
			 f->nChannels == 2 ? "stereo" : "multichannel",
			 f->wFormatTag);
		r = f->nAvgBytesPerSec * 8 / 1000.0;
	}
	wav_close(w);
	return r;
}

/* Samples: streams to give decoders */

/* A sample's place in s->samples, made room for; NULL if out of memory. */
static struct sample *new_sample(struct sweep *s)
{
	if (s->nsamples == s->samples_cap) {
		size_t cap = s->samples_cap ? s->samples_cap * 2 : 64;
		struct sample *t = realloc(s->samples, cap * sizeof(*t));
		if (!t)
			return NULL;
		s->samples = t;
		s->samples_cap = cap;
	}
	struct sample *x = &s->samples[s->nsamples];
	memset(x, 0, sizeof(*x));
	return x;
}

/* Reads dir/index.tsv, as tests/gen_samples.sh writes it. */
static int load_samples(struct sweep *s, const char *dir)
{
	char path[PATH_MAX], *line = NULL;
	size_t cap = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/index.tsv", dir);
	if (!(f = fopen(path, "r"))) {
		fprintf(stderr, "seance: %s: %s (make it with "
			"tests/gen_samples.sh)\n", path, strerror(errno));
		return -1;
	}
	while (getline(&line, &cap, f) > 0) {
		char *field[5], *p = line;
		int k = 0;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#')
			continue;
		while (k < 5) {
			field[k++] = p;
			if (!(p = strchr(p, '\t')))
				break;
			*p++ = 0;
		}
		if (k < 5 || strlen(field[2]) > 6)
			continue;
		struct sample *x = new_sample(s);
		if (!x)
			break;
		snprintf(x->name, sizeof(x->name), "%s", field[0]);
		x->audio = !strcmp(field[1], "audio");
		snprintf(x->tag, sizeof(x->tag), "%s", field[2]);
		snprintf(x->file, sizeof(x->file), "%s/%s", dir, field[3]);
		snprintf(x->ref, sizeof(x->ref), "%s/%s", dir, field[4]);
		s->nsamples++;
	}
	free(line);
	fclose(f);
	if (!s->nsamples) {
		fprintf(stderr, "seance: %s: no samples listed\n", path);
		return -1;
	}
	return 0;
}

/*
 * Keeps what an encoder made, and its own decode of it, for decoders of
 * the same FOURCC or tag to be given (the first of each).
 */
static void pool_add(struct sweep *s, const struct result *r, int audio,
		     const char *tag, const char *file, const char *ref)
{
	struct sample *x;

	for (size_t i = 0; i < s->nsamples; i++)
		if (s->samples[i].pooled && s->samples[i].audio == audio &&
		    !strcasecmp(s->samples[i].tag, tag))
			return;
	if (!(x = new_sample(s)))
		return;
	snprintf(x->name, sizeof(x->name), "%.63s", r->file);
	x->audio = audio;
	x->pooled = 1;
	snprintf(x->tag, sizeof(x->tag), "%s", tag);
	snprintf(x->file, sizeof(x->file), "%s/pool%03zu.%s", s->work,
		 s->npooled, audio ? "wav" : "avi");
	snprintf(x->ref, sizeof(x->ref), "%s/pool%03zu.%s", s->work,
		 s->npooled, audio ? "ref.wav" : "y4m");
	if (rename(file, x->file) || rename(ref, x->ref)) {
		unlink(x->file);
		return;
	}
	s->nsamples++;
	s->npooled++;
}

/* The FOURCC of an AVI's video, as a key; 0 if none or not printable. */
static int avi_fourcc(const char *path, char key[8])
{
	struct avi_file *avi = avi_open(path);
	int ok = 0;

	if (!avi)
		return 0;
	int v = avi_video_stream(avi);
	const BITMAPINFOHEADER *b = v >= 0 ?
		avi_stream_bih(&avi->streams[v]) : NULL;
	if (b && b->biCompression > 3) {
		DWORD c = b->biCompression;
		ok = 1;
		for (int i = 0; i < 4; i++) {
			key[i] = (char)(c >> (8 * i) & 0xff);
			if (!isprint((unsigned char)key[i]))
				ok = 0;
		}
		key[4] = 0;
	}
	avi_close(avi);
	return ok;
}

/* A WAV's format tag; 0 if unreadable. */
static unsigned wav_tag(const char *path)
{
	struct wav_reader *w = wav_open(path);
	unsigned tag = 0;
	size_t n;

	if (!w)
		return 0;
	const WAVEFORMATEX *f = wav_format(w, &n);
	if (f)
		tag = f->wFormatTag;
	wav_close(w);
	return tag;
}

/* A WAV as 16-bit samples (from 8- or 16-bit PCM, or float). */
static short *read_pcm16(const char *path, size_t *frames, unsigned *rate,
			 unsigned *ch, char *note, size_t size)
{
	struct wav_reader *w = wav_open(path);
	short *out = NULL;
	BYTE *raw = NULL;
	size_t n;

	if (!w) {
		snprintf(note, size, "can't read %s", base_name(path));
		return NULL;
	}
	const WAVEFORMATEX *f = wav_format(w, &n);
	if (!f) {
		snprintf(note, size, "%s has no format", base_name(path));
		goto out;
	}
	unsigned tag = f->wFormatTag, bits = f->wBitsPerSample;
	if (tag == 0xfffe && n >= 40)	/* WAVE_FORMAT_EXTENSIBLE: its GUID */
		tag = *(const WORD *)((const BYTE *)f + 24);
	*rate = f->nSamplesPerSec;
	*ch = f->nChannels;
	if (!*ch || !((tag == WAVE_FORMAT_PCM && (bits == 8 || bits == 16)) ||
		      (tag == 3 && bits == 32))) {
		snprintf(note, size, "decodes to tag 0x%04x, %u bits: not PCM "
			 "to compare", tag, bits);
		goto out;
	}
	uint64_t bytes = wav_data_size(w);
	if (bytes > 64u << 20)
		bytes = 64u << 20;
	size_t bps = bits / 8;
	if (!(raw = malloc(bytes + 1))) {
		snprintf(note, size, "can't read %s: out of memory",
			 base_name(path));
		goto out;
	}
	long got = wav_read(w, raw, bytes);
	if (got < 0)
		got = 0;
	*frames = (size_t)got / bps / *ch;
	if (!(out = malloc((*frames * *ch + 1) * sizeof(short)))) {
		snprintf(note, size, "can't read %s: out of memory",
			 base_name(path));
		goto out;
	}
	for (size_t i = 0; i < *frames * *ch; i++) {
		if (bits == 8) {
			out[i] = (short)((raw[i] - 128) * 256);
		} else if (bits == 16) {
			short v;
			memcpy(&v, raw + i * 2, 2);
			out[i] = v;
		} else {
			float v;
			memcpy(&v, raw + i * 4, 4);
			v *= 32768.0f;
			out[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 :
					 v);
		}
	}
out:
	free(raw);
	wav_close(w);
	return out;
}

/*
 * The SNR of dec (a WAV) against ref (ffmpeg's decode), dB, with dec moved
 * by the lag that matches best (decoders differ in how much of their
 * start-up delay they hand out). NAN with a note if they can't be compared.
 */
static double snr_of(const char *ref, const char *dec, char *note,
		     size_t size)
{
	enum { MAX_LAG = 3000, WINDOW = 16384 };
	size_t na = 0, nb = 0;
	unsigned ra, rb, ca, cb;
	char n2[128] = "";
	short *a = read_pcm16(ref, &na, &ra, &ca, note, size);
	short *b = a ? read_pcm16(dec, &nb, &rb, &cb, n2, sizeof(n2)) : NULL;
	double result = NAN;

	if (a && !b)
		snprintf(note, size, "%s", n2);
	if (!a || !b)
		goto out;
	if (ra != rb || ca != cb) {
		snprintf(note, size, "decodes to %u Hz, %u channel%s; ffmpeg "
			 "to %u Hz, %u", rb, cb, cb == 1 ? "" : "s", ra, ca);
		goto out;
	}
	/* The lag: b[i + lag] against a[i], on the first channel. */
	int lag = 0;
	double best = -1;
	for (int l = -MAX_LAG; l <= MAX_LAG; l++) {
		double sum = 0;
		size_t count = 0;
		for (size_t i = 0; i < WINDOW && i < na; i++) {
			long j = (long)i + l;
			if (j < 0 || (size_t)j >= nb)
				continue;
			double d = a[i * ca] - b[(size_t)j * cb];
			sum += d * d;
			count++;
		}
		if (count < WINDOW / 4 && count < na / 2)
			continue;
		if (best < 0 || sum / count < best) {
			best = sum / count;
			lag = l;
		}
	}
	double sig = 0, err = 0;
	size_t count = 0;
	for (size_t i = 0; i < na; i++) {
		long j = (long)i + lag;
		if (j < 0 || (size_t)j >= nb)
			continue;
		for (unsigned c = 0; c < ca; c++) {
			double x = a[i * ca + c], d = x - b[(size_t)j * cb + c];
			sig += x * x;
			err += d * d;
		}
		count++;
	}
	if (!count || count < na / 2) {
		snprintf(note, size, "decodes %zu frames of ffmpeg's %zu", nb,
			 na);
		goto out;
	}
	result = !err ? INFINITY : sig ? 10 * log10(sig / err) : -INFINITY;
	if (lag)
		snprintf(note, size, "%d samples %s", lag < 0 ? -lag : lag,
			 lag > 0 ? "later" : "earlier");
out:
	free(a);
	free(b);
	return result;
}

/* One codec */

static enum status status_of_rc(int rc)
{
	return rc == 124 ? ST_HANG : rc > 128 ? ST_CRASH : ST_FAILS;
}

/* Adds a FOURCC or "0x...." tag to what r decodes, once. */
static void add_input(struct result *r, const char *key)
{
	for (int i = 0; i < r->ninputs; i++)
		if (!strcasecmp(r->inputs[i], key))
			return;
	if (r->ninputs < (int)(sizeof(r->inputs) / sizeof(r->inputs[0])))
		snprintf(r->inputs[r->ninputs++], sizeof(r->inputs[0]), "%s",
			 key);
}

/*
 * The input types in a filter's or DMO's probe lines: "MEDIATYPE_Video /
 * 'DX50'", "MEDIATYPE_Audio / wave format 0x0007" or a named subtype.
 */
static void read_input_type(struct result *r, const char *t)
{
	static const struct {
		const char *name;
		const char *tag;
	} named[] = {
		{ "MEDIASUBTYPE_DOLBY_AC3", "0x2000" },
		{ "MEDIASUBTYPE_MPEG1Audio", "0x0050" },
		{ "MEDIASUBTYPE_RAW_AAC1", "0x00ff" },
	};
	char key[8];

	if (!(t = strstr(t, " / ")))
		return;
	t += 3;
	if (t[0] == '\'' && strlen(t) >= 6 && t[5] == '\'') {
		snprintf(key, sizeof(key), "%.4s", t + 1);
		add_input(r, key);
	} else if (!strncmp(t, "wave format 0x", 14)) {
		snprintf(key, sizeof(key), "0x%04lx", strtoul(t + 14, NULL, 16));
		add_input(r, key);
	} else {
		for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
			if (!strncmp(t, named[i].name, strlen(named[i].name)) &&
			    !isalnum((unsigned char)t[strlen(named[i].name)]))
				add_input(r, named[i].tag);
	}
}

/* What a decoder's probe says it takes: pin, DMO or ACM input types. */
static void read_inputs(struct result *r, char *text)
{
	int in_pin = 0;
	char *save = NULL;

	for (char *l = strtok_r(text, "\n", &save); l;
	     l = strtok_r(NULL, "\n", &save)) {
		const char *t = l + strspn(l, " ");
		unsigned tag;
		if (!strncmp(l, "  {", 3) || !strncmp(t, "output", 6))
			in_pin = 0;
		else if (!strncmp(t, "input pin", 9))
			in_pin = 1;
		else if (!strncmp(t, "takes '", 7) &&
			 strlen(t) == 12 && t[11] == '\'')	/* Real */
			add_input(r, (char[5]){ t[7], t[8], t[9], t[10], 0 });
		else if (!strncmp(t, "input MEDIATYPE_", 16))	/* a DMO's */
			read_input_type(r, t);
		else if (in_pin && strstr(t, "MEDIATYPE_"))
			read_input_type(r, t);
		else if (!strncmp(l, "    0x", 6) && strstr(l, "decodes") &&
			 (tag = (unsigned)strtoul(l + 4, NULL, 16)) !=
			 WAVE_FORMAT_PCM) {			/* ACM */
			char key[8];
			snprintf(key, sizeof(key), "0x%04x", tag);
			add_input(r, key);
		}
	}
}

/* The fields of `seance probe`'s output that say what a DLL is. */
static void read_probe(struct result *r, const char *out, int *video_enc,
		       int *video_dec, int *audio_enc, int *audio_dec,
		       int *decode_only)
{
	char *text = slurp_text(out), *p;

	*video_enc = *video_dec = *audio_enc = *audio_dec = *decode_only = 0;
	if (!text)
		return;
	if (!strncmp(text, "Codec:", 6)) {
		snprintf(r->kind, sizeof(r->kind), "VfW");
		*video_enc = *video_dec = 1;
	} else if (!strncmp(text, "DirectShow server:", 18)) {
		snprintf(r->kind, sizeof(r->kind), "DirectShow");
	} else if (!strncmp(text, "DMO server:", 11)) {
		snprintf(r->kind, sizeof(r->kind), "DMO");
	} else if (!strncmp(text, "RealPlayer codec:", 17)) {
		snprintf(r->kind, sizeof(r->kind), "Real");
	} else if (!strncmp(text, "ACM driver:", 11)) {
		snprintf(r->kind, sizeof(r->kind), "ACM");
		*audio_dec = 1;
	}
	if ((p = strstr(text, "handler '"))) {
		p += 9;
		size_t n = strcspn(p, "'\n");
		snprintf(r->id, sizeof(r->id), "%.*s", (int)(n < 8 ? n : 8), p);
	} else if (!strcmp(r->kind, "ACM") && (p = strstr(text, "\n  name "))) {
		p += 8;
		p += strspn(p, " ");
		snprintf(r->id, sizeof(r->id), "%.*s", (int)strcspn(p, "\n"), p);
	}
	if (strstr(text, "video encoder"))
		*video_enc = 1;
	if (strstr(text, "video decoder"))
		*video_dec = 1;
	if (strstr(text, "audio encoder"))
		*audio_enc = 1;
	if (strstr(text, "audio decoder"))
		*audio_dec = 1;
	if (strstr(text, "only decompresses"))
		*decode_only = 1;
	/* ACM: "    0x1101 L&H CELP: ...; decodes and encodes" */
	for (char *line = strstr(text, "\n    0x"); line && r->ntags < 8;
	     line = strstr(line + 1, "\n    0x")) {
		char *end = strchr(line + 1, '\n');
		size_t n = end ? (size_t)(end - line) : strlen(line);
		unsigned tag = (unsigned)strtoul(line + 5, NULL, 16);
		char *enc = strstr(line, "encodes");
		if (tag != WAVE_FORMAT_PCM && enc && (size_t)(enc - line) < n)
			r->tags[r->ntags++] = tag;
	}
	if (!strcmp(r->kind, "ACM"))
		*audio_enc = r->ntags > 0;
	/* "  {GUID} name", then lines saying what each class is */
	for (char *c = strstr(text, "\n  {"); c && r->nclsids < 8;
	     c = strstr(c + 1, "\n  {")) {
		char *end = strstr(c + 1, "\n  {"), *enc = strstr(c, "video encoder");
		size_t n = strcspn(c + 3, "}");
		if (enc && (!end || enc < end) && n < sizeof(r->clsids[0]) - 1)
			snprintf(r->clsids[r->nclsids++], sizeof(r->clsids[0]),
				 "%.*s", (int)n + 1, c + 3);
	}
	/* VfW: "Decompression of 'XXXX' ...\n    decompress query: ICERR_OK" */
	if ((p = strstr(text, "\nDecompression of '"))) {
		char *q = strstr(p, "decompress query: ");
		char *c = strstr(p, "\nCompression (");
		r->decodes_own = q && (!c || q < c) &&
				 !strncmp(q + 18, "ICERR_OK", 8);
	}
	if (!r->id[0] && (*video_enc || *video_dec || *audio_enc || *audio_dec))
		snprintf(r->id, sizeof(r->id), "%s%s%s%s",
			 *video_enc ? "venc " : "", *video_dec ? "vdec " : "",
			 *audio_enc ? "aenc " : "", *audio_dec ? "adec" : "");
	size_t n = strlen(r->id);
	while (n && r->id[n - 1] == ' ')
		r->id[--n] = 0;
	read_inputs(r, text);	/* (last: it splits the text) */
	free(text);
}

/* Appends the space-separated options in opts to args[*n]; buf holds them. */
static void add_options(const char **args, int *n, int max, const char *opts,
			char *buf, size_t size)
{
	char *save = NULL;

	snprintf(buf, size, "%s", opts);
	for (char *t = strtok_r(buf, " ", &save); t && *n < max;
	     t = strtok_r(NULL, " ", &save))
		args[(*n)++] = t;
	args[*n] = NULL;
}

/*
 * Encodes with the options in pre, and again with the size option the
 * refusal names if it names one (not over a --scale of pre's). Returns
 * the last exit status; r->fix gets the options of one that worked.
 */
static int encode_with(struct sweep *s, const char *dll, const char *avi,
		       const char *out, const char *err, const char *pre,
		       struct result *r, char *msg, size_t msize)
{
	char fix[sizeof(r->fix)], add[32], frames[32], buf[sizeof(r->fix)];
	int rc = -1;

	snprintf(fix, sizeof(fix), "%s", pre);
	snprintf(frames, sizeof(frames), "%lu", s->frames);
	for (int attempt = 0; attempt < 2; attempt++) {
		const char *args[24] = { "encode", dll, s->video_in, "-o", avi,
					 "--frames", frames };
		int n = 7;
		add_options(args, &n, 23, fix, buf, sizeof(buf));
		rc = run(s, args, out, err);
		if (!rc) {
			snprintf(r->fix, sizeof(r->fix), "%s", fix);
			return 0;
		}
		message_of(err, rc > 128, msg, msize);
		if (attempt || rc >= 124 || strstr(pre, "--scale") ||
		    !fix_from(msg, add, sizeof(add)))
			break;
		snprintf(fix, sizeof(fix), "%s%s%s", pre, pre[0] ? " " : "", add);
	}
	return rc;
}

/* Sizes to try a codec that takes nothing at the clip's size at. */
static const int ladder[][2] = {
	{ 352, 288 }, { 640, 480 }, { 720, 480 }, { 720, 576 },
	{ 1280, 720 }, { 1920, 1080 },
};

/* Whether an encoder's refusal is of the input's format or size. */
static int refused_input(const char *msg)
{
	return strstr(msg, "takes neither") || strstr(msg, "ICERR_BADFORMAT") ||
	       strstr(msg, "standard input format");
}

/* Decodes avi with dll ("auto": any codec here) to y4m, as format if given. */
static int decode_to(struct sweep *s, const char *dll, const char *avi,
		     const char *y4m, const char *format, const char *out,
		     const char *err)
{
	const char *args[] = { "decode", dll, avi, "-o", y4m,
			       format ? "--output-format" : NULL, format, NULL };

	return run(s, args, out, err);
}

/* A round trip this far from what went in has gone wrong somewhere. */
#define PSNR_BROKEN 10.0
/* A round trip good enough to test other decoders with. */
#define POOL_PSNR 30.0
/* Audio: the SNR a decode needs, of a sample or of the test signal. */
#define SAMPLE_SNR 15.0

static void sweep_video(struct sweep *s, size_t idx, const char *dll,
			struct result *r)
{
	char avi[PATH_MAX], y4m[PATH_MAX], y4m2[PATH_MAX], out[PATH_MAX];
	char err[PATH_MAX], msg[400], first[400], pre[sizeof(r->fix)];
	char size_note[64] = "";
	struct source_options ro = { .stream = -1 };

	work_path(s, idx, "avi", avi, sizeof(avi));
	work_path(s, idx, "y4m", y4m, sizeof(y4m));
	work_path(s, idx, "rgb.y4m", y4m2, sizeof(y4m2));
	work_path(s, idx, "out", out, sizeof(out));
	work_path(s, idx, "err", err, sizeof(err));

	int rc = encode_with(s, dll, avi, out, err, "", r, msg, sizeof(msg));
	/* A DLL of several encoders (utv_dmo.dll): each in turn. */
	int each = rc && rc < 124 && strstr(msg, "several video encoders") &&
		   r->nclsids;
	int size_refused = 0;	/* by any of them */
	if (each) {
		int first_rc = -1;
		for (int k = 0; k < r->nclsids && rc; k++) {
			snprintf(pre, sizeof(pre), "--clsid %s", r->clsids[k]);
			rc = encode_with(s, dll, avi, out, err, pre, r, msg,
					 sizeof(msg));
			if (rc && rc < 124 && refused_input(msg))
				size_refused = 1;
			if (rc && first_rc < 0) {
				first_rc = rc;
				snprintf(first, sizeof(first), "with --clsid %.39s: "
					 "%.320s", r->clsids[k], msg);
			}
		}
		if (rc) {
			rc = first_rc;
			snprintf(msg, sizeof(msg), "none of its %d video encoders "
				 "works; %.340s", r->nclsids, first);
		}
	}
	/*
	 * One that takes nothing at this size may at another (each encoder
	 * of several). Encode itself asks a VfW codec these sizes, and names
	 * one it takes.
	 */
	if (rc && rc < 124 && strcmp(r->kind, "VfW") &&
	    (size_refused || refused_input(msg))) {
		snprintf(first, sizeof(first), "%s", msg);
		for (int k = 0; k < (each ? r->nclsids : 1) && rc; k++)
			for (size_t z = 0;
			     z < sizeof(ladder) / sizeof(ladder[0]) && rc; z++) {
				if (ladder[z][0] == s->width &&
				    ladder[z][1] == s->height)
					continue;
				snprintf(pre, sizeof(pre), "%s%.39s%s--scale %dx%d",
					 each ? "--clsid " : "",
					 each ? r->clsids[k] : "", each ? " " : "",
					 ladder[z][0], ladder[z][1]);
				/* (a crash at a size it was never asked for is no news) */
				if (!encode_with(s, dll, avi, out, err, pre, r, msg,
						 sizeof(msg)))
					rc = 0;
			}
		if (rc)
			snprintf(msg, sizeof(msg), "%.340s (nor at any size from "
				 "352x288 to 1920x1080)", first);
	}
	if (rc) {
		/* AJPG.DLL: compresses to nothing, decompresses its FOURCC. */
		if (strstr(msg, "only decompresses") ||
		    (r->decodes_own && strstr(msg, "gives no output format"))) {
			r->status = ST_DECODER;
			snprintf(r->note, sizeof(r->note), "decodes only (encode: "
				 "%s)", msg);
			return;
		}
		r->status = status_of_rc(rc);
		snprintf(r->note, sizeof(r->note), "encode: %s", msg);
		return;
	}
	/* The options that change the picture, as the clip is read for PSNR. */
	char *o;
	int w, h;
	if ((o = strstr(r->fix, "--pad ")))
		ro.pad = atoi(o + 6);
	else if ((o = strstr(r->fix, "--crop ")))
		ro.crop = atoi(o + 7);
	else if ((o = strstr(r->fix, "--scale ")) &&
		 sscanf(o + 8, "%dx%d", &w, &h) == 2) {
		ro.scale_w = w;
		ro.scale_h = h;
	}
	r->kbits = avi_kbits(avi, size_note, sizeof(size_note));

	rc = decode_to(s, dll, avi, y4m, NULL, out, err);
	int by_auto = 0;
	if (rc && rc < 124) {
		/*
		 * An encoder whose DLL has no decoder for what it makes
		 * (aslcodec_dshow.dll): whatever codec here decodes it.
		 */
		message_of(err, 0, msg, sizeof(msg));
		by_auto = !decode_to(s, "auto", avi, y4m, NULL, out, err);
		if (by_auto)
			rc = 0;
	}
	if (rc) {
		if (rc >= 124)
			message_of(err, rc > 128, msg, sizeof(msg));
		r->status = rc >= 124 ? status_of_rc(rc) : ST_ENCODES;
		snprintf(r->note, sizeof(r->note), "decode: %s", msg);
		return;
	}
	r->status = ST_ROUND_TRIP;
	r->psnr = psnr_of(s->video_in, &ro, y4m, 0, r->note, sizeof(r->note));
	const char *decoded = y4m;
	/*
	 * Nothing like what went in: a decoder whose YUV output is wrong
	 * (mhuffyuv.dll writes nothing as YUY2) may do better as RGB.
	 */
	if (r->psnr < PSNR_BROKEN &&
	    !decode_to(s, by_auto ? "auto" : dll, avi, y4m2, "BGR24", out,
		       err)) {
		char note2[128] = "";
		double p2 = psnr_of(s->video_in, &ro, y4m2, 0, note2,
				    sizeof(note2));
		/* (better by more than a dB, or enough to be a round trip) */
		if (p2 > r->psnr + 1 ||
		    (p2 >= PSNR_BROKEN && p2 > r->psnr)) {
			r->psnr = p2;
			decoded = y4m2;
			snprintf(r->note, sizeof(r->note), "decoded as BGR24: "
				 "its own choice of output gives nothing like the "
				 "input%s%s", note2[0] ? "; " : "", note2);
		}
	}
	if (r->psnr < PSNR_BROKEN) {
		r->status = ST_ENCODES;
		snprintf(r->note, sizeof(r->note), by_auto ?
			 "encodes, but no decoder here reads it back (decode "
			 "auto gives %.1f dB)" :
			 "encodes, but its own decoder gives nothing like the "
			 "input back (%.1f dB)", r->psnr);
		return;
	}
	if (by_auto && !r->note[0])
		snprintf(r->note, sizeof(r->note), "decoded by another codec "
			 "here (decode auto): its own DLL doesn't decode it");
	/* A good stream, for decoders of the same FOURCC, with its own decode. */
	char key[8];
	if (!by_auto && r->psnr >= POOL_PSNR && avi_fourcc(avi, key))
		pool_add(s, r, 0, key, avi, decoded);
}

/*
 * Encodes test audio and decodes it back: to each format tag an ACM driver
 * encodes to until one works, offering each PCM layout in turn to one that
 * takes none of those before.
 */
static void sweep_audio(struct sweep *s, size_t idx, const char *dll,
			struct result *r)
{
	char enc[PATH_MAX], dec[PATH_MAX], out[PATH_MAX], err[PATH_MAX];
	char msg[300] = "", first[300] = "", fe[96], fd[96], tagopt[16];
	int ntries = r->ntags ? r->ntags : 1, rc = -1, first_rc = -1;
	size_t layout = 0;

	work_path(s, idx, "enc.wav", enc, sizeof(enc));
	work_path(s, idx, "dec.wav", dec, sizeof(dec));
	work_path(s, idx, "out", out, sizeof(out));
	work_path(s, idx, "err", err, sizeof(err));

	for (int t = 0; t < ntries && rc; t++) {
		snprintf(tagopt, sizeof(tagopt), "0x%04x",
			 r->ntags ? r->tags[t] : 0);
		for (layout = 0; layout < NLAYOUTS; layout++) {
			const char *in = test_audio(s, layout);
			if (!in)
				return;
			const char *args[] = { "encode", dll, in, "-o", enc,
					       r->ntags ? "--format-tag" : NULL,
					       tagopt, NULL };
			rc = run(s, args, out, err);
			if (!rc)
				break;
			message_of(err, rc > 128, msg, sizeof(msg));
			if (first_rc < 0) {
				first_rc = rc;
				snprintf(first, sizeof(first), "%s", msg);
			}
			if (rc >= 124 || !strstr(msg, "takes neither PCM"))
				break;
		}
		if (!rc && r->ntags > 1)
			snprintf(r->fix, sizeof(r->fix), "--format-tag %s",
				 tagopt);
	}
	if (rc) {
		r->status = status_of_rc(first_rc);
		snprintf(r->note, sizeof(r->note), "encode: %s", first);
		return;
	}
	r->kbits = wav_describe(enc, fe, sizeof(fe));
	const char *dargs[] = { "decode", dll, enc, "-o", dec, NULL };
	rc = run(s, dargs, out, err);
	char from[64];
	snprintf(from, sizeof(from), "%lu Hz %s", (unsigned long)
		 pcm_layouts[layout].rate,
		 pcm_layouts[layout].channels == 1 ? "mono" : "stereo");
	if (rc) {
		message_of(err, rc > 128, msg, sizeof(msg));
		r->status = rc >= 124 ? status_of_rc(rc) : ST_ENCODES;
		snprintf(r->note, sizeof(r->note), "%s -> %s; decode: %s", from,
			 fe, msg);
		return;
	}
	wav_describe(dec, fd, sizeof(fd));
	r->status = ST_ROUND_TRIP;
	snprintf(r->note, sizeof(r->note), "%s -> %s -> %s", from, fe, fd);
	unsigned tag = wav_tag(enc);
	char key[8];
	snprintf(key, sizeof(key), "0x%04x", tag);
	/*
	 * Pooled only if what came back is the test signal: a decoder that
	 * gives silence mustn't become the reference others are held to.
	 */
	char gate[160] = "";
	const char *in = test_audio(s, layout);
	double q = in ? snr_of(in, dec, gate, sizeof(gate)) : NAN;
	if (tag && tag != WAVE_FORMAT_PCM && wav_tag(dec) == WAVE_FORMAT_PCM &&
	    !isnan(q) && q >= SAMPLE_SNR)
		pool_add(s, r, 1, key, enc, dec);
}

static void sweep_one(struct sweep *s, const char *dll, struct result *r)
{
	char out[PATH_MAX], err[PATH_MAX], msg[400];
	size_t idx = s->count;
	int venc, vdec, aenc, adec, donly;

	memset(r, 0, sizeof(*r));
	snprintf(r->file, sizeof(r->file), "%s", base_name(dll));
	r->psnr = r->kbits = NAN;
	work_path(s, idx, "probe", out, sizeof(out));
	work_path(s, idx, "err", err, sizeof(err));

	const char *args[] = { "probe", dll, NULL };
	int rc = run(s, args, out, err);
	read_probe(r, out, &venc, &vdec, &aenc, &adec, &donly);
	if (rc || !r->kind[0]) {
		message_of(err, rc > 128, msg, sizeof(msg));
		r->status = rc >= 124 ? status_of_rc(rc) :
			    strstr(msg, "not a driver") ||
			    strstr(msg, "RealPlayer") ||
			    strstr(msg, "not a PE") ? ST_NOT_CODEC : ST_FAILS;
		snprintf(r->note, sizeof(r->note), "%s", msg[0] ? msg :
			 "probe found no codec in it");
		goto done;
	}
	if (venc && !donly) {
		sweep_video(s, idx, dll, r);
	} else if (vdec) {
		r->status = ST_DECODER;
		snprintf(r->note, sizeof(r->note), "decodes only");
	} else if (aenc) {
		r->audio = 1;
		sweep_audio(s, idx, dll, r);
	} else if (adec) {
		r->audio = 1;
		r->status = ST_DECODER;
		snprintf(r->note, sizeof(r->note), "decodes audio only");
	} else {
		r->status = ST_NOT_CODEC;
		snprintf(r->note, sizeof(r->note), "no video or audio codec");
	}
done:
	/* What every step made, however far it got. */
	if (!s->keep) {
		static const char *const made[] = {
			"out", "avi", "y4m", "rgb.y4m", "enc.wav", "dec.wav",
		};
		unlink(out);
		unlink(err);
		for (size_t i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
			work_path(s, idx, made[i], out, sizeof(out));
			unlink(out);
		}
	}
}

/* Decoders, given samples */

/* Whether the DLL's bytes hold tag, as is or in upper or lower case. */
static int dll_names(const BYTE *dll, size_t n, const char *tag)
{
	char up[5], low[5];

	if (!dll || strlen(tag) != 4)
		return 0;
	for (int i = 0; i < 4; i++) {
		up[i] = (char)toupper((unsigned char)tag[i]);
		low[i] = (char)tolower((unsigned char)tag[i]);
	}
	return memmem(dll, n, tag, 4) || memmem(dll, n, up, 4) ||
	       memmem(dll, n, low, 4);
}

/* A file's bytes (at most 64 MB); NULL if unreadable. */
static BYTE *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	BYTE *buf = NULL;
	long len;

	*n = 0;
	if (!f)
		return NULL;
	if (!fseek(f, 0, SEEK_END) && (len = ftell(f)) > 0 &&
	    len < (64L << 20) && !fseek(f, 0, SEEK_SET) &&
	    (buf = malloc((size_t)len)))
		*n = fread(buf, 1, (size_t)len, f);
	fclose(f);
	return buf;
}

#define MAX_TRIES 4
/* As close to ffmpeg's decode as a decoder of the same stream gets. */
#define SAMPLE_PSNR 25.0

/*
 * Gives a decoder streams of what it takes: a sample of its VfW handler,
 * of the types it registers or the tags it decodes, then (VfW) of
 * FOURCCs its DLL names. The first that decodes close to the reference
 * makes it "decodes"; otherwise the note says what the samples got.
 */
static void check_decoder(struct sweep *s, size_t idx, const char *dll,
			  struct result *r)
{
	size_t pick[MAX_TRIES], npick = 0, nbytes = 0;
	int vfw = !strcmp(r->kind, "VfW");
	BYTE *bytes = NULL;

	for (int pass = 0; pass < 3 && npick < MAX_TRIES; pass++) {
		if (pass == 2 && vfw && !r->audio)
			bytes = slurp(dll, &nbytes);
		/* ffmpeg's samples before the sweep's own, in each pass */
		for (int pooled = 0; pooled < 2; pooled++)
			for (size_t i = 0; i < s->nsamples && npick < MAX_TRIES;
			     i++) {
				const struct sample *x = &s->samples[i];
				int match = 0, dup = 0;
				if (x->pooled != pooled || x->audio != r->audio)
					continue;
				for (size_t k = 0; k < npick; k++)
					dup |= pick[k] == i;
				if (pass == 0)
					match = vfw && !strcasecmp(x->tag, r->id);
				else if (pass == 1)
					for (int k = 0; k < r->ninputs; k++)
						match |= !strcasecmp(x->tag,
								     r->inputs[k]);
				else
					match = dll_names(bytes, nbytes, x->tag);
				if (match && !dup)
					pick[npick++] = i;
			}
	}
	free(bytes);
	if (!npick)
		return;

	char out[PATH_MAX], err[PATH_MAX], dec[PATH_MAX], msg[300];
	char tried[400] = "", note[160];
	size_t len = 0;
	work_path(s, idx, "out", out, sizeof(out));
	work_path(s, idx, "err", err, sizeof(err));
	for (size_t k = 0; k < npick; k++) {
		const struct sample *x = &s->samples[pick[k]];
		struct source_options ro = { .stream = -1 };
		double q;

		char name[32];
		snprintf(name, sizeof(name), "sample%zu.%s", k + 1,
			 x->audio ? "wav" : "y4m");
		work_path(s, idx, name, dec, sizeof(dec));
		const char *args[] = { "decode", dll, x->file, "-o", dec, NULL };
		int rc = run(s, args, out, err);
		note[0] = 0;
		if (rc) {
			message_of(err, rc > 128, msg, sizeof(msg));
			q = NAN;
			snprintf(note, sizeof(note), "%s%.140s", rc == 124 ?
				 "hangs; " : rc > 128 ? "crashes; " : "", msg);
		} else {
			q = x->audio ? snr_of(x->ref, dec, note, sizeof(note)) :
			    psnr_of(x->ref, &ro, dec, 1, note, sizeof(note));
		}
		if (!s->keep)
			unlink(dec);
		if (!isnan(q) && q >= (x->audio ? SAMPLE_SNR : SAMPLE_PSNR)) {
			r->status = ST_DECODES;
			r->psnr = q;
			if (x->pooled)
				snprintf(r->note, sizeof(r->note), "decodes what "
					 "%s made ('%s') %s its own decoder "
					 "does%s%s%s", x->name, x->tag,
					 isinf(q) ? "exactly as" : "close to how",
					 x->audio ? " (SNR)" : "",
					 note[0] ? "; " : "", note);
			else
				snprintf(r->note, sizeof(r->note), "decodes the "
					 "sample %s ('%s') %s its reference%s%s%s",
					 x->name, x->tag, isinf(q) ? "exactly as" :
					 "close to", x->audio ? " (SNR)" : "",
					 note[0] ? "; " : "", note);
			return;
		}
		if (!note[0] && !isnan(q))
			snprintf(note, sizeof(note), "%.1f dB", q);
		else if (!isnan(q))
			snprintf(note + strlen(note), sizeof(note) - strlen(note),
				 " (%.1f dB)", q);
		len += (size_t)snprintf(tried + len, len < sizeof(tried) ?
					sizeof(tried) - len : 0, "%s%s '%s': %s",
					k ? "; " : "", x->name, x->tag, note);
		if (len >= sizeof(tried))
			len = sizeof(tried) - 1;
	}
	size_t nl = strlen(r->note);
	snprintf(r->note + nl, sizeof(r->note) - nl, "%sgiven %s",
		 nl ? "; " : "", tried);
}

/* Reporting */

static void format_psnr(const struct result *r, char *buf, size_t size)
{
	if (isinf(r->psnr))
		snprintf(buf, size, "lossless");
	else if (!isnan(r->psnr))
		snprintf(buf, size, "%.1f dB", r->psnr);
	else
		buf[0] = 0;
}

static void print_line(FILE *f, const struct result *r)
{
	char psnr[32], kb[32] = "";

	format_psnr(r, psnr, sizeof(psnr));
	if (!isnan(r->kbits))
		snprintf(kb, sizeof(kb), "%.0f kbit/s", r->kbits);
	fprintf(f, "%-10s %-24s %-10s %-10s %-9s %-13s %s%s\n",
		status_names[r->status], r->file, r->kind, r->id, psnr, kb,
		r->fix[0] ? "with " : "", r->fix);
	if (r->note[0])
		fprintf(f, "%*s%s\n", 11, "", r->note);
}

/* Text for a table cell or TSV field: no tabs, newlines or bars. */
static void clean(const char *in, char *out, size_t size, int md)
{
	size_t n = 0;

	for (; *in && n + 3 < size; in++) {
		char c = *in;
		if (c == '\t' || c == '\n' || c == '\r')
			c = ' ';
		if (md && c == '|')
			out[n++] = '\\';
		out[n++] = c;
	}
	out[n] = 0;
}

static int write_tsv(const struct sweep *s, const char *path)
{
	FILE *f = fopen(path, "w");

	if (!f) {
		fprintf(stderr, "seance: %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(f, "#seance-sweep 1\tfile\tkind\tid\tstatus\tpsnr\tkbits\tfix\t"
		"note\n");
	for (size_t i = 0; i < s->count; i++) {
		const struct result *r = &s->results[i];
		char id[128], note[1024];
		clean(r->id, id, sizeof(id), 0);
		clean(r->note, note, sizeof(note), 0);
		fprintf(f, "\t%s\t%s\t%s\t%s\t", r->file, r->kind, id,
			status_names[r->status]);
		if (isinf(r->psnr))
			fprintf(f, "inf");
		else if (!isnan(r->psnr))
			fprintf(f, "%.2f", r->psnr);
		fprintf(f, "\t");
		if (!isnan(r->kbits))
			fprintf(f, "%.1f", r->kbits);
		fprintf(f, "\t%s\t%s\n", r->fix, note);
	}
	return fclose(f) ? -1 : 0;
}

static int write_markdown(const struct sweep *s, const char *path,
			  const char *what)
{
	FILE *f = fopen(path, "w");
	unsigned counts[ST_COUNT] = { 0 };

	if (!f) {
		fprintf(stderr, "seance: %s: %s\n", path, strerror(errno));
		return -1;
	}
	for (size_t i = 0; i < s->count; i++)
		counts[s->results[i].status]++;
	fprintf(f, "# Codec sweep\n\n%zu DLLs, given %s.\n\n", s->count, what);
	for (int st = ST_COUNT - 1; st >= 0; st--)
		if (counts[st])
			fprintf(f, "- %s: %u\n", status_names[st], counts[st]);
	fprintf(f, "\n| DLL | Kind | Id | Result | PSNR | kbit/s | Needed | "
		"Note |\n|---|---|---|---|---|---|---|---|\n");
	for (int st = ST_COUNT - 1; st >= 0; st--)
		for (size_t i = 0; i < s->count; i++) {
			const struct result *r = &s->results[i];
			char id[128], note[1100], psnr[32], kb[32] = "";
			if (r->status != (enum status)st)
				continue;
			clean(r->id, id, sizeof(id), 1);
			clean(r->note, note, sizeof(note), 1);
			format_psnr(r, psnr, sizeof(psnr));
			if (!isnan(r->kbits))
				snprintf(kb, sizeof(kb), "%.0f", r->kbits);
			fprintf(f, "| `%s` | %s%s | %s | %s | %s | %s | %s%s%s | "
				"%s |\n", r->file, r->kind,
				r->audio ? " (audio)" : "", id,
				status_names[r->status], psnr, kb,
				r->fix[0] ? "`" : "", r->fix,
				r->fix[0] ? "`" : "", note);
		}
	return fclose(f) ? -1 : 0;
}

/* Reads an earlier sweep's TSV; returns the number of results, or -1. */
static long read_tsv(const char *path, struct result **out)
{
	FILE *f = fopen(path, "r");
	char *line = NULL;
	size_t cap = 0, n = 0, alloc = 0;
	struct result *rs = NULL;

	*out = NULL;
	if (!f) {
		fprintf(stderr, "seance: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (getline(&line, &cap, f) < 0 ||
	    strncmp(line, "#seance-sweep 1\t", 16)) {
		fprintf(stderr, "seance: %s: not a sweep's TSV\n", path);
		free(line);
		fclose(f);
		return -1;
	}
	while (getline(&line, &cap, f) > 0) {
		char *field[9] = { 0 }, *p = line;
		int k = 0;
		line[strcspn(line, "\n")] = 0;
		while (k < 9) {
			field[k++] = p;
			if (!(p = strchr(p, '\t')))
				break;
			*p++ = 0;
		}
		if (k < 9 || !field[1][0])
			continue;
		if (n == alloc) {
			size_t na = alloc ? alloc * 2 : 64;
			struct result *t = realloc(rs, na * sizeof(*rs));
			if (!t)
				break;
			rs = t;
			alloc = na;
		}
		struct result *r = &rs[n];
		memset(r, 0, sizeof(*r));
		snprintf(r->file, sizeof(r->file), "%s", field[1]);
		snprintf(r->kind, sizeof(r->kind), "%s", field[2]);
		snprintf(r->id, sizeof(r->id), "%s", field[3]);
		r->status = ST_FAILS;
		for (int st = 0; st < ST_COUNT; st++)
			if (!strcmp(field[4], status_names[st]))
				r->status = (enum status)st;
		r->psnr = !field[5][0] ? NAN : !strcmp(field[5], "inf") ?
			  INFINITY : strtod(field[5], NULL);
		r->kbits = field[6][0] ? strtod(field[6], NULL) : NAN;
		snprintf(r->fix, sizeof(r->fix), "%s", field[7]);
		snprintf(r->note, sizeof(r->note), "%s", field[8]);
		n++;
	}
	free(line);
	fclose(f);
	*out = rs;
	return (long)n;
}

/* Prints what changed since old; returns how many got worse. */
static int compare(const struct sweep *s, const struct result *old, long nold)
{
	int worse = 0, changes = 0;
	char a[32], b[32];

	printf("\nChanges since the earlier sweep:\n");
	for (size_t i = 0; i < s->count; i++) {
		const struct result *r = &s->results[i], *o = NULL;
		for (long k = 0; k < nold && !o; k++)
			if (!strcasecmp(old[k].file, r->file))
				o = &old[k];
		if (!o) {
			printf("  new     %s: %s\n", r->file,
			       status_names[r->status]);
			changes++;
			continue;
		}
		int lower = r->status < o->status;
		int psnr_drop = r->status == o->status &&
				!isnan(o->psnr) && !isnan(r->psnr) &&
				r->psnr < o->psnr - 0.5;
		int psnr_lost = r->status == o->status && !isnan(o->psnr) &&
				isnan(r->psnr);
		if (r->status == o->status && !psnr_drop && !psnr_lost &&
		    !(isnan(o->psnr) && !isnan(r->psnr)) &&
		    !(!isnan(o->psnr) && !isnan(r->psnr) &&
		      r->psnr > o->psnr + 0.5))
			continue;
		changes++;
		format_psnr(o, a, sizeof(a));
		format_psnr(r, b, sizeof(b));
		int bad = lower || psnr_drop || psnr_lost;
		worse += bad;
		printf("  %s  %s: %s%s%s -> %s%s%s%s%s\n",
		       bad ? "WORSE " : "better", r->file,
		       status_names[o->status], a[0] ? " " : "", a,
		       status_names[r->status], b[0] ? " " : "", b,
		       bad && r->note[0] ? ": " : "", bad ? r->note : "");
	}
	for (long k = 0; k < nold; k++) {
		int found = 0;
		for (size_t i = 0; i < s->count && !found; i++)
			found = !strcasecmp(old[k].file, s->results[i].file);
		if (!found) {
			printf("  gone    %s (was %s)\n", old[k].file,
			       status_names[old[k].status]);
			changes++;
		}
	}
	if (!changes)
		printf("  none\n");
	return worse;
}

/* The command */

static int add_target(struct sweep *s, const char *path, char ***list,
		      size_t *n, size_t *cap)
{
	struct stat st;

	if (stat(path, &st)) {
		fprintf(stderr, "seance: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		if (*n == *cap) {
			*cap = *cap ? *cap * 2 : 64;
			*list = realloc(*list, *cap * sizeof(**list));
			if (!*list)
				return -1;
		}
		(*list)[(*n)++] = strdup(path);
		return 0;
	}
	struct dirent **names;
	int k = scandir(path, &names, NULL, alphasort);
	if (k < 0) {
		fprintf(stderr, "seance: %s: %s\n", path, strerror(errno));
		return -1;
	}
	for (int i = 0; i < k; i++) {
		char full[PATH_MAX];
		if (wanted(names[i]->d_name)) {
			snprintf(full, sizeof(full), "%s/%s", path,
				 names[i]->d_name);
			if (stat(full, &st) || !S_ISREG(st.st_mode) ||
			    add_target(s, full, list, n, cap)) {
				free(names[i]);
				continue;
			}
		}
		free(names[i]);
	}
	free(names);
	return 0;
}

static void remove_work(const struct sweep *s)
{
	char p[PATH_MAX];

	for (size_t i = 0; i < s->nsamples; i++)
		if (s->samples[i].pooled && !s->keep) {
			unlink(s->samples[i].file);
			unlink(s->samples[i].ref);
		}
	if (s->keep)
		return;
	snprintf(p, sizeof(p), "%s/clip.y4m", s->work);
	unlink(p);
	for (size_t k = 0; k < NLAYOUTS; k++)
		if (s->made_audio[k][0])
			unlink(s->made_audio[k]);
	rmdir(s->work);
}

int cmd_sweep(int argc, char **argv)
{
	struct sweep s = { .timeout = 30, .frames = 10, .width = 320,
			   .height = 240 };
	const char *md = NULL, *tsv = NULL, *old_tsv = NULL, *keep = NULL;
	const char *samples = NULL;
	char **targets = NULL, self[PATH_MAX], what[PATH_MAX + 64];
	size_t ntargets = 0, cap = 0;
	int status = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int v = i + 1 < argc;
		if (!strcmp(a, "-h") || !strcmp(a, "--help"))
			return usage();
		else if (!strcmp(a, "-i") && v)
			s.video_in = argv[++i];
		else if (!strcmp(a, "--markdown") && v)
			md = argv[++i];
		else if (!strcmp(a, "--tsv") && v)
			tsv = argv[++i];
		else if (!strcmp(a, "--compare") && v)
			old_tsv = argv[++i];
		else if (!strcmp(a, "--keep") && v)
			keep = argv[++i];
		else if (!strcmp(a, "--samples") && v)
			samples = argv[++i];
		else if (!strcmp(a, "--timeout") && v) {
			if (cli_parse_count(a, argv[++i], &s.timeout))
				return 2;
		} else if (!strcmp(a, "--frames") && v) {
			if (cli_parse_count(a, argv[++i], &s.frames))
				return 2;
			if (!s.frames || s.frames > 10000) {
				fprintf(stderr, "seance: --frames is 1 to 10000\n");
				return 2;
			}
		} else if (!strcmp(a, "--size") && v) {
			LONG w, h;
			if (cli_parse_size(a, argv[++i], &w, &h))
				return 2;
			s.width = (int)w;
			s.height = (int)h;
		} else if (a[0] == '-')
			return usage();
		else if (add_target(&s, a, &targets, &ntargets, &cap))
			return 2;
	}
	if (!ntargets) {
		if (argc > 1)
			fprintf(stderr, "seance: sweep: no codecs (*.dll, *.drv, "
				"*.ax, *.acm) there\n");
		return argc > 1 ? 2 : usage();
	}
	if (!s.timeout)
		s.timeout = 1;

	struct result *old = NULL;
	long nold = 0;
	if (old_tsv && (nold = read_tsv(old_tsv, &old)) < 0)
		return 2;
	if (samples && load_samples(&s, samples)) {
		free(old);
		free(s.samples);
		return 2;
	}

	ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (sl <= 0) {
		fprintf(stderr, "seance: sweep: can't find this program: %s\n",
			strerror(errno));
		return 2;
	}
	self[sl] = 0;
	s.self = self;
	char *emu = getenv("VFW_EMULATOR");
	char *emu_copy = emu && *emu ? strdup(emu) : NULL;
	for (char *t = emu_copy ? strtok(emu_copy, " ") : NULL; t && s.nemu < 8;
	     t = strtok(NULL, " "))
		s.emu[s.nemu++] = t;

	if (keep) {
		if (mkdir(keep, 0755) && errno != EEXIST) {
			fprintf(stderr, "seance: %s: %s\n", keep, strerror(errno));
			return 2;
		}
		snprintf(s.work, sizeof(s.work), "%s", keep);
		s.keep = 1;
	} else {
		const char *tmp = getenv("TMPDIR");
		snprintf(s.work, sizeof(s.work), "%s/seance-sweep-XXXXXX",
			 tmp && *tmp ? tmp : "/tmp");
		if (!mkdtemp(s.work)) {
			fprintf(stderr, "seance: sweep: %s: %s\n", s.work,
				strerror(errno));
			return 2;
		}
	}

	char tv[32];
	snprintf(tv, sizeof(tv), "%lu", s.timeout);
	setenv("VFW_TIMEOUT", tv, 1);
	if (!s.video_in && make_video(&s)) {
		remove_work(&s);
		return 2;
	}
	if (s.video_in == s.made_video)
		snprintf(what, sizeof(what), "a %dx%d test pattern (%lu frames) "
			 "and 16-bit PCM", s.width, s.height,
			 s.frames);
	else
		snprintf(what, sizeof(what), "%lu frames of %s and 16-bit "
			 "PCM", s.frames, s.video_in);
	if (samples) {
		size_t n = strlen(what);
		snprintf(what + n, sizeof(what) - n, ", and decoders the %zu "
			 "samples in %s", s.nsamples, samples);
	}

	if (!(s.results = calloc(ntargets, sizeof(*s.results)))) {
		remove_work(&s);
		return 2;
	}
	printf("Sweeping %zu DLLs, given %s:\n\n", ntargets, what);
	for (size_t i = 0; i < ntargets; i++) {
		sweep_one(&s, targets[i], &s.results[i]);
		s.count++;
		print_line(stdout, &s.results[i]);
		fflush(stdout);
	}

	/* Decoders, given the samples and what the encoders here made. */
	size_t ndec = 0;
	for (size_t i = 0; i < s.count; i++)
		ndec += s.results[i].status == ST_DECODER;
	if (ndec && s.nsamples) {
		printf("\nGiving %zu decoder%s %zu sample%s (%zu made here):\n\n",
		       ndec, ndec == 1 ? "" : "s", s.nsamples,
		       s.nsamples == 1 ? "" : "s", s.npooled);
		for (size_t i = 0; i < s.count; i++) {
			struct result *r = &s.results[i];
			if (r->status != ST_DECODER)
				continue;
			check_decoder(&s, i, targets[i], r);
			print_line(stdout, r);
			fflush(stdout);
			if (!s.keep) {
				char p[PATH_MAX];
				work_path(&s, i, "out", p, sizeof(p));
				unlink(p);
				work_path(&s, i, "err", p, sizeof(p));
				unlink(p);
			}
		}
	}
	for (size_t i = 0; i < ntargets; i++)
		free(targets[i]);
	free(targets);

	unsigned counts[ST_COUNT] = { 0 };
	for (size_t i = 0; i < s.count; i++)
		counts[s.results[i].status]++;
	printf("\n");
	for (int st = ST_COUNT - 1; st >= 0; st--)
		if (counts[st])
			printf("%s: %u  ", status_names[st], counts[st]);
	printf("\n");

	if (tsv && write_tsv(&s, tsv))
		status = 2;
	if (md && write_markdown(&s, md, what))
		status = 2;
	if (old_tsv && compare(&s, old, nold) && !status)
		status = 1;
	free(old);
	free(s.results);
	free(emu_copy);
	remove_work(&s);
	free(s.samples);
	return status;
}
