/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FOURCC helpers.
 */
#ifndef VFW_COMMON_FOURCC_H
#define VFW_COMMON_FOURCC_H

#include "win32.h"

#define FOURCC_STR_SIZE 20

/*
 * Formats a FOURCC for display: printable characters as they are, anything
 * else as \xNN (FOURCCs come from files and may hold any bytes). Returns buf.
 */
const char *fourcc_str(FOURCC f, char buf[FOURCC_STR_SIZE]);

#endif /* VFW_COMMON_FOURCC_H */
