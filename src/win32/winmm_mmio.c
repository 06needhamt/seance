/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * winmm's multimedia file I/O (ravi32.dll reads and writes its files this
 * way): mmioOpen over a host file (FOURCC_DOS), a memory block (FOURCC_MEM)
 * or an application's I/O procedure, and the RIFF chunk walking on top.
 *
 * A file is unbuffered unless it is opened with MMIO_ALLOCBUF, given a
 * buffer of the caller's (pchBuffer and cchBuffer) or given one with
 * mmioSetBuffer. Through a buffer, reads and writes go to it and the I/O
 * procedure sees whole buffers, and the caller may fill or drain it itself
 * with mmioGetInfo, mmioAdvance and mmioSetInfo. A FOURCC_MEM file is
 * its memory block and stays unbuffered.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/builtin.h"
#include "runtime/gate.h"
#include "win32/codepage.h"
#include "win32/handle.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

struct mmio {
	MMIOINFO info;
	int fd;			/* FOURCC_DOS */
	HANDLE borrowed;	/* the handle an MMIO_FHOPEN-style open was given */
	LONG mem_size;		/* FOURCC_MEM: bytes in pchBuffer */
	int mem_owned;		/* FOURCC_MEM buffer allocated here */
	char *own_buf;		/* an I/O buffer allocated here */
};

/* Installed I/O procedures (mmioInstallIOProc); the latest installed for a
 * FOURCC is the one found, and the first removed. */
#define MAX_PROCS 16
static struct {
	FOURCC fcc;
	LPMMIOPROC proc;
	unsigned seq;		/* installation order; 0 for a free slot */
} procs[MAX_PROCS];
static unsigned next_seq = 1;
static pthread_mutex_t procs_lock = PTHREAD_MUTEX_INITIALIZER;

/* The slot of the latest procedure for fcc (and proc, if given), or -1;
 * with procs_lock held. */
static int latest_proc(FOURCC fcc, LPMMIOPROC proc)
{
	int best = -1;

	for (int i = 0; i < MAX_PROCS; i++)
		if (procs[i].seq && procs[i].fcc == fcc &&
		    (!proc || procs[i].proc == proc) &&
		    (best < 0 || procs[i].seq > procs[best].seq))
			best = i;
	return best;
}

static LPMMIOPROC find_proc(FOURCC fcc)
{
	pthread_mutex_lock(&procs_lock);
	int i = latest_proc(fcc, NULL);
	LPMMIOPROC p = i >= 0 ? procs[i].proc : NULL;
	pthread_mutex_unlock(&procs_lock);
	return p;
}

static int open_flags(DWORD flags)
{
	int o = O_CLOEXEC;

	switch (flags & MMIO_RWMODE) {
	case MMIO_WRITE:     o |= O_WRONLY; break;
	case MMIO_READWRITE: o |= O_RDWR; break;
	default:             o |= O_RDONLY; break;
	}
	if (flags & MMIO_CREATE)
		o |= O_CREAT | O_TRUNC;
	return o;
}

/* The host path for an ANSI Windows name (malloc()ed), or NULL. */
static char *host_path(const char *name)
{
	char *u = acp_to_utf8(name), *p;

	if (!u)
		return NULL;
	p = path_to_unix(u);
	free(u);
	return p;
}

