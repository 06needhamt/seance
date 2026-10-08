/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Finding DirectShow filters: running a filter server's DllRegisterServer
 * with quartz capturing what it registers (in a registry sandbox, or for
 * real), and reading the filters registered on the machine back from the
 * registry, in the form Windows' filter mappers write.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/error.h"
#include "common/fourcc.h"
#include "common/log.h"
#include "dshow/discover.h"
#include "loader/modules.h"
#include "runtime/crash.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "win32/ole.h"
#include "win32/path.h"
#include "win32/reg.h"

/* HKEY_CLASSES_ROOT: the user's classes, then the machine's. */
static const char *const class_roots[] = {
	"HKEY_CURRENT_USER\\Software\\Classes",
	"HKEY_LOCAL_MACHINE\\Software\\Classes",
};
#define NROOTS (sizeof(class_roots) / sizeof(*class_roots))

static const GUID guid_null;

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

/* Roles. */

static int fourcc_shaped(const GUID *g)
{
	static const BYTE tail[8] = { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38,
				      0x9b, 0x71 };
	return g->Data2 == 0 && g->Data3 == 0x0010 &&
	       !memcmp(g->Data4, tail, sizeof(tail));
}

int dshow_uncompressed_video(const GUID *minor)
{
	static const GUID *const rgb[] = {
		&MEDIASUBTYPE_RGB1, &MEDIASUBTYPE_RGB4, &MEDIASUBTYPE_RGB8,
		&MEDIASUBTYPE_RGB555, &MEDIASUBTYPE_RGB565,
		&MEDIASUBTYPE_RGB24, &MEDIASUBTYPE_RGB32,
		&MEDIASUBTYPE_ARGB32,
	};
	static const FOURCC yuv[] = {
		mmioFOURCC('Y', 'U', 'Y', '2'), mmioFOURCC('U', 'Y', 'V', 'Y'),
		mmioFOURCC('Y', 'V', 'Y', 'U'), mmioFOURCC('Y', 'U', 'Y', 'V'),
		mmioFOURCC('Y', 'V', '1', '2'), mmioFOURCC('I', 'Y', 'U', 'V'),
		mmioFOURCC('I', '4', '2', '0'), mmioFOURCC('N', 'V', '1', '2'),
		mmioFOURCC('N', 'V', '2', '1'), mmioFOURCC('Y', '8', '0', '0'),
		mmioFOURCC('Y', '8', ' ', ' '), mmioFOURCC('G', 'R', 'E', 'Y'),
		mmioFOURCC('Y', '4', '1', 'P'), mmioFOURCC('Y', 'V', 'U', '9'),
		mmioFOURCC('A', 'Y', 'U', 'V'), mmioFOURCC('V', '4', '2', '2'),
	};

	for (size_t i = 0; i < sizeof(rgb) / sizeof(*rgb); i++)
		if (guid_eq(minor, rgb[i]))
			return 1;
	if (!fourcc_shaped(minor))
		return 0;
	for (size_t i = 0; i < sizeof(yuv) / sizeof(*yuv); i++)
		if (minor->Data1 == yuv[i])
			return 1;
	return 0;
}

int dshow_uncompressed_audio(const GUID *minor)
{
	return guid_eq(minor, &MEDIASUBTYPE_PCM) ||
	       guid_eq(minor, &MEDIASUBTYPE_IEEE_FLOAT);
}

/*
 * Whether a pin takes the major type (video or audio): compressed (want 1)
 * or not. A pin with no major type counts as either, one with no subtype
 * as uncompressed.
 */
static int pin_takes(const struct qz_pin *p, const GUID *major, int compressed)
{
	int audio = guid_eq(major, &MEDIATYPE_Audio);

	for (size_t i = 0; i < p->ntypes; i++) {
		const struct qz_type *t = &p->types[i];
		if (!guid_eq(&t->major, major) && !guid_eq(&t->major, &guid_null))
			continue;
		int raw = guid_eq(&t->minor, &guid_null) ||
			  (audio ? dshow_uncompressed_audio(&t->minor) :
				   dshow_uncompressed_video(&t->minor));
		if (compressed ? !raw : raw)
			return 1;
	}
	return 0;
}

/* Whether some input (or output) pin takes compressed (or raw) major. */
static int has_pin(const struct qz_filter *f, const GUID *major, int output,
		   int compressed)
{
	for (size_t i = 0; i < f->npins; i++)
		if (!!(f->pins[i].flags & REG_PINFLAG_B_OUTPUT) == output &&
		    pin_takes(&f->pins[i], major, compressed))
			return 1;
	return 0;
}

