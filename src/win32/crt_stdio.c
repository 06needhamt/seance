/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt: <stdio.h> and <ctype.h>'s _pctype.
 *
 * A codec DLL almost never does its own file I/O or console output (it is
 * driven by the harness, not a program with a main() of its own), but its
 * CRT startup code commonly touches stdout/stderr/_iob and _pctype/isalpha
 * whether the codec's own code does or not, so a dynamically linked CRT
 * needs these to exist and behave.
 *
 * FILE is the classic MSVC layout (fixed since VC6): a leading underscore
 * on the raw handle in _file is the only field code outside this file
 * should ever read, and only through the fs:[0]-blind _fileno()-style
 * accessor a codec would actually call, but old code that reaches into
 * _iob[1] directly (rather than going through a stdout()/__iob_func()
 * accessor) needs the layout to still be right at that address.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/crt.h"
#include "win32/internal.h"
#include "win32/path.h"

#define CRTFN(ret) static ret STUB_CDECL

typedef struct crt_FILE {
	char *_ptr;
	int _cnt;
	char *_base;
	int _flag;
	int _file;
	int _charbuf;
	int _bufsiz;
	char *_tmpfname;
} crt_FILE;

#define _IOFBF 0
#define _IOREAD  0x0001
#define _IOWRT   0x0002
#define _IOEOF   0x0010
#define _IOERR   0x0020

static crt_FILE crt_iob[3];	/* zero-initialised; crt_stdio_init() fills it */

static void crt_stdio_init(void) __attribute__((constructor));

/* _iob[0..2] map straight to the process's own stdin/stdout/stderr fds. */
static void crt_stdio_init(void)
{
	crt_iob[0]._file = STDIN_FILENO;
	crt_iob[0]._flag = _IOREAD;
	crt_iob[1]._file = STDOUT_FILENO;
	crt_iob[1]._flag = _IOWRT;
	crt_iob[2]._file = STDERR_FILENO;
	crt_iob[2]._flag = _IOWRT;
}

/* A codec-opened file also gets one of these, heap-allocated; its _file is
 * the only field this implementation itself ever reads back. */
static crt_FILE *file_new(int fd, int writing)
{
	crt_FILE *f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	f->_file = fd;
	f->_flag = writing ? _IOWRT : _IOREAD;
	return f;
}

CRTFN(crt_FILE *) crt__fdopen(int fd, const char *mode)
{
	if (fd < 0 || !mode) { errno = EINVAL; return NULL; }
	return file_new(fd, strpbrk(mode, "wa+") != NULL);
}

CRTFN(crt_FILE *) crt_fopen(const char *path, const char *mode)
{
	int writing = strpbrk(mode, "wa+") != NULL;
	int flags = 0;
	if (strchr(mode, '+'))
		flags = O_RDWR;
	else if (writing)
		flags = O_WRONLY;
	else
		flags = O_RDONLY;
	if (strchr(mode, 'w'))
		flags |= O_CREAT | O_TRUNC;
	else if (strchr(mode, 'a'))
		flags |= O_CREAT | O_APPEND;

	char *unix_path = path_to_unix(path);
	if (!unix_path)
		return NULL;
	int fd = open(unix_path, flags, 0666);
	free(unix_path);
	if (fd < 0)
		return NULL;
	return file_new(fd, writing);
}

CRTFN(crt_FILE *) crt__wfopen(const WCHAR *path, const WCHAR *mode)
{
	char *p = path ? utf16_to_utf8_dup(path) : NULL;
	char *m = mode ? utf16_to_utf8_dup(mode) : NULL;
	crt_FILE *f = NULL;

	if (p && m)
		f = crt_fopen(p, m);
	else
		errno = EINVAL;
	free(p);
	free(m);
	return f;
}

CRTFN(int) crt_fclose(crt_FILE *f)
{
	if (!f || f == &crt_iob[0] || f == &crt_iob[1] || f == &crt_iob[2])
		return f ? 0 : EOF;
	int r = close(f->_file);
	free(f);
	return r < 0 ? EOF : 0;
}

CRTFN(int) crt_remove(const char *path)
{
	char *unix_path = path_to_unix(path);
	if (!unix_path)
		return -1;
	int r = unlink(unix_path);
	free(unix_path);
	return r;
}