/* The built-in FOURCC_DOS and FOURCC_MEM procedures. */
static LRESULT builtin_proc(struct mmio *m, UINT msg, LPARAM l1, LPARAM l2)
{
	MMIOINFO *mi = &m->info;
	int mem = mi->fccIOProc == FOURCC_MEM;

	switch (msg) {
	case MMIOM_OPEN:
		if (mem) {
			mi->lDiskOffset = 0;
			return 0;
		} else {
			if (!l1 && m->borrowed) {
				/* No name: the file handle in adwInfo[0], used
				 * from where it is. */
				m->fd = file_handle_fd(m->borrowed);
				off_t at = m->fd < 0 ? -1 : lseek(m->fd, 0, SEEK_CUR);
				if (at < 0)
					return MMIOERR_CANNOTOPEN;
				mi->lDiskOffset = (LONG)at;
				return 0;
			}
			char *path = host_path((const char *)l1);
			if (!path)
				return MMIOERR_FILENOTFOUND;
			m->fd = open(path, open_flags(mi->dwFlags), 0644);
			free(path);
			if (m->fd < 0)
				return errno == ENOENT ? MMIOERR_FILENOTFOUND :
							 MMIOERR_CANNOTOPEN;
			mi->lDiskOffset = 0;
			return 0;
		}
	case MMIOM_CLOSE:
		if (m->borrowed) {
			/* The handle is the caller's to keep with MMIO_FHOPEN;
			 * otherwise closing the file closes it. */
			if (!(l1 & MMIO_FHOPEN) && !handle_close(m->borrowed))
				return MMIOERR_CANNOTCLOSE;
		} else if (!mem && m->fd >= 0 && close(m->fd)) {
			return MMIOERR_CANNOTCLOSE;
		}
		m->fd = -1;
		return 0;
	case MMIOM_READ: {
		LONG n = (LONG)l2;
		if (n < 0)
			return -1;
		if (mem) {
			if (n > m->mem_size - mi->lDiskOffset)
				n = m->mem_size - mi->lDiskOffset;
			if (n < 0)
				n = 0;
			memcpy((void *)l1, mi->pchBuffer + mi->lDiskOffset, n);
		} else {
			ssize_t r = read(m->fd, (void *)l1, (size_t)n);
			if (r < 0)
				return -1;
			n = (LONG)r;
		}
		mi->lDiskOffset += n;
		return n;
	}
	case MMIOM_WRITE:
	case MMIOM_WRITEFLUSH: {
		LONG n = (LONG)l2;
		if (n < 0)
			return -1;
		if (mem) {
			LONG end = mi->lDiskOffset + n;
			if (end > mi->cchBuffer) {
				/* Grows only a block allocated here, by
				 * the increment mmioOpen was given. */
				LONG inc = (LONG)mi->adwInfo[0];
				if (!m->mem_owned || !inc)
					return -1;
				LONG size = mi->cchBuffer;
				while (size < end)
					size += inc;
				char *b = realloc(mi->pchBuffer, size);
				if (!b)
					return -1;
				memset(b + mi->cchBuffer, 0, size - mi->cchBuffer);
				mi->pchBuffer = b;
				mi->cchBuffer = size;
			}
			memcpy(mi->pchBuffer + mi->lDiskOffset, (void *)l1, n);
			if (end > m->mem_size)
				m->mem_size = end;
		} else {
			ssize_t r = write(m->fd, (const void *)l1, (size_t)n);
			if (r < 0)
				return -1;
			n = (LONG)r;
		}
		mi->lDiskOffset += n;
		return n;
	}
	case MMIOM_SEEK: {
		LONG off = (LONG)l1, pos;
		int whence = (int)l2;
		if (mem) {
			pos = whence == SEEK_SET ? off :
			      whence == SEEK_CUR ? mi->lDiskOffset + off :
			      whence == SEEK_END ? m->mem_size + off : -1;
			if (pos < 0 || pos > mi->cchBuffer)
				return -1;
		} else {
			off_t r = lseek(m->fd, off, whence);
			if (r < 0)
				return -1;
			pos = (LONG)r;
		}
		mi->lDiskOffset = pos;
		return pos;
	}
	case MMIOM_RENAME: {
		char *a = host_path((const char *)l1), *b = host_path((const char *)l2);
		int r = a && b && !rename(a, b);
		free(a);
		free(b);
		return r ? 0 : MMIOERR_FILENOTFOUND;
	}
	}
	return -1;
}

static LRESULT send_msg(struct mmio *m, UINT msg, LPARAM l1, LPARAM l2)
{
	if (!m->info.pIOProc)
		return builtin_proc(m, msg, l1, l2);
	uintptr_t args[4] = { (uintptr_t)&m->info, msg, (uintptr_t)l1,
			      (uintptr_t)l2 };
	return (LRESULT)gate_call((const void *)m->info.pIOProc, 4, args);
}

static struct mmio *mmio_of(HMMIO h)
{
	return (struct mmio *)h;
}

/* Buffered I/O. info.lBufOffset is the file offset of pchBuffer; the data
 * there runs to pchEndRead, and pchNext is the current position. */

static int buffered(const struct mmio *m)
{
	return m->info.pchBuffer && m->info.cchBuffer > 0 &&
	       !(m->info.fccIOProc == FOURCC_MEM && !m->info.pIOProc);
}

static LONG io_tell(const struct mmio *m)
{
	return buffered(m) ? m->info.lBufOffset +
			     (LONG)(m->info.pchNext - m->info.pchBuffer) :
			     m->info.lDiskOffset;
}

/* Empties the buffer, to start at the file offset at. */
static void buffer_reset(struct mmio *m, LONG at)
{
	m->info.lBufOffset = at;
	m->info.pchNext = m->info.pchEndRead = m->info.pchBuffer;
	m->info.pchEndWrite = m->info.pchBuffer + m->info.cchBuffer;
}