enum dshow_role dshow_role(const struct qz_filter *f)
{
	const GUID *v = &MEDIATYPE_Video, *a = &MEDIATYPE_Audio;

	if (has_pin(f, v, 0, 1) && has_pin(f, v, 1, 0))
		return DSHOW_VIDEO_DECODER;
	if (guid_eq(&f->category, &CLSID_VideoCompressorCategory) ||
	    (has_pin(f, v, 0, 0) && has_pin(f, v, 1, 1)))
		return DSHOW_VIDEO_ENCODER;
	if (has_pin(f, a, 0, 1) && has_pin(f, a, 1, 0))
		return DSHOW_AUDIO_DECODER;
	if (guid_eq(&f->category, &CLSID_AudioCompressorCategory) ||
	    (has_pin(f, a, 0, 0) && has_pin(f, a, 1, 1)))
		return DSHOW_AUDIO_ENCODER;
	return DSHOW_OTHER;
}

enum dshow_role dshow_codec_role(int audio, int encoder)
{
	return audio ? (encoder ? DSHOW_AUDIO_ENCODER : DSHOW_AUDIO_DECODER) :
		       (encoder ? DSHOW_VIDEO_ENCODER : DSHOW_VIDEO_DECODER);
}

const char *dshow_role_name(enum dshow_role role)
{
	switch (role) {
	case DSHOW_VIDEO_DECODER: return "video decoder";
	case DSHOW_VIDEO_ENCODER: return "video encoder";
	case DSHOW_AUDIO_DECODER: return "audio decoder";
	case DSHOW_AUDIO_ENCODER: return "audio encoder";
	default: return "other";
	}
}

int dshow_is_server(const struct pe_image *img)
{
	struct pe_export e;

	return !pe_find_export(img, "DllGetClassObject", 0, &e) &&
	       pe_find_export(img, "DriverProc", 0, &e);
}

/* Running a server's registration. */

typedef HRESULT (WINAPI *server_fn)(void);

static HRESULT call_server(struct module *m, const char *entry)
{
	server_fn fn = (server_fn)mod_get_proc(m, entry);
	struct crash_frame saved;

	TRACE("dshow: %s!%s", m->name, entry);
	crash_enter(entry, (const void *)fn, 0, NULL, &saved);
	HRESULT hr = fn();
	crash_leave(&saved);
	TRACE("dshow: %s!%s = 0x%08lx", m->name, entry, (unsigned long)hr);
	return hr;
}

/* A string value as UTF-8, malloc()ed; NULL if it isn't one. */
static char *value_sz(struct reg_key *k, const char *name)
{
	struct reg_value *v = k ? reg_get(k, name) : NULL;

	if (!v || (v->type != REG_SZ && v->type != REG_EXPAND_SZ))
		return NULL;
	size_t n = v->size / sizeof(WCHAR);
	WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
	if (!w)
		return NULL;
	memcpy(w, v->data, n * sizeof(WCHAR));
	w[n] = 0;
	char *s = utf16_to_utf8_dup(w);
	free(w);
	return s;
}

static WCHAR *value_wsz(struct reg_key *k, const char *name)
{
	char *s = value_sz(k, name);
	WCHAR *w = s ? utf8_to_utf16_dup(s) : NULL;

	free(s);
	return w;
}

static int value_dword(struct reg_key *k, const char *name, DWORD *out)
{
	struct reg_value *v = k ? reg_get(k, name) : NULL;

	if (!v || v->type != REG_DWORD || v->size != sizeof(DWORD))
		return 0;
	memcpy(out, v->data, sizeof(DWORD));
	return 1;
}

/* A GUID in a string, or in a key's name. */
static int parse_guid(const char *s, GUID *out)
{
	WCHAR *w = s ? utf8_to_utf16_dup(s) : NULL;
	int r = w ? ole_guid_parse(w, out) : -1;

	free(w);
	return r;
}

/* The InprocServer32 of clsid (UTF-8), the registry lock held. */
static char *server_path(const GUID *clsid)
{
	char g[OLE_GUID_CHARS], path[512];

	ole_guid_format(clsid, g);
	for (size_t i = 0; i < NROOTS; i++) {
		snprintf(path, sizeof(path), "%s\\CLSID\\%s\\InprocServer32",
			 class_roots[i], g);
		char *s = value_sz(reg_open(path), "");
		if (s && *s)
			return s;
		free(s);
	}
	return NULL;
}

