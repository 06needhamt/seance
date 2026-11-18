// SPDX-License-Identifier: GPL-2.0-only
/*
 * RealMedia files. See rm.h.
 *
 * A file is a run of chunks, each a FOURCC, a 32-bit size (counting the
 * 10-byte head) and a 16-bit version, all big-endian: .RMF, then PROP
 * (the file's properties), MDPR (one per stream), CONT (title and such),
 * then DATA, holding the packets, and maybe INDX. A packet is a 12-byte
 * head (13 in version 1) and what its stream put in it.
 */
#include "in/rm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/log.h"

#define BE4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | \
			 (uint32_t)(c) << 8 | (uint32_t)(d))
/* The largest packet taken: their length is a 16-bit field. */
#define MAX_PACKET 65536
/* The largest stream header (MDPR) taken. */
#define MAX_MDPR (1u << 20)
/* The largest interleaving group taken (sipr's are some KB, cook's tens). */
#define MAX_GROUP (16u << 20)

struct video_state {
	BYTE *buf;		/* the frame being put together */
	size_t cap, len, total;
	unsigned nslices;
	uint32_t offsets[RM_MAX_SLICES];
	int pic, active, key;
	uint32_t time;
};

struct audio_state {
	BYTE *group;		/* sub_packet_h packets' worth */
	size_t group_len;
	unsigned filled;	/* packets put in so far */
	size_t out_pos;		/* the next block to give, once full */
	int ready;
	uint32_t time;
};

struct rm_file {
	FILE *f;
	char path[256];
	struct rm_stream streams[RM_MAX_STREAMS];
	unsigned nstreams;
	uint64_t data_left;	/* bytes left of the current DATA chunk */
	int ended;
	/* the packet being taken apart */
	BYTE packet[MAX_PACKET];
	size_t packet_len, packet_pos;
	int packet_stream;	/* index into streams, or -1 */
	uint32_t packet_time;
	int packet_key;
	struct video_state video[RM_MAX_STREAMS];
	struct audio_state audio[RM_MAX_STREAMS];
};

static uint32_t be16(const BYTE *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const BYTE *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}
/* A FOURCC stored as four bytes in order, as a Windows FOURCC. */
static FOURCC fourcc_at(const BYTE *p)
{
	return (FOURCC)p[0] | (FOURCC)p[1] << 8 | (FOURCC)p[2] << 16 |
	       (FOURCC)p[3] << 24;
}

static int read_exact(struct rm_file *rm, void *buf, size_t n)
{
	if (fread(buf, 1, n, rm->f) == n)
		return 0;
	if (ferror(rm->f))
		vfw_set_error("%s: %s", rm->path, strerror(errno));
	else
		vfw_set_error("%s: ends early", rm->path);
	return -1;
}

static int skip(struct rm_file *rm, uint64_t n)
{
	BYTE buf[4096];

	while (n) {
		size_t k = n < sizeof(buf) ? (size_t)n : sizeof(buf);
		if (read_exact(rm, buf, k))
			return -1;
		n -= k;
	}
	return 0;
}

/*
 * A reader over a header's bytes: each read past the end gives zeros and
 * marks it short, so a parse checks once at the end.
 */
struct cursor {
	const BYTE *p;
	size_t len, pos;
	int short_;
};

static const BYTE *take(struct cursor *c, size_t n)
{
	static const BYTE zeros[8];

	if (c->short_ || n > c->len - c->pos) {
		c->short_ = 1;
		return zeros;
	}
	c->pos += n;
	return c->p + c->pos - n;
}
static uint32_t take8(struct cursor *c) { return *take(c, 1); }
static uint32_t take16(struct cursor *c) { return be16(take(c, 2)); }
static uint32_t take32(struct cursor *c) { return be32(take(c, 4)); }

/* A length-prefixed (8-bit) string as a FOURCC, or 0 if it isn't one. */
static FOURCC take_str4(struct cursor *c)
{
	unsigned n = take8(c);
	const BYTE *s = take(c, n);
	return n == 4 && !c->short_ ? fourcc_at(s) : 0;
}