CRTFN(size_t) crt_fread(void *buf, size_t size, size_t count, crt_FILE *f)
{
	if (!size || !count)
		return 0;
	size_t want = size * count, got = 0;
	char *p = buf;
	while (got < want) {
		ssize_t r = read(f->_file, p + got, want - got);
		if (r <= 0) {
			f->_flag |= r == 0 ? _IOEOF : _IOERR;
			break;
		}
		got += (size_t)r;
	}
	return got / size;
}

CRTFN(size_t) crt_fwrite(const void *buf, size_t size, size_t count, crt_FILE *f)
{
	if (!size || !count)
		return 0;
	size_t want = size * count, put = 0;
	const char *p = buf;
	while (put < want) {
		ssize_t w = write(f->_file, p + put, want - put);
		if (w <= 0) {
			f->_flag |= _IOERR;
			break;
		}
		put += (size_t)w;
	}
	return put / size;
}

CRTFN(int) crt_fputc(int c, crt_FILE *f)
{
	unsigned char ch = (unsigned char)c;
	return crt_fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}

CRTFN(int) crt_fgetc(crt_FILE *f)
{
	unsigned char ch;
	return crt_fread(&ch, 1, 1, f) == 1 ? ch : EOF;
}
CRTFN(int) crt_getc(crt_FILE *f) { return crt_fgetc(f); }

CRTFN(int) crt__filbuf(crt_FILE *f) { return crt_fgetc(f); }
CRTFN(int) crt__flsbuf(int c, crt_FILE *f) { return crt_fputc(c, f); }
CRTFN(int) crt_ungetc(int c, crt_FILE *f)
{
	if (!f || c == EOF || lseek(f->_file, -1, SEEK_CUR) < 0) return EOF;
	f->_flag &= ~_IOEOF;
	return (unsigned char)c;
}

CRTFN(int) crt_fputs(const char *s, crt_FILE *f)
{
	size_t n = strlen(s);
	return crt_fwrite(s, 1, n, f) == n ? 0 : EOF;
}

CRTFN(char *) crt_fgets(char *buf, int n, crt_FILE *f)
{
	int i = 0;
	while (i < n - 1) {
		int c = crt_fgetc(f);
		if (c == EOF)
			break;
		buf[i++] = (char)c;
		if (c == '\n')
			break;
	}
	if (!i)
		return NULL;
	buf[i] = 0;
	return buf;
}

CRTFN(int) crt_fflush(crt_FILE *f) { (void)f; return 0; }
CRTFN(int) crt_feof(crt_FILE *f) { return (f->_flag & _IOEOF) != 0; }
CRTFN(int) crt_ferror(crt_FILE *f) { return (f->_flag & _IOERR) != 0; }
CRTFN(void) crt_clearerr(crt_FILE *f) { f->_flag &= ~(_IOEOF | _IOERR); }
CRTFN(long) crt_ftell(crt_FILE *f) { return (long)lseek(f->_file, 0, SEEK_CUR); }
/* A successful seek clears end-of-file, as C requires. */
CRTFN(int) crt_fseek(crt_FILE *f, long off, int whence)
{
	if (lseek(f->_file, off, whence) < 0)
		return -1;
	f->_flag &= ~_IOEOF;
	return 0;
}
CRTFN(int) crt_fgetpos(crt_FILE *f, int64_t *pos)
{
	off_t n = lseek(f->_file, 0, SEEK_CUR);
	if (n < 0)
		return -1;
	*pos = n;
	return 0;
}
CRTFN(int) crt_fsetpos(crt_FILE *f, const int64_t *pos)
{
	if (!pos || lseek(f->_file, *pos, SEEK_SET) < 0)
		return -1;
	f->_flag &= ~_IOEOF;
	return 0;
}
CRTFN(void) crt_rewind(crt_FILE *f) { lseek(f->_file, 0, SEEK_SET); crt_clearerr(f); }
CRTFN(int) crt_setvbuf(crt_FILE *f, char *buf, int mode, size_t size)
{
	(void)f; (void)buf; (void)mode; (void)size;
	return 0;	/* unbuffered either way: every write reaches the fd. */
}

