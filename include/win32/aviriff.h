/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AVI file format definitions (aviriff.h): chunk ids, header flags, index
 * types, and the main and stream headers. The index chunks are parsed field
 * by field rather than through structs, since Windows packs them to 2 bytes
 * around 64-bit offsets.
 */
#ifndef VFW_WIN32_AVIRIFF_H
#define VFW_WIN32_AVIRIFF_H

#include "types.h"
#include "mmsystem.h"

/* AVIMAINHEADER.dwFlags. */
#define AVIF_HASINDEX       0x00000010
#define AVIF_MUSTUSEINDEX   0x00000020
#define AVIF_ISINTERLEAVED  0x00000100
#define AVIF_TRUSTCKTYPE    0x00000800
#define AVIF_WASCAPTUREFILE 0x00010000
#define AVIF_COPYRIGHTED    0x00020000

/* Index entry flags beyond vfw.h's AVIIF_LIST/TWOCC/KEYFRAME. */
#define AVIIF_NO_TIME    0x00000100
#define AVIIF_COMPRESSOR 0x0fff0000

/* AVISTREAMHEADER.dwFlags. */
#define AVISF_DISABLED         0x00000001
#define AVISF_VIDEO_PALCHANGES 0x00010000

/* OpenDML index types. */
#define AVI_INDEX_OF_INDEXES      0x0
#define AVI_INDEX_OF_CHUNKS       0x1
#define AVI_INDEX_OF_TIMED_CHUNKS 0x2
#define AVI_INDEX_OF_SUB_2FIELD   0x3
#define AVI_INDEX_IS_DATA         0x80
#define AVI_INDEX_SUB_DEFAULT     0x0
#define AVI_INDEX_SUB_2FIELD      0x1
#define AVISTDINDEX_DELTAFRAME    0x80000000

typedef struct _avimainheader {
	FOURCC fcc;
	DWORD cb;
	DWORD dwMicroSecPerFrame;
	DWORD dwMaxBytesPerSec;
	DWORD dwPaddingGranularity;
	DWORD dwFlags;
	DWORD dwTotalFrames;
	DWORD dwInitialFrames;
	DWORD dwStreams;
	DWORD dwSuggestedBufferSize;
	DWORD dwWidth;
	DWORD dwHeight;
	DWORD dwReserved[4];
} AVIMAINHEADER;

typedef struct _avistreamheader {
	FOURCC fcc;
	DWORD cb;
	FOURCC fccType;
	FOURCC fccHandler;
	DWORD dwFlags;
	WORD wPriority;
	WORD wLanguage;
	DWORD dwInitialFrames;
	DWORD dwScale;
	DWORD dwRate;
	DWORD dwStart;
	DWORD dwLength;
	DWORD dwSuggestedBufferSize;
	DWORD dwQuality;
	DWORD dwSampleSize;
	struct {
		SHORT left;
		SHORT top;
		SHORT right;
		SHORT bottom;
	} rcFrame;
} AVISTREAMHEADER;

#endif /* VFW_WIN32_AVIRIFF_H */