/* The VIDO header: size, "VIDO", FOURCC, width, height, ... then extra */
static int parse_video(struct rm_stream *st)
{
	struct cursor c = { st->type_data, st->type_len, 0, 0 };

	take32(&c);
	if (take32(&c) != BE4('V', 'I', 'D', 'O'))
		return -1;
	st->fourcc = fourcc_at(take(&c, 4));
	st->width = take16(&c);
	st->height = take16(&c);
	take16(&c);		/* bits per pixel, or so */
	take32(&c);
	st->fps = take32(&c);
	if (c.short_)
		return -1;
	st->extra = st->type_data + c.pos;
	st->extra_len = st->type_len - c.pos;
	st->video = 1;
	return 0;
}

/* The .ra header, versions 3 (14.4 kbit/s), 4 and 5. */
static int parse_audio(struct rm_stream *st)
{
	struct cursor c = { st->type_data, st->type_len, 0, 0 };

	if (take32(&c) != BE4('.', 'r', 'a', 0xfd))
		return -1;
	st->version = take16(&c);
	if (st->version == 3) {
		/* Only ever lpcJ: 20-byte frames of 8 kHz mono */
		st->fourcc = fourcc_at((const BYTE *)"lpcJ");
		st->rate = 8000;
		st->bits = 16;
		st->channels = 1;
		st->coded_frame_size = st->frame_size = 20;
		st->sub_packet_h = 1;
		st->audio = 1;
		return 0;
	}
	if (st->version != 4 && st->version != 5)
		return -1;
	take16(&c);		/* unused */
	take32(&c);		/* .ra4 or .ra5 */
	take32(&c);		/* data size */
	take16(&c);		/* version again */
	take32(&c);		/* header size */
	st->flavor = take16(&c);
	st->coded_frame_size = take32(&c);
	take(&c, 12);		/* bytes per minute and such */
	st->sub_packet_h = take16(&c);
	st->frame_size = take16(&c);
	st->sub_packet_size = take16(&c);
	take16(&c);
	if (st->version == 5)
		take(&c, 6);
	st->rate = take16(&c);
	take16(&c);
	st->bits = take16(&c);
	st->channels = take16(&c);
	if (st->version == 5) {
		st->interleaver = fourcc_at(take(&c, 4));
		st->fourcc = fourcc_at(take(&c, 4));
	} else {
		st->interleaver = take_str4(&c);
		st->fourcc = take_str4(&c);
	}
	if (c.short_ || !st->fourcc)
		return -1;
	/* Then, for some codecs (cook, atrc, sipr, raac), their own data */
	size_t skip_n = st->version == 5 ? 4 : 3;
	if (st->type_len - c.pos >= skip_n + 4) {
		take(&c, skip_n);
		uint32_t n = take32(&c);
		if (n && n <= st->type_len - c.pos) {
			st->codec_data = st->type_data + c.pos;
			st->codec_data_len = n;
		}
	}
	st->audio = 1;
	return 0;
}