char *dshow_server_path(const GUID *clsid)
{
	reg_lock();
	char *s = server_path(clsid);
	reg_unlock();
	return s;
}

static const char *base_name(const char *path)
{
	const char *b = path;

	for (const char *p = path; *p; p++)
		if (*p == '\\' || *p == '/')
			b = p + 1;
	return b;
}

/*
 * Whether a server path (a Windows path, as GetModuleFileName gave it)
 * names m: by file name, or as the same file once links are resolved.
 */
static int names_module(const char *path, const struct module *m)
{
	char a[PATH_MAX], b[PATH_MAX];

	if (!strcasecmp(base_name(path), m->name))
		return 1;
	char *u = path_to_unix(path);
	int same = u && m->path && realpath(u, a) && realpath(m->path, b) &&
		   !strcmp(a, b);
	free(u);
	return same;
}

/*
 * Keeps the filters whose class this module serves, as far as the
 * registry says: those with no InprocServer32, or one naming its file.
 */
static size_t keep_own(struct qz_filter *f, size_t n, const struct module *m)
{
	size_t kept = 0;

	reg_lock();
	for (size_t i = 0; i < n; i++) {
		char *path = server_path(&f[i].clsid);
		int own = !path || names_module(path, m);
		if (!own) {
			char g[OLE_GUID_CHARS];
			ole_guid_format(&f[i].clsid, g);
			TRACE("dshow: %s registered %s, served by %s", m->name,
			      g, path);
			qz_filter_clear(&f[i]);
		} else if (kept != i) {
			f[kept] = f[i];
			memset(&f[i], 0, sizeof(f[i]));
		}
		kept += own;
		free(path);
	}
	reg_unlock();
	return kept;
}

/*
 * Keeps the DMOs this module serves: those DMORegister recorded (with no
 * InprocServer32 or one naming its file), then those found in the
 * registry whose InprocServer32 names it. found is taken.
 */
static size_t own_dmos(struct dmo_reg *d, size_t n, struct dmo_reg *found,
		       size_t nfound, const struct module *m,
		       struct dmo_reg **out)
{
	struct dmo_reg *all = realloc(d, (n + nfound + 1) * sizeof(*all));
	size_t kept = 0;

	if (!all) {
		dmo_regs_free(d, n);
		dmo_regs_free(found, nfound);
		*out = NULL;
		return 0;
	}
	reg_lock();
	for (size_t i = 0; i < n + nfound; i++) {
		struct dmo_reg *r = i < n ? &all[i] : &found[i - n];
		char *path = server_path(&r->clsid);
		int own = path ? names_module(path, m) : i < n;
		for (size_t j = 0; j < kept && own; j++)
			own = !guid_eq(&all[j].clsid, &r->clsid) ||
			      !guid_eq(&all[j].category, &r->category);
		if (!own) {
			dmo_reg_clear(r);
		} else if (r != &all[kept]) {
			all[kept] = *r;
			memset(r, 0, sizeof(*r));
		}
		kept += own;
		free(path);
	}
	reg_unlock();
	free(found);
	if (!kept) {
		free(all);
		all = NULL;
	}
	*out = all;
	return kept;
}

static int run_server(const char *path, const char *entry, int sandbox,
		      struct dshow_server_found *out, HRESULT *result)
{
	memset(out, 0, sizeof(*out));
	*result = E_FAIL;
	if (runtime_enter())
		return -1;
	struct module *m = mod_load(path, NULL);
	if (!m)
		return -1;
	if (!mod_get_proc(m, entry)) {
		vfw_set_error("%s has no %s", m->name, entry);
		mod_free(m);
		return -1;
	}
	if (sandbox && reg_sandbox_begin()) {
		vfw_set_error("the registry is already sandboxed");
		mod_free(m);
		return -1;
	}

	/* Most servers initialise COM themselves; this is for those that don't. */
	struct module *ole = mod_load("ole32.dll", NULL);
	HRESULT (WINAPI *init)(LPVOID) = ole ? (HRESULT (WINAPI *)(LPVOID))
		mod_get_proc(ole, "CoInitialize") : NULL;
	void (WINAPI *uninit)(void) = ole ? (void (WINAPI *)(void))
		mod_get_proc(ole, "CoUninitialize") : NULL;
	int inited = init && uninit && SUCCEEDED(init(NULL));

