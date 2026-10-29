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
#include "win32/codepage.h"
#include "win32/k32.h"
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
/* The clock DLLs see, which VFW_DATE may set. */
CRTFN(time_t) crt_time(time_t *out)
{
	time_t t = (time_t)win_unix_now();

	if (out)
		*out = t;
	return t;
}
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
	/* A DLL's stdout is our stderr (see crt_stdio.c). */
	ssize_t n = write(fd == STDOUT_FILENO ? STDERR_FILENO : fd, buf, count); return n < 0 ? -1 : (int)n;
}
CRTFN(long) crt__lseek(int fd, long off, int origin)
{
	off_t n = lseek(fd, off, origin); return n < 0 ? -1 : (long)n;
}
CRTFN(long long) crt__filelengthi64(int fd)
{
	struct stat st; return fstat(fd, &st) ? -1 : (long long)st.st_size;
}
/*
 * MSVCRT's struct _stat (the 32-bit one, which _stat, _wstat and _fstat fill):
 * the device, a 16-bit inode, mode and link count and owner ids, then the
 * size and the times as 32-bit values. The mode's type bits are the Unix
 * ones; the permission bits are the owner's, copied to group and others.
 */
struct crt_stat32 {
	uint32_t dev;
	uint16_t ino, mode;
	int16_t nlink, uid, gid;
	uint32_t rdev;
	int32_t size, atime, mtime, ctime;
};

static void stat_fill(const struct stat *st, struct crt_stat32 *w)
{
	unsigned owner = st->st_mode & 0700;

	memset(w, 0, sizeof(*w));
	w->dev = (uint32_t)st->st_dev;
	w->ino = (uint16_t)st->st_ino;
	w->mode = (uint16_t)((st->st_mode & 0170000) | owner | owner >> 3 | owner >> 6);
	w->nlink = (int16_t)st->st_nlink;
	w->rdev = (uint32_t)st->st_rdev;
	w->size = (int32_t)st->st_size;
	w->atime = (int32_t)st->st_atime;
	w->mtime = (int32_t)st->st_mtime;
	w->ctime = (int32_t)st->st_ctime;
}

CRTFN(int) crt__fstat(int fd, struct crt_stat32 *out)
{
	struct stat st;

	if (!out || fstat(fd, &st)) {
		if (!out)
			errno = EINVAL;
		return -1;
	}
	stat_fill(&st, out);
	return 0;
}

CRTFN(int) crt__stat(const char *name, struct crt_stat32 *out)
{
	char *path = name ? path_to_unix(name) : NULL;
	struct stat st;
	int r = -1;

	if (!out || !path)
		errno = !out ? EINVAL : ENOENT;
	else if (!stat(path, &st)) {
		stat_fill(&st, out);
		r = 0;
	}
	free(path);
	return r;
}