static int read_mdpr(struct rm_file *rm, const BYTE *body, size_t len)
{
	struct cursor c = { body, len, 0, 0 };

	if (rm->nstreams == RM_MAX_STREAMS) {
		vfw_set_error("%s: more than %d streams", rm->path,
			      RM_MAX_STREAMS);
		return -1;
	}
	struct rm_stream *st = &rm->streams[rm->nstreams];
	st->number = take16(&c);
	take32(&c);		/* max bit rate */
	st->avg_bit_rate = take32(&c);
	take32(&c);		/* max packet size */
	take32(&c);		/* average packet size */
	take32(&c);		/* start time */
	take32(&c);		/* preroll */
	st->duration = take32(&c);
	unsigned n = take8(&c);
	take(&c, n);		/* name */
	n = take8(&c);
	const BYTE *mime = take(&c, n);
	if (!c.short_)
		snprintf(st->mime, sizeof(st->mime), "%.*s", (int)n,
			 (const char *)mime);
	uint32_t tl = take32(&c);
	const BYTE *td = take(&c, tl);
	if (c.short_) {
		vfw_set_error("%s: a stream header ends early", rm->path);
		return -1;
	}
	if (tl && !(st->type_data = malloc(tl))) {
		vfw_set_error("out of memory");
		return -1;
	}
	if (tl)
		memcpy(st->type_data, td, tl);
	st->type_len = tl;
	rm->nstreams++;
	if (tl >= 8 && be32(td + 4) == BE4('V', 'I', 'D', 'O'))
		parse_video(st);
	else if (tl >= 4 && be32(td) == BE4('.', 'r', 'a', 0xfd))
		parse_audio(st);
	return 0;
}

int rm_sniff(const char *path)
{
	BYTE head[4];
	FILE *f = fopen(path, "rb");
	int yes = 0;

	if (f) {
		yes = fread(head, 1, 4, f) == 4 && !memcmp(head, ".RMF", 4);
		fclose(f);
	}
	return yes;
}

struct rm_file *rm_open(const char *path)
{
	struct rm_file *rm = calloc(1, sizeof(*rm));
	BYTE head[10];

	if (!rm) {
		vfw_set_error("out of memory");
		return NULL;
	}
	snprintf(rm->path, sizeof(rm->path), "%s", path);
	rm->packet_stream = -1;
	if (!(rm->f = fopen(path, "rb"))) {
		vfw_set_error("%s: %s", path, strerror(errno));
		free(rm);
		return NULL;
	}
	if (read_exact(rm, head, 10) || memcmp(head, ".RMF", 4)) {
		vfw_set_error("%s: not a RealMedia file", path);
		goto fail;
	}
	uint32_t size = be32(head + 4);
	if (size < 10) {
		vfw_set_error("%s: a .RMF chunk of %u bytes", path, size);
		goto fail;
	}
	if (skip(rm, size - 10))
		goto fail;
	for (;;) {
		if (read_exact(rm, head, 10))
			goto fail;
		uint32_t id = be32(head);
		size = be32(head + 4);
		if (size < 10) {
			vfw_set_error("%s: a chunk of %u bytes", path, size);
			goto fail;
		}
		if (id == BE4('D', 'A', 'T', 'A')) {
			BYTE d[8];
			if (size < 18 || read_exact(rm, d, 8))
				goto fail;
			rm->data_left = size - 18;
			break;
		}
		if (id != BE4('M', 'D', 'P', 'R')) {
			if (skip(rm, size - 10))
				goto fail;
			continue;
		}
		/* A stream's header: its type data is some bytes, never MBs */
		if (size - 10 > MAX_MDPR) {
			vfw_set_error("%s: an MDPR chunk of %u bytes", path,
				      size);
			goto fail;
		}
		BYTE *body = malloc(size - 10);
		if (!body) {
			vfw_set_error("out of memory");
			goto fail;
		}
		int rc = read_exact(rm, body, size - 10) ? -1 :
			 read_mdpr(rm, body, size - 10);
		free(body);
		if (rc)
			goto fail;
	}
	if (!rm->nstreams) {
		vfw_set_error("%s: no streams", path);
		goto fail;
	}
	return rm;
fail:
	rm_close(rm);
	return NULL;
}

void rm_close(struct rm_file *rm)
{
	if (!rm)
		return;
	for (unsigned i = 0; i < rm->nstreams; i++) {
		free(rm->streams[i].type_data);
		free(rm->video[i].buf);
		free(rm->audio[i].group);
	}
	if (rm->f)
		fclose(rm->f);
	free(rm);
}

unsigned rm_nstreams(const struct rm_file *rm) { return rm->nstreams; }

const struct rm_stream *rm_stream(const struct rm_file *rm, unsigned i)
{
	return i < rm->nstreams ? &rm->streams[i] : NULL;
}

