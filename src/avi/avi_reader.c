/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AVI reader: RIFF structure, stream headers, and the chunk index.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "avi/avi.h"
#include "common/error.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)
#define ID_RIFF FCC('R', 'I', 'F', 'F')
#define ID_LIST FCC('L', 'I', 'S', 'T')
#define ID_AVI  FCC('A', 'V', 'I', ' ')
#define ID_AVIX FCC('A', 'V', 'I', 'X')
#define ID_HDRL FCC('h', 'd', 'r', 'l')
#define ID_STRL FCC('s', 't', 'r', 'l')
#define ID_MOVI FCC('m', 'o', 'v', 'i')
#define ID_REC  FCC('r', 'e', 'c', ' ')
#define ID_ODML FCC('o', 'd', 'm', 'l')
#define ID_AVIH FCC('a', 'v', 'i', 'h')
#define ID_STRH FCC('s', 't', 'r', 'h')
#define ID_STRF FCC('s', 't', 'r', 'f')
#define ID_STRD FCC('s', 't', 'r', 'd')
#define ID_STRN FCC('s', 't', 'r', 'n')
#define ID_INDX FCC('i', 'n', 'd', 'x')
#define ID_IDX1 FCC('i', 'd', 'x', '1')
#define ID_DMLH FCC('d', 'm', 'l', 'h')

#define MAX_STREAMS      100		/* stream ids are two decimal digits */
#define MAX_HEADER_LIST  (16u << 20)
#define MAX_INDEX_CHUNK  (64u << 20)
#define MAX_MOVI_LISTS   4096

#define FAIL(...) do { vfw_set_error(__VA_ARGS__); return -1; } while (0)

static DWORD rd32(const BYTE *p)
{
	DWORD v;
	memcpy(&v, p, 4);
	return v;
}

static WORD rd16(const BYTE *p)
{
	WORD v;
	memcpy(&v, p, 2);
	return v;
}