/* Writes the buffer out if it was written to; 0, or an MMIOERR. */
static MMRESULT buffer_flush(struct mmio *m)
{
	MMIOINFO *mi = &m->info;

	if (!buffered(m) || !(mi->dwFlags & MMIO_DIRTY))
		return 0;
	HPSTR end = mi->pchNext > mi->pchEndRead ? mi->pchNext : mi->pchEndRead;
	LONG n = (LONG)(end - mi->pchBuffer);
	if (n > 0) {
		if (mi->lDiskOffset != mi->lBufOffset &&
		    send_msg(m, MMIOM_SEEK, mi->lBufOffset, SEEK_SET) == -1)
			return MMIOERR_CANNOTSEEK;
		if (send_msg(m, MMIOM_WRITE, (LPARAM)mi->pchBuffer, n) != n)
			return MMIOERR_CANNOTWRITE;
	}
	mi->pchEndRead = end;
	mi->dwFlags &= ~MMIO_DIRTY;
	return 0;
}

/* Moves the buffer to the current position, filled from the file if
 * fill; 0, or an MMIOERR. */
static MMRESULT buffer_next(struct mmio *m, int fill)
{
	MMIOINFO *mi = &m->info;
	MMRESULT r = buffer_flush(m);

	if (r)
		return r;
	LONG at = io_tell(m);
	buffer_reset(m, at);
	if (!fill)
		return 0;
	if (mi->lDiskOffset != at &&
	    send_msg(m, MMIOM_SEEK, at, SEEK_SET) == -1)
		return MMIOERR_CANNOTSEEK;
	LONG n = (LONG)send_msg(m, MMIOM_READ, (LPARAM)mi->pchBuffer,
				mi->cchBuffer);
	if (n < 0)
		return MMIOERR_CANNOTREAD;
	mi->pchEndRead = mi->pchBuffer + n;
	return 0;
}

static LONG io_read(struct mmio *m, void *buf, LONG n)
{
	MMIOINFO *mi = &m->info;
	LONG done = 0;

	if (n < 0)
		return -1;
	if (!buffered(m))
		return (LONG)send_msg(m, MMIOM_READ, (LPARAM)buf, n);
	while (done < n) {
		LONG avail = (LONG)(mi->pchEndRead - mi->pchNext);
		if (avail <= 0) {
			if (buffer_next(m, 1))
				return done ? done : -1;
			if (!(avail = (LONG)(mi->pchEndRead - mi->pchNext)))
				break;		/* the end of the file */
		}
		if (avail > n - done)
			avail = n - done;
		memcpy((char *)buf + done, mi->pchNext, avail);
		mi->pchNext += avail;
		done += avail;
	}
	return done;
}

static LONG io_write(struct mmio *m, const void *buf, LONG n)
{
	MMIOINFO *mi = &m->info;
	LONG done = 0;

	if (n < 0)
		return -1;
	if (!buffered(m)) {
		LONG r = (LONG)send_msg(m, MMIOM_WRITE, (LPARAM)buf, n);
		if (r > 0)
			mi->dwFlags |= MMIO_DIRTY;
		return r;
	}
	while (done < n) {
		LONG room = (LONG)(mi->pchEndWrite - mi->pchNext);
		if (room <= 0) {
			if (buffer_next(m, 0))
				return done ? done : -1;
			room = (LONG)(mi->pchEndWrite - mi->pchNext);
		}
		if (room > n - done)
			room = n - done;
		memcpy(mi->pchNext, (const char *)buf + done, room);
		mi->pchNext += room;
		if (mi->pchNext > mi->pchEndRead)
			mi->pchEndRead = mi->pchNext;
		mi->dwFlags |= MMIO_DIRTY;
		done += room;
	}
	return done;
}

static LONG io_seek(struct mmio *m, LONG off, int whence)
{
	MMIOINFO *mi = &m->info;

	if (!buffered(m))
		return (LONG)send_msg(m, MMIOM_SEEK, off, whence);
	LONG to;
	if (whence == SEEK_END) {
		if (buffer_flush(m))
			return -1;
		to = (LONG)send_msg(m, MMIOM_SEEK, off, SEEK_END);
		if (to != -1)
			buffer_reset(m, to);
		return to;
	}
	to = whence == SEEK_SET ? off : whence == SEEK_CUR ? io_tell(m) + off : -1;
	if (to < 0)
		return -1;
	/* Within what the buffer holds: just move in it. */
	if (to >= mi->lBufOffset &&
	    to <= mi->lBufOffset + (LONG)(mi->pchEndRead - mi->pchBuffer)) {
		mi->pchNext = mi->pchBuffer + (to - mi->lBufOffset);
		return to;
	}
	if (buffer_flush(m) || send_msg(m, MMIOM_SEEK, to, SEEK_SET) == -1)
		return -1;
	buffer_reset(m, to);
	return to;
}

