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
	ST_ENCODES,		/* encodes, but doesn't decode its output */
	ST_ROUND_TRIP,		/* encodes and decodes */
	ST_COUNT
};

static const char *const status_names[ST_COUNT] = {
	"hang", "crash", "not-codec", "fails", "decoder", "encodes",
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
	char fix[32];		/* the option it needed: "--pad 16" */
	char note[512];
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
		"  --keep <dir>         keep the files made, in dir\n");
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

/*
 * The PSNR of dec (a Y4M) against the clip as the codec was given it (ref
 * through the same --pad, --crop or --scale), over all three planes as
 * FFmpeg's average is. NAN with a note if they can't be compared.
 */
static double psnr_of(const char *ref, const struct source_options *ro,
		      const char *dec, char *note, size_t size)
{
	struct source_options dopt = { .stream = -1 };
	struct source *a = source_open(ref, ro), *b = NULL;
	struct image_conv *ca = NULL, *cb = NULL;
	BYTE *fa = NULL, *fb = NULL;
	double result = NAN, sum = 0;
	uint64_t count = 0;

	if (!a || !(b = source_open(dec, &dopt))) {
		snprintf(note, size, "can't compare: %s", vfw_error());
		goto out;
	}
	size_t na, nb;
	const BITMAPINFOHEADER *ha = source_format(a, &na);
	const BITMAPINFOHEADER *hb = source_format(b, &nb);
	int w = ha->biWidth, h = ha->biHeight < 0 ? -ha->biHeight : ha->biHeight;
	int bw = hb->biWidth, bh = hb->biHeight < 0 ? -hb->biHeight : hb->biHeight;
	if (w != bw || h != bh) {
		snprintf(note, size, "decodes at %dx%d, not %dx%d", bw, bh, w, h);
		goto out;
	}
	size_t fs = image_i420_size(w, h);
	if (!(ca = image_conv_new(ha, na)) || !(cb = image_conv_new(hb, nb)) ||
	    !(fa = malloc(fs)) || !(fb = malloc(fs))) {
		snprintf(note, size, "can't compare: %s", vfw_error());
		goto out;
	}
	for (;;) {
		int ra = frame_i420(a, ca, fa), rb = frame_i420(b, cb, fb);
		if (ra != 1 || rb != 1)
			break;
		for (size_t i = 0; i < fs; i++) {
			int d = (int)fa[i] - (int)fb[i];
			sum += (double)(d * d);
		}
		count += fs;
	}
	if (!count) {
		snprintf(note, size, "no frames to compare");
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

/* One codec */

static enum status status_of_rc(int rc)
{
	return rc == 124 ? ST_HANG : rc > 128 ? ST_CRASH : ST_FAILS;
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
	if (!r->id[0] && (*video_enc || *video_dec || *audio_enc || *audio_dec))
		snprintf(r->id, sizeof(r->id), "%s%s%s%s",
			 *video_enc ? "venc " : "", *video_dec ? "vdec " : "",
			 *audio_enc ? "aenc " : "", *audio_dec ? "adec" : "");
	size_t n = strlen(r->id);
	while (n && r->id[n - 1] == ' ')
		r->id[--n] = 0;
	free(text);
}

static void sweep_video(struct sweep *s, size_t idx, const char *dll,
			struct result *r)
{
	char avi[PATH_MAX], y4m[PATH_MAX], out[PATH_MAX], err[PATH_MAX];
	char frames[32], msg[400], size_note[64] = "";
	struct source_options ro = { .stream = -1 };

	work_path(s, idx, "avi", avi, sizeof(avi));
	work_path(s, idx, "y4m", y4m, sizeof(y4m));
	work_path(s, idx, "out", out, sizeof(out));
	work_path(s, idx, "err", err, sizeof(err));
	snprintf(frames, sizeof(frames), "%lu", s->frames);

	int rc = -1;
	for (int attempt = 0; attempt < 2; attempt++) {
		char opt[16] = "", val[16] = "";
		if (r->fix[0])
			sscanf(r->fix, "%15s %15s", opt, val);
		const char *args[] = { "encode", dll, s->video_in, "-o", avi,
				       "--frames", frames,
				       opt[0] ? opt : NULL, val, NULL };
		rc = run(s, args, out, err);
		if (!rc)
			break;
		message_of(err, rc > 128, msg, sizeof(msg));
		if (attempt || rc >= 124 || !fix_from(msg, r->fix, sizeof(r->fix)))
			break;
	}
	if (rc) {
		r->status = strstr(msg, "only decompresses") ? ST_DECODER :
			    status_of_rc(rc);
		snprintf(r->note, sizeof(r->note), "encode: %s", msg);
		return;
	}
	if (r->fix[0]) {
		int w, h;
		unsigned long n = strtoul(strchr(r->fix, ' ') + 1, NULL, 10);
		if (!strncmp(r->fix, "--pad", 5))
			ro.pad = (int)n;
		else if (!strncmp(r->fix, "--crop", 6))
			ro.crop = (int)n;
		else if (sscanf(strchr(r->fix, ' ') + 1, "%dx%d", &w, &h) == 2) {
			ro.scale_w = w;
			ro.scale_h = h;
		}
	}
	r->kbits = avi_kbits(avi, size_note, sizeof(size_note));

	const char *dargs[] = { "decode", dll, avi, "-o", y4m, NULL };
	rc = run(s, dargs, out, err);
	int by_auto = 0;
	if (rc && rc < 124) {
		/*
		 * An encoder whose DLL has no decoder for what it makes
		 * (aslcodec_dshow.dll): whatever codec here decodes it.
		 */
		message_of(err, 0, msg, sizeof(msg));
		const char *aargs[] = { "decode", "auto", avi, "-o", y4m, NULL };
		by_auto = !run(s, aargs, out, err);
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
	r->psnr = psnr_of(s->video_in, &ro, y4m, r->note, sizeof(r->note));
	if (by_auto && !r->note[0])
		snprintf(r->note, sizeof(r->note), "decoded by another codec "
			 "here (decode auto): its own DLL doesn't decode it");
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
			"out", "avi", "y4m", "enc.wav", "dec.wav",
		};
		unlink(out);
		unlink(err);
		for (size_t i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
			work_path(s, idx, made[i], out, sizeof(out));
			unlink(out);
		}
	}
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
		free(targets[i]);
	}
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
	return status;
}