int rm_video_stream(const struct rm_file *rm)
{
	for (unsigned i = 0; i < rm->nstreams; i++)
		if (rm->streams[i].video)
			return (int)i;
	return -1;
}

int rm_audio_stream(const struct rm_file *rm)
{
	for (unsigned i = 0; i < rm->nstreams; i++)
		if (rm->streams[i].audio)
			return (int)i;
	return -1;
}

/*
 * The next packet, into rm->packet. 1, 0 at the end (of the packets: a
 * file may stop short of its DATA chunk's size), -1 with vfw_error().
 */
static int next_packet(struct rm_file *rm)
{
	BYTE h[13];

	rm->packet_stream = -1;
	rm->packet_len = rm->packet_pos = 0;
	while (!rm->ended) {
		if (rm->data_left < 12) {
			/* Another DATA chunk may follow (rarely) */
			BYTE c[18];
			if (rm->data_left && skip(rm, rm->data_left))
				return -1;
			size_t got = fread(c, 1, 18, rm->f);
			if (got != 18 && ferror(rm->f)) {
				vfw_set_error("%s: %s", rm->path,
					      strerror(errno));
				return -1;
			}
			if (got != 18 ||
			    be32(c) != BE4('D', 'A', 'T', 'A') ||
			    be32(c + 4) < 18) {
				rm->ended = 1;
				return 0;
			}
			rm->data_left = be32(c + 4) - 18;
			continue;
		}
		size_t got = fread(h, 1, 12, rm->f);
		if (got < 12 && ferror(rm->f)) {
			vfw_set_error("%s: %s", rm->path, strerror(errno));
			return -1;
		}
		if (got < 12) {
			rm->ended = 1;
			return 0;
		}
		unsigned version = be16(h), len = be16(h + 2);
		size_t head = version == 1 ? 13 : 12;
		if (version > 1 || len < head || len > rm->data_left) {
			/* Not a packet: the end, or an index after short data */
			rm->ended = 1;
			return 0;
		}
		if (head == 13 && read_exact(rm, h + 12, 1))
			return -1;
		rm->data_left -= len;
		unsigned number = be16(h + 4);
		rm->packet_time = be32(h + 6);
		rm->packet_key = version == 1 ? !!(h[12] & 2) : !!(h[11] & 2);
		if (read_exact(rm, rm->packet, len - head))
			return -1;
		rm->packet_len = len - head;
		for (unsigned i = 0; i < rm->nstreams; i++)
			if (rm->streams[i].number == number)
				rm->packet_stream = (int)i;
		if (rm->packet_stream >= 0)
			return 1;
	}
	return 0;
}

/* A 14- or 30-bit number in a video packet's head; -1 if it's cut off. */
static long take_num(const BYTE *p, size_t len, size_t *pos)
{
	if (*pos + 2 > len)
		return -1;
	uint32_t n = be16(p + *pos) & 0x7fff;
	*pos += 2;
	if (n >= 0x4000)
		return n - 0x4000;
	if (*pos + 2 > len)
		return -1;
	n = n << 16 | be16(p + *pos);
	*pos += 2;
	return n;
}

static int frame_room(struct video_state *v, size_t n)
{
	if (n <= v->cap)
		return 0;
	BYTE *b = realloc(v->buf, n);
	if (!b) {
		vfw_set_error("out of memory");
		return -1;
	}
	v->buf = b;
	v->cap = n;
	return 0;
}

/* A frame of one slice, from bytes of the packet. */
static void whole_frame(struct rm_frame *f, const BYTE *p, size_t len,
			uint32_t time, int key)
{
	f->data = p;
	f->len = len;
	f->nslices = 1;
	f->offsets[0] = 0;
	f->time = time;
	f->key = key;
}