/* Gives m a buffer of size bytes (buf, or one of its own if NULL), or none
 * for 0, keeping the position; 0, or an MMIOERR. */
static MMRESULT set_buffer(struct mmio *m, LPSTR buf, LONG size)
{
	MMRESULT r = buffer_flush(m);
	LONG at = io_tell(m);
	char *own = NULL;

	if (r)
		return r;
	if (size < 0)
		return MMSYSERR_INVALPARAM;
	if (size && !buf && !(buf = own = malloc(size)))
		return MMIOERR_OUTOFMEMORY;
	/* The procedure's own position is where the buffer leaves off. */
	if (buffered(m) && m->info.lDiskOffset != at &&
	    send_msg(m, MMIOM_SEEK, at, SEEK_SET) == -1) {
		free(own);
		return MMIOERR_CANNOTSEEK;
	}
	free(m->own_buf);
	m->own_buf = own;
	m->info.pchBuffer = size ? buf : NULL;
	m->info.cchBuffer = size;
	if (size)
		buffer_reset(m, at);
	else
		m->info.pchNext = m->info.pchEndRead = m->info.pchEndWrite = NULL;
	return 0;
}

/*
 * The procedure a file name asks for: "x.ABC+y" means ABC's installed
 * procedure, as Windows parses compound names; otherwise FOURCC_DOS.
 */
static FOURCC name_fourcc(const char *name)
{
	const char *plus = name ? strchr(name, '+') : NULL;
	if (!plus)
		return FOURCC_DOS;
	const char *dot = plus;
	while (dot > name && dot[-1] != '.')
		dot--;
	if (dot == name)
		return FOURCC_DOS;
	char ext[4] = { ' ', ' ', ' ', ' ' };
	for (int i = 0; i < 4 && dot + i < plus; i++)
		ext[i] = (char)toupper((unsigned char)dot[i]);
	FOURCC f = mmioFOURCC(ext[0], ext[1], ext[2], ext[3]);
	return find_proc(f) ? f : FOURCC_DOS;
}

/* name as a full Windows path, into name itself (128 bytes); 0 or -1. */
static int qualify(LPSTR name)
{
	char *u = acp_to_utf8(name ? name : ""), *full;

	full = u ? k32_full_path(u) : NULL;
	free(u);
	if (!full)
		return -1;
	char *a = utf8_to_acp_dup(full);
	free(full);
	if (!a)
		return -1;
	snprintf(name, 128, "%s", a);
	free(a);
	return 0;
}

/*
 * MMIO_GETTEMP: a new temporary file's name, into name (128 bytes). It may
 * name a drive and a prefix, as "C:F"; with neither, the temporary
 * directory and "MMI". 0 or -1.
 */
static int temp_name(LPSTR name)
{
	const struct builtin_dll *k = builtin_find("kernel32.dll");
	DWORD (WINAPI *temp_path)(DWORD, LPSTR) =
		k ? builtin_export(k, "GetTempPathA") : NULL;
	UINT (WINAPI *temp_file)(LPCSTR, LPCSTR, UINT, LPSTR) =
		k ? builtin_export(k, "GetTempFileNameA") : NULL;
	char dir[MAX_PATH], prefix[4] = "MMI", out[MAX_PATH];
	const char *p = name ? name : "";

	if (!temp_path || !temp_file)
		return -1;
	if (p[0] && p[1] == ':') {
		snprintf(dir, sizeof(dir), "%c:\\", p[0]);
		p += 2;
	} else if (!temp_path(sizeof(dir), dir)) {
		return -1;
	}
	if (*p)
		snprintf(prefix, sizeof(prefix), "%s", p);
	size_t n;
	if (!temp_file(dir, prefix, 0, out) || (n = strlen(out)) >= 128)
		return -1;
	memcpy(name, out, n + 1);
	return 0;
}