	quartz_capture_begin();
	msdmo_capture_begin();
	*result = call_server(m, entry);
	struct qz_filter *f = NULL;
	size_t count = quartz_capture_end(&f);
	count = keep_own(f, count, m);
	struct dmo_reg *d = NULL, *found = NULL;
	size_t ndmos = msdmo_capture_end(&d), nfound = 0;
	int registering = strcmp(entry, "DllUnregisterServer") != 0;
	if (registering)
		nfound = msdmo_registered(&found);
	ndmos = own_dmos(d, ndmos, found, nfound, m, &d);

	if (inited)
		uninit();
	if (sandbox)
		reg_sandbox_end();
	if (ole)
		mod_free(ole);
	mod_free(m);
	if (!count) {
		free(f);
		f = NULL;
	}
	out->filters = f;
	out->nfilters = count;
	out->dmos = d;
	out->ndmos = ndmos;
	return 0;
}

void dshow_server_found_free(struct dshow_server_found *f)
{
	qz_filters_free(f->filters, f->nfilters);
	dmo_regs_free(f->dmos, f->ndmos);
	memset(f, 0, sizeof(*f));
}

int dshow_server_discover(const char *path, struct dshow_server_found *out,
			  HRESULT *hr)
{
	return run_server(path, "DllRegisterServer", 1, out, hr);
}

int dshow_server_register(const char *path, int unregister,
			  struct dshow_server_found *out, HRESULT *hr)
{
	return run_server(path, unregister ? "DllUnregisterServer" :
			  "DllRegisterServer", 0, out, hr);
}

/* The filters alone. */
static int filters_only(int r, struct dshow_server_found *found,
			struct qz_filter **out, size_t *n)
{
	*out = found->filters;
	*n = found->nfilters;
	found->filters = NULL;
	found->nfilters = 0;
	dshow_server_found_free(found);
	return r;
}

int dshow_discover(const char *path, struct qz_filter **out, size_t *n,
		   HRESULT *hr)
{
	struct dshow_server_found found;
	int r = dshow_server_discover(path, &found, hr);

	return filters_only(r, &found, out, n);
}

int dshow_register(const char *path, int unregister, struct qz_filter **out,
		   size_t *n, HRESULT *hr)
{
	struct dshow_server_found found;
	int r = dshow_server_register(path, unregister, &found, hr);

	return filters_only(r, &found, out, n);
}

/* Registered filters. */

struct found {
	struct dshow_registered *v;
	size_t n, cap;
};

static int found_has(const struct found *l, const GUID *clsid,
		     const GUID *category)
{
	for (size_t i = 0; i < l->n; i++)
		if (guid_eq(&l->v[i].f.clsid, clsid) &&
		    guid_eq(&l->v[i].f.category, category))
			return 1;
	return 0;
}

/* Adds f (taking what it holds), with its server's path. */
static void found_add(struct found *l, struct qz_filter *f)
{
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 16;
		struct dshow_registered *v = realloc(l->v, cap * sizeof(*v));
		if (!v) {
			qz_filter_clear(f);
			return;
		}
		l->v = v;
		l->cap = cap;
	}
	l->v[l->n].f = *f;
	l->v[l->n].path = server_path(&f->clsid);
	l->n++;
	memset(f, 0, sizeof(*f));
}

/* The filters under HKCR\CLSID\{category}\Instance in one root. */
static void read_category(struct found *l, const char *root, const GUID *cat)
{
	char g[OLE_GUID_CHARS], path[512];

	ole_guid_format(cat, g);
	snprintf(path, sizeof(path), "%s\\CLSID\\%s\\Instance", root, g);
	struct reg_key *k = reg_open(path);
	for (size_t i = 0; k && i < k->nsub; i++) {
		struct reg_key *inst = k->sub[i];
		struct qz_filter f;
		GUID clsid;
		char *s = value_sz(inst, "CLSID");
		int bad = parse_guid(s ? s : inst->name, &clsid);
		free(s);
		if (bad || found_has(l, &clsid, cat))
			continue;

		struct reg_value *data = reg_get(inst, "FilterData");
		if (!data || data->type != REG_BINARY ||
		    qz_filterdata_parse(data->data, data->size, &f))
			memset(&f, 0, sizeof(f));
		if (!data)
			value_dword(inst, "Merit", &f.merit);
		f.clsid = clsid;
		f.category = *cat;
		f.name = value_wsz(inst, "FriendlyName");
		found_add(l, &f);
	}
}

