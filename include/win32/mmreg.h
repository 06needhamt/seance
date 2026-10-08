/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio format definitions (mmreg.h, and the ksmedia.h subtypes it shares):
 * the format tags codecs name their streams by, the extended format
 * structures that follow a WAVEFORMATEX, and speaker positions.
 */
#ifndef VFW_WIN32_MMREG_H
#define VFW_WIN32_MMREG_H

#include "mmsystem.h"
#include "objbase.h"
#include "types.h"

/* WAVEFORMATEX.wFormatTag (WAVE_FORMAT_PCM is in mmsystem.h). */
#define WAVE_FORMAT_UNKNOWN              0x0000
#define WAVE_FORMAT_ADPCM                0x0002
#define WAVE_FORMAT_IEEE_FLOAT           0x0003
#define WAVE_FORMAT_ALAW                 0x0006
#define WAVE_FORMAT_MULAW                0x0007
#define WAVE_FORMAT_DTS                  0x0008
#define WAVE_FORMAT_DRM                  0x0009
#define WAVE_FORMAT_WMAVOICE9            0x000a
#define WAVE_FORMAT_IMA_ADPCM            0x0011
#define WAVE_FORMAT_GSM610               0x0031
#define WAVE_FORMAT_MSNAUDIO             0x0032
#define WAVE_FORMAT_MPEG                 0x0050
#define WAVE_FORMAT_MPEGLAYER3           0x0055
#define WAVE_FORMAT_DOLBY_AC3_SPDIF      0x0092
#define WAVE_FORMAT_RAW_AAC1             0x00ff
#define WAVE_FORMAT_WMAUDIO2             0x0161
#define WAVE_FORMAT_WMAUDIO3             0x0162
#define WAVE_FORMAT_WMAUDIO_LOSSLESS     0x0163
#define WAVE_FORMAT_WMASPDIF             0x0164
#define WAVE_FORMAT_MPEG_ADTS_AAC        0x1600
#define WAVE_FORMAT_MPEG_RAW_AAC         0x1601
#define WAVE_FORMAT_MPEG_LOAS            0x1602
#define WAVE_FORMAT_MPEG_HEAAC           0x1610
#define WAVE_FORMAT_OPUS                 0x704f
#define WAVE_FORMAT_FLAC                 0xf1ac
#define WAVE_FORMAT_EXTENSIBLE           0xfffe
#define WAVE_FORMAT_DEVELOPMENT          0xffff

/* WAVEFORMATEXTENSIBLE.dwChannelMask: one bit per speaker. */
#define SPEAKER_FRONT_LEFT            0x1
#define SPEAKER_FRONT_RIGHT           0x2
#define SPEAKER_FRONT_CENTER          0x4
#define SPEAKER_LOW_FREQUENCY         0x8
#define SPEAKER_BACK_LEFT             0x10
#define SPEAKER_BACK_RIGHT            0x20
#define SPEAKER_FRONT_LEFT_OF_CENTER  0x40
#define SPEAKER_FRONT_RIGHT_OF_CENTER 0x80
#define SPEAKER_BACK_CENTER           0x100
#define SPEAKER_SIDE_LEFT             0x200
#define SPEAKER_SIDE_RIGHT            0x400
#define SPEAKER_TOP_CENTER            0x800
#define SPEAKER_TOP_FRONT_LEFT        0x1000
#define SPEAKER_TOP_FRONT_CENTER      0x2000
#define SPEAKER_TOP_FRONT_RIGHT       0x4000
#define SPEAKER_TOP_BACK_LEFT         0x8000
#define SPEAKER_TOP_BACK_CENTER       0x10000
#define SPEAKER_TOP_BACK_RIGHT        0x20000
#define SPEAKER_RESERVED              0x7ffc0000
#define SPEAKER_ALL                   0x80000000

