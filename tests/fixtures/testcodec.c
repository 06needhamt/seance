/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * testcodec.dll: a small lossless VfW codec, FOURCC 'TST0', for testing
 * the harness's driver and ICM layers the way a real codec would use them.
 *
 * Input is BI_RGB 24 or 32 bits, either orientation, or I420 (IYUV). A
 * compressed frame is a 4-byte header ('K' keyframe or 'D' delta, bits per
 * pixel, 0, 0) and a byte-wise RLE of the frame: of the frame itself for a
 * keyframe, or of its XOR with the previous frame for a delta. RGB
 * decompresses to BI_RGB 24 or 32 bits in either orientation, I420 to
 * I420, so both round trips are lossless.
 *
 * It also draws TST0 RGB frames itself (ICM_DRAW_BEGIN / ICM_DRAW /
 * ICM_DRAW_END, onto the DC it is given, through msvfw32's DrawDib), as
 * codecs that render their own output do.
 *
 * Messages it does not handle go to winmm's DefDriverProc, and
 * testcodec_selftest() calls winmm and msvfw32 from the Win32 side.
 *
 * With TESTCODEC_STRICT=1 in its (Windows) environment, it refuses to work
 * with any pointer at or above 2 GB, the way code that isn't large-address
 * aware can go wrong with one: its instance, the ICOPEN, the (de)compress
 * structures, the formats, the frames and its own stack.
 *
 * With TESTCODEC_NOCOMPRESS=1 it only decompresses, answering every
 * ICM_COMPRESS_QUERY with ICERR_UNSUPPORTED, as Sunplus's webcam codecs do.
 * Four more, each as a real codec does and each set to 1 by itself:
 *   TESTCODEC_STATUS      it calls the status callback from
 *                         ICM_SET_STATUS_PROC on every frame without
 *                         checking one was given (ADV601.dll)
 *   TESTCODEC_PADDING     it reads the 64 bytes after its compressed input
 *                         and wants them zero (sp6x_32.dll, vgpix32d.dll)
 *   TESTCODEC_ASK_FORMAT  it decodes to nothing unless asked
 *                         ICM_DECOMPRESS_GET_FORMAT before
 *                         ICM_DECOMPRESS_BEGIN (wv32vfw.dll)
 *   TESTCODEC_PICKY       it refuses ICM_COMPRESS_QUERY with an output
 *                         format, its own included (vsslight.dll)
 *   TESTCODEC_NAMED_ONLY  it answers ICM_GETINFO only when opened as 'TST0'
 *                         (wnvwinx.dll, as 'WINX'); set to 2, only when
 *                         opened as 'TSTP' or 'tstp', a pair it keeps in
 *                         its data (VFCodec.dll, as 'VIFP' or 'vifp'); set
 *                         to 3, it answers otherwise too, but with no
 *                         FOURCC nor name (vp7vfw.dll)
 *   TESTCODEC_DECODE_ONLY it refuses DRV_OPEN to compress, though it answers
 *                         ICM_COMPRESS_QUERY when opened otherwise
 *                         (vgpix32d.dll)
 *   TESTCODEC_OVERRUN     it writes on past the end of its largest
 *                         frame when compressing (SN4Codec.dll)
 *   TESTCODEC_END_CRASH   it divides by zero in ICM_COMPRESS_END and
 *                         ICM_DECOMPRESS_END (WavCWAIP.dll, by a byte count
 *                         it never kept)
 *   TESTCODEC_EMPTY_FORMAT it answers ICM_COMPRESS_GET_FORMAT without
 *                         writing a format (kdvyuv8.dll, wnvplay1.dll)
 *   TESTCODEC_NO_KEYS     it never flags a frame AVIIF_KEYFRAME
 *                         (WavCWAIP.dll, icmw_32.dll)
 *   TESTCODEC_ALIGN16     it takes only inputs whose height is a multiple
 *                         of 16 (SN4Codec.dll, vsslight.dll, ...)
 *   TESTCODEC_CHATTY      it writes a line to its standard output when
 *                         loaded (blox.dll)
 *   TESTCODEC_DETACH_CRASH its DllMain faults in DLL_PROCESS_DETACH
 *                         (packed.dll)
 * With TESTCODEC_LATESIZE=1 it only knows how big a frame can get once
 * ICM_COMPRESS_BEGIN has been sent, and says 16 bytes before (in
 * ICM_COMPRESS_GET_SIZE and the output format), as wtvc.dll does.
 *
 * Two private messages misbehave on purpose, for the harness's crash and
 * hang reports: TESTCODEC_CRASH writes through a null pointer and
 * TESTCODEC_HANG never returns.
 * Built like old MSVC code, with a 4-byte stack boundary.
 */
#include "fixture.h"
#include <vfw.h>

#define TST0 mmioFOURCC('T', 'S', 'T', '0')
#define VIDC mmioFOURCC('v', 'i', 'd', 'c')
#define I420 mmioFOURCC('I', '4', '2', '0')
#define IYUV mmioFOURCC('I', 'Y', 'U', 'V')
#define MAX_LOG 64
#define TESTCODEC_CRASH (ICM_USER + 100)
#define TESTCODEC_HANG  (ICM_USER + 101)

struct state {
	DWORD quality;
	DWORD keyrate;
};

struct instance {
	DWORD magic;
	struct state st;
	/* Compression: the previous input frame. */
	BYTE *cprev;
	DWORD cframe;
	/* Decompression: the current picture, in the compressed layout. */
	BYTE *dcur;
	int dvalid;
	/* Opened as 'TST0' (TESTCODEC_NAMED_ONLY answers only then). */
	int named;
	/* TESTCODEC_ASK_FORMAT: asked for its format since the last BEGIN; ready. */
	int asked, ready;
	/* ICM_SET_STATUS_PROC's callback. */
	LPARAM status_param;
	LONG (CALLBACK *status)(LPARAM, UINT, LONG);
	/* Drawing: DrawDib, where to, and a BGR24 frame to draw from. */
	HDRAWDIB hdd;
	HDC hdc;
	int x, y, dx, dy;
	BITMAPINFOHEADER dout;
	BYTE *dbuf;
};

#define MAGIC 0x54535430

static HINSTANCE self;
static int strict;	/* TESTCODEC_STRICT */
static int nocompress;	/* TESTCODEC_NOCOMPRESS */
static int latesize;	/* TESTCODEC_LATESIZE */
static int needs_status, needs_padding, ask_format, picky, decode_only;
static int named_only;	/* 1, 3: as 'TST0'; 2: as its pair */
/* NAMED_ONLY=2's handlers, read from data rather than compared as
 * immediates, and standing alone in it. */
static volatile const DWORD pair_name[4] __attribute__((aligned(16))) = {
	mmioFOURCC('T', 'S', 'T', 'P'), mmioFOURCC('t', 's', 't', 'p'),
};

static int pair_named(DWORD h)
{
	return h && (h == pair_name[0] || h == pair_name[1]);
}
static int end_crash;
static int overrun;
static int empty_format, no_keys, align16;
static volatile int zero;
#define EARLY_SIZE 16	/* what it says before ICM_COMPRESS_BEGIN */
static DWORD last_quality = 0xffffffff;	/* of the last ICM_COMPRESS */
static DWORD msg_log[MAX_LOG];
static int nlog;
static LONG opens;

static void log_msg(DWORD msg)
{
	if (nlog < MAX_LOG)
		msg_log[nlog++] = msg;
}

/*
 * Copies the DRV_* message log into out (up to n entries) and returns the
 * number of entries logged; with reset, clears it.
 */
EXPORT int testcodec_stats(DWORD *out, int n, int reset)
{
	int i, count = nlog;

	for (i = 0; i < n && i < nlog; i++)
		out[i] = msg_log[i];
	if (reset)
		nlog = 0;
	return count;
}

EXPORT LONG testcodec_open_count(void)
{
	return opens;
}

/*
 * How many DRV_LOADs and DRV_OPENs found GetDriverModuleHandle of their
 * driver handle wrong: ACM drivers load their names from it then.
 */
static LONG module_misses;

static void check_module(HDRVR hdrvr)
{
	if (hdrvr && GetDriverModuleHandle(hdrvr) != self)
		InterlockedIncrement(&module_misses);
}

EXPORT LONG testcodec_module_misses(void)
{
	return module_misses;
}

/* The dwQuality of the last frame compressed. */
EXPORT DWORD testcodec_last_quality(void)
{
	return last_quality;
}

static void *alloc(DWORD size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *p)
{
	if (p)
		HeapFree(GetProcessHeap(), 0, p);
}

/* In strict mode, whether any of the n pointers is at or above 2 GB. */
static int high(int n, ...)
{
	__builtin_va_list ap;
	int i, bad = 0;
	char here;

	if (!strict)
		return 0;
	bad = (DWORD)&here >= 0x80000000u;
	__builtin_va_start(ap, n);
	for (i = 0; i < n; i++)
		bad |= (DWORD)__builtin_va_arg(ap, const void *) >= 0x80000000u;
	__builtin_va_end(ap);
	return bad;
}

static LONG abs_height(const BITMAPINFOHEADER *b)
{
	return b->biHeight < 0 ? -b->biHeight : b->biHeight;
}

static DWORD stride(LONG w, int bpp)
{
	return ((DWORD)w * bpp + 31) / 32 * 4;
}

static DWORD frame_bytes(const BITMAPINFOHEADER *b)
{
	LONG w = b->biWidth, h = abs_height(b);

	if (b->biBitCount == 12)
		return (DWORD)(w * h + 2 * ((w + 1) / 2) * ((h + 1) / 2));
	return stride(w, b->biBitCount) * h;
}

/* The worst case of the RLE, plus the header. */
static DWORD max_size(const BITMAPINFOHEADER *b)
{
	DWORD n = frame_bytes(b);
	return 4 + n + n / 128 + 2;
}

static int ok_rgb(const BITMAPINFOHEADER *b)
{
	return b && b->biSize >= sizeof(*b) && b->biCompression == BI_RGB &&
	       (b->biBitCount == 24 || b->biBitCount == 32) &&
	       b->biWidth > 0 && b->biWidth <= 16384 && b->biHeight != 0 &&
	       abs_height(b) <= 16384;
}

static int ok_yuv(const BITMAPINFOHEADER *b)
{
	return b && b->biSize >= sizeof(*b) &&
	       (b->biCompression == I420 || b->biCompression == IYUV) &&
	       b->biBitCount == 12 &&
	       b->biWidth > 0 && b->biWidth <= 16384 && b->biHeight != 0 &&
	       abs_height(b) <= 16384;
}

/* An input the compressor takes. */
static int ok_input(const BITMAPINFOHEADER *b)
{
	if (align16 && b && abs_height(b) % 16)
		return 0;
	return ok_rgb(b) || ok_yuv(b);
}

static int ok_tst0(const BITMAPINFOHEADER *b)
{
	return b && b->biSize >= sizeof(*b) && b->biCompression == TST0 &&
	       (b->biBitCount == 12 || b->biBitCount == 24 ||
		b->biBitCount == 32) &&
	       b->biWidth > 0 && b->biWidth <= 16384 && b->biHeight != 0 &&
	       abs_height(b) <= 16384;
}

static int same_size(const BITMAPINFOHEADER *a, const BITMAPINFOHEADER *b)
{
	return a->biWidth == b->biWidth && abs_height(a) == abs_height(b);
}

/*
 * RLE: c < 128 is c + 1 literal bytes; c >= 128 repeats the next byte
 * c - 126 times. Only runs of 3 or more are repeats, so the output is at
 * most n + n / 128 + 1 bytes.
 */
static int run_at(const BYTE *src, DWORD i, DWORD n)
{
	return i + 2 < n && src[i + 1] == src[i] && src[i + 2] == src[i];
}

static DWORD rle_encode(const BYTE *src, DWORD n, BYTE *dst)
{
	DWORD i = 0, o = 0;

	while (i < n) {
		if (run_at(src, i, n)) {
			DWORD run = 3;
			while (i + run < n && src[i + run] == src[i] && run < 129)
				run++;
			dst[o++] = (BYTE)(run + 126);
			dst[o++] = src[i];
			i += run;
			continue;
		}
		DWORD lit = 0, start = i;
		while (i < n && lit < 128 && (lit == 0 || !run_at(src, i, n))) {
			i++;
			lit++;
		}
		dst[o++] = (BYTE)(lit - 1);
		for (DWORD k = 0; k < lit; k++)
			dst[o++] = src[start + k];
	}
	return o;
}

/* Decodes exactly n bytes; XORs them into dst for a delta. 0 or -1. */
static int rle_decode(const BYTE *src, DWORD len, BYTE *dst, DWORD n, int xor)
{
	DWORD i = 0, o = 0;

	while (o < n) {
		if (i >= len)
			return -1;
		BYTE c = src[i++];
		if (c < 128) {
			DWORD lit = c + 1u;
			if (i + lit > len || o + lit > n)
				return -1;
			for (DWORD k = 0; k < lit; k++, o++)
				dst[o] = xor ? dst[o] ^ src[i + k] : src[i + k];
			i += lit;
		} else {
			DWORD run = c - 126u;
			if (i >= len || o + run > n)
				return -1;
			for (DWORD k = 0; k < run; k++, o++)
				dst[o] = xor ? dst[o] ^ src[i] : src[i];
			i++;
		}
	}
	return 0;
}

static LRESULT get_info(ICINFO *info, DWORD size)
{
	static const char name[] = "TST0";
	static const char desc[] = "seance test codec";
	int i;

	if (!info || size < sizeof(*info))
		return 0;
	info->dwSize = sizeof(*info);
	info->fccType = ICTYPE_VIDEO;
	info->fccHandler = TST0;
	info->dwFlags = VIDCF_TEMPORAL | VIDCF_FASTTEMPORALC | VIDCF_DRAW;
	info->dwVersion = 0x00010002;
	info->dwVersionICM = ICVERSION;
	for (i = 0; name[i]; i++)
		info->szName[i] = name[i];
	info->szName[i] = 0;
	for (i = 0; desc[i]; i++)
		info->szDescription[i] = desc[i];
	info->szDescription[i] = 0;
	return sizeof(*info);
}

static LRESULT compress_query(const BITMAPINFOHEADER *in,
			      const BITMAPINFOHEADER *out)
{
	if (!ok_input(in))
		return ICERR_BADFORMAT;
	if (out && (!ok_tst0(out) || !same_size(in, out) ||
		    out->biBitCount != in->biBitCount))
		return ICERR_BADFORMAT;
	return ICERR_OK;
}

static LRESULT compress_get_format(const BITMAPINFOHEADER *in,
				   BITMAPINFOHEADER *out)
{
	if (!ok_input(in))
		return ICERR_BADFORMAT;
	if (!out)
		return sizeof(BITMAPINFOHEADER);
	if (empty_format)
		return ICERR_OK;
	*out = *in;
	out->biSize = sizeof(*out);
	out->biCompression = TST0;
	out->biSizeImage = latesize ? EARLY_SIZE : max_size(in);
	out->biClrUsed = out->biClrImportant = 0;
	return ICERR_OK;
}

static LRESULT compress(struct instance *inst, ICCOMPRESS *c)
{
	const BITMAPINFOHEADER *in = c->lpbiInput;
	DWORD n = frame_bytes(in), i;
	BYTE *dst = c->lpOutput;
	const BYTE *src = c->lpInput;
	int key;

	if (high(8, inst, inst->cprev, c, c->lpbiInput, c->lpInput, c->lpbiOutput,
		 c->lpOutput, c->lpdwFlags))
		return ICERR_BADPARAM;
	if (compress_query(in, c->lpbiOutput) != ICERR_OK || !inst->cprev)
		return ICERR_BADFORMAT;
	last_quality = c->dwQuality;
	key = (c->dwFlags & ICCOMPRESS_KEYFRAME) || inst->cframe == 0 ||
	      (inst->st.keyrate && inst->cframe % inst->st.keyrate == 0);
	dst[0] = key ? 'K' : 'D';
	dst[1] = (BYTE)in->biBitCount;
	dst[2] = dst[3] = 0;
	if (key) {
		c->lpbiOutput->biSizeImage = 4 + rle_encode(src, n, dst + 4);
	} else {
		/* XOR into cprev, encode, then make cprev the new frame. */
		for (i = 0; i < n; i++)
			inst->cprev[i] ^= src[i];
		c->lpbiOutput->biSizeImage = 4 + rle_encode(inst->cprev, n, dst + 4);
	}
	for (i = 0; i < n; i++)
		inst->cprev[i] = src[i];
	if (c->lpdwFlags)
		*c->lpdwFlags = key && !no_keys ? AVIIF_KEYFRAME : 0;
	if (c->lpckid)
		*c->lpckid = mmioFOURCC('d', 'c', 0, 0);
	inst->cframe++;
	return ICERR_OK;
}

static LRESULT decompress_query(const BITMAPINFOHEADER *in,
				const BITMAPINFOHEADER *out)
{
	if (!ok_tst0(in))
		return ICERR_BADFORMAT;
	if (out && (in->biBitCount == 12 ? !ok_yuv(out) : !ok_rgb(out)))
		return ICERR_BADFORMAT;
	if (out && !same_size(in, out))
		return ICERR_BADFORMAT;
	return ICERR_OK;
}

static LRESULT decompress_get_format(const BITMAPINFOHEADER *in,
				     BITMAPINFOHEADER *out)
{
	if (!ok_tst0(in))
		return ICERR_BADFORMAT;
	if (!out)
		return sizeof(BITMAPINFOHEADER);
	*out = *in;
	out->biSize = sizeof(*out);
	out->biCompression = in->biBitCount == 12 ? I420 : BI_RGB;
	out->biBitCount = in->biBitCount == 12 ? 12 : 24;
	out->biHeight = abs_height(in);
	out->biSizeImage = frame_bytes(out);
	out->biClrUsed = out->biClrImportant = 0;
	return ICERR_OK;
}

static LRESULT decompress(struct instance *inst, ICDECOMPRESS *d)
{
	const BITMAPINFOHEADER *in = d->lpbiInput, *out = d->lpbiOutput;
	const BYTE *src = d->lpInput;
	BYTE *dst = d->lpOutput;
	LONG w = in->biWidth, h = abs_height(in), y, x;
	int ib = in->biBitCount / 8, ob;

	if (high(7, inst, inst->dcur, d, in, src, out, dst))
		return ICERR_BADPARAM;
	if (decompress_query(in, out) != ICERR_OK || !inst->dcur)
		return ICERR_BADFORMAT;
	/* As wv32vfw.dll: set up only by ICM_DECOMPRESS_GET_FORMAT, else nothing. */
	if (ask_format && !inst->ready)
		return ICERR_OK;
	/* As decoders that read past their input: there must be zeros. */
	if (needs_padding && !(d->dwFlags & ICDECOMPRESS_NULLFRAME))
		for (DWORD i = 0; i < 64; i++)
			if (src[in->biSizeImage + i])
				return ICERR_BADFORMAT;
	if (!(d->dwFlags & ICDECOMPRESS_NULLFRAME)) {
		if (in->biSizeImage < 4 || src[1] != in->biBitCount ||
		    (src[0] != 'K' && src[0] != 'D'))
			return ICERR_BADFORMAT;
		if (src[0] == 'D' && !inst->dvalid)
			return ICERR_GOTOKEYFRAME;
		if (rle_decode(src + 4, in->biSizeImage - 4, inst->dcur,
			       frame_bytes(in), src[0] == 'D'))
			return ICERR_BADFORMAT;
		inst->dvalid = 1;
	}

	if (in->biBitCount == 12) {
		DWORD n = frame_bytes(in), i;
		for (i = 0; i < n; i++)
			dst[i] = inst->dcur[i];
		return ICERR_OK;
	}

	/* Convert to the output's depth and orientation. */
	ob = out->biBitCount / 8;
	for (y = 0; y < h; y++) {
		/* Row y from the top in each layout. */
		LONG sy = in->biHeight > 0 ? h - 1 - y : y;
		LONG dy = out->biHeight > 0 ? h - 1 - y : y;
		const BYTE *s = inst->dcur + sy * stride(w, in->biBitCount);
		BYTE *o = dst + dy * stride(w, out->biBitCount);
		for (x = 0; x < w; x++) {
			o[x * ob] = s[x * ib];
			o[x * ob + 1] = s[x * ib + 1];
			o[x * ob + 2] = s[x * ib + 2];
			if (ob == 4)
				o[x * ob + 3] = ib == 4 ? s[x * ib + 3] : 0;
		}
	}
	return ICERR_OK;
}

static void draw_end(struct instance *inst)
{
	if (inst->hdd)
		DrawDibClose(inst->hdd);
	inst->hdd = NULL;
	release(inst->dbuf);
	inst->dbuf = NULL;
}

/* An RGB TST0 stream, decompressed to BGR24 and drawn with DrawDib. */
static LRESULT draw_begin(struct instance *inst, const ICDRAWBEGIN *b)
{
	const BITMAPINFOHEADER *in = b ? b->lpbi : NULL;

	if (!in || decompress_query(in, NULL) != ICERR_OK ||
	    in->biBitCount == 12)
		return ICERR_BADFORMAT;
	if (b->dwFlags & ICDRAW_QUERY)
		return ICERR_OK;
	if (!(b->dwFlags & ICDRAW_HDC) || !b->hdc)
		return ICERR_BADPARAM;
	draw_end(inst);
	decompress_get_format(in, &inst->dout);
	inst->dout.biBitCount = 24;
	inst->dout.biSizeImage = frame_bytes(&inst->dout);
	release(inst->dcur);
	inst->dcur = alloc(frame_bytes(in));
	inst->dbuf = alloc(inst->dout.biSizeImage);
	inst->dvalid = 0;
	inst->hdd = DrawDibOpen();
	inst->hdc = b->hdc;
	inst->x = b->xDst;
	inst->y = b->yDst;
	inst->dx = b->dxDst;
	inst->dy = b->dyDst;
	if (!inst->dcur || !inst->dbuf || !inst->hdd ||
	    !DrawDibBegin(inst->hdd, inst->hdc, inst->dx, inst->dy,
			  &inst->dout, in->biWidth, abs_height(in), 0)) {
		draw_end(inst);
		return ICERR_MEMORY;
	}
	return ICERR_OK;
}

static LRESULT draw(struct instance *inst, const ICDRAW *d)
{
	if (!d || !inst->hdd)
		return ICERR_BADPARAM;
	if (!(d->dwFlags & ICDRAW_UPDATE) || d->lpData) {
		BITMAPINFOHEADER in = *(const BITMAPINFOHEADER *)d->lpFormat;
		ICDECOMPRESS dc;
		LRESULT r;
		in.biSizeImage = d->cbData;
		dc.dwFlags = d->cbData ? 0 : ICDECOMPRESS_NULLFRAME;
		dc.lpbiInput = &in;
		dc.lpInput = d->lpData;
		dc.lpbiOutput = &inst->dout;
		dc.lpOutput = inst->dbuf;
		dc.ckid = 0;
		if ((r = decompress(inst, &dc)) != ICERR_OK)
			return r;
		if (d->dwFlags & (ICDRAW_PREROLL | ICDRAW_HURRYUP))
			return ICERR_OK;
	}
	if (!DrawDibDraw(inst->hdd, inst->hdc, inst->x, inst->y, inst->dx,
			 inst->dy, &inst->dout, inst->dbuf, 0, 0,
			 inst->dout.biWidth, inst->dout.biHeight,
			 DDF_SAME_HDC | DDF_SAME_DRAW))
		return ICERR_ERROR;
	return ICERR_OK;
}

LRESULT WINAPI DriverProc(DWORD_PTR id, HDRVR hdrvr, UINT msg, LPARAM l1,
			  LPARAM l2)
{
	struct instance *inst = (struct instance *)id;

	switch (msg) {
	case DRV_LOAD: {
		char v[8];
		strict = GetEnvironmentVariableA("TESTCODEC_STRICT", v, sizeof(v)) == 1 &&
			 v[0] == '1';
		nocompress = GetEnvironmentVariableA("TESTCODEC_NOCOMPRESS", v,
						     sizeof(v)) == 1 && v[0] == '1';
		latesize = GetEnvironmentVariableA("TESTCODEC_LATESIZE", v,
						   sizeof(v)) == 1 && v[0] == '1';
		needs_status = GetEnvironmentVariableA("TESTCODEC_STATUS", v,
						       sizeof(v)) == 1 && v[0] == '1';
		needs_padding = GetEnvironmentVariableA("TESTCODEC_PADDING", v,
							sizeof(v)) == 1 && v[0] == '1';
		ask_format = GetEnvironmentVariableA("TESTCODEC_ASK_FORMAT", v,
						     sizeof(v)) == 1 && v[0] == '1';
		picky = GetEnvironmentVariableA("TESTCODEC_PICKY", v,
						sizeof(v)) == 1 && v[0] == '1';
		decode_only = GetEnvironmentVariableA("TESTCODEC_DECODE_ONLY", v,
						      sizeof(v)) == 1 && v[0] == '1';
		named_only = GetEnvironmentVariableA("TESTCODEC_NAMED_ONLY", v,
						     sizeof(v)) == 1 &&
			     v[0] >= '1' && v[0] <= '3' ? v[0] - '0' : 0;
		end_crash = GetEnvironmentVariableA("TESTCODEC_END_CRASH", v,
						    sizeof(v)) == 1 && v[0] == '1';
		overrun = GetEnvironmentVariableA("TESTCODEC_OVERRUN", v,
						  sizeof(v)) == 1 && v[0] == '1';
		empty_format = GetEnvironmentVariableA("TESTCODEC_EMPTY_FORMAT", v,
						       sizeof(v)) == 1 && v[0] == '1';
		no_keys = GetEnvironmentVariableA("TESTCODEC_NO_KEYS", v,
						  sizeof(v)) == 1 && v[0] == '1';
		align16 = GetEnvironmentVariableA("TESTCODEC_ALIGN16", v,
						  sizeof(v)) == 1 && v[0] == '1';
		if (GetEnvironmentVariableA("TESTCODEC_CHATTY", v, sizeof(v)) == 1 &&
		    v[0] == '1') {
			static const char hello[] = "testcodec: hello\n";
			DWORD put;
			WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), hello,
				  sizeof(hello) - 1, &put, NULL);
		}
		log_msg(msg);
		check_module(hdrvr);
		return DefDriverProc(id, hdrvr, msg, l1, l2);
	}
	case DRV_FREE:
	case DRV_ENABLE:
	case DRV_DISABLE:
		log_msg(msg);
		return DefDriverProc(id, hdrvr, msg, l1, l2);

	case DRV_OPEN: {
		ICOPEN *op = (ICOPEN *)l2;
		log_msg(msg);
		check_module(hdrvr);
		if (high(1, op))
			return 0;
		if (op) {
			DWORD t = op->fccType | 0x20202020;
			DWORD hnd = op->fccHandler & ~0x20202020;
			if ((op->fccType && t != VIDC) ||
			    (op->fccHandler && hnd != (TST0 & ~0x20202020) &&
			     !pair_named(op->fccHandler)) ||
			    (decode_only && (op->dwFlags == ICMODE_COMPRESS ||
					     op->dwFlags == ICMODE_FASTCOMPRESS))) {
				op->dwError = ICERR_UNSUPPORTED;
				return 0;
			}
			op->dwError = ICERR_OK;
		}
		inst = alloc(sizeof(*inst));
		if (!inst)
			return 0;
		inst->magic = MAGIC;
		inst->named = op && (named_only == 2 ?
				     pair_named(op->fccHandler) :
				     op->fccHandler == TST0);
		inst->st.quality = 7500;
		inst->st.keyrate = 15;
		InterlockedIncrement(&opens);
		return (LRESULT)inst;
	}
	case DRV_CLOSE:
		log_msg(msg);
		if (inst && inst->magic == MAGIC) {
			draw_end(inst);
			release(inst->cprev);
			release(inst->dcur);
			inst->magic = 0;
			release(inst);
			InterlockedDecrement(&opens);
		}
		return 1;
	}

	if (msg < DRV_USER || !inst || inst->magic != MAGIC)
		return DefDriverProc(id, hdrvr, msg, l1, l2);

	switch (msg) {
	case TESTCODEC_CRASH:
		*(volatile DWORD *)l1 = 1;	/* l1 is 0 */
		return ICERR_OK;
	case TESTCODEC_HANG:
		for (;;)
			*(volatile DWORD *)&inst->magic;
	case ICM_GETINFO:
		if (named_only == 3 && !inst->named) {
			ICINFO *info = (ICINFO *)l1;
			LRESULT r = get_info(info, (DWORD)l2);
			if (r) {
				info->fccHandler = 0;
				info->szName[0] = info->szDescription[0] = 0;
			}
			return r;
		}
		if (named_only && !inst->named)
			return 0;
		return get_info((ICINFO *)l1, (DWORD)l2);
	case ICM_GETSTATE:
		if (!l1)
			return sizeof(inst->st);
		if ((DWORD)l2 < sizeof(inst->st))
			return ICERR_BADSIZE;
		*(struct state *)l1 = inst->st;
		return sizeof(inst->st);
	case ICM_SETSTATE:
		if (!l1) {
			inst->st.quality = 7500;
			inst->st.keyrate = 15;
			return 0;
		}
		if ((DWORD)l2 < sizeof(inst->st))
			return 0;
		inst->st = *(const struct state *)l1;
		return sizeof(inst->st);
	case ICM_GETDEFAULTQUALITY:
		if (l1)
			*(DWORD *)l1 = 7500;
		return ICERR_OK;
	case ICM_GETQUALITY:
		if (l1)
			*(DWORD *)l1 = inst->st.quality;
		return ICERR_OK;
	case ICM_GETDEFAULTKEYFRAMERATE:
		if (l1)
			*(DWORD *)l1 = 15;
		return ICERR_OK;
	case ICM_CONFIGURE:
	case ICM_ABOUT:
		return ICERR_UNSUPPORTED;

	case ICM_COMPRESS_QUERY:
		if (nocompress)
			return ICERR_UNSUPPORTED;
		if (picky && l2)
			return ICERR_BADFORMAT;
		return compress_query((BITMAPINFOHEADER *)l1, (BITMAPINFOHEADER *)l2);
	case ICM_COMPRESS_GET_FORMAT:
		return compress_get_format((BITMAPINFOHEADER *)l1,
					   (BITMAPINFOHEADER *)l2);
	case ICM_COMPRESS_GET_SIZE:
		if (!ok_input((BITMAPINFOHEADER *)l1))
			return ICERR_BADFORMAT;
		if (latesize && !inst->cprev)
			return EARLY_SIZE;
		return max_size((BITMAPINFOHEADER *)l1);
	case ICM_COMPRESS_FRAMES_INFO: {
		ICCOMPRESSFRAMES *f = (ICCOMPRESSFRAMES *)l1;
		if (!f)
			return ICERR_BADPARAM;
		inst->st.keyrate = f->lKeyRate >= 0 ? (DWORD)f->lKeyRate : 0;
		if (f->lQuality >= 0)
			inst->st.quality = (DWORD)f->lQuality;
		return ICERR_OK;
	}
	case ICM_COMPRESS_BEGIN: {
		const BITMAPINFOHEADER *in = (BITMAPINFOHEADER *)l1;
		LRESULT r = compress_query(in, (BITMAPINFOHEADER *)l2);
		if (r != ICERR_OK)
			return r;
		release(inst->cprev);
		if (!(inst->cprev = alloc(frame_bytes(in))))
			return ICERR_MEMORY;
		inst->cframe = 0;
		return ICERR_OK;
	}
	case ICM_SET_STATUS_PROC: {
		const ICSETSTATUSPROC *sp = (const ICSETSTATUSPROC *)l1;
		if (!sp)
			return ICERR_BADPARAM;
		inst->status_param = sp->lParam;
		inst->status = sp->Status;
		return ICERR_OK;
	}
	case ICM_COMPRESS:
		/* Unchecked, as ADV601.dll calls it. */
		if (needs_status && inst->status(inst->status_param,
						 ICSTATUS_STATUS, 0))
			return ICERR_ABORT;
		if (overrun) {
			/* On past the end, a byte at a time, as an overrun goes. */
			ICCOMPRESS *c = (ICCOMPRESS *)l1;
			volatile BYTE *end = (volatile BYTE *)c->lpOutput +
					     max_size(c->lpbiInput);
			for (DWORD i = 0; i < 2 * 4096 + 64; i++)
				end[i] = 0;
		}
		return compress(inst, (ICCOMPRESS *)l1);
	case ICM_COMPRESS_END:
		release(inst->cprev);
		inst->cprev = NULL;
		return end_crash ? (LRESULT)inst->magic / zero : ICERR_OK;

	case ICM_DECOMPRESS_QUERY:
		return decompress_query((BITMAPINFOHEADER *)l1,
					(BITMAPINFOHEADER *)l2);
	case ICM_DECOMPRESS_GET_FORMAT:
		inst->asked = 1;
		return decompress_get_format((BITMAPINFOHEADER *)l1,
					     (BITMAPINFOHEADER *)l2);
	case ICM_DECOMPRESS_BEGIN: {
		const BITMAPINFOHEADER *in = (BITMAPINFOHEADER *)l1;
		LRESULT r = decompress_query(in, (BITMAPINFOHEADER *)l2);
		if (r != ICERR_OK)
			return r;
		inst->ready = inst->asked;
		inst->asked = 0;
		release(inst->dcur);
		if (!(inst->dcur = alloc(frame_bytes(in))))
			return ICERR_MEMORY;
		inst->dvalid = 0;
		return ICERR_OK;
	}
	case ICM_DECOMPRESS:
		return decompress(inst, (ICDECOMPRESS *)l1);
	case ICM_DECOMPRESS_END:
		release(inst->dcur);
		inst->dcur = NULL;
		return end_crash ? (LRESULT)inst->magic / zero : ICERR_OK;

	case ICM_DRAW_QUERY:
		return ok_tst0((BITMAPINFOHEADER *)l1) &&
		       ((BITMAPINFOHEADER *)l1)->biBitCount != 12 ?
		       ICERR_OK : ICERR_BADFORMAT;
	case ICM_DRAW_BEGIN:
		return draw_begin(inst, (ICDRAWBEGIN *)l1);
	case ICM_DRAW:
		return draw(inst, (ICDRAW *)l1);
	case ICM_DRAW_END:
		draw_end(inst);
		return ICERR_OK;
	}
	return ICERR_UNSUPPORTED;
}

