/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Video Compression Manager (ICM) definitions: the message numbers, flags and
 * structures exchanged with a codec's DriverProc.
 */
#ifndef VFW_WIN32_VFW_H
#define VFW_WIN32_VFW_H

#include "types.h"
#include "wingdi.h"
#include "mmsystem.h"

DECLARE_HANDLE(HIC);

#define ICVERSION 0x0104

#define ICTYPE_VIDEO mmioFOURCC('v', 'i', 'd', 'c')
#define ICTYPE_AUDIO mmioFOURCC('a', 'u', 'd', 'c')

/* Return values. */
#define ICERR_OK            0
#define ICERR_DONTDRAW      1
#define ICERR_NEWPALETTE    2
#define ICERR_GOTOKEYFRAME  3
#define ICERR_STOPDRAWING   4
#define ICERR_UNSUPPORTED   (-1)
#define ICERR_BADFORMAT     (-2)
#define ICERR_MEMORY        (-3)
#define ICERR_INTERNAL      (-4)
#define ICERR_BADFLAGS      (-5)
#define ICERR_BADPARAM      (-6)
#define ICERR_BADSIZE       (-7)
#define ICERR_BADHANDLE     (-8)
#define ICERR_CANTUPDATE    (-9)
#define ICERR_ABORT         (-10)
#define ICERR_ERROR         (-100)
#define ICERR_BADBITDEPTH   (-200)
#define ICERR_BADIMAGESIZE  (-201)
#define ICERR_CUSTOM        (-400)

/* ICOPEN.dwFlags / ICOpen() modes. */
#define ICMODE_COMPRESS       1
#define ICMODE_DECOMPRESS     2
#define ICMODE_FASTDECOMPRESS 3
#define ICMODE_QUERY          4
#define ICMODE_FASTCOMPRESS   5
#define ICMODE_DRAW           8

/* Chunk flags returned through ICCOMPRESS.lpdwFlags. */
#define AVIIF_LIST     0x00000001
#define AVIIF_TWOCC    0x00000002
#define AVIIF_KEYFRAME 0x00000010

#define ICQUALITY_LOW     0
#define ICQUALITY_HIGH    10000
#define ICQUALITY_DEFAULT (-1)

/* Message ranges. */
#define ICM_USER          (DRV_USER + 0x0000)
#define ICM_RESERVED_LOW  (DRV_USER + 0x1000)
#define ICM_RESERVED_HIGH (DRV_USER + 0x2000)
#define ICM_RESERVED      ICM_RESERVED_LOW

/* Generic messages. */
#define ICM_GETSTATE          (ICM_RESERVED + 0)
#define ICM_SETSTATE          (ICM_RESERVED + 1)
#define ICM_GETINFO           (ICM_RESERVED + 2)
#define ICM_CONFIGURE         (ICM_RESERVED + 10)
#define ICM_ABOUT             (ICM_RESERVED + 11)
#define ICM_GETERRORTEXT      (ICM_RESERVED + 12)
#define ICM_GETFORMATNAME     (ICM_RESERVED + 20)
#define ICM_ENUMFORMATS       (ICM_RESERVED + 21)
#define ICM_GETDEFAULTQUALITY (ICM_RESERVED + 30)
#define ICM_GETQUALITY        (ICM_RESERVED + 31)
#define ICM_SETQUALITY        (ICM_RESERVED + 32)
#define ICM_SET               (ICM_RESERVED + 40)
#define ICM_GET               (ICM_RESERVED + 41)

/* Selectors for ICM_SET / ICM_GET. */
#define ICM_FRAMERATE    mmioFOURCC('F', 'r', 'm', 'R')
#define ICM_KEYFRAMERATE mmioFOURCC('K', 'e', 'y', 'R')

/* Compression. */
#define ICM_COMPRESS_GET_FORMAT (ICM_USER + 4)
#define ICM_COMPRESS_GET_SIZE   (ICM_USER + 5)
#define ICM_COMPRESS_QUERY      (ICM_USER + 6)
#define ICM_COMPRESS_BEGIN      (ICM_USER + 7)
#define ICM_COMPRESS            (ICM_USER + 8)
#define ICM_COMPRESS_END        (ICM_USER + 9)