/* Hands out the frame put together so far. */
static void assembled(struct video_state *v, struct rm_frame *f)
{
	f->data = v->buf;
	f->len = v->len;
	f->nslices = v->nslices;
	memcpy(f->offsets, v->offsets, v->nslices * sizeof(v->offsets[0]));
	f->time = v->time;
	f->key = v->key;
	v->active = 0;
}

/*
 * Takes the next part of the packet. 1 with a frame in f, 0 if the packet
 * is used up without one, -1 if it makes no sense.
 *
 * Each part starts with a byte whose top two bits say what it is: 0, part
 * of a frame; 1, a whole frame (the rest of the packet); 2, the last part
 * of a frame; 3, one of several whole frames in the packet. All but 3 have
 * a sequence byte next (its low 7 bits number the parts of a frame from
 * 1). All but 1 then have the frame's length, the part's offset in it (for
 * 2, the part's length; for 3, the frame's time) and the frame's number.
 */
static int video_part(struct rm_file *rm, struct video_state *v,
		      struct rm_frame *f)
{
	const BYTE *p = rm->packet;
	size_t len = rm->packet_len, *pos = &rm->packet_pos;
	unsigned seq = 0, pic = 0;
	long total = 0, at = 0;

	if (*pos >= len)
		return 0;
	unsigned hdr = p[(*pos)++], type = hdr >> 6;
	if (type != 3) {
		if (*pos >= len)
			return -1;
		seq = p[(*pos)++];
	}
	if (type != 1) {
		if ((total = take_num(p, len, pos)) < 0 ||
		    (at = take_num(p, len, pos)) < 0 || *pos >= len)
			return -1;
		pic = p[(*pos)++];
	}
	size_t left = len - *pos;
	if (type == 1) {
		whole_frame(f, p + *pos, left, rm->packet_time, rm->packet_key);
		*pos = len;
		return 1;
	}
	if (type == 3) {
		if ((size_t)total > left)
			return -1;
		whole_frame(f, p + *pos, (size_t)total, (uint32_t)at,
			    rm->packet_key);
		*pos += (size_t)total;
		return 1;
	}
	/* A part of a frame: a new frame at part 1, or a new frame number */
	if (type == 2 && (size_t)at > left) {
		/* The last part, cut short: the frame can't be whole */
		v->active = 0;
		return -1;
	}
	size_t n = type == 2 ? (size_t)at : left;
	if ((seq & 0x7f) == 1 || !v->active || (int)pic != v->pic) {
		if (v->active)
			TRACE("rm: frame %d dropped, %zu of %zu bytes", v->pic,
			      v->len, v->total);
		v->active = 1;
		v->pic = (int)pic;
		v->len = v->nslices = 0;
		v->total = (size_t)total;
		v->time = rm->packet_time;
		v->key = rm->packet_key;
	}
	if (v->nslices == RM_MAX_SLICES || frame_room(v, v->len + n))
		return -1;
	v->offsets[v->nslices++] = (uint32_t)v->len;
	memcpy(v->buf + v->len, p + *pos, n);
	v->len += n;
	*pos += n;
	if (type == 2 || v->len >= v->total) {
		assembled(v, f);
		return 1;
	}
	return 0;
}

int rm_read_frame(struct rm_file *rm, unsigned i, struct rm_frame *f)
{
	if (i >= rm->nstreams || !rm->streams[i].video) {
		vfw_set_error("%s: stream %u isn't video", rm->path, i);
		return -1;
	}
	struct video_state *v = &rm->video[i];
	for (;;) {
		if (rm->packet_stream == (int)i) {
			int rc = video_part(rm, v, f);
			if (rc < 0) {
				TRACE("rm: a video packet at %u ms makes no "
				      "sense; skipped", rm->packet_time);
				rm->packet_pos = rm->packet_len;
			} else if (rc) {
				return 1;
			}
			if (rm->packet_pos < rm->packet_len)
				continue;
		}
		int rc = next_packet(rm);
		if (rc < 0)
			return -1;
		if (!rc) {
			/* A frame cut off at the end isn't given */
			if (v->active)
				TRACE("rm: frame %d cut off at the end, %zu of "
				      "%zu bytes", v->pic, v->len, v->total);
			v->active = 0;
			return 0;
		}
	}
}

