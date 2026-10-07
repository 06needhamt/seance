/* SPDX-License-Identifier: GPL-2.0-only */
/* Small, ABI-compatible pieces of the old MSVCRT/MSVCP surface. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
/*
 * The harness runs in UTC (see k32_time.c): no zone offset, no daylight
 * saving. MSVC's struct tm is the nine ints of the C standard, without
 * glibc's trailing fields.
 */
static int tz_daylight, tz_dstbias;
static long tz_timezone;

CRTFN(void) crt__tzset(void) { }
CRTFN(int *) crt___p__daylight(void) { return &tz_daylight; }
CRTFN(int *) crt___p__dstbias(void) { return &tz_dstbias; }
CRTFN(long *) crt___p__timezone(void) { return &tz_timezone; }

CRTFN(struct tm *) crt_gmtime(const time_t *t)
{
	static struct { int fields[9]; } out;
	struct tm tm;

	if (!t || !gmtime_r(t, &tm))
		return NULL;
	out.fields[0] = tm.tm_sec;
	out.fields[1] = tm.tm_min;
	out.fields[2] = tm.tm_hour;
	out.fields[3] = tm.tm_mday;
	out.fields[4] = tm.tm_mon;
	out.fields[5] = tm.tm_year;
	out.fields[6] = tm.tm_wday;
	out.fields[7] = tm.tm_yday;
	out.fields[8] = 0;
	return (struct tm *)&out;
}

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
CRTFN(int *) crt__errno(void) { return &errno; }
CRTFN(int) crt__read(int fd, void *buf, unsigned count)
{
	ssize_t n = read(fd, buf, count); return n < 0 ? -1 : (int)n;
}
CRTFN(int) crt__write(int fd, const void *buf, unsigned count)
{
	ssize_t n = write(fd, buf, count); return n < 0 ? -1 : (int)n;
}
CRTFN(long) crt__lseek(int fd, long off, int origin)
{
	off_t n = lseek(fd, off, origin); return n < 0 ? -1 : (long)n;
}
CRTFN(long long) crt__filelengthi64(int fd)
{
	struct stat st; return fstat(fd, &st) ? -1 : (long long)st.st_size;
}
CRTFN(int) crt__fstat(int fd, void *out)
{
	/* The beginning of MSVCRT's _stat structure is compatible for the
	 * fields callers normally inspect: device, inode, mode and links. */
	struct stat st;
	if (!out || fstat(fd, &st)) return -1;
	struct win_stat { uint32_t dev, ino; uint16_t mode, nlink, uid, gid;
		uint32_t rdev; int32_t size, atime, mtime, ctime; } *w = out;
	*w = (struct win_stat){ st.st_dev, st.st_ino, st.st_mode, st.st_nlink,
		st.st_uid, st.st_gid, st.st_rdev, st.st_size,
		st.st_atime, st.st_mtime, st.st_ctime };
	return 0;
}
CRTFN(int) crt__isctype(int c, int mask)
{
	if (c < 0 || c > 255) return 0;
	unsigned f = 0;
	if (isupper(c)) f |= 0x101;
	if (islower(c)) f |= 0x102;
	if (isdigit(c)) f |= 0x004;
	if (isspace(c)) f |= 0x008;
	if (ispunct(c)) f |= 0x010;
	if (iscntrl(c)) f |= 0x020;
	if (c == ' ') f |= 0x040;
	if (isxdigit(c)) f |= 0x080;
	return f & mask;
}
CRTFN(char *) crt_setlocale(int category, const char *locale)
{
	/* The emulation intentionally remains in the deterministic C locale. */
	(void)category;
	if (!locale || !*locale || !strcmp(locale, "C") || !strcmp(locale, ".C"))
		return "C";
	errno = EINVAL; return NULL;
}
CRTFN(struct lconv *) crt_localeconv(void) { return localeconv(); }
CRTFN(void) crt__assert(const char *expr, const char *file, unsigned line)
{
	fprintf(stderr, "Assertion failed: %s, file %s, line %u\n",
		expr ? expr : "", file ? file : "", line);
	abort();
}

__attribute__((naked)) static void STUB_CDECL crt__EH_prolog(void)
{
	__asm__ volatile("pushl $-1; pushl %eax; pushl %fs:0; movl %esp,%fs:0; "
		"movl 12(%esp),%eax; movl %ebp,12(%esp); leal 12(%esp),%ebp; "
		"pushl %eax; ret");
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
	E(_EH_prolog), E(_access), E(_adjust_fdiv), E(_assert), E(_errno), E(_filelengthi64),
	E(_fstat), E(_isctype), E(_lseek), E(_read), E(_write),
	E(_tzset), E(__p__daylight), E(__p__dstbias), E(__p__timezone), E(gmtime),
	E(clock), E(exit), E(fscanf), E(getenv), E(localeconv), E(setlocale),
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
