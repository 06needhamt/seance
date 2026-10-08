/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AVI files: a reader for RIFF AVI including OpenDML (AVI 2.0, over 1 GB),
 * and a writer of video and audio streams.
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
	/*
	 * Where the chunk starts in the stream, in units of dwScale / dwRate
	 * seconds from dwStart: the frame number for video and for streams
	 * without a sample size (one unit a chunk), else the bytes before it
	 * divided by dwSampleSize (audio blocks).
	 */
	uint64_t start;
};

struct avi_stream {
	AVISTREAMHEADER header;	/* strh */
	/*
	 * strf: a BITMAPINFOHEADER (+ extra) for video; for audio a
	 * WAVEFORMATEX (+ extra) of at least 18 bytes, a 16-byte one given
	 * cbSize 0 and cbSize trimmed to the bytes there are.
	 */
	BYTE *format;
	DWORD format_size;
	BYTE *codec_data;	/* strd, passed to some codecs */
	DWORD codec_data_size;
	char name[128];		/* strn, UTF-8 or as found */

	struct avi_chunk *chunks;
	size_t nchunks;
	size_t frames;		/* chunks that are frames (not palette changes) */
	uint64_t length;	/* the units of all its chunks (see start) */
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

/* The first audio stream, or -1. */
int avi_audio_stream(const struct avi_file *avi);

/* A video stream's BITMAPINFOHEADER, or NULL if strf is not one. */
const BITMAPINFOHEADER *avi_stream_bih(const struct avi_stream *s);

/* An audio stream's format, or NULL if it has none. */
const WAVEFORMATEX *avi_stream_wfx(const struct avi_stream *s);

/*
 * The time of a position in a stream (in its units, as avi_chunk.start),
 * in 100 ns units, counting dwStart. 0 when the stream has no rate.
 */
int64_t avi_stream_time(const struct avi_stream *s, uint64_t units);

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
 * Starts an AVI file, to which streams are added and then chunks written.
 * opts may be NULL.
 */
struct avi_writer *avi_writer_create(const char *path,
				     const struct avi_writer_options *opts);

/*
 * Adds a video stream whose format is fmt (a BITMAPINFOHEADER plus any
 * palette or extra data), at rate / scale frames per second. Returns the
 * stream's number, or -1. Streams are added before the first chunk.
 */
int avi_writer_add_video(struct avi_writer *w, const void *fmt,
			 DWORD fmt_size, FOURCC handler, DWORD rate,
			 DWORD scale);

/*
 * Adds an audio stream whose format is fmt (a WAVEFORMATEX and its extra
 * bytes). rate, scale and sample_size go into its header; when rate or
 * scale is 0 the stream is constant-rate, counted in blocks
 * (nBlockAlign / nAvgBytesPerSec seconds each, sample size nBlockAlign),
 * as PCM always is. A variable-rate stream has a sample size of 0 and a
 * chunk is one frame of scale / rate seconds. Returns the stream's
 * number, or -1.
 */
int avi_writer_add_audio(struct avi_writer *w, const void *fmt,
			 DWORD fmt_size, DWORD rate, DWORD scale,
			 DWORD sample_size);

/*
 * Appends a chunk to a stream. Audio chunks are always keyframes.
 * Returns 0 or -1.
 */
int avi_writer_write(struct avi_writer *w, unsigned stream, const void *data,
		     DWORD size, int keyframe);

/*
 * avi_writer_create and avi_writer_add_video in one: a file with one video
 * stream. NULL with vfw_error() on failure.
 */
struct avi_writer *avi_writer_open(const char *path, const void *fmt,
				   DWORD fmt_size, FOURCC handler, DWORD rate,
				   DWORD scale,
				   const struct avi_writer_options *opts);

/* Appends one frame to stream 0. Returns 0 or -1. */
int avi_writer_add(struct avi_writer *w, const void *data, DWORD size,
		   int keyframe);

/* Finishes the file (indexes and headers). Returns 0 or -1; frees w. */
int avi_writer_close(struct avi_writer *w);

#endif /* VFW_AVI_AVI_H */