/* Decompression. */
#define ICM_DECOMPRESS_GET_FORMAT  (ICM_USER + 10)
#define ICM_DECOMPRESS_QUERY       (ICM_USER + 11)
#define ICM_DECOMPRESS_BEGIN       (ICM_USER + 12)
#define ICM_DECOMPRESS             (ICM_USER + 13)
#define ICM_DECOMPRESS_END         (ICM_USER + 14)
#define ICM_DECOMPRESS_SET_PALETTE (ICM_USER + 29)
#define ICM_DECOMPRESS_GET_PALETTE (ICM_USER + 30)

/* Drawing. */
#define ICM_DRAW_QUERY         (ICM_USER + 31)
#define ICM_DRAW_BEGIN         (ICM_USER + 15)
#define ICM_DRAW_GET_PALETTE   (ICM_USER + 16)
#define ICM_DRAW_UPDATE        (ICM_USER + 17)
#define ICM_DRAW_START         (ICM_USER + 18)
#define ICM_DRAW_STOP          (ICM_USER + 19)
#define ICM_DRAW_BITS          (ICM_USER + 20)
#define ICM_DRAW_END           (ICM_USER + 21)
#define ICM_DRAW_GETTIME       (ICM_USER + 32)
#define ICM_DRAW               (ICM_USER + 33)
#define ICM_DRAW_WINDOW        (ICM_USER + 34)
#define ICM_DRAW_SETTIME       (ICM_USER + 35)
#define ICM_DRAW_REALIZE       (ICM_USER + 36)
#define ICM_DRAW_FLUSH         (ICM_USER + 37)
#define ICM_DRAW_RENDERBUFFER  (ICM_USER + 38)
#define ICM_DRAW_START_PLAY    (ICM_USER + 39)
#define ICM_DRAW_STOP_PLAY     (ICM_USER + 40)
#define ICM_DRAW_SUGGESTFORMAT (ICM_USER + 50)
#define ICM_DRAW_CHANGEPALETTE (ICM_USER + 51)
#define ICM_DRAW_IDLE          (ICM_USER + 52)

/* Miscellaneous. */
#define ICM_GETBUFFERSWANTED       (ICM_USER + 41)
#define ICM_GETDEFAULTKEYFRAMERATE (ICM_USER + 42)
#define ICM_DECOMPRESSEX_BEGIN     (ICM_USER + 60)
#define ICM_DECOMPRESSEX_QUERY     (ICM_USER + 61)
#define ICM_DECOMPRESSEX           (ICM_USER + 62)
#define ICM_DECOMPRESSEX_END       (ICM_USER + 63)
#define ICM_COMPRESS_FRAMES_INFO   (ICM_USER + 70)
#define ICM_COMPRESS_FRAMES        (ICM_USER + 71)
#define ICM_SET_STATUS_PROC        (ICM_USER + 72)

/* Passed as lParam1 of ICM_CONFIGURE / ICM_ABOUT to ask "do you have one?". */
#define ICMF_CONFIGURE_QUERY 0x00000001
#define ICMF_ABOUT_QUERY     0x00000001

/* The lParam2 of DRV_OPEN for a VfW codec. */
typedef struct {
	DWORD dwSize;
	DWORD fccType;
	DWORD fccHandler;
	DWORD dwVersion;
	DWORD dwFlags;
	LRESULT dwError;
	LPVOID pV1Reserved;
	LPVOID pV2Reserved;
	DWORD dnDevNode;
} ICOPEN;

/* ICINFO.dwFlags. */
#define VIDCF_QUALITY        0x0001
#define VIDCF_CRUNCH         0x0002
#define VIDCF_TEMPORAL       0x0004
#define VIDCF_COMPRESSFRAMES 0x0008
#define VIDCF_DRAW           0x0010
#define VIDCF_FASTTEMPORALC  0x0020
#define VIDCF_QUALITYTIME    0x0040
#define VIDCF_FASTTEMPORALD  0x0080
#define VIDCF_FASTTEMPORAL   (VIDCF_FASTTEMPORALC | VIDCF_FASTTEMPORALD)