static HMMIO mmio_open(LPSTR name, LPMMIOINFO user, DWORD flags)
{
	if (user)
		user->wErrorRet = 0;
	if (flags & MMIO_PARSE) {
		if (qualify(name)) {
			if (user)
				user->wErrorRet = MMIOERR_FILENOTFOUND;
			return NULL;
		}
		return (HMMIO)TRUE;
	}
	if (flags & MMIO_GETTEMP) {
		/* Only the name is made; as documented, success is
		 * MMSYSERR_NOERROR cast to a handle. */
		int bad = !name || temp_name(name);
		if (user)
			user->wErrorRet = bad ? MMIOERR_FILENOTFOUND : 0;
		return (HMMIO)(ULONG_PTR)(bad ? MMIOERR_FILENOTFOUND :
					  MMSYSERR_NOERROR);
	}
	if (flags & (MMIO_EXIST | MMIO_DELETE)) {
		char *path = name ? host_path(name) : NULL;
		struct stat st;
		int ok = path && !stat(path, &st);
		if (ok && (flags & MMIO_DELETE))
			ok = !unlink(path);
		free(path);
		/* MMIO_EXIST gives back the file's full name. */
		if (ok && (flags & MMIO_EXIST) && !(flags & MMIO_DELETE))
			qualify(name);
		if (!ok && user)
			user->wErrorRet = MMIOERR_FILENOTFOUND;
		return ok ? (HMMIO)TRUE : NULL;
	}

	struct mmio *m = calloc(1, sizeof(*m));
	if (!m) {
		if (user)
			user->wErrorRet = MMIOERR_OUTOFMEMORY;
		return NULL;
	}
	m->fd = -1;
	if (user) {
		m->info.fccIOProc = user->fccIOProc;
		m->info.pIOProc = user->pIOProc;
		m->info.cchBuffer = user->cchBuffer;
		m->info.pchBuffer = user->pchBuffer;
		memcpy(m->info.adwInfo, user->adwInfo, sizeof(m->info.adwInfo));
	}
	if (m->info.pIOProc)
		m->info.fccIOProc = 0;
	else if (!m->info.fccIOProc)
		m->info.fccIOProc = name_fourcc(name);
	/* An installed procedure comes first, even for DOS and MEM. */
	if (!m->info.pIOProc)
		m->info.pIOProc = find_proc(m->info.fccIOProc);
	if (!m->info.pIOProc && m->info.fccIOProc != FOURCC_DOS &&
	    m->info.fccIOProc != FOURCC_MEM) {
		free(m);
		if (user)
			user->wErrorRet = MMIOERR_CANNOTOPEN;
		return NULL;
	}
	if (!name && !m->info.pIOProc && m->info.fccIOProc == FOURCC_DOS)
		m->borrowed = (HANDLE)(ULONG_PTR)m->info.adwInfo[0];
	m->info.dwFlags = flags & ~(MMIO_ALLOCBUF | MMIO_GETTEMP);
	m->info.hmmio = (HMMIO)m;
	int mem = m->info.fccIOProc == FOURCC_MEM && !m->info.pIOProc;
	LPSTR io_buf = NULL;
	LONG io_size = 0;
	if (mem) {
		if (!m->info.pchBuffer) {
			/* A block of our own, growable by adwInfo[0]. */
			LONG size = m->info.cchBuffer > 0 ? m->info.cchBuffer : 0;
			m->info.pchBuffer = calloc(1, size ? size : 1);
			m->info.cchBuffer = size;
			m->mem_owned = 1;
			if (!m->info.pchBuffer) {
				free(m);
				if (user)
					user->wErrorRet = MMIOERR_OUTOFMEMORY;
				return NULL;
			}
		}
		/* A block opened for reading holds data already. */
		m->mem_size = (flags & MMIO_CREATE) || m->mem_owned ?
			      0 : m->info.cchBuffer;
	} else {
		/* The I/O buffer: the caller's, or one of ours. */
		if (m->info.pchBuffer && m->info.cchBuffer > 0) {
			io_buf = m->info.pchBuffer;
			io_size = m->info.cchBuffer;
		} else if (flags & MMIO_ALLOCBUF) {
			io_size = m->info.cchBuffer > 0 ? m->info.cchBuffer :
							  MMIO_DEFAULTBUFFER;
		}
		m->info.pchBuffer = NULL;
		m->info.cchBuffer = 0;
	}

	LRESULT r = send_msg(m, MMIOM_OPEN, (LPARAM)name, 0);
	if (!r && io_size && set_buffer(m, io_buf, io_size)) {
		send_msg(m, MMIOM_CLOSE, MMIO_FHOPEN, 0);
		r = MMIOERR_OUTOFMEMORY;
	}
	if (r) {
		if (m->mem_owned)
			free(m->info.pchBuffer);
		free(m->own_buf);
		free(m);
		if (user)
			user->wErrorRet = (UINT)r;
		return NULL;
	}
	return (HMMIO)m;
}

WINFN(HMMIO) mm_mmioOpenA(LPSTR name, LPMMIOINFO info, DWORD flags)
{
	return mmio_open(name, info, flags);
}

WINFN(HMMIO) mm_mmioOpenW(LPWSTR name, LPMMIOINFO info, DWORD flags)
{
	char buf[MAX_PATH] = "";

	if (name) {
		char *u = utf16_to_utf8_dup(name), *a = u ? utf8_to_acp_dup(u) : NULL;
		snprintf(buf, sizeof(buf), "%s", a ? a : "");
		free(a);
		free(u);
	}
	HMMIO h = mmio_open(name ? buf : NULL, info, flags);
	/* The forms that give a name back: szFilename is 128 characters. */
	int named = (flags & (MMIO_PARSE | MMIO_EXIST)) ? h != NULL :
		    (flags & MMIO_GETTEMP) ? h == (HMMIO)MMSYSERR_NOERROR : 0;
	if (named && name && !(flags & MMIO_DELETE)) {
		int n = cp_to_utf16(CP_ACP, buf, -1, name, 128, 0);
		if (n <= 0)
			name[0] = 0;
	}
	return h;
}

