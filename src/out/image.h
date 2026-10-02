/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Conversion of uncompressed DIB frames to the two layouts the output
 * writers use: packed BGR24 and planar I420 (YUV 4:2:0), both top-down.
 *
 * Inputs are the formats VfW files and codecs use for uncompressed video:
 *   BI_RGB 1/4/8 bits (palette), 16 (RGB555), 24 and 32 bits, bottom-up
 *   or top-down by the sign of biHeight; BI_BITFIELDS 16 and 32 bits;
 *   BI_RLE8 and BI_RLE4 (Microsoft RLE, delta frames included);
 *   YUV FOURCCs: YUY2 (YUYV, YUNV, V422), UYVY (Y422, UYNV, HDYC), YVYU,
 *   I420 (IYUV), YV12, NV12, NV21, and Y800 (Y8, GREY).
 *
 * YUV is BT.601 limited range. Chroma is subsampled by averaging each 2x2
 * block (centred siting, as Y4M's C420jpeg says); odd sizes are allowed.
 */
#ifndef VFW_OUT_IMAGE_H
#define VFW_OUT_IMAGE_H

#include <stddef.h>

#include "win32.h"

struct image_conv;

/*
 * A converter for frames in format bih, whose fmt_size bytes may carry a
 * palette or bitfield masks after the header. NULL with vfw_error() for a
 * format this can't convert (compressed ones need a codec).
 */
struct image_conv *image_conv_new(const BITMAPINFOHEADER *bih,
				  size_t fmt_size);
void image_conv_free(struct image_conv *c);

int image_width(const struct image_conv *c);
int image_height(const struct image_conv *c);

/* Bytes of a whole frame; RLE frames vary, so they report 0. */
size_t image_frame_size(const struct image_conv *c);

/* The input's name, e.g. "RGB24 bottom-up" or "YUY2". */
const char *image_format_name(const struct image_conv *c);

/*
 * Applies an AVI palette change (the data of a ##pc chunk: first entry,
 * count, flags, then PALETTEENTRY values). Returns 0 or -1.
 */
int image_palette_change(struct image_conv *c, const void *data, size_t size);

/*
 * Whether frames build on the previous one (RLE), so skipping frames
 * means feeding them with image_feed() instead.
 */
int image_is_delta(const struct image_conv *c);
int image_feed(struct image_conv *c, const void *src, size_t len);

/* Output sizes. */
size_t image_bgr24_size(int width, int height);	/* width * 3 * height */
size_t image_i420_size(int width, int height);

/*
 * Converts one frame of len bytes. RLE frames update the previous picture,
 * so frames have to be given in order from a keyframe. Returns 0 or -1.
 */
int image_to_bgr24(struct image_conv *c, const void *src, size_t len,
		   BYTE *dst);
int image_to_i420(struct image_conv *c, const void *src, size_t len,
		  BYTE *dst);

/*
 * The other way: packs a picture into a frame of the format c was made
 * for (image_frame_size() bytes), e.g. to give a codec an input it
 * accepts. The source is top-down BGR24 for the RGB formats and I420 for
 * the YUV ones (image_pack_wants_i420). Palettised and RLE formats can't
 * be packed. Returns 0 or -1.
 */
int image_can_pack(const struct image_conv *c);
int image_pack_wants_i420(const struct image_conv *c);
int image_pack(const struct image_conv *c, const BYTE *src, BYTE *dst);

/* Direct conversions between the two output layouts. */
void image_bgr24_to_i420(const BYTE *bgr, int width, int height, BYTE *dst);
void image_i420_to_bgr24(const BYTE *yuv, int width, int height, BYTE *dst);

#endif /* VFW_OUT_IMAGE_H */