typedef struct {
	DWORD dwSize;
	DWORD fccType;
	DWORD fccHandler;
	DWORD dwFlags;
	DWORD dwVersion;
	DWORD dwVersionICM;
	WCHAR szName[16];
	WCHAR szDescription[128];
	WCHAR szDriver[128];
} ICINFO;

/* ICCOMPRESS.dwFlags. */
#define ICCOMPRESS_KEYFRAME 0x00000001

typedef struct {
	DWORD dwFlags;
	LPBITMAPINFOHEADER lpbiOutput;
	LPVOID lpOutput;
	LPBITMAPINFOHEADER lpbiInput;
	LPVOID lpInput;
	LPDWORD lpckid;
	LPDWORD lpdwFlags;
	LONG lFrameNum;
	DWORD dwFrameSize;
	DWORD dwQuality;
	LPBITMAPINFOHEADER lpbiPrev;
	LPVOID lpPrev;
} ICCOMPRESS;

#define ICCOMPRESSFRAMES_PADDING 0x00000001

typedef struct {
	DWORD dwFlags;
	LPBITMAPINFOHEADER lpbiOutput;
	LPARAM lOutput;
	LPBITMAPINFOHEADER lpbiInput;
	LPARAM lInput;
	LONG lStartFrame;
	LONG lFrameCount;
	LONG lQuality;
	LONG lDataRate;
	LONG lKeyRate;
	DWORD dwRate;
	DWORD dwScale;
	DWORD dwOverheadPerFrame;
	DWORD dwReserved2;
	LONG (CALLBACK *GetData)(LPARAM lInput, LONG lFrame, LPVOID lpBits, LONG len);
	LONG (CALLBACK *PutData)(LPARAM lOutput, LONG lFrame, LPVOID lpBits, LONG len);
} ICCOMPRESSFRAMES;

/* Messages sent to an ICSETSTATUSPROC callback. */
#define ICSTATUS_START  0
#define ICSTATUS_STATUS 1
#define ICSTATUS_END    2
#define ICSTATUS_ERROR  3
#define ICSTATUS_YIELD  4

typedef struct {
	DWORD dwFlags;
	LPARAM lParam;
	LONG (CALLBACK *Status)(LPARAM lParam, UINT message, LONG l);
} ICSETSTATUSPROC;

/* ICDECOMPRESS.dwFlags. */
#define ICDECOMPRESS_HURRYUP      0x80000000
#define ICDECOMPRESS_UPDATE       0x40000000
#define ICDECOMPRESS_PREROLL      0x20000000
#define ICDECOMPRESS_NULLFRAME    0x10000000
#define ICDECOMPRESS_NOTKEYFRAME  0x08000000

typedef struct {
	DWORD dwFlags;
	LPBITMAPINFOHEADER lpbiInput;
	LPVOID lpInput;
	LPBITMAPINFOHEADER lpbiOutput;
	LPVOID lpOutput;
	DWORD ckid;
} ICDECOMPRESS;

typedef struct {
	DWORD dwFlags;
	LPBITMAPINFOHEADER lpbiSrc;
	LPVOID lpSrc;
	LPBITMAPINFOHEADER lpbiDst;
	LPVOID lpDst;
	int xDst;
	int yDst;
	int dxDst;
	int dyDst;
	int xSrc;
	int ySrc;
	int dxSrc;
	int dySrc;
} ICDECOMPRESSEX;

/* ICDRAWBEGIN.dwFlags. */
#define ICDRAW_QUERY      0x00000001
#define ICDRAW_FULLSCREEN 0x00000002
#define ICDRAW_HDC        0x00000004
#define ICDRAW_ANIMATE    0x00000008
#define ICDRAW_CONTINUE   0x00000010
#define ICDRAW_MEMORYDC   0x00000020
#define ICDRAW_UPDATING   0x00000040
#define ICDRAW_RENDER     0x00000080
#define ICDRAW_BUFFER     0x00000100

typedef struct {
	DWORD dwFlags;
	HPALETTE hpal;
	HWND hwnd;
	HDC hdc;
	int xDst;
	int yDst;
	int dxDst;
	int dyDst;
	LPBITMAPINFOHEADER lpbi;
	int xSrc;
	int ySrc;
	int dxSrc;
	int dySrc;
	DWORD dwRate;
	DWORD dwScale;
} ICDRAWBEGIN;

