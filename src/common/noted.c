/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/noted.h"

void noted_add(struct noted *n, const char *name)
{
	char *copy = strdup(name);

	if (!copy)
		return;
	free(n->names[n->count % NOTED_MAX]);
	n->names[n->count++ % NOTED_MAX] = copy;
}

unsigned noted_mark(const struct noted *n)
{
	return n->count;
}

int noted_since(const struct noted *n, unsigned mark, char *buf, size_t size)
{
	size_t len = 0;
	int written = 0;

	if (size)
		buf[0] = 0;
	if (n->count - mark > NOTED_MAX)
		mark = n->count - NOTED_MAX;
	for (unsigned i = mark; i < n->count; i++) {
		const char *name = n->names[i % NOTED_MAX];
		int seen = 0;
		for (unsigned k = mark; k < i && !seen; k++)
			seen = !strcasecmp(n->names[k % NOTED_MAX], name);
		if (!seen && len < size) {
			len += snprintf(buf + len, size - len, "%s%s",
					len ? ", " : "", name);
			written++;
		}
	}
	return written;
}
