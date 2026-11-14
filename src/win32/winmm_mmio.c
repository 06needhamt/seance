/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * winmm's multimedia file I/O (ravi32.dll reads and writes its files this
 * way): mmioOpen over a host file (FOURCC_DOS), a memory block (FOURCC_MEM)
 * or an application's I/O procedure, and the RIFF chunk walking on top.
 *
 * Files are unbuffered here: every mmioRead and mmioWrite goes straight to
 * the I/O procedure. MMIO_ALLOCBUF and mmioSetBuffer are accepted and do
 * nothing, and mmioAdvance, which only means something for a buffer the
 * caller fills directly, says MMIOERR_UNBUFFERED.
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

#include "runtime/gate.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

struct mmio {
	MMIOINFO info;
	int fd;			/* FOURCC_DOS */
	LONG mem_size;		/* FOURCC_MEM: bytes in pchBuffer */
	int mem_owned;		/* FOURCC_MEM buffer allocated here */
};

/* Installed I/O procedures (mmioInstallIOProc). */
#define MAX_PROCS 16
static struct {
	FOURCC fcc;
	LPMMIOPROC proc;
} procs[MAX_PROCS];
static pthread_mutex_t procs_lock = PTHREAD_MUTEX_INITIALIZER;

static LPMMIOPROC find_proc(FOURCC fcc)
{
	LPMMIOPROC p = NULL;

	pthread_mutex_lock(&procs_lock);
	for (int i = 0; i < MAX_PROCS && !p; i++)
		if (procs[i].proc && procs[i].fcc == fcc)
			p = procs[i].proc;
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
		if (!mem && m->fd >= 0 && close(m->fd))
			return MMIOERR_CANNOTCLOSE;
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

static HMMIO mmio_open(LPSTR name, LPMMIOINFO user, DWORD flags)
{
	if (user)
		user->wErrorRet = 0;
	if (flags & MMIO_PARSE) {
		char *u = acp_to_utf8(name ? name : ""), *full;
		full = u ? k32_full_path(u) : NULL;
		free(u);
		if (!full) {
			if (user)
				user->wErrorRet = MMIOERR_FILENOTFOUND;
			return NULL;
		}
		char *a = utf8_to_acp_dup(full);
		free(full);
		if (a)
			snprintf(name, 128, "%s", a);
		free(a);
		return (HMMIO)TRUE;
	}
	if (flags & (MMIO_EXIST | MMIO_DELETE)) {
		char *path = name ? host_path(name) : NULL;
		struct stat st;
		int ok = path && !stat(path, &st);
		if (ok && (flags & MMIO_DELETE))
			ok = !unlink(path);
		free(path);
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
	if (!m->info.pIOProc && m->info.fccIOProc != FOURCC_DOS &&
	    m->info.fccIOProc != FOURCC_MEM &&
	    !(m->info.pIOProc = find_proc(m->info.fccIOProc))) {
		free(m);
		if (user)
			user->wErrorRet = MMIOERR_CANNOTOPEN;
		return NULL;
	}
	m->info.dwFlags = flags & ~(MMIO_ALLOCBUF | MMIO_GETTEMP);
	m->info.hmmio = (HMMIO)m;
	if (m->info.fccIOProc == FOURCC_MEM && !m->info.pIOProc) {
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
	}

	LRESULT r = send_msg(m, MMIOM_OPEN, (LPARAM)name, 0);
	if (r) {
		if (m->mem_owned)
			free(m->info.pchBuffer);
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
	if ((flags & MMIO_PARSE) && h && name) {
		/* szFilename is 128 characters. */
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
	MMRESULT r = (MMRESULT)send_msg(m, MMIOM_CLOSE, flags, 0);
	if (m->mem_owned)
		free(m->info.pchBuffer);
	free(m);
	return r;
}

WINFN(LONG) mm_mmioRead(HMMIO h, HPSTR buf, LONG n)
{
	struct mmio *m = mmio_of(h);

	return m ? (LONG)send_msg(m, MMIOM_READ, (LPARAM)buf, n) : -1;
}

WINFN(LONG) mm_mmioWrite(HMMIO h, const char *buf, LONG n)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return -1;
	LONG r = (LONG)send_msg(m, MMIOM_WRITE, (LPARAM)buf, n);
	if (r > 0)
		m->info.dwFlags |= MMIO_DIRTY;
	return r;
}

WINFN(LONG) mm_mmioSeek(HMMIO h, LONG off, int origin)
{
	struct mmio *m = mmio_of(h);

	return m ? (LONG)send_msg(m, MMIOM_SEEK, off, origin) : -1;
}

WINFN(MMRESULT) mm_mmioFlush(HMMIO h, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m)
		return MMSYSERR_INVALHANDLE;
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
	/* No buffer the caller may fill directly. */
	info->cchBuffer = 0;
	info->pchBuffer = info->pchNext = info->pchEndRead =
		info->pchEndWrite = NULL;
	info->lBufOffset = info->lDiskOffset;
	return 0;
}

WINFN(MMRESULT) mm_mmioSetInfo(HMMIO h, const MMIOINFO *info, UINT flags)
{
	struct mmio *m = mmio_of(h);

	if (!m || !info)
		return MMSYSERR_INVALHANDLE;
	memcpy(m->info.adwInfo, info->adwInfo, sizeof(m->info.adwInfo));
	return 0;
}

WINFN(MMRESULT) mm_mmioSetBuffer(HMMIO h, LPSTR buf, LONG size, UINT flags)
{
	return mmio_of(h) ? 0 : MMSYSERR_INVALHANDLE;
}

WINFN(MMRESULT) mm_mmioAdvance(HMMIO h, LPMMIOINFO info, UINT flags)
{
	return mmio_of(h) ? MMIOERR_UNBUFFERED : MMSYSERR_INVALHANDLE;
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

	pthread_mutex_lock(&procs_lock);
	if (flags & MMIO_INSTALLPROC) {
		for (int i = 0; i < MAX_PROCS; i++)
			if (!procs[i].proc) {
				procs[i].fcc = fcc;
				procs[i].proc = proc;
				r = proc;
				break;
			}
	} else if (flags & MMIO_REMOVEPROC) {
		for (int i = MAX_PROCS - 1; i >= 0; i--)
			if (procs[i].proc && procs[i].fcc == fcc &&
			    (!proc || procs[i].proc == proc)) {
				r = procs[i].proc;
				procs[i].proc = NULL;
				break;
			}
	} else if (flags & MMIO_FINDPROC) {
		for (int i = 0; i < MAX_PROCS && !r; i++)
			if (procs[i].proc && procs[i].fcc == fcc)
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

	if (send_msg(m, MMIOM_READ, (LPARAM)hdr, 8) != 8)
		return 0;
	ck->ckid = hdr[0];
	ck->cksize = hdr[1];
	ck->fccType = 0;
	ck->dwDataOffset = (DWORD)m->info.lDiskOffset;
	ck->dwFlags = 0;
	if (ck->ckid == FOURCC_RIFF || ck->ckid == FOURCC_LIST) {
		if (send_msg(m, MMIOM_READ, (LPARAM)&ck->fccType, 4) != 4)
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
		LONG pos = m->info.lDiskOffset;
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
		if (next <= (DWORD)pos ||
		    send_msg(m, MMIOM_SEEK, (LPARAM)next, SEEK_SET) == -1)
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
		/* A chunk written since mmioCreateChunk: fix up its size. */
		LONG cur = m->info.lDiskOffset;
		DWORD size = (DWORD)cur - ck->dwDataOffset;
		if (size != ck->cksize) {
			ck->cksize = size;
			if (send_msg(m, MMIOM_SEEK, (LPARAM)(ck->dwDataOffset - 4),
				     SEEK_SET) == -1 ||
			    send_msg(m, MMIOM_WRITE, (LPARAM)&ck->cksize, 4) != 4)
				return MMIOERR_CANNOTWRITE;
			if (send_msg(m, MMIOM_SEEK, cur, SEEK_SET) == -1)
				return MMIOERR_CANNOTSEEK;
		}
		if (size & 1) {
			static const char pad;
			if (send_msg(m, MMIOM_WRITE, (LPARAM)&pad, 1) != 1)
				return MMIOERR_CANNOTWRITE;
		}
		ck->dwFlags &= ~MMIO_DIRTY;
		return 0;
	}
	DWORD next = ck->dwDataOffset + ck->cksize + (ck->cksize & 1);
	return send_msg(m, MMIOM_SEEK, (LPARAM)next, SEEK_SET) == -1 ?
	       MMIOERR_CANNOTSEEK : 0;
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
	LONG pos = m->info.lDiskOffset;
	if (send_msg(m, MMIOM_WRITE, (LPARAM)hdr, n) != n)
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