/* A legacy pin, HKCR\CLSID\{clsid}\Pins\name, with its Types. */
static int read_legacy_pin(struct reg_key *k, struct qz_pin *p)
{
	DWORD v;

	memset(p, 0, sizeof(*p));
	if (!(p->name = utf8_to_utf16_dup(k->name)))
		return -1;
	if (value_dword(k, "Direction", &v) && v)
		p->flags |= REG_PINFLAG_B_OUTPUT;
	if (value_dword(k, "AllowedZero", &v) && v)
		p->flags |= REG_PINFLAG_B_ZERO;
	if (value_dword(k, "AllowedMany", &v) && v)
		p->flags |= REG_PINFLAG_B_MANY;
	if (value_dword(k, "IsRendered", &v) && v)
		p->flags |= REG_PINFLAG_B_RENDERER;

	struct reg_key *types = NULL;
	for (size_t i = 0; i < k->nsub && !types; i++)
		if (!strcasecmp(k->sub[i]->name, "Types"))
			types = k->sub[i];
	for (size_t i = 0; types && i < types->nsub; i++) {
		struct reg_key *major = types->sub[i];
		for (size_t j = 0; j < major->nsub; j++) {
			struct qz_type t;
			if (parse_guid(major->name, &t.major) ||
			    parse_guid(major->sub[j]->name, &t.minor))
				continue;
			struct qz_type *nt = realloc(p->types, (p->ntypes + 1) *
						     sizeof(*nt));
			if (!nt)
				return -1;
			p->types = nt;
			p->types[p->ntypes++] = t;
		}
	}
	return 0;
}

/* The filters under HKCR\Filter in one root. */
static void read_legacy(struct found *l, const char *root)
{
	char path[512];

	snprintf(path, sizeof(path), "%s\\Filter", root);
	struct reg_key *k = reg_open(path);
	for (size_t i = 0; k && i < k->nsub; i++) {
		struct qz_filter f;
		memset(&f, 0, sizeof(f));
		if (parse_guid(k->sub[i]->name, &f.clsid) ||
		    found_has(l, &f.clsid, &CLSID_LegacyAmFilterCategory))
			continue;
		f.category = CLSID_LegacyAmFilterCategory;
		f.name = value_wsz(k->sub[i], "");
		f.merit = MERIT_NORMAL;

		char g[OLE_GUID_CHARS];
		ole_guid_format(&f.clsid, g);
		snprintf(path, sizeof(path), "%s\\CLSID\\%s", root, g);
		struct reg_key *c = reg_open(path);
		value_dword(c, "Merit", &f.merit);
		snprintf(path, sizeof(path), "%s\\CLSID\\%s\\Pins", root, g);
		struct reg_key *pins = reg_open(path);
		if (pins && pins->nsub &&
		    (f.pins = calloc(pins->nsub, sizeof(*f.pins))))
			for (size_t j = 0; j < pins->nsub; j++)
				if (!read_legacy_pin(pins->sub[j],
						     &f.pins[f.npins]))
					f.npins++;
		found_add(l, &f);
	}
}

/* The categories to look in: the usual three, and every one declared. */
static size_t categories(GUID **out)
{
	char g[OLE_GUID_CHARS], path[512];
	size_t n = 3;
	GUID *c = malloc(n * sizeof(*c));

	if (!c)
		return 0;
	c[0] = CLSID_LegacyAmFilterCategory;
	c[1] = CLSID_VideoCompressorCategory;
	c[2] = CLSID_AudioCompressorCategory;
	ole_guid_format(&CLSID_ActiveMovieCategories, g);
	for (size_t r = 0; r < NROOTS; r++) {
		snprintf(path, sizeof(path), "%s\\CLSID\\%s\\Instance",
			 class_roots[r], g);
		struct reg_key *k = reg_open(path);
		for (size_t i = 0; k && i < k->nsub; i++) {
			GUID cat;
			char *s = value_sz(k->sub[i], "CLSID");
			int bad = parse_guid(s ? s : k->sub[i]->name, &cat);
			free(s);
			int dup = 0;
			for (size_t j = 0; j < n && !dup; j++)
				dup = guid_eq(&c[j], &cat);
			if (bad || dup)
				continue;
			GUID *nc = realloc(c, (n + 1) * sizeof(*nc));
			if (!nc)
				break;
			c = nc;
			c[n++] = cat;
		}
	}
	*out = c;
	return n;
}

size_t dshow_registered_filters(struct dshow_registered **out)
{
	struct found l = { 0 };
	GUID *cats = NULL;

	reg_lock();
	size_t ncats = categories(&cats);
	for (size_t r = 0; r < NROOTS; r++) {
		for (size_t i = 0; i < ncats; i++)
			read_category(&l, class_roots[r], &cats[i]);
		read_legacy(&l, class_roots[r]);
	}
	reg_unlock();
	free(cats);
	*out = l.v;
	return l.n;
}