CRTFN(int) crt__wstat(const WCHAR *name, struct crt_stat32 *out)
{
	char *u = name ? utf16_to_utf8_dup(name) : NULL;
	int r = crt__stat(u, out);

	free(u);
	return r;
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
	fflush(stderr);
	_exit(3);	/* as msvcrt's assert, by way of abort, exits */
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

/* The wide forms of a few path and environment calls, over the UTF-8 ones. */

CRTFN(WCHAR *) crt__wfullpath(WCHAR *abs, const WCHAR *rel, size_t max)
{
	char *u = rel ? utf16_to_utf8_dup(rel) : strdup(".");
	char *full = u ? k32_full_path(u) : NULL;
	int need;

	free(u);
	if (!full) {
		errno = ENOMEM;
		return NULL;
	}
	need = cp_to_utf16(CP_UTF8, full, -1, NULL, 0, 0);
	if (!abs) {
		if (!max || max < (size_t)need)
			max = (size_t)need;
		abs = malloc(max * sizeof(WCHAR));
		if (!abs) {
			free(full);
			errno = ENOMEM;
			return NULL;
		}
	} else if (max < (size_t)need) {
		free(full);
		errno = ERANGE;
		return NULL;
	}
	cp_to_utf16(CP_UTF8, full, -1, abs, (int)max, 0);
	free(full);
	return abs;
}

/* The value, as UTF-16 in a buffer that the next call replaces. */
CRTFN(WCHAR *) crt__wgetenv(const WCHAR *name)
{
	static WCHAR value[1024];
	char *u = name ? utf16_to_utf8_dup(name) : NULL;
	const char *v = u ? getenv(u) : NULL;

	free(u);
	if (!v)
		return NULL;
	cp_to_utf16(CP_UTF8, v, -1, value, sizeof(value) / sizeof(value[0]), 0);
	value[sizeof(value) / sizeof(value[0]) - 1] = 0;
	return value;
}

static size_t wcslen16(const WCHAR *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

/* To the ANSI code page. With no destination, the length needed. */
CRTFN(size_t) crt_wcstombs(char *dst, const WCHAR *src, size_t n)
{
	int len;

	if (!src) {
		errno = EINVAL;
		return (size_t)-1;
	}
	if (!dst) {
		len = cp_from_utf16(CP_ACP, src, -1, NULL, 0, '?', NULL);
		return len > 0 ? (size_t)len - 1 : (size_t)-1;
	}
	if (!n)
		return 0;
	len = cp_from_utf16(CP_ACP, src, -1, dst, (int)n, '?', NULL);
	if (len <= 0) {
		/* Too small for all of it: what fits, with no terminator. */
		len = cp_from_utf16(CP_ACP, src, (int)wcslen16(src), dst, (int)n, '?', NULL);
		return len > 0 ? (size_t)len : n;
	}
	return (size_t)len - 1;
}

CRTFN(size_t) crt_mbstowcs(WCHAR *dst, const char *src, size_t n)
{
	int len;

	if (!src) {
		errno = EINVAL;
		return (size_t)-1;
	}
	if (!dst) {
		len = cp_to_utf16(CP_ACP, src, -1, NULL, 0, 0);
		return len > 0 ? (size_t)len - 1 : (size_t)-1;
	}
	if (!n)
		return 0;
	len = cp_to_utf16(CP_ACP, src, -1, dst, (int)n, 0);
	if (len <= 0) {
		len = cp_to_utf16(CP_ACP, src, (int)strlen(src), dst, (int)n, 0);
		return len > 0 ? (size_t)len : n;
	}
	return (size_t)len - 1;
}

/*
 * signal and raise, in software: the process takes no real signals, but
 * code that raises one (an abort, a failed assertion) gets the handler it
 * installed, or MSVCRT's default of ending the process with status 3.
 */

typedef void (*crt_sighandler)(int);

#define CRT_SIG_DFL ((crt_sighandler)0)
#define CRT_SIG_IGN ((crt_sighandler)1)
#define CRT_SIG_ERR ((crt_sighandler)-1)

/* SIGINT, SIGILL, SIGFPE, SIGSEGV, SIGTERM, SIGBREAK, SIGABRT. */
static const int crt_signals[] = { 2, 4, 8, 11, 15, 21, 22 };
static crt_sighandler crt_handlers[sizeof(crt_signals) / sizeof(crt_signals[0])];

static int signal_index(int sig)
{
	for (size_t i = 0; i < sizeof(crt_signals) / sizeof(crt_signals[0]); i++)
		if (crt_signals[i] == sig)
			return (int)i;
	return -1;
}

CRTFN(crt_sighandler) crt_signal(int sig, crt_sighandler handler)
{
	int i = signal_index(sig);
	crt_sighandler old;

	if (i < 0 || handler == CRT_SIG_ERR) {
		errno = EINVAL;
		return CRT_SIG_ERR;
	}
	old = crt_handlers[i];
	crt_handlers[i] = handler;
	return old;
}

CRTFN(int) crt_raise(int sig)
{
	int i = signal_index(sig);
	crt_sighandler handler;

	if (i < 0) {
		errno = EINVAL;
		return -1;
	}
	handler = crt_handlers[i];
	if (handler == CRT_SIG_IGN)
		return 0;
	if (handler == CRT_SIG_DFL) {
		fflush(NULL);
		_exit(3);
	}
	crt_handlers[i] = CRT_SIG_DFL;	/* a handler is used once */
	handler(sig);
	return 0;
}

#define E(n) { #n, 0, (void *)crt_##n }
#define M(n, f) { n, 0, (void *)(f) }
const struct builtin_export crt_legacy_exports[] = {
	E(_EH_prolog), E(_access), E(_adjust_fdiv), E(_assert), E(_errno), E(_filelengthi64),
	E(_fstat), E(_stat), E(_wstat), E(_isctype), E(_lseek), E(_read), E(_write),
	E(_wfullpath), E(_wgetenv), E(wcstombs), E(mbstowcs), E(signal), E(raise),
	E(_tzset), E(__p__daylight), E(__p__dstbias), E(__p__timezone), E(gmtime),
	E(clock), E(exit), E(fscanf), E(getenv), E(localeconv), E(setlocale),
	E(localtime), E(strftime), E(time),
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
