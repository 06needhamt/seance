/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VFW_INPUT_PADDING: zeroed bytes kept after the compressed input handed
 * to a codec, as FFmpeg pads its packets (AV_INPUT_BUFFER_PADDING_SIZE).
 * Decoders read, and some write, a little past the end of their input:
 * sp6x_32.dll writes there, vgpix32d.dll's bit reader reads a word ahead.
 */
#ifndef VFW_COMMON_PADDING_H
#define VFW_COMMON_PADDING_H

#define VFW_INPUT_PADDING 64

#endif /* VFW_COMMON_PADDING_H */