static uint64_t rd64(const BYTE *p)
{
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

/* Reads len bytes at off, all or nothing. */
static int read_at(const struct avi_file *avi, uint64_t off, void *buf,
		   size_t len)
{
	size_t done = 0;

	if (off > avi->size || len > avi->size - off)
		FAIL("unexpected end of file");
	while (done < len) {
		ssize_t r = pread(avi->fd, (BYTE *)buf + done, len - done,
				  (off_t)(off + done));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			FAIL("read error: %s", r < 0 ? strerror(errno) : "short read");
		done += r;
	}
	return 0;
}

/* Private per-stream state while indexing. */
struct stream_extra {
	BYTE *indx;
	DWORD indx_size;
};

struct movi_list {
	uint64_t fourcc_pos;	/* of the "movi" list type, idx1's origin */
	uint64_t start, end;	/* its chunks */
};

struct parse {
	struct avi_file *avi;
	struct stream_extra *extra;	/* MAX_STREAMS entries */
	struct movi_list *movi;
	size_t nmovi;
	size_t first_movi;		/* how many are in the first RIFF */
	uint64_t idx1_pos;
	DWORD idx1_size;
	int have_idx1, have_hdrl;
};

/* The stream number in a chunk id like "01wb", or -1. */
static int stream_of(FOURCC id)
{
	int a = id & 0xff, b = (id >> 8) & 0xff;

	if (a < '0' || a > '9' || b < '0' || b > '9')
		return -1;
	return (a - '0') * 10 + (b - '0');
}

int avi_chunk_is_frame(const struct avi_chunk *c)
{
	return (c->id >> 16) != ('p' | 'c' << 8);
}

static int add_chunk(struct avi_stream *s, uint64_t offset, DWORD size,
		     DWORD flags, FOURCC id)
{
	if ((s->nchunks & (s->nchunks - 1)) == 0) {	/* at powers of two */
		size_t cap = s->nchunks ? s->nchunks * 2 : 1;
		struct avi_chunk *c = cap > SIZE_MAX / sizeof(*c) ? NULL :
				      realloc(s->chunks, cap * sizeof(*c));
		if (!c)
			FAIL("out of memory");
		s->chunks = c;
	}
	s->chunks[s->nchunks++] = (struct avi_chunk){ offset, size, flags, id };
	return 0;
}

/*
 * Walks the chunks of an in-memory list. Returns 1 and the next chunk, or
 * 0 at the end. A chunk that runs past the list is cut short.
 */
static int next_chunk(const BYTE *buf, size_t len, size_t *pos, FOURCC *id,
		      const BYTE **data, DWORD *size)
{
	if (*pos + 8 > len)
		return 0;
	*id = rd32(buf + *pos);
	DWORD cb = rd32(buf + *pos + 4);
	size_t avail = len - *pos - 8;
	*size = cb > avail ? (DWORD)avail : cb;
	*data = buf + *pos + 8;
	*pos += 8 + (size_t)*size + (*size & 1);
	return 1;
}

static void parse_strl(struct parse *p, const BYTE *buf, size_t len)
{
	struct avi_file *avi = p->avi;
	size_t pos = 0;
	FOURCC id;
	const BYTE *data;
	DWORD size;

	if (avi->nstreams >= MAX_STREAMS)
		return;
	struct avi_stream *s = &avi->streams[avi->nstreams];
	struct stream_extra *x = &p->extra[avi->nstreams];
	int have_strh = 0;

	while (next_chunk(buf, len, &pos, &id, &data, &size)) {
		BYTE **dst = NULL;
		DWORD *dst_size = NULL;

		if (id == ID_STRH) {
			size_t n = size < sizeof(s->header) - 8 ? size :
				   sizeof(s->header) - 8;
			memcpy((BYTE *)&s->header + 8, data, n);
			s->header.fcc = ID_STRH;
			s->header.cb = size;
			have_strh = 1;
		} else if (id == ID_STRF) {
			dst = &s->format;
			dst_size = &s->format_size;
		} else if (id == ID_STRD) {
			dst = &s->codec_data;
			dst_size = &s->codec_data_size;
		} else if (id == ID_INDX) {
			dst = &x->indx;
			dst_size = &x->indx_size;
		} else if (id == ID_STRN) {
			size_t n = size < sizeof(s->name) - 1 ? size : sizeof(s->name) - 1;
			memcpy(s->name, data, n);
			s->name[n] = '\0';
		}
		if (dst && !*dst && size) {
			*dst = malloc(size);
			if (*dst) {
				memcpy(*dst, data, size);
				*dst_size = size;
			}
		}
	}
	/* A strl without a header is not a stream. */
	if (have_strh)
		avi->nstreams++;
	else {
		free(s->format);
		free(s->codec_data);
		free(x->indx);
		memset(s, 0, sizeof(*s));
		memset(x, 0, sizeof(*x));
	}
}

static void parse_hdrl(struct parse *p, const BYTE *buf, size_t len)
{
	struct avi_file *avi = p->avi;
	size_t pos = 0;
	FOURCC id;
	const BYTE *data;
	DWORD size;

	while (next_chunk(buf, len, &pos, &id, &data, &size)) {
		if (id == ID_AVIH) {
			size_t n = size < sizeof(avi->header) - 8 ? size :
				   sizeof(avi->header) - 8;
			memcpy((BYTE *)&avi->header + 8, data, n);
			avi->header.fcc = ID_AVIH;
			avi->header.cb = size;
		} else if (id == ID_LIST && size >= 4) {
			FOURCC type = rd32(data);
			if (type == ID_STRL) {
				parse_strl(p, data + 4, size - 4);
			} else if (type == ID_ODML) {
				size_t q = 0;
				FOURCC cid;
				const BYTE *cdata;
				DWORD csize;
				while (next_chunk(data + 4, size - 4, &q, &cid, &cdata, &csize))
					if (cid == ID_DMLH && csize >= 4)
						avi->total_frames = rd32(cdata);
			}
		}
	}
	p->have_hdrl = 1;
}

/* Reads and walks one RIFF chunk's children. */
static int parse_riff(struct parse *p, uint64_t pos, uint64_t end, int first)
{
	struct avi_file *avi = p->avi;
	BYTE hdr[12];

	while (pos + 8 <= end) {
		if (read_at(avi, pos, hdr, 8))
			return -1;
		FOURCC id = rd32(hdr);
		uint64_t cb = rd32(hdr + 4);
		uint64_t data = pos + 8;
		if (data + cb > end) {
			avi->truncated = 1;
			cb = end - data;
		}
		uint64_t next = data + cb + (cb & 1);

		if (id == ID_LIST && cb >= 4) {
			if (read_at(avi, data, hdr + 8, 4))
				return -1;
			FOURCC type = rd32(hdr + 8);
			if (type == ID_HDRL && first && !p->have_hdrl) {
				if (cb - 4 > MAX_HEADER_LIST)
					FAIL("AVI header list too large");
				BYTE *buf = malloc(cb - 4 ? cb - 4 : 1);
				if (!buf)
					FAIL("out of memory");
				if (read_at(avi, data + 4, buf, cb - 4)) {
					free(buf);
					return -1;
				}
				parse_hdrl(p, buf, cb - 4);
				free(buf);
			} else if (type == ID_MOVI && p->nmovi < MAX_MOVI_LISTS) {
				struct movi_list *m = realloc(p->movi,
							      (p->nmovi + 1) * sizeof(*m));
				if (!m)
					FAIL("out of memory");
				p->movi = m;
				m[p->nmovi++] = (struct movi_list){ data, data + 4,
								     data + cb };
			}
		} else if (id == ID_IDX1 && first && !p->have_idx1) {
			p->idx1_pos = data;
			p->idx1_size = cb;
			p->have_idx1 = 1;
		}
		pos = next;
	}
	return 0;
}

/* An OpenDML standard index (ix##): entries relative to a base offset. */
static int load_std_index(struct parse *p, struct avi_stream *s,
			  const BYTE *buf, size_t len)
{
	if (len < 24)
		FAIL("OpenDML index too short");
	WORD longs = rd16(buf);
	BYTE type = buf[3];
	DWORD n = rd32(buf + 4);
	FOURCC id = rd32(buf + 8);
	uint64_t base = rd64(buf + 12);

	if (type != AVI_INDEX_OF_CHUNKS || longs < 2)
		FAIL("unsupported OpenDML index (type %u, %u longs per entry)",
		     type, longs);
	size_t stride = longs * 4u;
	if (n > (len - 24) / stride)
		n = (len - 24) / stride;
	for (DWORD i = 0; i < n; i++) {
		const BYTE *e = buf + 24 + i * stride;
		DWORD off = rd32(e), size = rd32(e + 4);
		uint64_t at = base + off;
		DWORD bytes = size & ~AVISTDINDEX_DELTAFRAME;
		if (at < 8 || at > p->avi->size || bytes > p->avi->size - at) {
			p->avi->truncated = 1;
			continue;
		}
		if (add_chunk(s, at, bytes,
			      (size & AVISTDINDEX_DELTAFRAME) ? 0 : AVIIF_KEYFRAME, id))
			return -1;
	}
	return 0;
}

/* The OpenDML index of one stream: a super index of ix## chunks. */
static int load_odml(struct parse *p, struct avi_stream *s,
		     const struct stream_extra *x)
{
	const BYTE *buf = x->indx;
	size_t len = x->indx_size;

	if (len < 24)
		FAIL("OpenDML super index too short");
	if (buf[3] == AVI_INDEX_OF_CHUNKS)	/* a standard index in strl */
		return load_std_index(p, s, buf, len);
	if (buf[3] != AVI_INDEX_OF_INDEXES || rd16(buf) != 4)
		FAIL("unsupported OpenDML super index");

	DWORD n = rd32(buf + 4);
	if (n > (len - 24) / 16)
		n = (len - 24) / 16;
	for (DWORD i = 0; i < n; i++) {
		const BYTE *e = buf + 24 + i * 16;
		uint64_t off = rd64(e);
		BYTE hdr[8];

		if (read_at(p->avi, off, hdr, 8)) {
			p->avi->truncated = 1;
			continue;
		}
		DWORD cb = rd32(hdr + 4);
		if (cb > MAX_INDEX_CHUNK || off + 8 + cb > p->avi->size) {
			p->avi->truncated = 1;
			continue;
		}
		BYTE *ix = malloc(cb ? cb : 1);
		if (!ix)
			FAIL("out of memory");
		int r = read_at(p->avi, off + 8, ix, cb) ||
			load_std_index(p, s, ix, cb);
		free(ix);
		if (r)
			return -1;
	}
	return 0;
}

/* Whether the chunk header at pos has id (for guessing idx1's origin). */
static int chunk_at(struct parse *p, uint64_t pos, FOURCC id)
{
	BYTE b[4];
	return !read_at(p->avi, pos, b, 4) && rd32(b) == id;
}

static int load_idx1(struct parse *p)
{
	struct avi_file *avi = p->avi;
	size_t n = p->idx1_size / 16;
	BYTE *buf;

	if (!n || !p->nmovi)
		return 0;
	if (!(buf = malloc(n * 16)))
		FAIL("out of memory");
	if (read_at(avi, p->idx1_pos, buf, n * 16)) {
		free(buf);
		return -1;
	}

	/*
	 * Offsets are meant to be relative to the "movi" list type, but some
	 * writers use absolute file offsets: see which one the first entry
	 * that can be checked agrees with.
	 */
	uint64_t origin = p->movi[0].fourcc_pos;
	for (size_t i = 0; i < n; i++) {
		FOURCC id = rd32(buf + i * 16);
		DWORD off = rd32(buf + i * 16 + 8);
		if (stream_of(id) < 0)
			continue;
		if (chunk_at(p, origin + off, id))
			break;
		if (chunk_at(p, off, id)) {
			origin = 0;
			break;
		}
	}

	for (size_t i = 0; i < n; i++) {
		const BYTE *e = buf + i * 16;
		FOURCC id = rd32(e);
		DWORD flags = rd32(e + 4), off = rd32(e + 8), size = rd32(e + 12);
		int sn = stream_of(id);
		uint64_t at = origin + off + 8;

		if ((flags & AVIIF_LIST) || sn < 0 || (unsigned)sn >= avi->nstreams)
			continue;
		if (at > avi->size || size > avi->size - at) {
			avi->truncated = 1;
			continue;
		}
		if (add_chunk(&avi->streams[sn], at, size, flags, id)) {
			free(buf);
			return -1;
		}
	}
	free(buf);
	return 0;
}

/* Indexes by walking the chunks of the movi lists, from index `from`. */
static int scan_movi(struct parse *p, size_t from)
{
	struct avi_file *avi = p->avi;
	BYTE hdr[12];

	for (size_t m = from; m < p->nmovi; m++) {
		uint64_t pos = p->movi[m].start, end = p->movi[m].end;
		while (pos + 8 <= end) {
			if (read_at(avi, pos, hdr, 8))
				return -1;
			FOURCC id = rd32(hdr);
			uint64_t cb = rd32(hdr + 4);
			if (pos + 8 + cb > end) {
				avi->truncated = 1;
				cb = end - pos - 8;
			}
			if (id == ID_LIST) {
				/* Descend into 'rec ' groups; skip other lists. */
				if (cb >= 4 && !read_at(avi, pos + 8, hdr + 8, 4) &&
				    rd32(hdr + 8) == ID_REC) {
					pos += 12;
					continue;
				}
			} else {
				int sn = stream_of(id);
				if (sn >= 0 && (unsigned)sn < avi->nstreams &&
				    add_chunk(&avi->streams[sn], pos + 8, cb,
					      AVIIF_KEYFRAME, id))
					return -1;
			}
			pos += 8 + cb + (cb & 1);
		}
	}
	return 0;
}

static int build_index(struct parse *p)
{
	struct avi_file *avi = p->avi;
	unsigned with_indx = 0;

	for (unsigned i = 0; i < avi->nstreams; i++)
		with_indx += p->extra[i].indx != NULL;

	if (with_indx && with_indx == avi->nstreams) {
		int ok = 1;
		size_t total = 0;
		for (unsigned i = 0; i < avi->nstreams && ok; i++) {
			ok = !load_odml(p, &avi->streams[i], &p->extra[i]);
			total += avi->streams[i].nchunks;
		}
		if (ok && total) {
			avi->index_source = AVI_INDEX_ODML;
			return 0;
		}
		/* A broken OpenDML index: start over from the other sources. */
		for (unsigned i = 0; i < avi->nstreams; i++)
			avi->streams[i].nchunks = 0;
	}

	if (p->have_idx1) {
		if (load_idx1(p))
			return -1;
		size_t total = 0;
		for (unsigned i = 0; i < avi->nstreams; i++)
			total += avi->streams[i].nchunks;
		if (total) {
			avi->index_source = AVI_INDEX_IDX1;
			/* idx1 only covers the first RIFF; scan any extensions. */
			return p->nmovi > p->first_movi ?
			       scan_movi(p, p->first_movi) : 0;
		}
	}

	avi->index_source = AVI_INDEX_SCAN;
	return scan_movi(p, 0);
}

struct avi_file *avi_open(const char *path)
{
	struct avi_file *avi = calloc(1, sizeof(*avi));
	struct parse p = { .avi = avi };
	struct stat st;
	BYTE hdr[12];

	if (!avi) {
		vfw_set_error("out of memory");
		return NULL;
	}
	avi->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (avi->fd < 0) {
		vfw_set_error("%s: %s", path, strerror(errno));
		free(avi);
		return NULL;
	}
	if (fstat(avi->fd, &st)) {
		vfw_set_error("%s: %s", path, strerror(errno));
		goto fail;
	}
	avi->size = st.st_size;
	avi->streams = calloc(MAX_STREAMS, sizeof(*avi->streams));
	p.extra = calloc(MAX_STREAMS, sizeof(*p.extra));
	if (!avi->streams || !p.extra) {
		vfw_set_error("out of memory");
		goto fail;
	}

	if (read_at(avi, 0, hdr, 12) || rd32(hdr) != ID_RIFF ||
	    rd32(hdr + 8) != ID_AVI) {
		vfw_set_error("%s: not an AVI file", path);
		goto fail;
	}

	/* RIFF 'AVI ', then any number of RIFF 'AVIX' extensions. */
	uint64_t pos = 0;
	while (pos + 12 <= avi->size) {
		if (read_at(avi, pos, hdr, 12))
			break;
		FOURCC type = rd32(hdr + 8);
		if (rd32(hdr) != ID_RIFF ||
		    (pos == 0 ? type != ID_AVI : type != ID_AVIX))
			break;
		uint64_t cb = rd32(hdr + 4);
		/* A zero size is what an interrupted capture leaves: to EOF. */
		if (!cb || pos + 8 + cb > avi->size) {
			avi->truncated |= cb != 0;
			cb = avi->size - pos - 8;
		}
		if (parse_riff(&p, pos + 12, pos + 8 + cb, pos == 0))
			goto fail_path;
		if (pos == 0)
			p.first_movi = p.nmovi;
		avi->riff_count++;
		pos += 8 + cb + (cb & 1);
	}

	if (!p.have_hdrl) {
		vfw_set_error("%s: no AVI header (hdrl)", path);
		goto fail;
	}
	if (!avi->nstreams) {
		vfw_set_error("%s: no streams", path);
		goto fail;
	}
	if (build_index(&p))
		goto fail_path;

	for (unsigned i = 0; i < avi->nstreams; i++) {
		struct avi_stream *s = &avi->streams[i];
		for (size_t c = 0; c < s->nchunks; c++)
			s->frames += avi_chunk_is_frame(&s->chunks[c]);
	}
	if (!avi->total_frames)
		avi->total_frames = avi->header.dwTotalFrames;

	for (unsigned i = 0; i < MAX_STREAMS; i++)
		free(p.extra[i].indx);
	free(p.extra);
	free(p.movi);
	return avi;

fail_path: {
		char msg[512];
		snprintf(msg, sizeof(msg), "%s", vfw_error());
		vfw_set_error("%s: %s", path, msg);
	}
fail:
	if (p.extra)
		for (unsigned i = 0; i < MAX_STREAMS; i++)
			free(p.extra[i].indx);
	free(p.extra);
	free(p.movi);
	avi_close(avi);
	return NULL;
}

void avi_close(struct avi_file *avi)
{
	if (!avi)
		return;
	if (avi->streams) {
		for (unsigned i = 0; i < MAX_STREAMS; i++) {
			free(avi->streams[i].format);
			free(avi->streams[i].codec_data);
			free(avi->streams[i].chunks);
		}
		free(avi->streams);
	}
	if (avi->fd >= 0)
		close(avi->fd);
	free(avi);
}

int avi_video_stream(const struct avi_file *avi)
{
	for (unsigned i = 0; i < avi->nstreams; i++)
		if (avi->streams[i].header.fccType == FCC('v', 'i', 'd', 's'))
			return (int)i;
	return -1;
}

const BITMAPINFOHEADER *avi_stream_bih(const struct avi_stream *s)
{
	const BITMAPINFOHEADER *bih = (const BITMAPINFOHEADER *)s->format;

	if (s->header.fccType != FCC('v', 'i', 'd', 's') ||
	    s->format_size < sizeof(*bih) || bih->biSize < sizeof(*bih) ||
	    bih->biSize > s->format_size)
		return NULL;
	return bih;
}

int avi_read_chunk(const struct avi_file *avi, const struct avi_chunk *c,
		   void *buf)
{
	return read_at(avi, c->offset, buf, c->size);
}

const char *avi_index_source_name(enum avi_index_source s)
{
	switch (s) {
	case AVI_INDEX_IDX1: return "idx1";
	case AVI_INDEX_ODML: return "OpenDML";
	case AVI_INDEX_SCAN: return "scan of the movi lists";
	default:             return "none";
	}
}
