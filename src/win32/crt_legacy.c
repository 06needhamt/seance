/* SPDX-License-Identifier: GPL-2.0-only */
/* Small, ABI-compatible pieces of the old MSVCRT/MSVCP surface. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "runtime/builtin.h"
#include "win32/path.h"

#define CRTFN(ret) static ret STUB_CDECL
#define THISFN(ret) static ret __attribute__((thiscall, force_align_arg_pointer))

typedef struct { char *_ptr; int _cnt; char *_base; int _flag, _file; } crt_FILE;

CRTFN(int) crt__access(const char *path, int mode)
{
	char *unix_path = path_to_unix(path);
	int result;

	if (!unix_path)
		return -1;
	result = access(unix_path, mode);
	free(unix_path);
	return result;
}
CRTFN(int) crt__adjust_fdiv(void) { return 0; }
CRTFN(clock_t) crt_clock(void) { return clock(); }
CRTFN(void) crt_exit(int status) { exit(status); }
CRTFN(char *) crt_getenv(const char *name) { return getenv(name); }
CRTFN(time_t) crt_time(time_t *out) { return time(out); }
CRTFN(struct tm *) crt_localtime(const time_t *t) { return localtime(t); }
CRTFN(size_t) crt_strftime(char *s, size_t n, const char *fmt,
			    const struct tm *tm) { return strftime(s, n, fmt, tm); }
CRTFN(int) crt_sscanf(const char *s, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); int r = vsscanf(s, fmt, ap); va_end(ap); return r;
}
CRTFN(int) crt_fscanf(crt_FILE *f, const char *fmt, ...)
{
	if (!f) { errno = EINVAL; return EOF; }
	int fd = dup(f->_file);
	if (fd < 0) return EOF;
	FILE *host = fdopen(fd, "r");
	if (!host) { close(fd); return EOF; }
	va_list ap; va_start(ap, fmt); int r = vfscanf(host, fmt, ap); va_end(ap);
	fclose(host); return r;
}

/* VC6 iostream initialisers and locks are process-wide bookkeeping only. */
THISFN(void *) crt_ios_init_ctor(void *self) { return self; }
THISFN(void) crt_ios_init_dtor(void *self) { (void)self; }
THISFN(void *) crt_lockit_ctor(void *self) { return self; }
THISFN(void) crt_lockit_dtor(void *self) { (void)self; }
THISFN(void *) crt_winit_ctor(void *self) { return self; }
THISFN(void) crt_winit_dtor(void *self) { (void)self; }
static unsigned char crt_cout[64];
THISFN(void *) crt_ostream_int(void *self, int value)
{
	fprintf(stdout, "%d", value); return self;
}
CRTFN(void *) crt_ostream_string(void *self, const char *value)
{
	fputs(value ? value : "", stdout); return self;
}
CRTFN(void *) crt_ostream_endl(void *self)
{
	fputc('\n', stdout); fflush(stdout); return self;
}

#define E(n) { #n, 0, (void *)crt_##n }
#define M(n, f) { n, 0, (void *)(f) }
const struct builtin_export crt_legacy_exports[] = {
	E(_access), E(_adjust_fdiv), E(clock), E(exit), E(fscanf), E(getenv),
	E(localtime), E(sscanf), E(strftime), E(time),
	M("??0Init@ios_base@std@@QAE@XZ", crt_ios_init_ctor),
	M("??1Init@ios_base@std@@QAE@XZ", crt_ios_init_dtor),
	M("??0_Lockit@std@@QAE@XZ", crt_lockit_ctor),
	M("??1_Lockit@std@@QAE@XZ", crt_lockit_dtor),
	M("??0_Winit@std@@QAE@XZ", crt_winit_ctor),
	M("??1_Winit@std@@QAE@XZ", crt_winit_dtor),
	M("??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@H@Z", crt_ostream_int),
	M("??6std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@0@AAV10@PBD@Z", crt_ostream_string),
	M("?cout@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A", crt_cout),
	M("?endl@std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@1@AAV21@@Z", crt_ostream_endl),
	BUILTIN_END
};