/* Get and set the nibble at index i of buf (low nibble first). */
static unsigned nibble(const BYTE *buf, size_t i)
{
	return buf[i >> 1] >> (i & 1 ? 4 : 0) & 0xf;
}
static void set_nibble(BYTE *buf, size_t i, unsigned v)
{
	unsigned shift = i & 1 ? 4 : 0;
	buf[i >> 1] = (BYTE)((buf[i >> 1] & ~(0xf << shift)) |
			     (v & 0xf) << shift);
}

void rm_sipr_reorder(BYTE *buf, unsigned sub_packet_h, unsigned frame_size)
{
	/* The group is 96 blocks; these pairs of them are swapped. */
	static const BYTE swaps[38][2] = {
		{ 0, 63 }, { 1, 22 }, { 2, 44 }, { 3, 90 }, { 5, 81 },
		{ 7, 31 }, { 8, 86 }, { 9, 58 }, { 10, 36 }, { 12, 68 },
		{ 13, 39 }, { 14, 73 }, { 15, 53 }, { 16, 69 }, { 17, 57 },
		{ 19, 88 }, { 20, 34 }, { 21, 71 }, { 24, 46 }, { 25, 94 },
		{ 26, 54 }, { 28, 75 }, { 29, 50 }, { 32, 70 }, { 33, 92 },
		{ 35, 74 }, { 38, 85 }, { 40, 56 }, { 42, 87 }, { 43, 65 },
		{ 45, 59 }, { 48, 79 }, { 49, 93 }, { 51, 89 }, { 55, 95 },
		{ 61, 76 }, { 67, 83 }, { 77, 80 },
	};
	/* Each block is this many nibbles */
	size_t block = (size_t)sub_packet_h * frame_size * 2 / 96;

	for (size_t k = 0; k < sizeof(swaps) / sizeof(swaps[0]); k++) {
		size_t a = block * swaps[k][0], b = block * swaps[k][1];
		for (size_t j = 0; j < block; j++) {
			unsigned x = nibble(buf, a + j), y = nibble(buf, b + j);
			set_nibble(buf, a + j, y);
			set_nibble(buf, b + j, x);
		}
	}
}

/*
 * Puts a packet of an interleaved stream into its place in the group.
 * 0, or -1 if it isn't the size the interleaver needs.
 */
static int deinterleave(const struct rm_stream *st, struct audio_state *a,
			const BYTE *p, size_t len)
{
	/* All 16-bit but cfs, checked by interleaved(): no product wraps */
	uint64_t h = st->sub_packet_h, w = st->frame_size;
	uint64_t y = a->filled, cfs = st->coded_frame_size;
	uint64_t sps = st->sub_packet_size;

	switch (st->interleaver) {
	case mmioFOURCC('I', 'n', 't', '4'):
		/* h / 2 frames of cfs bytes, every other row */
		if (len < h / 2 * cfs ||
		    (h / 2 - 1) * 2 * w + y * cfs + cfs > a->group_len)
			return -1;
		for (uint64_t x = 0; x < h / 2; x++)
			memcpy(a->group + x * 2 * w + y * cfs, p + x * cfs,
			       (size_t)cfs);
		return 0;
	case mmioFOURCC('g', 'e', 'n', 'r'):
		/* w / sps sub-packets, even packets' to the first half */
		if (len < w)
			return -1;
		for (uint64_t x = 0; x < w / sps; x++) {
			uint64_t at = sps * (h * x + (h + 1) / 2 * (y & 1) +
					     (y >> 1));
			if (at + sps > a->group_len)
				return -1;
			memcpy(a->group + at, p + x * sps, (size_t)sps);
		}
		return 0;
	case mmioFOURCC('s', 'i', 'p', 'r'):
		if (len < w)
			return -1;
		memcpy(a->group + y * w, p, w);
		return 0;
	}
	return -1;
}

