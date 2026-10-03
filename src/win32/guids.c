/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The GUIDs include/win32 declares in lists (VFW_OBJBASE_GUIDS,
 * VFW_DSHOW_GUIDS), defined once here.
 */
#include "win32.h"

#define DEFINE(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
	const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } };

VFW_OBJBASE_GUIDS(DEFINE)
VFW_DSHOW_GUIDS(DEFINE)