WINFN(MMRESULT) mm_mmioClose(HMMIO h, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return MMSYSERR_INVALHANDLE;
	MMRESULT f = buffer_flush(m);
	MMRESULT r = (MMRESULT)send_msg(m, MMIOM_CLOSE, flags, 0);
	if (m->mem_owned)
		free(m->info.pchBuffer);
	free(m->own_buf);
	free(m);
	return r ? r : f;
}

WINFN(LONG) mm_mmioRead(HMMIO h, HPSTR buf, LONG n)
{
	struct mmio *m = mmio_of(h);

	return m ? io_read(m, buf, n) : -1;
}

WINFN(LONG) mm_mmioWrite(HMMIO h, const char *buf, LONG n)
{
	struct mmio *m = mmio_of(h);

	return m ? io_write(m, buf, n) : -1;
}

WINFN(LONG) mm_mmioSeek(HMMIO h, LONG off, int origin)
{
	struct mmio *m = mmio_of(h);

	return m ? io_seek(m, off, origin) : -1;
}

WINFN(MMRESULT) mm_mmioFlush(HMMIO h, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return MMSYSERR_INVALHANDLE;
	MMRESULT r = buffer_flush(m);
	if (r)
		return r;
	if (buffered(m) && (flags & MMIO_EMPTYBUF))
		buffer_reset(m, io_tell(m));
	m->info.dwFlags &= ~MMIO_DIRTY;
	if (!m->info.pIOProc && m->info.fccIOProc == FOURCC_DOS && m->fd >= 0)
		fsync(m->fd);
	return 0;
}

WINFN(MMRESULT) mm_mmioGetInfo(HMMIO h, LPMMIOINFO info, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m || !info)
		return MMSYSERR_INVALHANDLE;
	*info = m->info;
	if (!buffered(m)) {
		/* No buffer the caller may fill directly. */
		info->cchBuffer = 0;
		info->pchBuffer = info->pchNext = info->pchEndRead =
			info->pchEndWrite = NULL;
		info->lBufOffset = info->lDiskOffset;
	}
	return 0;
}

/* Takes back what the caller did to the buffer: its position, how much it
 * holds, and whether the caller wrote to it. */
static void take_buffer_state(struct mmio *m, const MMIOINFO *info)
{
	MMIOINFO *mi = &m->info;

	if (!buffered(m) || info->pchBuffer != mi->pchBuffer)
		return;
	if (info->pchNext >= mi->pchBuffer && info->pchNext <= mi->pchEndWrite)
		mi->pchNext = info->pchNext;
	if (info->pchEndRead >= mi->pchBuffer &&
	    info->pchEndRead <= mi->pchEndWrite)
		mi->pchEndRead = info->pchEndRead;
	if (info->dwFlags & MMIO_DIRTY)
		mi->dwFlags |= MMIO_DIRTY;
}

WINFN(MMRESULT) mm_mmioSetInfo(HMMIO h, const MMIOINFO *info, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m || !info)
		return MMSYSERR_INVALHANDLE;
	take_buffer_state(m, info);
	memcpy(m->info.adwInfo, info->adwInfo, sizeof(m->info.adwInfo));
	return 0;
}

WINFN(MMRESULT) mm_mmioSetBuffer(HMMIO h, LPSTR buf, LONG size, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return MMSYSERR_INVALHANDLE;
	if (m->info.fccIOProc == FOURCC_MEM && !m->info.pIOProc)
		return 0;	/* a memory file is its block */
	return set_buffer(m, buf, size);
}

/*
 * The caller has filled (MMIO_WRITE) or drained (MMIO_READ) the buffer up to
 * pchNext: write it out if so, and move it on, read full again for reading.
 */
WINFN(MMRESULT) mm_mmioAdvance(HMMIO h, LPMMIOINFO info, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return MMSYSERR_INVALHANDLE;
	if (!buffered(m))
		return MMIOERR_UNBUFFERED;
	if (info)
		take_buffer_state(m, info);
	if ((flags & MMIO_WRITE) && m->info.pchNext > m->info.pchBuffer)
		m->info.dwFlags |= MMIO_DIRTY;
	MMRESULT r = buffer_next(m, !(flags & MMIO_WRITE));
	if (r == MMIOERR_CANNOTWRITE && !(flags & MMIO_WRITE))
		r = MMIOERR_CANNOTREAD;
	if (info)
		*info = m->info;
	return r;
}

