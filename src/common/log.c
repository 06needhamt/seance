#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common/log.h"

int vfw_trace_enabled;

void vfw_log_init(void)
{
	const char *v = getenv("VFW_TRACE");
	vfw_trace_enabled = v && *v && strcmp(v, "0");
}

static void vlog(const char *prefix, const char *fmt, va_list ap)
{
	char line[1024];
	int n = snprintf(line, sizeof(line), "%s[%ld] ", prefix,
			 (long)syscall(SYS_gettid));

	if (n < 0 || n >= (int)sizeof(line))
		n = 0;
	vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	/* One write per line, so lines from different threads don't mix. */
	fprintf(stderr, "%s\n", line);
}

void vfw_trace(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog("trace", fmt, ap);
	va_end(ap);
}

void vfw_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog("warning", fmt, ap);
	va_end(ap);
}
