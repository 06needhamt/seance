/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msacm32.dll: drivers and streams (see msacm32.h). The format and filter
 * calls are in msacm_format.c, the PCM converter in msacm_pcm.c.
 */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "common/error.h"
#include "common/log.h"
#include "vfw/driver.h"
#include "vfw/registry.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/msacm32.h"

#define MAX_TAGS 256		/* format or filter tags a driver may have */

/* A stream: an HACMSTREAM. Its formats follow it. */
struct acm_stream {
	struct acm_stream *next;
	struct acm_driver *drv;
	int own_driver;		/* opened for it, closed with it */
	ACMDRVSTREAMINSTANCE inst;
};

static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static struct acm_id *ids, *pcm;
static struct acm_stream *streams;
static unsigned synced = ~0u;

void acm_lock(void)
{
	pthread_mutex_lock(&lock);
}

void acm_unlock(void)
{
	pthread_mutex_unlock(&lock);
}

struct acm_id *acm_first(void)
{
	return ids;
}

/* Strings. */

void acm_wcopy(WCHAR *dst, size_t n, const WCHAR *src)
{
	size_t i = 0;

	for (; src && src[i] && i + 1 < n; i++)
		dst[i] = src[i];
	if (n)
		dst[i] = 0;
}

void acm_w_to_a(char *dst, size_t n, const WCHAR *src)
{
	WCHAR tmp[1024];

	/* Code page 1252 is a byte for each character: truncate first. */
	if (n > sizeof(tmp) / sizeof(*tmp))
		n = sizeof(tmp) / sizeof(*tmp);
	acm_wcopy(tmp, n, src);
	if (n && cp_from_utf16(CODEPAGE_ANSI, tmp, -1, dst, (int)n, '?',
			       NULL) < 0)
		dst[0] = 0;
}

void acm_a_to_w(WCHAR *dst, size_t n, const char *src)
{
	char tmp[1024];

	if (n > sizeof(tmp))
		n = sizeof(tmp);
	snprintf(tmp, n, "%s", src ? src : "");
	if (n && cp_to_utf16(CODEPAGE_ANSI, tmp, -1, dst, (int)n, 0) < 0)
		dst[0] = 0;
}

DWORD acm_wfx_size(const WAVEFORMATEX *wf)
{
	return wf->wFormatTag == WAVE_FORMAT_PCM ? sizeof(PCMWAVEFORMAT) :
	       sizeof(WAVEFORMATEX) + wf->cbSize;
}

const char *msacm_error_name(MMRESULT r, char buf[24])
{
	static const char *const sys[] = {
		"MMSYSERR_NOERROR", "MMSYSERR_ERROR", "MMSYSERR_BADDEVICEID",
		"MMSYSERR_NOTENABLED", "MMSYSERR_ALLOCATED",
		"MMSYSERR_INVALHANDLE", "MMSYSERR_NODRIVER", "MMSYSERR_NOMEM",
		"MMSYSERR_NOTSUPPORTED", "MMSYSERR_BADERRNUM",
		"MMSYSERR_INVALFLAG", "MMSYSERR_INVALPARAM",
		"MMSYSERR_HANDLEBUSY", "MMSYSERR_INVALIDALIAS", "MMSYSERR_BADDB",
		"MMSYSERR_KEYNOTFOUND", "MMSYSERR_READERROR",
		"MMSYSERR_WRITEERROR", "MMSYSERR_DELETEERROR",
		"MMSYSERR_VALNOTFOUND", "MMSYSERR_NODRIVERCB",
		"MMSYSERR_MOREDATA",
	};
	static const char *const acm[] = {
		"ACMERR_NOTPOSSIBLE", "ACMERR_BUSY", "ACMERR_UNPREPARED",
		"ACMERR_CANCELED",
	};

	if (r < sizeof(sys) / sizeof(*sys))
		return sys[r];
	if (r >= ACMERR_BASE && r - ACMERR_BASE < sizeof(acm) / sizeof(*acm))
		return acm[r - ACMERR_BASE];
	snprintf(buf, 24, "error %u", r);
	return buf;
}

/* The driver list. */

static void id_free(struct acm_id *id)
{
	free(id->alias);
	free(id->name);
	free(id->path);
	free(id->formats);
	free(id->filters);
	free(id);
}

static struct acm_id *id_new(const char *name, const char *path,
			     DRIVERPROC proc)
{
	struct acm_id *id = calloc(1, sizeof(*id));

	if (!id)
		return NULL;
	id->proc = proc;
	if (!(id->name = strdup(name)) ||
	    !(id->alias = utf8_to_utf16_dup(name)) ||
	    (path && !(id->path = strdup(path)))) {
		id_free(id);
		return NULL;
	}
	return id;
}

/* Puts id after `after` (NULL: at the head). */
static void insert_after(struct acm_id *after, struct acm_id *id)
{
	struct acm_id **p = after ? &after->next : &ids;

	id->next = *p;
	*p = id;
}

/* Where a table entry goes: after the last one. */
static struct acm_id *last_table_id(void)
{
	struct acm_id *last = NULL;

	for (struct acm_id *i = ids; i; i = i->next)
		if (i->from_table)
			last = i;
	return last;
}

/* Where a run-time addition goes: before the PCM converter. */
static struct acm_id *before_pcm(void)
{
	struct acm_id *last = NULL;

	for (struct acm_id *i = ids; i && i != pcm; i = i->next)
		last = i;
	return last;
}

static void forget_cache(struct acm_id *id)
{
	free(id->formats);
	free(id->filters);
	id->formats = id->filters = NULL;
	memset(&id->details, 0, sizeof(id->details));
	id->cached = 0;
}