/* Formatting is crt_printf.c's; a wide result is written as ANSI. */
static int print_flags(crt_FILE *f, int wide, unsigned flags, const void *fmt,
		       va_list ap)
{
	void *out;
	size_t len;

	if (crt_vformat(wide, flags, fmt, ap, &out, &len))
		return -1;
	char *text = out;
	size_t n = len;
	if (wide && len) {
		int an = cp_from_utf16(CP_ACP, out, (int)len, NULL, 0, '?', NULL);
		text = an > 0 ? malloc((size_t)an) : NULL;
		if (!text) {
			free(out);
			return -1;
		}
		cp_from_utf16(CP_ACP, out, (int)len, text, an, '?', NULL);
		n = (size_t)an;
	}
	size_t w = crt_fwrite(text, 1, n, f);
	if (text != out)
		free(text);
	free(out);
	return w == n ? (int)len : -1;
}

static int print(crt_FILE *f, int wide, const void *fmt, va_list ap)
{
	return print_flags(f, wide, 0, fmt, ap);
}

CRTFN(int) crt_vfprintf(crt_FILE *f, const char *fmt, va_list ap)
{
	return print(f, 0, fmt, ap);
}

CRTFN(int) crt_fprintf(crt_FILE *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = print(f, 0, fmt, ap);
	va_end(ap);
	return r;
}

CRTFN(int) crt_vprintf(const char *fmt, va_list ap)
{
	return print(&crt_iob[1], 0, fmt, ap);
}

CRTFN(int) crt_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = print(&crt_iob[1], 0, fmt, ap);
	va_end(ap);
	return r;
}

CRTFN(int) crt_vfwprintf(crt_FILE *f, const WCHAR *fmt, va_list ap)
{
	return print(f, 1, fmt, ap);
}

CRTFN(int) crt_fwprintf(crt_FILE *f, const WCHAR *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = print(f, 1, fmt, ap);
	va_end(ap);
	return r;
}

CRTFN(int) crt_vwprintf(const WCHAR *fmt, va_list ap)
{
	return print(&crt_iob[1], 1, fmt, ap);
}

CRTFN(int) crt_wprintf(const WCHAR *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = print(&crt_iob[1], 1, fmt, ap);
	va_end(ap);
	return r;
}

CRTFN(int) crt_puts(const char *s)
{
	return crt_fputs(s, &crt_iob[1]) == 0 && crt_fputc('\n', &crt_iob[1]) != EOF ? 0 : EOF;
}

CRTFN(crt_FILE *) crt___iob_func(void) { return crt_iob; }
/* ucrtbase's stdin, stdout and stderr: its FILE is opaque, so ours do. */
CRTFN(crt_FILE *) crt___acrt_iob_func(unsigned i) { return i < 3 ? &crt_iob[i] : NULL; }

/* ucrtbase's fprintf family (see crt_printf.c for its options). */
CRTFN(int) crt___stdio_common_vfprintf(ULONGLONG options, crt_FILE *f,
					const char *fmt, void *locale, va_list ap)
{
	(void)locale;
	return print_flags(f, 0, CRT_UCRT | (unsigned)(options & CRT_PRINTF_OPTIONS),
			   fmt, ap);
}
CRTFN(int) crt___stdio_common_vfwprintf(ULONGLONG options, crt_FILE *f,
					 const WCHAR *fmt, void *locale, va_list ap)
{
	(void)locale;
	return print_flags(f, 1, CRT_UCRT | (unsigned)(options & CRT_PRINTF_OPTIONS),
			   fmt, ap);
}

CRTFN(int) crt__fileno(crt_FILE *f) { return f->_file; }

/*
 * The classic ctype table, at _pctype + c for c in [-1, 255] (-1 is EOF).
 * MSVC's flag bits (<ctype.h>): computed from the host's own <ctype.h> in
 * the "C" locale, which is all a codec doing plain ASCII classification
 * needs.
 */
#define _UPPER  0x0001
#define _LOWER  0x0002
#define _DIGIT  0x0004
#define _SPACE  0x0008
#define _PUNCT  0x0010
#define _CONTROL 0x0020
#define _BLANK  0x0040
#define _HEX    0x0080
#define _ALPHA  0x0100

static unsigned short ctype_table[257];	/* [0] is EOF's entry */
static const unsigned short *pctype_ptr = ctype_table + 1;
static int mb_cur_max = 1;
static unsigned lc_codepage;	/* the "C" locale has no code page */