/*
 * Drives winmm and msvfw32 from the Win32 side. Returns 0, or the number
 * of the first check that failed. Expects 'TST0' to be registered.
 */
EXPORT int testcodec_selftest(void)
{
	WCHAR path[MAX_PATH];
	ICINFO info;
	HDRVR h;
	HIC hic;

	if (DefDriverProc(0, 0, DRV_LOAD, 0, 0) != 1)
		return 1;
	if (DefDriverProc(0, 0, DRV_QUERYCONFIGURE, 0, 0) != 0)
		return 2;
	if (DefDriverProc(0, 0, DRV_INSTALL, 0, 0) != DRVCNF_RESTART)
		return 3;
	if (DefDriverProc(0, 0, DRV_USER + 5, 0, 0) != 0)
		return 4;

	/* OpenDriver on this DLL by its Windows path. */
	if (!GetModuleFileNameW(self, path, MAX_PATH))
		return 5;
	h = OpenDriver(path, NULL, 0);
	if (!h)
		return 6;
	if (GetDriverModuleHandle(h) != self)
		return 7;
	if (SendDriverMessage(h, ICM_GETINFO, (LPARAM)&info, sizeof(info)) !=
	    sizeof(info) || info.fccHandler != TST0)
		return 8;
	if (CloseDriver(h, 0, 0) != 1)
		return 9;
	if (SendDriverMessage(h, ICM_GETINFO, (LPARAM)&info, sizeof(info)) != 0)
		return 10;

	/* msvfw32, through the registry. */
	hic = ICOpen(ICTYPE_VIDEO, TST0, ICMODE_QUERY);
	if (!hic)
		return 11;
	info.szDriver[0] = 0;
	if (ICGetInfo(hic, &info, sizeof(info)) != sizeof(info) ||
	    info.szName[0] != 'T' || !info.szDriver[0])
		return 12;
	if (ICSendMessage(hic, ICM_GETSTATE, 0, 0) != sizeof(struct state))
		return 13;
	if (ICClose(hic) != ICERR_OK)
		return 14;
	if (ICSendMessage(hic, ICM_GETSTATE, 0, 0) != ICERR_BADHANDLE)
		return 15;
	if (ICOpen(ICTYPE_VIDEO, mmioFOURCC('N', 'O', 'N', 'E'), ICMODE_QUERY))
		return 16;
	if (HIWORD(VideoForWindowsVersion()) < 0x0400)
		return 17;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	char v[8];

	if (reason == DLL_PROCESS_ATTACH)
		self = inst;
	if (reason == DLL_PROCESS_DETACH &&
	    GetEnvironmentVariableA("TESTCODEC_DETACH_CRASH", v, sizeof(v)) == 1 &&
	    v[0] == '1')
		*(volatile DWORD *)(DWORD_PTR)zero = 1;	/* zero is 0 */
	return TRUE;
}