void acm_sync(void)
{
	if (!pcm && (pcm = id_new("msacm32.dll", NULL,
				  (DRIVERPROC)msacm_pcm_proc))) {
		pcm->local = 0;
		insert_after(before_pcm(), pcm);
	}
	unsigned g = vfw_registry_generation();
	if (g == synced)
		return;
	synced = g;

	for (struct acm_id *i = ids; i; i = i->next)
		if (i->from_table)
			i->from_table = 2;	/* not seen yet */
	char *alias, *path;
	for (unsigned n = 0; !vfw_acm_get(n, &alias, &path); n++) {
		struct acm_id *id = NULL;
		for (struct acm_id *i = ids; i && !id; i = i->next)
			if (i->from_table && !strcasecmp(i->name, alias))
				id = i;
		if (id) {
			id->from_table = 1;
			if (strcmp(id->path, path) && !id->open) {
				free(id->path);
				id->path = path;
				path = NULL;
				forget_cache(id);
			}
		} else if ((id = id_new(alias, path, NULL))) {
			struct acm_id *after = last_table_id();
			id->from_table = 1;
			insert_after(after, id);
			TRACE("msacm: %s = %s", alias, path);
		}
		free(alias);
		free(path);
	}
	for (struct acm_id *i = ids; i; i = i->next)
		if (i->from_table == 2) {
			i->from_table = 1;
			i->removed = 1;
		}
}

struct acm_id *acm_find_id(HACMDRIVERID h)
{
	for (struct acm_id *i = ids; i; i = i->next)
		if ((HACMDRIVERID)i == h && !i->removed)
			return i;
	return NULL;
}

struct acm_driver *acm_find_driver(HACMDRIVER h)
{
	for (struct acm_id *i = ids; i; i = i->next)
		for (struct acm_driver *d = i->open; d; d = d->next)
			if ((HACMDRIVER)d == h)
				return d;
	return NULL;
}

static struct acm_stream *find_stream(HACMSTREAM h)
{
	for (struct acm_stream *s = streams; s; s = s->next)
		if ((HACMSTREAM)s == h)
			return s;
	return NULL;
}

/* The driver behind any handle. */
static struct acm_id *object_id(HACMOBJ h)
{
	struct acm_id *id = acm_find_id((HACMDRIVERID)h);
	struct acm_driver *d;
	struct acm_stream *s;

	if (id)
		return id;
	if ((d = acm_find_driver((HACMDRIVER)h)))
		return d->id;
	if ((s = find_stream((HACMSTREAM)h)))
		return s->drv->id;
	return NULL;
}

/* Opening drivers, and what is read from them once. */

MMRESULT acm_driver_open(struct acm_id *id, struct acm_driver **out)
{
	struct acm_driver *d = calloc(1, sizeof(*d));

	*out = NULL;
	if (!d)
		return MMSYSERR_NOMEM;
	d->id = id;
	d->desc = (ACMDRVOPENDESCW){
		sizeof(d->desc), ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC,
		ACMDRIVERDETAILS_FCCCOMP_UNDEFINED, ACM_VERSION, 0, 0,
		NULL, id->alias, 0,
	};
	static const WCHAR section[] = { 'D', 'r', 'i', 'v', 'e', 'r', 's',
					 '3', '2', 0 };
	d->desc.pszSectionName = section;
	d->drv = id->path ? drv_open(id->path, id->alias, (LPARAM)&d->desc) :
		 drv_open_proc(id->proc, id->alias, (LPARAM)&d->desc);
	if (!d->drv) {
		MMRESULT r = d->desc.dwError ? d->desc.dwError : MMSYSERR_ERROR;
		TRACE("msacm: %s doesn't open: %s", id->name, vfw_error());
		free(d);
		return r;
	}
	d->next = id->open;
	id->open = d;
	*out = d;
	return MMSYSERR_NOERROR;
}

void acm_driver_close(struct acm_driver *d)
{
	struct acm_id *id = d->id;

	for (struct acm_driver **p = &id->open; *p; p = &(*p)->next)
		if (*p == d) {
			*p = d->next;
			break;
		}
	drv_close(d->drv, 0, 0);
	free(d);
}

MMRESULT acm_message(struct acm_driver *d, UINT msg, LPARAM l1, LPARAM l2)
{
	return (MMRESULT)drv_send(d->drv, msg, l1, l2);
}

/* Reads a tag list: ACMDM_FORMATTAG_DETAILS or ACMDM_FILTERTAG_DETAILS. */
static struct acm_tag *read_tags(struct acm_driver *d, DWORD n, int filters)
{
	struct acm_tag *t = calloc(n ? n : 1, sizeof(*t));

	for (DWORD i = 0; t && i < n; i++) {
		if (filters) {
			ACMFILTERTAGDETAILSW f = { .cbStruct = sizeof(f), .dwFilterTagIndex = i };
			if (!acm_message(d, ACMDM_FILTERTAG_DETAILS, (LPARAM)&f,
					 ACM_FILTERTAGDETAILSF_INDEX))
				t[i] = (struct acm_tag){ f.dwFilterTag,
							f.cbFilterSize };
		} else {
			ACMFORMATTAGDETAILSW f = { .cbStruct = sizeof(f), .dwFormatTagIndex = i };
			if (!acm_message(d, ACMDM_FORMATTAG_DETAILS, (LPARAM)&f,
					 ACM_FORMATTAGDETAILSF_INDEX))
				t[i] = (struct acm_tag){ f.dwFormatTag,
							f.cbFormatSize };
		}
	}
	return t;
}