/* MPEG1WAVEFORMAT fields. */
#define ACM_MPEG_LAYER1        0x0001
#define ACM_MPEG_LAYER2        0x0002
#define ACM_MPEG_LAYER3        0x0004
#define ACM_MPEG_STEREO        0x0001
#define ACM_MPEG_JOINTSTEREO   0x0002
#define ACM_MPEG_DUALCHANNEL   0x0004
#define ACM_MPEG_SINGLECHANNEL 0x0008
#define ACM_MPEG_PRIVATEBIT    0x0001
#define ACM_MPEG_COPYRIGHT     0x0002
#define ACM_MPEG_ORIGINALHOME  0x0004
#define ACM_MPEG_PROTECTIONBIT 0x0008
#define ACM_MPEG_ID_MPEG1      0x0010

/* MPEGLAYER3WAVEFORMAT fields. */
#define MPEGLAYER3_WFX_EXTRA_BYTES       12
#define MPEGLAYER3_ID_UNKNOWN            0
#define MPEGLAYER3_ID_MPEG               1
#define MPEGLAYER3_ID_CONSTANTFRAMESIZE  2
#define MPEGLAYER3_FLAG_PADDING_ISO      0x00000000
#define MPEGLAYER3_FLAG_PADDING_ON       0x00000001
#define MPEGLAYER3_FLAG_PADDING_OFF      0x00000002

/* WAVEFILTER.dwFilterTag. */
#define WAVE_FILTER_UNKNOWN     0x0000
#define WAVE_FILTER_VOLUME      0x0001
#define WAVE_FILTER_ECHO        0x0002
#define WAVE_FILTER_DEVELOPMENT 0xffff

#pragma pack(push, 1)
/* An ACM filter (echo, volume): what a WAVEFILTER-specific block extends. */
typedef struct wavefilter_tag {
	DWORD cbStruct;
	DWORD dwFilterTag;
	DWORD fdwFilter;
	DWORD dwReserved[5];
} WAVEFILTER, *PWAVEFILTER, *NPWAVEFILTER, *LPWAVEFILTER;

/*
 * A WAVEFORMATEX with wFormatTag WAVE_FORMAT_EXTENSIBLE and cbSize 22: the
 * real format is SubFormat, and the speakers the channels feed are named.
 */
typedef struct {
	WAVEFORMATEX Format;
	union {
		WORD wValidBitsPerSample;
		WORD wSamplesPerBlock;
		WORD wReserved;
	} Samples;
	DWORD dwChannelMask;
	GUID SubFormat;
} WAVEFORMATEXTENSIBLE, *PWAVEFORMATEXTENSIBLE;

/* WAVE_FORMAT_MPEG: MPEG-1 audio layers 1 and 2. */
typedef struct mpeg1waveformat_tag {
	WAVEFORMATEX wfx;
	WORD fwHeadLayer;
	DWORD dwHeadBitrate;
	WORD fwHeadMode, fwHeadModeExt, wHeadEmphasis, fwHeadFlags;
	DWORD dwPTSLow, dwPTSHigh;
} MPEG1WAVEFORMAT, *PMPEG1WAVEFORMAT, *LPMPEG1WAVEFORMAT;

/* WAVE_FORMAT_MPEGLAYER3. */
typedef struct mpeglayer3waveformat_tag {
	WAVEFORMATEX wfx;
	WORD wID;
	DWORD fdwFlags;
	WORD nBlockSize, nFramesPerBlock, nCodecDelay;
} MPEGLAYER3WAVEFORMAT, *PMPEGLAYER3WAVEFORMAT, *LPMPEGLAYER3WAVEFORMAT;
#pragma pack(pop)

/*
 * The subtype for a format tag: {tag-0000-0010-8000-00AA00389B71}, the same
 * shape as a FOURCC subtype (dshow.h).
 */
#define WAVE_FORMAT_SUBTYPE_INIT(tag) \
	{ (WORD)(tag), 0x0000, 0x0010, \
	  { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }

/* WAVEFORMATEXTENSIBLE.SubFormat values, in VFW_GUID_DECLARE's form. */
#define VFW_MMREG_GUIDS(G) \
	G(KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, \
	  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71) \
	G(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, \
	  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)

VFW_MMREG_GUIDS(VFW_GUID_DECLARE)

#endif /* VFW_WIN32_MMREG_H */
