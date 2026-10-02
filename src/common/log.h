/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Diagnostics: VFW_TRACE=1 logs calls into the Win32 stubs to stderr.
 */
#ifndef VFW_COMMON_LOG_H
#define VFW_COMMON_LOG_H

extern int vfw_trace_enabled;

/* Reads VFW_TRACE; called once by runtime_init(). */
void vfw_log_init(void);

void vfw_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vfw_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define TRACE(...) \
	do { \
		if (vfw_trace_enabled) \
			vfw_trace(__VA_ARGS__); \
	} while (0)

#endif /* VFW_COMMON_LOG_H */