/* Reads a driver's details and tags, once. 0, or -1 if it isn't one. */
static int fill_cache(struct acm_id *id)
{
	struct acm_driver *d;

	if (id->cached)
		return id->cached > 0 ? 0 : -1;
	id->cached = -1;
	if (acm_driver_open(id, &d))
		return -1;
	ACMDRIVERDETAILSW *dd = &id->details;
	memset(dd, 0, sizeof(*dd));
	dd->cbStruct = sizeof(*dd);
	MMRESULT r = acm_message(d, ACMDM_DRIVER_DETAILS, (LPARAM)dd, 0);
	/* A VfW codec may take the open, but has no ACM details. */
	if (r || dd->fccType != ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC) {
		char name[24];
		TRACE("msacm: %s: no ACM details (%s, type 0x%08x)", id->name,
		      msacm_error_name(r, name), (unsigned)dd->fccType);
		acm_driver_close(d);
		return -1;
	}
	dd->cbStruct = sizeof(*dd);
	if (dd->cFormatTags > MAX_TAGS)
		dd->cFormatTags = MAX_TAGS;
	if (dd->cFilterTags > MAX_TAGS)
		dd->cFilterTags = MAX_TAGS;
	id->formats = read_tags(d, dd->cFormatTags, 0);
	id->filters = read_tags(d, dd->cFilterTags, 1);
	acm_driver_close(d);
	if (!id->formats || !id->filters) {
		forget_cache(id);
		id->cached = -1;
		return -1;
	}
	id->cached = 1;
	return 0;
}

/* Whether acmDriverEnum would list it (disabled or not). */
static int listed(struct acm_id *id)
{
	return !id->removed && !fill_cache(id);
}

int acm_usable(struct acm_id *id)
{
	return !id->removed && !id->disabled && !fill_cache(id);
}

DWORD acm_support(const struct acm_id *id)
{
	return (id->details.fdwSupport &
		~(DWORD)(ACMDRIVERDETAILS_SUPPORTF_DISABLED |
			 ACMDRIVERDETAILS_SUPPORTF_LOCAL)) |
	       (id->disabled ? ACMDRIVERDETAILS_SUPPORTF_DISABLED : 0) |
	       (id->local ? ACMDRIVERDETAILS_SUPPORTF_LOCAL : 0);
}

int acm_has_format(const struct acm_id *id, DWORD tag)
{
	for (DWORD i = 0; id->cached > 0 && i < id->details.cFormatTags; i++)
		if (id->formats[i].tag == tag)
			return 1;
	return 0;
}

int acm_has_filter(const struct acm_id *id, DWORD tag)
{
	for (DWORD i = 0; id->cached > 0 && i < id->details.cFilterTags; i++)
		if (id->filters[i].tag == tag)
			return 1;
	return 0;
}

/* Two paths to the same file, or the same name. */
static int same_file(const char *a, const char *b)
{
	struct stat sa, sb;

	if (!stat(a, &sa) && !stat(b, &sb))
		return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
	return !strcmp(a, b);
}

MMRESULT msacm_add_path(const char *path, HACMDRIVERID *out)
{
	MMRESULT r = MMSYSERR_NOERROR;
	struct acm_id *id = NULL;

	*out = NULL;
	acm_lock();
	acm_sync();
	for (struct acm_id *i = ids; i && !id; i = i->next)
		if (!i->removed && i->path && same_file(i->path, path))
			id = i;
	if (!id) {
		/* msacm.<base name without its extension>, made unique. */
		const char *base = strrchr(path, '/');
		char stem[200], name[220];
		snprintf(stem, sizeof(stem), "%s", base ? base + 1 : path);
		char *dot = strrchr(stem, '.');
		if (dot && dot != stem)
			*dot = 0;
		snprintf(name, sizeof(name), "msacm.%s", stem);
		for (int n = 2, taken = 1; taken; n++) {
			taken = 0;
			for (struct acm_id *i = ids; i; i = i->next)
				taken |= !strcasecmp(i->name, name);
			if (taken)
				snprintf(name, sizeof(name), "msacm.%s#%d", stem, n);
		}
		if (!(id = id_new(name, path, NULL))) {
			vfw_set_error("out of memory");
			r = MMSYSERR_NOMEM;
			goto done;
		}
		id->local = 1;
		insert_after(before_pcm(), id);
	}
	if (fill_cache(id)) {
		const char *why = vfw_error();
		vfw_set_error("%s: not an ACM driver (%s)", path,
			      why && *why ? why : "no ACM driver details");
		id->removed = 1;
		r = MMSYSERR_ERROR;
		goto done;
	}
	*out = (HACMDRIVERID)id;
done:
	acm_unlock();
	return r;
}

unsigned msacm_stream_count(void)
{
	unsigned n = 0;

	acm_lock();
	for (struct acm_stream *s = streams; s; s = s->next)
		n++;
	acm_unlock();
	return n;
}

/* The API: version, metrics, drivers. */

DWORD STUB_WINAPI msacm_acmGetVersion(void)
{
	return ACM_VERSION;
}

/* What a metric counts of a driver: all, or the local ones. */
static DWORD count(DWORD want, int disabled, int local_only)
{
	DWORD n = 0;

	for (struct acm_id *i = ids; i; i = i->next) {
		if (i->removed || fill_cache(i) || (local_only && !i->local) ||
		    !!i->disabled != disabled)
			continue;
		if (!want || (i->details.fdwSupport & want))
			n++;
	}
	return n;
}

static DWORD largest(struct acm_id *only, int filters)
{
	DWORD max = 0;

	for (struct acm_id *i = only ? only : ids; i; i = only ? NULL : i->next) {
		if (!only && !acm_usable(i))
			continue;
		if (only && fill_cache(i))
			continue;
		DWORD n = filters ? i->details.cFilterTags :
			  i->details.cFormatTags;
		for (DWORD t = 0; t < n; t++) {
			DWORD s = filters ? i->filters[t].size :
				  i->formats[t].size;
			if (s > max)
				max = s;
		}
	}
	return max;
}