WINFN(LRESULT) mm_mmioSendMessage(HMMIO h, UINT msg, LPARAM l1, LPARAM l2)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return MMSYSERR_INVALHANDLE;
	if (msg < MMIOM_USER)
		return MMSYSERR_INVALPARAM;
	return send_msg(m, msg, l1, l2);
}

WINFN(MMRESULT) mm_mmioRenameA(LPCSTR from, LPCSTR to, const MMIOINFO *info,
			       DWORD flags)
{
	struct mmio m = { .fd = -1 };

	if (info) {
		m.info.fccIOProc = info->fccIOProc;
		m.info.pIOProc = info->pIOProc;
	}
	if (!m.info.pIOProc && m.info.fccIOProc && m.info.fccIOProc != FOURCC_DOS)
		m.info.pIOProc = find_proc(m.info.fccIOProc);
	if (!m.info.fccIOProc && !m.info.pIOProc)
		m.info.fccIOProc = FOURCC_DOS;
	return (MMRESULT)send_msg(&m, MMIOM_RENAME, (LPARAM)from, (LPARAM)to);
}

WINFN(MMRESULT) mm_mmioRenameW(LPCWSTR from, LPCWSTR to, const MMIOINFO *info,
			       DWORD flags)
{
	char *uf = utf16_to_utf8_dup(from), *ut = utf16_to_utf8_dup(to);
	char *af = uf ? utf8_to_acp_dup(uf) : NULL;
	char *at = ut ? utf8_to_acp_dup(ut) : NULL;
	MMRESULT r = af && at ? mm_mmioRenameA(af, at, info, flags) :
				MMIOERR_FILENOTFOUND;
	free(uf);
	free(ut);
	free(af);
	free(at);
	return r;
}

WINFN(LPMMIOPROC) mm_mmioInstallIOProcA(FOURCC fcc, LPMMIOPROC proc,
					DWORD flags)
{
	LPMMIOPROC r = NULL;
	int i;

	pthread_mutex_lock(&procs_lock);
	if (flags & MMIO_INSTALLPROC) {
		for (i = 0; i < MAX_PROCS && procs[i].seq; i++)
			;
		if (i < MAX_PROCS && proc) {
			procs[i].fcc = fcc;
			procs[i].proc = proc;
			procs[i].seq = next_seq++;
			r = proc;
		}
	} else if (flags & MMIO_REMOVEPROC) {
		if ((i = latest_proc(fcc, proc)) >= 0) {
			r = procs[i].proc;
			procs[i].seq = 0;
			procs[i].proc = NULL;
		}
	} else if (flags & MMIO_FINDPROC) {
		if ((i = latest_proc(fcc, NULL)) >= 0)
			r = procs[i].proc;
	}
	pthread_mutex_unlock(&procs_lock);
	return r;
}

WINFN(LPMMIOPROC) mm_mmioInstallIOProcW(FOURCC fcc, LPMMIOPROC proc,
					DWORD flags)
{
	return mm_mmioInstallIOProcA(fcc, proc, flags);
}

static FOURCC string_fourcc(const char *s, UINT flags)
{
	char c[4] = { ' ', ' ', ' ', ' ' };

	for (int i = 0; i < 4 && s && s[i]; i++)
		c[i] = (flags & MMIO_TOUPPER) ?
		       (char)toupper((unsigned char)s[i]) : s[i];
	return mmioFOURCC(c[0], c[1], c[2], c[3]);
}

WINFN(FOURCC) mm_mmioStringToFOURCCA(LPCSTR s, UINT flags)
{
	return string_fourcc(s, flags);
}

WINFN(FOURCC) mm_mmioStringToFOURCCW(LPCWSTR s, UINT flags)
{
	char a[5] = "";

	for (int i = 0; i < 4 && s && s[i]; i++)
		a[i] = s[i] < 0x80 ? (char)s[i] : '?';
	return string_fourcc(a, flags);
}

/* The 8-byte chunk header at the current position, and a list's type. */
static int read_header(struct mmio *m, MMCKINFO *ck)
{
	DWORD hdr[2];

	if (io_read(m, hdr, 8) != 8)
		return 0;
	ck->ckid = hdr[0];
	ck->cksize = hdr[1];
	ck->fccType = 0;
	ck->dwDataOffset = (DWORD)io_tell(m);
	ck->dwFlags = 0;
	if (ck->ckid == FOURCC_RIFF || ck->ckid == FOURCC_LIST) {
		if (io_read(m, &ck->fccType, 4) != 4)
			return 0;
	}
	return 1;
}