/* ICDRAW.dwFlags. */
#define ICDRAW_HURRYUP     0x80000000
#define ICDRAW_UPDATE      0x40000000
#define ICDRAW_PREROLL     0x20000000
#define ICDRAW_NULLFRAME   0x10000000
#define ICDRAW_NOTKEYFRAME 0x08000000

typedef struct {
	DWORD dwFlags;
	LPVOID lpFormat;
	LPVOID lpData;
	DWORD cbData;
	LONG lTime;
} ICDRAW;

typedef struct {
	LPBITMAPINFOHEADER lpbiIn;
	LPBITMAPINFOHEADER lpbiSuggest;
	int dxSrc;
	int dySrc;
	int dxDst;
	int dyDst;
	HIC hicDecompressor;
} ICDRAWSUGGEST;

typedef struct {
	DWORD dwFlags;
	int iStart;
	int iLen;
	LPPALETTEENTRY lppe;
} ICPALETTE;

/* ICCompressorChoose / ICSeqCompressFrame* state. */
#define ICMF_COMPVARS_VALID 0x00000001

/* ICCompressorChoose flags, and its "Full Frames (Uncompressed)" entry. */
#define ICMF_CHOOSE_KEYFRAME       0x0001
#define ICMF_CHOOSE_DATARATE       0x0002
#define ICMF_CHOOSE_PREVIEW        0x0004
#define ICMF_CHOOSE_ALLCOMPRESSORS 0x0008
#define comptypeDIB mmioFOURCC('D', 'I', 'B', ' ')

typedef struct {
	LONG cbSize;
	DWORD dwFlags;
	HIC hic;
	DWORD fccType;
	DWORD fccHandler;
	LPBITMAPINFO lpbiIn;
	LPBITMAPINFO lpbiOut;
	LPVOID lpBitsOut;
	LPVOID lpBitsPrev;
	LONG lFrame;
	LONG lKey;
	LONG lDataRate;
	LONG lQ;
	LONG lKeyCount;
	LPVOID lpState;
	LONG cbState;
} COMPVARS, *PCOMPVARS;

/* ICInstall() flags. */
#define ICINSTALL_UNICODE  0x8000
#define ICINSTALL_FUNCTION 0x0001
#define ICINSTALL_DRIVER   0x0002
#define ICINSTALL_HDRV     0x0004
#define ICINSTALL_DRIVERW  0x8002

/* DrawDib. */
typedef HANDLE HDRAWDIB;

#define DDF_0001          0x0001
#define DDF_UPDATE        0x0002
#define DDF_SAME_HDC      0x0004
#define DDF_SAME_DRAW     0x0008
#define DDF_DONTDRAW      0x0010
#define DDF_ANIMATE       0x0020
#define DDF_BUFFER        0x0040
#define DDF_JUSTDRAWIT    0x0080
#define DDF_FULLSCREEN    0x0100
#define DDF_BACKGROUNDPAL 0x0200
#define DDF_NOTKEYFRAME   0x0400
#define DDF_HURRYUP       0x0800
#define DDF_HALFTONE      0x1000
#define DDF_2000          0x2000
#define DDF_PREROLL       DDF_DONTDRAW
#define DDF_SAME_DIB      DDF_SAME_DRAW
#define DDF_SAME_SIZE     DDF_SAME_DRAW

typedef struct {
	LONG timeCount;
	LONG timeDraw;
	LONG timeDecompress;
	LONG timeDither;
	LONG timeStretch;
	LONG timeBlt;
	LONG timeSetDIBits;
} DRAWDIBTIME, *LPDRAWDIBTIME;

/* DrawDibProfileDisplay. */
#define PD_CAN_DRAW_DIB      0x0001
#define PD_CAN_STRETCHDIB    0x0002
#define PD_STRETCHDIB_1_1_OK 0x0004
#define PD_STRETCHDIB_1_2_OK 0x0008
#define PD_STRETCHDIB_1_N_OK 0x0010

#endif /* VFW_WIN32_VFW_H */