MMRESULT STUB_WINAPI msacm_acmMetrics(HACMOBJ hao, UINT metric, LPVOID out)
{
	MMRESULT r = MMSYSERR_NOERROR;
	DWORD *v = out;

	if (!out)
		return MMSYSERR_INVALPARAM;
	acm_lock();
	acm_sync();
	struct acm_id *id = hao ? object_id(hao) : NULL;
	if (hao && !id) {
		r = MMSYSERR_INVALHANDLE;
		goto done;
	}
	int local = metric >= ACM_METRIC_COUNT_LOCAL_DRIVERS &&
		    metric <= ACM_METRIC_COUNT_LOCAL_DISABLED;
	/* The counts are of every driver: they take no handle. */
	if (hao && metric < ACM_METRIC_HARDWARE_WAVE_INPUT) {
		r = MMSYSERR_INVALHANDLE;
		goto done;
	}
	switch (metric) {
	case ACM_METRIC_COUNT_DRIVERS:
	case ACM_METRIC_COUNT_LOCAL_DRIVERS:
		*v = count(0, 0, local);
		break;
	case ACM_METRIC_COUNT_CODECS:
	case ACM_METRIC_COUNT_LOCAL_CODECS:
		*v = count(ACMDRIVERDETAILS_SUPPORTF_CODEC, 0, local);
		break;
	case ACM_METRIC_COUNT_CONVERTERS:
	case ACM_METRIC_COUNT_LOCAL_CONVERTERS:
		*v = count(ACMDRIVERDETAILS_SUPPORTF_CONVERTER, 0, local);
		break;
	case ACM_METRIC_COUNT_FILTERS:
	case ACM_METRIC_COUNT_LOCAL_FILTERS:
		*v = count(ACMDRIVERDETAILS_SUPPORTF_FILTER, 0, local);
		break;
	case ACM_METRIC_COUNT_DISABLED:
	case ACM_METRIC_COUNT_LOCAL_DISABLED:
		*v = count(0, 1, local);
		break;
	case ACM_METRIC_COUNT_HARDWARE:
		*v = count(ACMDRIVERDETAILS_SUPPORTF_HARDWARE, 0, 0);
		break;
	case ACM_METRIC_MAX_SIZE_FORMAT:
		*v = largest(id, 0);
		break;
	case ACM_METRIC_MAX_SIZE_FILTER:
		*v = largest(id, 1);
		break;
	case ACM_METRIC_DRIVER_SUPPORT:
		if (!id || (HACMDRIVERID)id != (HACMDRIVERID)hao)
			r = MMSYSERR_INVALHANDLE;
		else if (fill_cache(id))
			r = MMSYSERR_ERROR;
		else
			*v = acm_support(id);
		break;
	case ACM_METRIC_DRIVER_PRIORITY: {
		DWORD n = 1;
		if (!id || (HACMDRIVERID)id != (HACMDRIVERID)hao) {
			r = MMSYSERR_INVALHANDLE;
			break;
		}
		for (struct acm_id *i = ids; i != id; i = i->next)
			n += listed(i);
		*v = n;
		break;
	}
	default:
		/* No hardware: there are no wave devices to name. */
		r = MMSYSERR_NOTSUPPORTED;
	}
done:
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmDriverEnum(ACMDRIVERENUMCB cb, DWORD_PTR inst,
					 DWORD flags)
{
	if (!cb)
		return MMSYSERR_INVALPARAM;
	if (flags & ~(DWORD)(ACM_DRIVERENUMF_NOLOCAL | ACM_DRIVERENUMF_DISABLED))
		return MMSYSERR_INVALFLAG;
	acm_lock();
	acm_sync();
	for (struct acm_id *i = ids; i; i = i->next) {
		if (i->removed || fill_cache(i) ||
		    (i->disabled && !(flags & ACM_DRIVERENUMF_DISABLED)) ||
		    (i->local && (flags & ACM_DRIVERENUMF_NOLOCAL)))
			continue;
		if (!cb((HACMDRIVERID)i, inst, acm_support(i)))
			break;
	}
	acm_unlock();
	return MMSYSERR_NOERROR;
}

static MMRESULT add_driver(LPHACMDRIVERID out, LPARAM param, const char *name,
			   DWORD priority, DWORD flags)
{
	MMRESULT r = MMSYSERR_NOERROR;
	DWORD type = flags & ACM_DRIVERADDF_TYPEMASK;

	if (!out)
		return MMSYSERR_INVALPARAM;
	*out = NULL;
	if (flags & ~(DWORD)(ACM_DRIVERADDF_TYPEMASK | ACM_DRIVERADDF_GLOBAL))
		return MMSYSERR_INVALFLAG;
	acm_lock();
	acm_sync();
	switch (type) {
	case ACM_DRIVERADDF_NAME:
		/* A Drivers32 value: the table already holds them. */
		r = MMSYSERR_INVALPARAM;
		for (struct acm_id *i = ids; name && i; i = i->next)
			if (!i->removed && !strcasecmp(i->name, name) &&
			    !fill_cache(i)) {
				*out = (HACMDRIVERID)i;
				r = MMSYSERR_NOERROR;
				break;
			}
		break;
	case ACM_DRIVERADDF_FUNCTION: {
		struct acm_id *id;
		if (priority || !param) {
			r = MMSYSERR_INVALPARAM;
			break;
		}
		char fn[40];
		snprintf(fn, sizeof(fn), "msacm.function@%08lx",
			 (unsigned long)param);
		if (!(id = id_new(fn, NULL, (DRIVERPROC)param))) {
			r = MMSYSERR_NOMEM;
			break;
		}
		id->local = 1;
		insert_after(before_pcm(), id);
		if (fill_cache(id)) {
			id->removed = 1;
			r = MMSYSERR_ERROR;
			break;
		}
		*out = (HACMDRIVERID)id;
		break;
	}
	case ACM_DRIVERADDF_NOTIFYHWND:
		/* There are no windows to tell. */
		r = MMSYSERR_NOTSUPPORTED;
		break;
	default:
		r = MMSYSERR_INVALFLAG;
	}
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmDriverAddW(LPHACMDRIVERID out, HINSTANCE module,
					 LPARAM param, DWORD priority,
					 DWORD flags)
{
	(void)module;
	char *name = (flags & ACM_DRIVERADDF_TYPEMASK) == ACM_DRIVERADDF_NAME &&
		     param ? utf16_to_utf8_dup((const WCHAR *)param) : NULL;
	MMRESULT r = add_driver(out, param, name, priority, flags);
	free(name);
	return r;
}

WINFN(MMRESULT) msacm_acmDriverAddA(LPHACMDRIVERID out, HINSTANCE module,
				    LPARAM param, DWORD priority, DWORD flags)
{
	(void)module;
	char *name = (flags & ACM_DRIVERADDF_TYPEMASK) == ACM_DRIVERADDF_NAME &&
		     param ? acp_to_utf8((const char *)param) : NULL;
	MMRESULT r = add_driver(out, param, name, priority, flags);
	free(name);
	return r;
}

MMRESULT STUB_WINAPI msacm_acmDriverRemove(HACMDRIVERID hadid, DWORD flags)
{
	MMRESULT r = MMSYSERR_NOERROR;

	if (flags)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	struct acm_id *id = acm_find_id(hadid);
	if (!id)
		r = MMSYSERR_INVALHANDLE;
	else if (id->open)
		r = ACMERR_BUSY;
	else
		id->removed = 1;	/* kept, so the table doesn't bring it back */
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmDriverDetailsW(HACMDRIVERID hadid,
					     LPACMDRIVERDETAILSW details,
					     DWORD flags)
{
	MMRESULT r = MMSYSERR_NOERROR;

	if (flags)
		return MMSYSERR_INVALFLAG;
	if (!details || details->cbStruct < sizeof(DWORD))
		return MMSYSERR_INVALPARAM;
	acm_lock();
	struct acm_id *id = acm_find_id(hadid);
	if (!id) {
		r = MMSYSERR_INVALHANDLE;
	} else if (fill_cache(id)) {
		r = MMSYSERR_ERROR;
	} else {
		ACMDRIVERDETAILSW d = id->details;
		DWORD n = details->cbStruct < sizeof(d) ? details->cbStruct :
			  sizeof(d);
		d.fdwSupport = acm_support(id);
		d.cbStruct = n;
		memcpy(details, &d, n);
	}
	acm_unlock();
	return r;
}

WINFN(MMRESULT) msacm_acmDriverDetailsA(HACMDRIVERID hadid,
				       LPACMDRIVERDETAILSA details, DWORD flags)
{
	ACMDRIVERDETAILSW w = { .cbStruct = sizeof(w) };
	ACMDRIVERDETAILSA a;

	if (flags)
		return MMSYSERR_INVALFLAG;
	if (!details || details->cbStruct < sizeof(DWORD))
		return MMSYSERR_INVALPARAM;
	MMRESULT r = msacm_acmDriverDetailsW(hadid, &w, 0);
	if (r)
		return r;
	memset(&a, 0, sizeof(a));
	memcpy(&a, &w, offsetof(ACMDRIVERDETAILSA, szShortName));
	acm_w_to_a(a.szShortName, sizeof(a.szShortName), w.szShortName);
	acm_w_to_a(a.szLongName, sizeof(a.szLongName), w.szLongName);
	acm_w_to_a(a.szCopyright, sizeof(a.szCopyright), w.szCopyright);
	acm_w_to_a(a.szLicensing, sizeof(a.szLicensing), w.szLicensing);
	acm_w_to_a(a.szFeatures, sizeof(a.szFeatures), w.szFeatures);
	DWORD n = details->cbStruct < sizeof(a) ? details->cbStruct : sizeof(a);
	a.cbStruct = n;
	memcpy(details, &a, n);
	return MMSYSERR_NOERROR;
}

MMRESULT STUB_WINAPI msacm_acmDriverOpen(LPHACMDRIVER out, HACMDRIVERID hadid,
					 DWORD flags)
{
	struct acm_driver *d = NULL;
	MMRESULT r;

	if (!out)
		return MMSYSERR_INVALPARAM;
	*out = NULL;
	if (flags)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	struct acm_id *id = acm_find_id(hadid);
	r = !id ? MMSYSERR_INVALHANDLE : acm_driver_open(id, &d);
	if (!r)
		*out = (HACMDRIVER)d;
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmDriverClose(HACMDRIVER had, DWORD flags)
{
	MMRESULT r = MMSYSERR_NOERROR;

	if (flags)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	struct acm_driver *d = acm_find_driver(had);
	if (!d) {
		r = MMSYSERR_INVALHANDLE;
	} else {
		for (struct acm_stream *s = streams; s; s = s->next)
			if (s->drv == d)
				r = ACMERR_BUSY;
		if (!r)
			acm_driver_close(d);
	}
	acm_unlock();
	return r;
}

LRESULT STUB_WINAPI msacm_acmDriverMessage(HACMDRIVER had, UINT msg,
					   LPARAM l1, LPARAM l2)
{
	LRESULT r;

	acm_lock();
	struct acm_driver *d = acm_find_driver(had);
	if (!d) {
		r = MMSYSERR_INVALHANDLE;
	} else if ((msg >= ACMDM_USER && msg < ACMDM_RESERVED_LOW) ||
		   msg == ACMDM_DRIVER_ABOUT || msg == DRV_QUERYCONFIGURE ||
		   msg == DRV_CONFIGURE) {
		static const WCHAR section[] = { 'D', 'r', 'i', 'v', 'e', 'r',
						 's', '3', '2', 0 };
		DRVCONFIGINFO dci = { sizeof(dci), section, d->id->alias };
		if (msg == DRV_CONFIGURE && !l2)
			l2 = (LPARAM)&dci;
		r = drv_send(d->drv, msg, l1, l2);
	} else {
		r = MMSYSERR_INVALPARAM;
	}
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmDriverID(HACMOBJ hao, LPHACMDRIVERID out,
				       DWORD flags)
{
	if (flags)
		return MMSYSERR_INVALFLAG;
	if (!out)
		return MMSYSERR_INVALPARAM;
	acm_lock();
	struct acm_id *id = object_id(hao);
	*out = (HACMDRIVERID)id;
	acm_unlock();
	return id ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

MMRESULT STUB_WINAPI msacm_acmDriverPriority(HACMDRIVERID hadid,
					     DWORD priority, DWORD flags)
{
	DWORD able = flags & ACM_DRIVERPRIORITYF_ABLEMASK;
	DWORD defer = flags & ACM_DRIVERPRIORITYF_DEFERMASK;
	MMRESULT r = MMSYSERR_NOERROR;

	if ((flags & ~(DWORD)(ACM_DRIVERPRIORITYF_ABLEMASK |
			      ACM_DRIVERPRIORITYF_DEFERMASK)) ||
	    able == ACM_DRIVERPRIORITYF_ABLEMASK ||
	    defer == ACM_DRIVERPRIORITYF_DEFERMASK)
		return MMSYSERR_INVALFLAG;
	if (defer)	/* Batching changes: nothing to batch here. */
		return hadid || priority || able ? MMSYSERR_INVALPARAM :
		       MMSYSERR_NOERROR;
	acm_lock();
	struct acm_id *id = acm_find_id(hadid);
	if (!id) {
		r = MMSYSERR_INVALHANDLE;
		goto done;
	}
	if (priority && id->local) {
		r = MMSYSERR_NOTSUPPORTED;	/* local drivers keep theirs */
		goto done;
	}
	if (able == ACM_DRIVERPRIORITYF_ENABLE)
		id->disabled = 0;
	else if (able == ACM_DRIVERPRIORITYF_DISABLE)
		id->disabled = 1;
	if (priority) {
		/* Out of the list, then in at that place (-1: the end). */
		for (struct acm_id **p = &ids; *p; p = &(*p)->next)
			if (*p == id) {
				*p = id->next;
				break;
			}
		struct acm_id **p = &ids;
		for (DWORD n = 1; *p && n < priority; p = &(*p)->next)
			n += listed(*p);
		id->next = *p;
		*p = id;
	}
done:
	acm_unlock();
	return r;
}

/* Streams. */

/* A copy of a format, PCM with a cbSize of 0 for drivers that read it. */
static DWORD copy_size(const WAVEFORMATEX *wf)
{
	DWORD n = acm_wfx_size(wf);
	return n < sizeof(WAVEFORMATEX) ? sizeof(WAVEFORMATEX) : n;
}

static void copy_format(WAVEFORMATEX *dst, const WAVEFORMATEX *src)
{
	memset(dst, 0, copy_size(src));
	memcpy(dst, src, acm_wfx_size(src));
	if (src->wFormatTag == WAVE_FORMAT_PCM)
		dst->cbSize = 0;
}

static void stream_free(struct acm_stream *s)
{
	if (s->own_driver && s->drv)
		acm_driver_close(s->drv);
	free(s);
}

MMRESULT STUB_WINAPI msacm_acmStreamOpen(LPHACMSTREAM out, HACMDRIVER had,
					 LPWAVEFORMATEX src,
					 LPWAVEFORMATEX dst,
					 LPWAVEFILTER filter,
					 DWORD_PTR callback, DWORD_PTR inst,
					 DWORD flags)
{
	MMRESULT r = ACMERR_NOTPOSSIBLE;
	int query = flags & ACM_STREAMOPENF_QUERY;
	char n1[24], n2[24];

	if (out)
		*out = NULL;
	if (!src || !dst || (query && out) || (!query && !out))
		return MMSYSERR_INVALPARAM;
	if (flags & ~(DWORD)(ACM_STREAMOPENF_QUERY | ACM_STREAMOPENF_ASYNC |
			     ACM_STREAMOPENF_NONREALTIME | CALLBACK_TYPEMASK))
		return MMSYSERR_INVALFLAG;
	if (filter && src->wFormatTag != dst->wFormatTag)
		return ACMERR_NOTPOSSIBLE;

	DWORD ssize = copy_size(src), dsize = copy_size(dst);
	struct acm_stream *s = calloc(1, sizeof(*s) + ssize + dsize +
				      (filter ? sizeof(*filter) : 0));
	if (!s)
		return MMSYSERR_NOMEM;
	WAVEFORMATEX *swf = (WAVEFORMATEX *)(s + 1);
	WAVEFORMATEX *dwf = (WAVEFORMATEX *)((BYTE *)swf + ssize);
	copy_format(swf, src);
	copy_format(dwf, dst);
	WAVEFILTER *f = NULL;
	if (filter) {
		f = (WAVEFILTER *)((BYTE *)dwf + dsize);
		*f = *filter;
	}
	s->inst = (ACMDRVSTREAMINSTANCE){
		sizeof(s->inst), swf, dwf, f, callback, inst, flags, 0, 0,
		(HACMSTREAM)s,
	};

	acm_lock();
	acm_sync();
	if (had) {
		struct acm_driver *d = acm_find_driver(had);
		if (!d) {
			r = MMSYSERR_INVALHANDLE;
		} else if ((flags & ACM_STREAMOPENF_ASYNC) &&
			   !(d->id->details.fdwSupport &
			     ACMDRIVERDETAILS_SUPPORTF_ASYNC)) {
			r = ACMERR_NOTPOSSIBLE;
		} else {
			s->drv = d;
			r = acm_message(d, ACMDM_STREAM_OPEN, (LPARAM)&s->inst, 0);
		}
	} else {
		/* The first driver, in priority order, that takes it. */
		for (struct acm_id *i = ids; i && r; i = i->next) {
			struct acm_driver *d;
			if (!acm_usable(i) || !acm_has_format(i, src->wFormatTag) ||
			    !acm_has_format(i, dst->wFormatTag) ||
			    (filter && !acm_has_filter(i, filter->dwFilterTag)) ||
			    ((flags & ACM_STREAMOPENF_ASYNC) &&
			     !(i->details.fdwSupport &
			       ACMDRIVERDETAILS_SUPPORTF_ASYNC)) ||
			    acm_driver_open(i, &d))
				continue;
			s->inst.fdwDriver = 0;
			s->inst.dwDriver = 0;
			r = acm_message(d, ACMDM_STREAM_OPEN, (LPARAM)&s->inst, 0);
			TRACE("msacm: stream open by %s: %s", i->name,
			      msacm_error_name(r, n1));
			if (r) {
				acm_driver_close(d);
			} else {
				s->drv = d;
				s->own_driver = 1;
			}
		}
		if (r && r != MMSYSERR_NOMEM)
			r = ACMERR_NOTPOSSIBLE;
	}
	TRACE("msacm: acmStreamOpen(tag 0x%04x -> 0x%04x, flags 0x%x) = %s",
	      src->wFormatTag, dst->wFormatTag, (unsigned)flags,
	      msacm_error_name(r, n2));
	if (r || query) {
		/* A query leaves nothing open: drivers don't allocate for one. */
		if (!s->own_driver)
			s->drv = NULL;
		stream_free(s);
	} else {
		s->next = streams;
		streams = s;
		*out = (HACMSTREAM)s;
	}
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmStreamClose(HACMSTREAM has, DWORD flags)
{
	MMRESULT r;

	if (flags)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	struct acm_stream *s = find_stream(has);
	if (!s) {
		r = MMSYSERR_INVALHANDLE;
	} else if (!(r = acm_message(s->drv, ACMDM_STREAM_CLOSE,
				     (LPARAM)&s->inst, 0))) {
		for (struct acm_stream **p = &streams; *p; p = &(*p)->next)
			if (*p == s) {
				*p = s->next;
				break;
			}
		if (!s->own_driver)
			s->drv = NULL;
		stream_free(s);
	}
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmStreamSize(HACMSTREAM has, DWORD in,
					 LPDWORD out, DWORD flags)
{
	ACMDRVSTREAMSIZE size = { sizeof(size), flags, 0, 0 };
	MMRESULT r;

	if (!out)
		return MMSYSERR_INVALPARAM;
	*out = 0;
	switch (flags & ACM_STREAMSIZEF_QUERYMASK) {
	case ACM_STREAMSIZEF_SOURCE:
		size.cbSrcLength = in;
		break;
	case ACM_STREAMSIZEF_DESTINATION:
		size.cbDstLength = in;
		break;
	default:
		return MMSYSERR_INVALFLAG;
	}
	if (flags & ~(DWORD)ACM_STREAMSIZEF_QUERYMASK)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	struct acm_stream *s = find_stream(has);
	if (!s) {
		r = MMSYSERR_INVALHANDLE;
	} else if (!(r = acm_message(s->drv, ACMDM_STREAM_SIZE,
				     (LPARAM)&s->inst, (LPARAM)&size))) {
		*out = (flags & ACM_STREAMSIZEF_QUERYMASK) ==
		       ACM_STREAMSIZEF_SOURCE ? size.cbDstLength :
		       size.cbSrcLength;
	}
	acm_unlock();
	return r;
}

/* The header as the driver sees it, if it's a header of has's. */
static MMRESULT header(HACMSTREAM has, LPACMSTREAMHEADER h,
		       struct acm_stream **s)
{
	if (!h || h->cbStruct < sizeof(ACMSTREAMHEADER))
		return MMSYSERR_INVALPARAM;
	if (!(*s = find_stream(has)))
		return MMSYSERR_INVALHANDLE;
	return MMSYSERR_NOERROR;
}

/* Whether its buffers are still the ones prepared. */
static int as_prepared(const ACMDRVSTREAMHEADER *d)
{
	return d->pbPreparedSrc == d->pbSrc &&
	       d->cbPreparedSrcLength >= d->cbSrcLength &&
	       d->pbPreparedDst == d->pbDst &&
	       d->cbPreparedDstLength >= d->cbDstLength;
}

MMRESULT STUB_WINAPI msacm_acmStreamPrepareHeader(HACMSTREAM has,
						  LPACMSTREAMHEADER h,
						  DWORD flags)
{
	struct acm_stream *s;
	MMRESULT r;

	if (flags)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	if ((r = header(has, h, &s)))
		goto done;
	ACMDRVSTREAMHEADER *d = (ACMDRVSTREAMHEADER *)h;
	if (d->fdwStatus & ACMSTREAMHEADER_STATUSF_PREPARED) {
		r = MMSYSERR_NOERROR;	/* already */
		goto done;
	}
	d->fdwConvert = flags;
	d->padshNext = NULL;
	d->fdwDriver = 0;
	d->dwDriver = 0;
	d->fdwPrepared = 0;
	d->dwPrepared = 0;
	d->pbPreparedSrc = d->pbPreparedDst = NULL;
	d->cbPreparedSrcLength = d->cbPreparedDstLength = 0;
	r = acm_message(s->drv, ACMDM_STREAM_PREPARE, (LPARAM)&s->inst,
			(LPARAM)d);
	/* A driver with nothing to prepare says it doesn't. */
	if (r == MMSYSERR_NOERROR || r == MMSYSERR_NOTSUPPORTED) {
		r = MMSYSERR_NOERROR;
		d->fdwStatus &= ~(DWORD)(ACMSTREAMHEADER_STATUSF_INQUEUE |
					 ACMSTREAMHEADER_STATUSF_DONE);
		d->fdwStatus |= ACMSTREAMHEADER_STATUSF_PREPARED;
		d->fdwPrepared = d->fdwStatus;
		d->pbPreparedSrc = d->pbSrc;
		d->cbPreparedSrcLength = d->cbSrcLength;
		d->pbPreparedDst = d->pbDst;
		d->cbPreparedDstLength = d->cbDstLength;
	}
done:
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmStreamUnprepareHeader(HACMSTREAM has,
						    LPACMSTREAMHEADER h,
						    DWORD flags)
{
	struct acm_stream *s;
	MMRESULT r;

	if (flags)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	if ((r = header(has, h, &s)))
		goto done;
	ACMDRVSTREAMHEADER *d = (ACMDRVSTREAMHEADER *)h;
	if (!(d->fdwStatus & ACMSTREAMHEADER_STATUSF_PREPARED)) {
		r = ACMERR_UNPREPARED;
		goto done;
	}
	if (!as_prepared(d)) {
		r = MMSYSERR_INVALPARAM;
		goto done;
	}
	d->fdwConvert = flags;
	r = acm_message(s->drv, ACMDM_STREAM_UNPREPARE, (LPARAM)&s->inst,
			(LPARAM)d);
	if (r == MMSYSERR_NOERROR || r == MMSYSERR_NOTSUPPORTED) {
		r = MMSYSERR_NOERROR;
		d->fdwStatus &= ~(DWORD)(ACMSTREAMHEADER_STATUSF_DONE |
					 ACMSTREAMHEADER_STATUSF_INQUEUE |
					 ACMSTREAMHEADER_STATUSF_PREPARED);
	}
done:
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmStreamConvert(HACMSTREAM has,
					    LPACMSTREAMHEADER h, DWORD flags)
{
	struct acm_stream *s;
	MMRESULT r;

	if (flags & ~(DWORD)(ACM_STREAMCONVERTF_BLOCKALIGN |
			     ACM_STREAMCONVERTF_START | ACM_STREAMCONVERTF_END))
		return MMSYSERR_INVALFLAG;
	acm_lock();
	if ((r = header(has, h, &s)))
		goto done;
	ACMDRVSTREAMHEADER *d = (ACMDRVSTREAMHEADER *)h;
	if (!(d->fdwStatus & ACMSTREAMHEADER_STATUSF_PREPARED)) {
		r = ACMERR_UNPREPARED;
		goto done;
	}
	if (!as_prepared(d)) {
		r = MMSYSERR_INVALPARAM;
		goto done;
	}
	d->fdwConvert = flags;
	d->fdwStatus &= ~(DWORD)ACMSTREAMHEADER_STATUSF_DONE;
	d->cbSrcLengthUsed = d->cbDstLengthUsed = 0;
	r = acm_message(s->drv, ACMDM_STREAM_CONVERT, (LPARAM)&s->inst,
			(LPARAM)d);
	if (!r) {
		/* What a driver says it used can't be more than it had. */
		if (d->cbSrcLengthUsed > d->cbSrcLength)
			d->cbSrcLengthUsed = d->cbSrcLength;
		if (d->cbDstLengthUsed > d->cbDstLength)
			d->cbDstLengthUsed = d->cbDstLength;
		d->fdwStatus |= ACMSTREAMHEADER_STATUSF_DONE;
	}
done:
	acm_unlock();
	return r;
}

MMRESULT STUB_WINAPI msacm_acmStreamReset(HACMSTREAM has, DWORD flags)
{
	MMRESULT r = MMSYSERR_NOERROR;

	if (flags)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	struct acm_stream *s = find_stream(has);
	if (!s)
		r = MMSYSERR_INVALHANDLE;
	else if (s->inst.fdwOpen & ACM_STREAMOPENF_ASYNC)
		/* A synchronous stream has nothing queued to stop. */
		r = acm_message(s->drv, ACMDM_STREAM_RESET, (LPARAM)&s->inst, 0);
	acm_unlock();
	return r;
}

WINFN(MMRESULT) msacm_acmStreamMessage(HACMSTREAM has, UINT msg, LPARAM l1,
				       LPARAM l2)
{
	/* No stream messages are defined for applications to send. */
	TRACE("msacm: acmStreamMessage(%p, 0x%x, 0x%lx, 0x%lx): not supported",
	      (void *)has, msg, (unsigned long)l1, (unsigned long)l2);
	return MMSYSERR_NOTSUPPORTED;
}

#define E(name) { #name, 0, (void *)msacm_##name }

static const struct builtin_export exports[] = {
	E(acmDriverAddA),
	E(acmDriverAddW),
	E(acmDriverClose),
	E(acmDriverDetailsA),
	E(acmDriverDetailsW),
	E(acmDriverEnum),
	E(acmDriverID),
	E(acmDriverMessage),
	E(acmDriverOpen),
	E(acmDriverPriority),
	E(acmDriverRemove),
	E(acmGetVersion),
	E(acmMetrics),
	E(acmStreamClose),
	E(acmStreamConvert),
	E(acmStreamMessage),
	E(acmStreamOpen),
	E(acmStreamPrepareHeader),
	E(acmStreamReset),
	E(acmStreamSize),
	E(acmStreamUnprepareHeader),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = {
	exports, msacm_format_exports, NULL
};

const struct builtin_dll builtin_msacm32 = { "msacm32.dll", NULL, tables, 0 };