static void ctype_init(void) __attribute__((constructor));
static void ctype_init(void)
{
	for (int c = -1; c <= 255; c++) {
		unsigned short f = 0;
		if (c >= 0) {
			if (isupper(c)) f |= _UPPER | _ALPHA;
			if (islower(c)) f |= _LOWER | _ALPHA;
			if (isdigit(c)) f |= _DIGIT | _HEX;
			if (isspace(c)) f |= _SPACE;
			if (ispunct(c)) f |= _PUNCT;
			if (iscntrl(c)) f |= _CONTROL;
			if (c == ' ') f |= _BLANK;	/* MSVC's table: not tab */
			if (isxdigit(c)) f |= _HEX;
		}
		ctype_table[c + 1] = f;
	}
}

/*
 * <ctype.h>: real functions, not just the _pctype table - mingw-w64 (and
 * MSVC itself, in a release build without the header's own inline
 * fast-path expanding at the call site) compiles these as calls. The
 * classification bits mean the same thing as the table's; using it here
 * too keeps both paths agreeing with each other.
 */
CRTFN(int) crt_isalpha(int c) { return ctype_table[c + 1] & _ALPHA; }
CRTFN(int) crt_isupper(int c) { return ctype_table[c + 1] & _UPPER; }
CRTFN(int) crt_islower(int c) { return ctype_table[c + 1] & _LOWER; }
CRTFN(int) crt_isdigit(int c) { return ctype_table[c + 1] & _DIGIT; }
CRTFN(int) crt_isxdigit(int c) { return ctype_table[c + 1] & _HEX; }
CRTFN(int) crt_isspace(int c) { return ctype_table[c + 1] & _SPACE; }
CRTFN(int) crt_ispunct(int c) { return ctype_table[c + 1] & _PUNCT; }
CRTFN(int) crt_iscntrl(int c) { return ctype_table[c + 1] & _CONTROL; }
CRTFN(int) crt_isalnum(int c) { return ctype_table[c + 1] & (_ALPHA | _DIGIT); }
/* As MSVC's <ctype.h> defines them; high bytes have no class in "C". */
CRTFN(int) crt_isprint(int c)
{
	return c >= -1 && c <= 255 && (ctype_table[c + 1] & (_BLANK | _PUNCT | _ALPHA | _DIGIT));
}
CRTFN(int) crt_isgraph(int c)
{
	return c >= -1 && c <= 255 && (ctype_table[c + 1] & (_PUNCT | _ALPHA | _DIGIT));
}
CRTFN(int) crt_toupper(int c) { return crt_islower(c) ? c - 'a' + 'A' : c; }
CRTFN(int) crt_tolower(int c) { return crt_isupper(c) ? c - 'A' + 'a' : c; }
CRTFN(int) crt__ctype(int c)
{
	return c >= -1 && c <= 255 ? ctype_table[c + 1] : 0;
}

CRTFN(crt_FILE *) crt___p__iob(void) { return crt_iob; }
CRTFN(int *) crt___p___mb_cur_max(void) { return &mb_cur_max; }

#define E(name) { #name, 0, (void *)crt_##name }

const struct builtin_export crt_stdio_exports[] = {
	{ "_iob", 0, crt_iob },
	{ "_pctype", 0, (void *)&pctype_ptr },
	{ "__mb_cur_max", 0, &mb_cur_max },
	{ "__lc_codepage", 0, &lc_codepage },
	E(fopen), E(_wfopen), E(fclose), E(fread), E(fwrite), E(fputc), E(fgetc),
	E(_fdopen), E(_filbuf), E(_flsbuf), E(ungetc), E(fgetpos), E(fsetpos),
	E(fputs), E(fgets), E(fflush), E(feof), E(ferror), E(clearerr),
	E(ftell), E(fseek), E(rewind), E(setvbuf), E(remove),
	E(fprintf), E(vfprintf), E(printf), E(vprintf), E(puts),
	E(fwprintf), E(vfwprintf), E(wprintf), E(vwprintf),
	E(__stdio_common_vfprintf), E(__stdio_common_vfwprintf),
	{ "__stdio_common_vfprintf_s", 0, (void *)crt___stdio_common_vfprintf },
	{ "__stdio_common_vfwprintf_s", 0, (void *)crt___stdio_common_vfwprintf },
	E(__iob_func), E(__acrt_iob_func), E(__p__iob), E(__p___mb_cur_max), E(_fileno), E(getc),
	E(isalpha), E(isupper), E(islower), E(isdigit), E(isxdigit),
	E(isspace), E(ispunct), E(iscntrl), E(isalnum), E(isprint),
	E(isgraph), E(toupper), E(tolower),
	E(_ctype),
	BUILTIN_END
};