static int interleaved(const struct rm_stream *st)
{
	switch (st->interleaver) {
	case mmioFOURCC('I', 'n', 't', '4'):
	case mmioFOURCC('g', 'e', 'n', 'r'):
	case mmioFOURCC('s', 'i', 'p', 'r'):
		return 1;
	}
	return 0;
}

/*
 * Whether an interleaved stream's geometry makes sense: a group that
 * fits in memory, Int4 frames that fill its rows exactly, genr rows of
 * whole sub-packets. 0, or -1 with vfw_error().
 */
static int check_geometry(const struct rm_file *rm, const struct rm_stream *st)
{
	uint64_t h = st->sub_packet_h, w = st->frame_size;
	uint64_t cfs = st->coded_frame_size, sps = st->sub_packet_size;
	int ok = h && w && h * w <= MAX_GROUP;

	switch (st->interleaver) {
	case mmioFOURCC('I', 'n', 't', '4'):
		/* Rows of frames that fill the group, as FFmpeg's reader wants */
		ok = ok && h >= 2 && cfs && cfs <= w && cfs * h == 2 * w;
		break;
	case mmioFOURCC('g', 'e', 'n', 'r'):
		ok = ok && sps && w % sps == 0;
		break;
	}
	if (!ok)
		vfw_set_error("%s: '%.4s' interleaving of %u x %u bytes "
			      "(frames %u, sub-packets %u) makes no sense",
			      rm->path, (const char *)&st->interleaver,
			      st->sub_packet_h, st->frame_size,
			      st->coded_frame_size, st->sub_packet_size);
	return ok ? 0 : -1;
}

int rm_read_audio(struct rm_file *rm, unsigned i, const BYTE **data,
		  size_t *len, uint32_t *time)
{
	if (i >= rm->nstreams || !rm->streams[i].audio) {
		vfw_set_error("%s: stream %u isn't audio", rm->path, i);
		return -1;
	}
	const struct rm_stream *st = &rm->streams[i];
	struct audio_state *a = &rm->audio[i];

	if (!interleaved(st)) {
		int rc;
		while ((rc = next_packet(rm)) > 0 && rm->packet_stream != (int)i)
			;
		if (rc <= 0)
			return rc;
		*data = rm->packet;
		*len = rm->packet_len;
		*time = rm->packet_time;
		rm->packet_pos = rm->packet_len;
		return 1;
	}
	if (!a->group) {
		if (check_geometry(rm, st))
			return -1;
		a->group_len = (size_t)st->sub_packet_h * st->frame_size;
		if (!(a->group = calloc(1, a->group_len))) {
			vfw_set_error("out of memory");
			return -1;
		}
	}
	while (!a->ready) {
		int rc = next_packet(rm);
		if (rc <= 0)
			return rc;	/* a part-filled group is dropped */
		if (rm->packet_stream != (int)i)
			continue;
		if (!a->filled)
			a->time = rm->packet_time;
		if (deinterleave(st, a, rm->packet, rm->packet_len)) {
			vfw_set_error("%s: a packet of %zu bytes doesn't fit "
				      "the '%.4s' interleaving", rm->path,
				      rm->packet_len,
				      (const char *)&st->interleaver);
			return -1;
		}
		rm->packet_pos = rm->packet_len;
		if (++a->filled < st->sub_packet_h)
			continue;
		if (st->interleaver == mmioFOURCC('s', 'i', 'p', 'r'))
			rm_sipr_reorder(a->group, st->sub_packet_h,
					st->frame_size);
		a->filled = 0;
		a->out_pos = 0;
		a->ready = 1;
	}
	*data = a->group + a->out_pos;
	*len = st->frame_size;
	*time = a->time;
	a->out_pos += st->frame_size;
	if (a->out_pos >= a->group_len)
		a->ready = 0;
	return 1;
}
