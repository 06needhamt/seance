/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RealMedia files (.rm, .ra, .rmvb): the header chunks, and the packets
 * of one stream put back together as its codec takes them.
 *
 * Video packets carry parts of frames (slices), whole frames, or several
 * frames each; they come out as whole frames with a table of where each
 * slice starts, as RealVideo's decoders want. Audio packets come out in
 * blocks of the stream's frame size, after undoing the interleaving
 * ("genr", "Int4", "sipr") that spreads a group of packets' worth over
 * many packets against loss. "Int0" streams (and ones with no
 * interleaver) come a packet at a time.
 *
 * The layout is RealNetworks' own, as documented in the Helix DNA
 * sources, re-implemented here.
 */
#ifndef VFW_IN_RM_H
#define VFW_IN_RM_H

#include <stddef.h>
#include <stdint.h>

#include "win32.h"

#define RM_MAX_STREAMS 16
#define RM_MAX_SLICES 128

struct rm_stream {
	unsigned number;	/* as the packets name it */
	int video, audio;	/* one of these, or neither (logical streams) */
	char mime[64];
	FOURCC fourcc;		/* 'RV20', 'cook', 'dnet', ... as stored */
	uint32_t duration;	/* ms */
	uint32_t avg_bit_rate;
	BYTE *type_data;	/* the type-specific data, as stored */
	size_t type_len;

	/* video: after the VIDO header */
	unsigned width, height;
	uint32_t fps;		/* 16.16 */
	const BYTE *extra;	/* in type_data: subformat, version, then more */
	size_t extra_len;

	/* audio: from the .ra header */
	unsigned version;	/* 3, 4 or 5 */
	unsigned flavor;
	uint32_t coded_frame_size;
	unsigned sub_packet_h, frame_size, sub_packet_size;
	unsigned rate, bits, channels;
	FOURCC interleaver;	/* 'Int0', 'Int4', 'genr', 'sipr', ... or 0 */
	const BYTE *codec_data;	/* in type_data */
	size_t codec_data_len;
};

struct rm_file;

/* Opens a file and reads its headers. NULL with vfw_error(). */
struct rm_file *rm_open(const char *path);
void rm_close(struct rm_file *rm);

/*
 * Whether a file starts like a RealMedia one: ".RMF". A bare RealAudio
 * file (".ra\xfd", with no RealMedia chunks around it) isn't taken; the
 * same audio in a .RMF file is.
 */
int rm_sniff(const char *path);

unsigned rm_nstreams(const struct rm_file *rm);
const struct rm_stream *rm_stream(const struct rm_file *rm, unsigned i);
/* The first video (audio) stream's index, or -1. */
int rm_video_stream(const struct rm_file *rm);
int rm_audio_stream(const struct rm_file *rm);

/* A video frame: its data, and where in it each slice starts. */
struct rm_frame {
	const BYTE *data;
	size_t len;
	unsigned nslices;
	uint32_t offsets[RM_MAX_SLICES];
	uint32_t time;		/* ms */
	int key;
};

/*
 * The next frame of video stream i, valid until the next call. 1, 0 at
 * the end, -1 with vfw_error(). Packets of other streams are skipped.
 */
int rm_read_frame(struct rm_file *rm, unsigned i, struct rm_frame *f);

/*
 * The next block of audio stream i, valid until the next call; *time in
 * ms, or the time of the group it came in. 1, 0 at the end, -1 with
 * vfw_error().
 */
int rm_read_audio(struct rm_file *rm, unsigned i, const BYTE **data,
		  size_t *len, uint32_t *time);

/*
 * The sipr codec's nibble reordering, done in place over a group of
 * sub_packet_h packets of frame_size bytes each. For tests.
 */
void rm_sipr_reorder(BYTE *buf, unsigned sub_packet_h, unsigned frame_size);

#endif /* VFW_IN_RM_H */
