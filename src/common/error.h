/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Error reporting and small file helpers shared by the harness.
 */
#ifndef VFW_COMMON_ERROR_H
#define VFW_COMMON_ERROR_H

#include <stddef.h>

/*
 * Functions that fail return NULL or -1 and describe why with
 * vfw_set_error(); callers read the message back with vfw_error().
 * The message is per thread.
 */
void vfw_set_error(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
const char *vfw_error(void);

/* Reads a whole file into a malloc()ed buffer. Returns 0 or -1. */
int vfw_read_file(const char *path, void **buf, size_t *len);

#endif /* VFW_COMMON_ERROR_H */
