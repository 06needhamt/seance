/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RealPlayer's codec DLLs (drv2.dll, drvc.dll, cook.dll, the *3260.dll),
 * through their own entry points: no COM, no driver messages, plain C
 * functions the player calls.
 *
 *   video  RV20toYUV420Init / Transform / CustomMessage / Free (RV10 and
 *          RV20 streams) and the RV40toYUV420 set (RV30 and RV40), which
 *          take a frame and its slice table and give I420
 *   audio  RAOpenCodec2 (or RAOpenCodec), RAInitDecoder, RASetFlavor,
 *          RADecode, RAFreeDecoder, RACloseCodec: a block in, PCM out
 *
 * Not handled: the older PNCodec_ interface (rv10.dll, the RealVideo 1
 * and ClearVideo plug-ins) and RACreateDecoderInstance, which gives a
 * Helix object (raac.dll, amrn.dll, ...); real_kind() tells them apart so
 * callers can say so.
 *
 * The structures are as RealNetworks' Helix DNA sources and MPlayer's
 * notes on these DLLs describe them, re-implemented here.
 */
#ifndef VFW_REAL_REAL_H
#define VFW_REAL_REAL_H

#include <stddef.h>
#include <stdint.h>

#include "in/rm.h"
#include "win32.h"

enum real_kind {
	REAL_NONE,		/* not a RealPlayer codec */
	REAL_VIDEO,		/* RV20toYUV420 / RV40toYUV420 */
	REAL_AUDIO,		/* RAOpenCodec2 / RAInitDecoder / RADecode */
	REAL_PNCODEC,		/* PNCodec_ only */
	REAL_HELIX,		/* RACreateDecoderInstance only */
};

/*
 * What sort of RealPlayer codec dll is, by its exports, and (for video)
 * which entry points: "RV20toYUV420" or "RV40toYUV420". REAL_NONE with
 * vfw_error() if it can't be loaded.
 */
enum real_kind real_kind(const char *dll, const char **entry);
const char *real_kind_name(enum real_kind kind);

struct real_video;

/* Opens dll's decoder for stream st. NULL with vfw_error(). */
struct real_video *real_video_open(const char *dll, const struct rm_stream *st);
void real_video_close(struct real_video *v);

/*
 * Decodes a frame. 1 with an I420 picture in *pic (valid until the next
 * call) of *w x *h, 0 if the decoder gave none for it (yet), -1 with
 * vfw_error().
 */
int real_video_decode(struct real_video *v, const struct rm_frame *f,
		      const BYTE **pic, int *w, int *h);

struct real_audio;

/* Opens dll's decoder for stream st. NULL with vfw_error(). */
struct real_audio *real_audio_open(const char *dll, const struct rm_stream *st);
void real_audio_close(struct real_audio *a);

/* The PCM it gives: 16-bit, at the stream's rate and channels. */
const WAVEFORMATEX *real_audio_format(const struct real_audio *a);

/*
 * Decodes a block. 0 with *pcm (valid until the next call) of *len bytes,
 * maybe none, or -1 with vfw_error().
 */
int real_audio_decode(struct real_audio *a, const BYTE *in, size_t len,
		      const BYTE **pcm, size_t *pcm_len);

#endif /* VFW_REAL_REAL_H */
