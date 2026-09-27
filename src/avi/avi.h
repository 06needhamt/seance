/*
 * AVI files: a reader for RIFF AVI including OpenDML (AVI 2.0, over 1 GB),
 * and a writer for single-video-stream files.
 *
 * The reader builds a per-stream index of data chunks from, in order of
 * preference, the OpenDML indexes (indx / ix##), the AVI 1.0 idx1 index,
 * or a scan of the movi lists. Every offset and size taken from the file
 * is checked, so damaged or truncated files fail cleanly or yield what
 * can be recovered.
 */
#ifndef VFW_AVI_AVI_H
#define VFW_AVI_AVI_H

#include <stddef.h>
#include <stdint.h>

#include "win32.h"

/* One data chunk of a stream. */
struct avi_chunk {
	uint64_t offset;	/* of the data, just after the 8-byte chunk header */
	DWORD size;
	DWORD flags;		/* AVIIF_KEYFRAME, ... */
	FOURCC id;		/* e.g. "00dc"; "00pc" for a palette change */
};

struct avi_stream {
	AVISTREAMHEADER header;	/* strh */
	BYTE *format;		/* strf: BITMAPINFOHEADER (+ extra) for video */
	DWORD format_size;
	BYTE *codec_data;	/* strd, passed to some codecs */
	DWORD codec_data_size;
	char name[128];		/* strn, UTF-8 or as found */

	struct avi_chunk *chunks;
	size_t nchunks;
	size_t frames;		/* chunks that are frames (not palette changes) */
};

enum avi_index_source {
	AVI_INDEX_NONE,
	AVI_INDEX_IDX1,
	AVI_INDEX_ODML,
	AVI_INDEX_SCAN,
};

struct avi_file {
	int fd;
	uint64_t size;
	AVIMAINHEADER header;		/* avih */
	DWORD total_frames;		/* dmlh dwGrandFrames, else avih */
	struct avi_stream *streams;
	unsigned nstreams;
	unsigned riff_count;		/* 1 + the number of AVIX extensions */
	enum avi_index_source index_source;
	int truncated;			/* chunks ran past the end of the file */
};

/* Opens and indexes an AVI file. NULL with vfw_error() on failure. */
struct avi_file *avi_open(const char *path);
void avi_close(struct avi_file *avi);

/* The first video stream, or -1. */
int avi_video_stream(const struct avi_file *avi);

/* A video stream's BITMAPINFOHEADER, or NULL if strf is not one. */
const BITMAPINFOHEADER *avi_stream_bih(const struct avi_stream *s);

/* Reads a chunk's data (chunk->size bytes). Returns 0 or -1. */
int avi_read_chunk(const struct avi_file *avi, const struct avi_chunk *chunk,
		   void *buf);

/* Whether a chunk carries a frame, rather than a palette change. */
int avi_chunk_is_frame(const struct avi_chunk *chunk);

const char *avi_index_source_name(enum avi_index_source s);

/* Writer. */

struct avi_writer_options {
	/*
	 * RIFF chunk size limit; once the first one would pass it the file
	 * becomes OpenDML (AVIX extensions). 0 means 1 GB, as AVI 1.0 readers
	 * expect.
	 */
	uint64_t riff_limit;
	int force_odml;		/* write OpenDML indexes even if small */
	int no_idx1;		/* leave out idx1 (for testing readers) */
	int idx1_absolute;	/* idx1 offsets from the file start */
};

struct avi_writer;

/*
 * Starts an AVI with one video stream whose format is fmt (a
 * BITMAPINFOHEADER plus any palette or extra data), at rate / scale frames
 * per second. opts may be NULL.
 */
struct avi_writer *avi_writer_open(const char *path, const void *fmt,
				   DWORD fmt_size, FOURCC handler, DWORD rate,
				   DWORD scale,
				   const struct avi_writer_options *opts);

/* Appends one frame. Returns 0 or -1. */
int avi_writer_add(struct avi_writer *w, const void *data, DWORD size,
		   int keyframe);

/* Finishes the file (indexes and headers). Returns 0 or -1; frees w. */
int avi_writer_close(struct avi_writer *w);

#endif /* VFW_AVI_AVI_H */
