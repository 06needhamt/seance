/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Helpers the CLI commands share.
 */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "avi/avi.h"
#include "cmd_util.h"
#include "common/error.h"
#include "common/fourcc.h"
#include "in/audio.h"
#include "runtime/runtime.h"
#include "vfw/icm.h"
#include "vfw/registry.h"
#include "win32/msacm32.h"

int cli_parse_count(const char *opt, const char *s, unsigned long *out)
{
	char *end;

	errno = 0;
	unsigned long v = strtoul(s, &end, 10);
	if (!*s || *end || errno || s[0] == '-') {
		fprintf(stderr, "seance: %s wants a number, not '%s'\n", opt, s);
		return -1;
	}
	*out = v;
	return 0;
}

int cli_parse_position(const char *opt, const char *s, struct cli_position *p)
{
	const char *q = s;
	double secs = 0, part;
	char *end;
	int fields = 0;

	memset(p, 0, sizeof(*p));
	if (*s && strspn(s, "0123456789") == strlen(s)) {
		errno = 0;
		p->count = strtoull(s, &end, 10);
		if (!errno)
			return 0;
	} else {
		/* [h:]m:s, or a number and a unit. */
		int ok = 1;
		for (;;) {
			if (!isdigit((unsigned char)*q) && *q != '.') {
				ok = 0;
				break;
			}
			errno = 0;
			part = strtod(q, &end);
			if (end == q || errno) {
				ok = 0;
				break;
			}
			secs = secs * 60 + part;
			fields++;
			q = end;
			if (*q != ':')
				break;
			if (fields == 3) {
				ok = 0;
				break;
			}
			q++;
		}
		if (ok && fields > 1 && !*q) {
			p->is_time = 1;
		} else if (ok && fields == 1 && !strcmp(q, "s")) {
			p->is_time = 1;
		} else if (ok && fields == 1 && !strcmp(q, "ms")) {
			p->is_time = 1;
			secs /= 1000;
		}
		if (p->is_time && secs < 1e12) {
			p->seconds = secs;
			return 0;
		}
		p->is_time = 0;
	}
	fprintf(stderr, "seance: %s wants a count, or a time such as 2.5s, "
		"1500ms or 1:02.5, not '%s'\n", opt, s);
	return -1;
}

unsigned long long cli_position_units(const struct cli_position *p,
				      DWORD rate, DWORD scale)
{
	if (!p->is_time)
		return p->count;
	if (!rate || !scale)
		return 0;
	/* A hair over, so 0.1s at 10 per second is 1, not 0.99999. */
	return (unsigned long long)(p->seconds * rate / scale + 1e-6);
}

int cli_wants_audio(const char *in, long stream, int want, const char *format,
		    const char *out, const char *const *audio_kinds)
{
	const char *dot = out ? strrchr(out, '.') : NULL;
	const char *name = format ? format :
			   dot && !strchr(dot, '/') ? dot + 1 : "";
	enum audio_source_kind k = audio_source_kind_of_path(in);
	struct avi_file *avi;
	int audio;

	if (want || k == AUDIO_SOURCE_WAV || k == AUDIO_SOURCE_RAW)
		return 1;
	if (!strcmp(in, "-"))
		return 0;
	for (size_t i = 0; audio_kinds && audio_kinds[i]; i++)
		if (!strcasecmp(name, audio_kinds[i]))
			return 1;
	k = audio_source_sniff(in);
	if (k != AUDIO_SOURCE_AVI)
		return k == AUDIO_SOURCE_WAV;
	if (!(avi = avi_open(in)))
		return 0;	/* reported by the video path */
	if (stream >= 0)
		audio = (unsigned long)stream < avi->nstreams &&
			avi_stream_wfx(&avi->streams[stream]);
	else
		audio = avi_video_stream(avi) < 0 && avi_audio_stream(avi) >= 0;
	avi_close(avi);
	return audio;
}

int cli_parse_size(const char *opt, const char *s, LONG *w, LONG *h)
{
	long x, y;
	char end;

	if (sscanf(s, "%ldx%ld%c", &x, &y, &end) != 2 || x <= 0 || y <= 0 ||
	    x > 16384 || y > 16384) {
		fprintf(stderr, "seance: %s wants WxH, not '%s'\n", opt, s);
		return -1;
	}
	*w = x;
	*h = y;
	return 0;
}

int cli_parse_rate(const char *opt, const char *s, DWORD *rate, DWORD *scale)
{
	unsigned long n, d;
	double f;
	char end;

	if ((sscanf(s, "%lu/%lu%c", &n, &d, &end) == 2 ||
	     sscanf(s, "%lu:%lu%c", &n, &d, &end) == 2) &&
	    n && d && n <= UINT32_MAX && d <= UINT32_MAX) {
		*rate = (DWORD)n;
		*scale = (DWORD)d;
		return 0;
	}
	if (sscanf(s, "%lf%c", &f, &end) == 1 && f > 0 && f <= 1000000) {
		/* 29.97 -> 30000/1001; other decimals to 1/1000. */
		if (f > 29.969 && f < 29.971) {
			*rate = 30000;
			*scale = 1001;
		} else if (f > 23.975 && f < 23.977) {
			*rate = 24000;
			*scale = 1001;
		} else if (f == (double)(DWORD)f) {
			*rate = (DWORD)f;
			*scale = 1;
		} else {
			*rate = (DWORD)(f * 1000 + 0.5);
			*scale = 1000;
		}
		return 0;
	}
	fprintf(stderr, "seance: %s wants a frame rate (25, 30000/1001, "
		"29.97), not '%s'\n", opt, s);
	return -1;
}