WINFN(MMRESULT) mm_mmioDescend(HMMIO h, LPMMCKINFO ck,
			       const MMCKINFO *parent, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return MMSYSERR_INVALHANDLE;
	if (!ck)
		return MMSYSERR_INVALPARAM;
	FOURCC want_id = ck->ckid, want_type = ck->fccType;
	DWORD end = parent ? parent->dwDataOffset + parent->cksize : 0xffffffff;

	for (;;) {
		LONG pos = io_tell(m);
		if (parent && (DWORD)pos + 8 > end)
			return MMIOERR_CHUNKNOTFOUND;
		MMCKINFO c;
		if (!read_header(m, &c))
			return MMIOERR_CHUNKNOTFOUND;
		int match =
			(flags & MMIO_FINDCHUNK) ? c.ckid == want_id :
			(flags & MMIO_FINDRIFF) ?
				c.ckid == FOURCC_RIFF && c.fccType == want_type :
			(flags & MMIO_FINDLIST) ?
				c.ckid == FOURCC_LIST && c.fccType == want_type :
			1;
		if (match) {
			*ck = c;
			return 0;
		}
		DWORD next = c.dwDataOffset + c.cksize + (c.cksize & 1);
		if (next <= (DWORD)pos || io_seek(m, (LONG)next, SEEK_SET) == -1)
			return MMIOERR_CHUNKNOTFOUND;
	}
}

WINFN(MMRESULT) mm_mmioAscend(HMMIO h, LPMMCKINFO ck, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return MMSYSERR_INVALHANDLE;
	if (!ck)
		return MMSYSERR_INVALPARAM;
	if (ck->dwFlags & MMIO_DIRTY) {
		/* A chunk written since mmioCreateChunk: fix up its size.
		 * ck and the position change only once that is written. */
		LONG cur = io_tell(m);
		DWORD size = (DWORD)cur - ck->dwDataOffset;
		if (size != ck->cksize) {
			if (io_seek(m, (LONG)(ck->dwDataOffset - 4), SEEK_SET) == -1)
				return MMIOERR_CANNOTSEEK;
			if (io_write(m, &size, 4) != 4) {
				io_seek(m, cur, SEEK_SET);
				return MMIOERR_CANNOTWRITE;
			}
			if (io_seek(m, cur, SEEK_SET) == -1)
				return MMIOERR_CANNOTSEEK;
			ck->cksize = size;
		}
		if (size & 1) {
			static const char pad;
			if (io_write(m, &pad, 1) != 1) {
				io_seek(m, cur, SEEK_SET);
				return MMIOERR_CANNOTWRITE;
			}
		}
		ck->dwFlags &= ~MMIO_DIRTY;
		return 0;
	}
	DWORD next = ck->dwDataOffset + ck->cksize + (ck->cksize & 1);
	return io_seek(m, (LONG)next, SEEK_SET) == -1 ? MMIOERR_CANNOTSEEK : 0;
}

WINFN(MMRESULT) mm_mmioCreateChunk(HMMIO h, LPMMCKINFO ck, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return MMSYSERR_INVALHANDLE;
	if (!ck)
		return MMSYSERR_INVALPARAM;
	if (flags & MMIO_CREATERIFF)
		ck->ckid = FOURCC_RIFF;
	else if (flags & MMIO_CREATELIST)
		ck->ckid = FOURCC_LIST;
	DWORD hdr[3] = { ck->ckid, ck->cksize, ck->fccType };
	LONG n = (flags & (MMIO_CREATERIFF | MMIO_CREATELIST)) ? 12 : 8;
	LONG pos = io_tell(m);
	if (io_write(m, hdr, n) != n)
		return MMIOERR_CANNOTWRITE;
	ck->dwDataOffset = (DWORD)pos + 8;
	ck->dwFlags = MMIO_DIRTY;
	return 0;
}

#define E(name) { #name, 0, (void *)mm_##name }

const struct builtin_export winmm_mmio_exports[] = {
	E(mmioAdvance),
	E(mmioAscend),
	E(mmioClose),
	E(mmioCreateChunk),
	E(mmioDescend),
	E(mmioFlush),
	E(mmioGetInfo),
	E(mmioInstallIOProcA),
	E(mmioInstallIOProcW),
	E(mmioOpenA),
	E(mmioOpenW),
	E(mmioRead),
	E(mmioRenameA),
	E(mmioRenameW),
	E(mmioSeek),
	E(mmioSendMessage),
	E(mmioSetBuffer),
	E(mmioSetInfo),
	E(mmioStringToFOURCCA),
	E(mmioStringToFOURCCW),
	E(mmioWrite),
	BUILTIN_END
};
