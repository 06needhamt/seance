/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"

static _Thread_local char last_error[512];

void vfw_set_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
}

const char *vfw_error(void)
{
	return last_error[0] ? last_error : "unknown error";
}

int vfw_read_file(const char *path, void **buf, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		vfw_set_error("%s: %s", path, strerror(errno));
		return -1;
	}

	size_t cap = 0, used = 0;
	char *data = NULL;
	for (;;) {
		if (used == cap) {
			size_t ncap = cap ? cap * 2 : 65536;
			char *n = realloc(data, ncap);
			if (!n) {
				vfw_set_error("%s: out of memory", path);
				goto fail;
			}
			data = n;
			cap = ncap;
		}
		size_t got = fread(data + used, 1, cap - used, f);
		used += got;
		if (got == 0) {
			if (ferror(f)) {
				vfw_set_error("%s: read error", path);
				goto fail;
			}
			break;
		}
	}

	fclose(f);
	*buf = data;
	*len = used;
	return 0;

fail:
	free(data);
	fclose(f);
	return -1;
}