const char *cli_format_desc(const BITMAPINFOHEADER *b, char *buf, size_t size)
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

static int looks_like_dll(const char *s)
{
	size_t n = strlen(s);
	return strchr(s, '/') || !access(s, F_OK) ||
	       (n > 4 && (!strcasecmp(s + n - 4, ".dll") ||
			  !strcasecmp(s + n - 4, ".drv") ||
			  !strcasecmp(s + n - 3, ".ax")));
}

static HIC try_open(const struct cli_codec *c, FOURCC handler,
		    const UINT *modes, size_t nmodes)
{
	HIC hic = NULL;

	for (size_t i = 0; i < nmodes && !hic; i++)
		hic = c->is_dll ?
		      ic_open_dll(c->target, ICTYPE_VIDEO, handler, modes[i]) :
		      ic_open(ICTYPE_VIDEO, handler, modes[i]);
	return hic;
}

int cli_open_codec(const char *arg, const char *fourcc, FOURCC guess,
		   const UINT *modes, size_t nmodes, struct cli_codec *c)
{
	FOURCC type = ICTYPE_VIDEO, handler = 0;
	int named = 0;

	memset(c, 0, sizeof(*c));
	/* An existing file named without a directory is a path, not a name. */
	if (!strchr(arg, '/') && !access(arg, F_OK))
		snprintf(c->target, sizeof(c->target), "./%s", arg);
	else
		snprintf(c->target, sizeof(c->target), "%s", arg);
	if (runtime_enter()) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}

	c->is_dll = looks_like_dll(c->target);
	if (fourcc) {
		if (vfw_parse_fourcc(fourcc, &type, &handler)) {
			fprintf(stderr, "seance: bad FOURCC '%s'\n", fourcc);
			return 2;
		}
		named = 1;
	}
	if (!c->is_dll && vfw_parse_fourcc(c->target, &type, &handler)) {
		fprintf(stderr, "seance: '%s' is neither a DLL nor a FOURCC\n",
			c->target);
		return 2;
	}
	/*
	 * An ACM driver isn't opened as a video codec: some (vdowave.acm)
	 * refuse DRV_OPEN for any type but 'audc' and then use the instance
	 * they didn't make, so asking is a crash.
	 */
	HACMDRIVERID acm;
	if (c->is_dll && !msacm_add_path(c->target, &acm)) {
		fprintf(stderr, "seance: %s is an ACM audio driver, not a video "
			"codec; give it audio (WAV, raw PCM or an AVI's audio)\n",
			c->target);
		return 1;
	}
	if (c->is_dll && !named) {
		const struct vfw_codec *r = vfw_find_path(c->target);
		if (r) {
			handler = r->handler;
			named = 1;
		}
	}

	/* The handlers to try, in order. */
	FOURCC tries[2];
	size_t ntries = 0;
	if (c->is_dll && !named) {
		if (guess)
			tries[ntries++] = guess;
		tries[ntries++] = 0;
	} else {
		tries[ntries++] = handler;
	}
	char first_error[512] = "";
	for (size_t i = 0; i < ntries && !c->hic; i++) {
		c->hic = try_open(c, tries[i], modes, nmodes);
		c->handler = tries[i];
		if (!c->hic && !i)
			snprintf(first_error, sizeof(first_error), "%s", vfw_error());
	}
	if (!c->hic) {
		char fcc[FOURCC_STR_SIZE];
		const char *e = first_error[0] ? first_error : vfw_error();
		fprintf(stderr, "seance: %s\n", e);
		if (c->is_dll && !named && strstr(e, "DRV_OPEN"))
			fprintf(stderr, "seance: some codecs only open under their "
				"FOURCC; try --fourcc\n");
		else if (!c->is_dll && strstr(e, "no codec is registered"))
			fprintf(stderr, "seance: codecs are registered in %s, "
				"as vidc.%s=<path to the DLL>\n", vfw_ini_path(),
				fourcc_str(handler, fcc));
		return 1;
	}

	c->have_info = ic_get_info(c->hic, &c->info, sizeof(c->info)) != 0;
	/* Reopen under the codec's own FOURCC if it was opened without one. */
	if (c->is_dll && c->have_info && !c->handler && c->info.fccHandler) {
		HIC again = try_open(c, c->info.fccHandler, modes, nmodes);
		if (again) {
			ic_close(c->hic);
			c->hic = again;
			c->handler = c->info.fccHandler;
		}
	}
	if (c->is_dll && c->handler && !vfw_find(ICTYPE_VIDEO, c->handler))
		vfw_register(ICTYPE_VIDEO, c->handler, c->target);
	return 0;
}

const char *cli_codec_name(const struct cli_codec *c)
{
	return c->is_dll ? c->target : ic_driver_name(c->hic);
}

void cli_close_codec(struct cli_codec *c)
{
	if (c->hic)
		ic_close(c->hic);
	c->hic = NULL;
}