void dshow_registered_free(struct dshow_registered *r, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		qz_filter_clear(&r[i].f);
		free(r[i].path);
	}
	free(r);
}

/* Printing. */

#define NAME(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) { &n, #n },

static const struct {
	const GUID *guid;
	const char *name;
} guid_names[] = {
	VFW_DSHOW_GUIDS(NAME)
};

const char *dshow_guid_name(const GUID *g, char *buf)
{
	if (guid_eq(g, &guid_null)) {
		snprintf(buf, DSHOW_GUID_NAME_SIZE, "any");
		return buf;
	}
	for (size_t i = 0; i < sizeof(guid_names) / sizeof(*guid_names); i++) {
		if (guid_eq(g, guid_names[i].guid)) {
			snprintf(buf, DSHOW_GUID_NAME_SIZE, "%s",
				 guid_names[i].name);
			return buf;
		}
	}
	if (fourcc_shaped(g) && g->Data1 <= 0xffff) {
		/* A WAVEFORMATEX format tag rather than a FOURCC. */
		snprintf(buf, DSHOW_GUID_NAME_SIZE, "wave format 0x%04lx",
			 (unsigned long)g->Data1);
		return buf;
	}
	if (fourcc_shaped(g)) {
		char f[FOURCC_STR_SIZE];
		snprintf(buf, DSHOW_GUID_NAME_SIZE, "'%s'",
			 fourcc_str(g->Data1, f));
		return buf;
	}
	ole_guid_format(g, buf);
	return buf;
}

void dshow_print_pins(const struct qz_filter *f, int indent)
{
	char a[DSHOW_GUID_NAME_SIZE], b[DSHOW_GUID_NAME_SIZE];

	if (!f->npins)
		printf("%*sno pins registered\n", indent, "");
	for (size_t i = 0; i < f->npins; i++) {
		const struct qz_pin *p = &f->pins[i];
		char *name = p->name ? utf16_to_utf8_dup(p->name) : NULL;
		printf("%*s%s pin%s%s%s%s%s%s", indent, "",
		       (p->flags & REG_PINFLAG_B_OUTPUT) ? "output" : "input",
		       name ? " \"" : "", name ? name : "", name ? "\"" : "",
		       (p->flags & REG_PINFLAG_B_MANY) ? ", many" : "",
		       (p->flags & REG_PINFLAG_B_ZERO) ? ", optional" : "",
		       (p->flags & REG_PINFLAG_B_RENDERER) ? ", rendered" : "");
		if (p->has_category)
			printf(", category %s", dshow_guid_name(&p->category, a));
		if (p->nmediums)
			printf(", %zu medium(s)", p->nmediums);
		printf("\n");
		free(name);
		for (size_t j = 0; j < p->ntypes; j++)
			printf("%*s  %s / %s\n", indent, "",
			       dshow_guid_name(&p->types[j].major, a),
			       dshow_guid_name(&p->types[j].minor, b));
	}
}

/* A merit and, for the standard ones, its name. */
static void print_merit(DWORD merit)
{
	static const struct {
		DWORD merit;
		const char *name;
	} names[] = {
		{ MERIT_PREFERRED, "preferred" }, { MERIT_NORMAL, "normal" },
		{ MERIT_UNLIKELY, "unlikely" },
		{ MERIT_DO_NOT_USE, "do not use" },
		{ MERIT_SW_COMPRESSOR, "software compressor" },
		{ MERIT_HW_COMPRESSOR, "hardware compressor" },
	};

	printf("merit 0x%lx", (unsigned long)merit);
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (names[i].merit == merit)
			printf(" (%s)", names[i].name);
}

void dshow_print_filter(const struct qz_filter *f, const char *path,
			int pins)
{
	char g[OLE_GUID_CHARS], cat[DSHOW_GUID_NAME_SIZE];
	char *name = f->name ? utf16_to_utf8_dup(f->name) : NULL;

	ole_guid_format(&f->clsid, g);
	printf("  %s %s\n", g, name && *name ? name : "(no name)");
	printf("      %s, ", dshow_role_name(dshow_role(f)));
	print_merit(f->merit);
	printf(", category %s\n", dshow_guid_name(&f->category, cat));
	if (path)
		printf("      server %s\n", path);
	if (pins)
		dshow_print_pins(f, 6);
	free(name);
}
