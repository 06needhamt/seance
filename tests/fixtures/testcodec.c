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
 * Messages it does not handle go to winmm's DefDriverProc, and
 * testcodec_selftest() calls winmm and msvfw32 from the Win32 side.
 * Built like old MSVC code, with a 4-byte stack boundary.
 */
#include "fixture.h"
#include <vfw.h>

#define TST0 mmioFOURCC('T', 'S', 'T', '0')
#define VIDC mmioFOURCC('v', 'i', 'd', 'c')
#define I420 mmioFOURCC('I', '4', '2', '0')
#define IYUV mmioFOURCC('I', 'Y', 'U', 'V')
#define MAX_LOG 64

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
};

#define MAGIC 0x54535430

static HINSTANCE self;
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

static void *alloc(DWORD size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *p)
{
	if (p)
		HeapFree(GetProcessHeap(), 0, p);
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
	static const char desc[] = "vfw-harness test codec";
	int i;

	if (!info || size < sizeof(*info))
		return 0;
	info->dwSize = sizeof(*info);
	info->fccType = ICTYPE_VIDEO;
	info->fccHandler = TST0;
	info->dwFlags = VIDCF_TEMPORAL | VIDCF_FASTTEMPORALC;
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
	*out = *in;
	out->biSize = sizeof(*out);
	out->biCompression = TST0;
	out->biSizeImage = max_size(in);
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

	if (compress_query(in, c->lpbiOutput) != ICERR_OK || !inst->cprev)
		return ICERR_BADFORMAT;
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
		*c->lpdwFlags = key ? AVIIF_KEYFRAME : 0;
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

	if (decompress_query(in, out) != ICERR_OK || !inst->dcur)
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

LRESULT WINAPI DriverProc(DWORD_PTR id, HDRVR hdrvr, UINT msg, LPARAM l1,
			  LPARAM l2)
{
	struct instance *inst = (struct instance *)id;

	switch (msg) {
	case DRV_LOAD:
	case DRV_FREE:
	case DRV_ENABLE:
	case DRV_DISABLE:
		log_msg(msg);
		return DefDriverProc(id, hdrvr, msg, l1, l2);

	case DRV_OPEN: {
		ICOPEN *op = (ICOPEN *)l2;
		log_msg(msg);
		if (op) {
			DWORD t = op->fccType | 0x20202020;
			DWORD hnd = op->fccHandler & ~0x20202020;
			if ((op->fccType && t != VIDC) ||
			    (op->fccHandler && hnd != (TST0 & ~0x20202020))) {
				op->dwError = ICERR_UNSUPPORTED;
				return 0;
			}
			op->dwError = ICERR_OK;
		}
		inst = alloc(sizeof(*inst));
		if (!inst)
			return 0;
		inst->magic = MAGIC;
		inst->st.quality = 7500;
		inst->st.keyrate = 15;
		InterlockedIncrement(&opens);
		return (LRESULT)inst;
	}
	case DRV_CLOSE:
		log_msg(msg);
		if (inst && inst->magic == MAGIC) {
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
	case ICM_GETINFO:
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
		return compress_query((BITMAPINFOHEADER *)l1, (BITMAPINFOHEADER *)l2);
	case ICM_COMPRESS_GET_FORMAT:
		return compress_get_format((BITMAPINFOHEADER *)l1,
					   (BITMAPINFOHEADER *)l2);
	case ICM_COMPRESS_GET_SIZE:
		if (!ok_input((BITMAPINFOHEADER *)l1))
			return ICERR_BADFORMAT;
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
	case ICM_COMPRESS:
		return compress(inst, (ICCOMPRESS *)l1);
	case ICM_COMPRESS_END:
		release(inst->cprev);
		inst->cprev = NULL;
		return ICERR_OK;

	case ICM_DECOMPRESS_QUERY:
		return decompress_query((BITMAPINFOHEADER *)l1,
					(BITMAPINFOHEADER *)l2);
	case ICM_DECOMPRESS_GET_FORMAT:
		return decompress_get_format((BITMAPINFOHEADER *)l1,
					     (BITMAPINFOHEADER *)l2);
	case ICM_DECOMPRESS_BEGIN: {
		const BITMAPINFOHEADER *in = (BITMAPINFOHEADER *)l1;
		LRESULT r = decompress_query(in, (BITMAPINFOHEADER *)l2);
		if (r != ICERR_OK)
			return r;
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
	if (reason == DLL_PROCESS_ATTACH)
		self = inst;
	return TRUE;
}
