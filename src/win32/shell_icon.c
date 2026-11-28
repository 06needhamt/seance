/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32: icons. ExtractIcon(Ex), ExtractAssociatedIcon(Ex),
 * SHDefExtractIcon, the system image lists and where a file's icon comes
 * from, for SHGetFileInfo and IExtractIcon.
 *
 * Icons come from RT_GROUP_ICON resources of PE files, numbered as Windows
 * numbers them (in resource directory order: named groups, then ids
 * ascending; a negative index is a resource id), and from .ico files. As
 * everywhere in this user32, an icon handle has no pixels behind it: it
 * exists when the icon does. shell32.dll itself is built in, so its own
 * icons (the document, application and folder ones) exist without a file.
 *
 * A file's icon is its own for programs and .ico files; else the one its
 * class names in HKEY_CLASSES_ROOT\<class>\DefaultIcon ("file,index", with
 * "%1" for the file itself), else shell32's document icon. The system
 * image lists hold each icon location once, shell32's first few first, so
 * index 3 is the folder as on Windows.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "common/error.h"
#include "loader/pe.h"
#include "win32/codepage.h"
#include "win32/commctrl.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"
#include "win32/reg.h"
#include "win32/shell.h"

extern const struct builtin_dll builtin_user32, builtin_comctl32;

#define RT_GROUP_ICON_ID 14
/* How many icons the built-in shell32.dll says it has. */
#define SHELL32_ICONS 50

/* The icon handle for an icon that exists. */
static HICON new_icon(void)
{
	HICON (WINAPI *load)(HINSTANCE, LPCWSTR) =
		builtin_export(&builtin_user32, "LoadIconW");
	return load(NULL, (LPCWSTR)(ULONG_PTR)32512);	/* IDI_APPLICATION */
}

static const char *base_of(const char *win)
{
	const char *b = win;
	for (const char *p = win; *p; p++)
		if (*p == '\\' || *p == '/' || (*p == ':' && p == win + 1))
			b = p + 1;
	return b;
}

/* Whether win names the built-in shell32.dll, which has no file. */
static int is_shell32(const char *win, const char *unix)
{
	struct stat st;
	return !strcasecmp(base_of(win), "shell32.dll") &&
	       (!unix || stat(unix, &st));
}

struct groups {
	int count;
	DWORD want_id;		/* a negative index's id, or 0 */
	int found;
	/* The last entry seen, as languages of a name come together. */
	DWORD last_id;
	char last_name[256];
	int any;
};

static int count_group(void *ctx, const struct pe_resource *r)
{
	struct groups *g = ctx;

	if (r->type_id != RT_GROUP_ICON_ID || r->type_name)
		return 0;
	const char *name = r->name_name ? r->name_name : "";
	if (g->any && r->name_id == g->last_id &&
	    !strcmp(name, g->last_name))
		return 0;	/* another language of the same one */
	g->any = 1;
	g->last_id = r->name_id;
	snprintf(g->last_name, sizeof(g->last_name), "%s", name);
	g->count++;
	if (g->want_id && !r->name_name && r->name_id == g->want_id)
		g->found = 1;
	return 0;
}

/*
 * The icons in a file: their count, or -1 when the file is there but
 * holds none it could (not a program or .ico), or -2 when it isn't there.
 * With want_id, *found says whether the group with that id is there.
 */
static int icon_count(const char *win, DWORD want_id, int *found)
{
	char *unix = path_to_unix(win);
	int n = -2;

	if (found)
		*found = 0;
	if (!unix)
		return -2;
	if (is_shell32(win, unix)) {
		free(unix);
		if (found)
			*found = 0;
		return SHELL32_ICONS;
	}
	void *buf = NULL;
	size_t len = 0;
	if (vfw_read_file(unix, &buf, &len)) {
		free(unix);
		return -2;
	}
	const BYTE *p = buf;
	if (len >= 6 && !p[0] && !p[1] && p[2] == 1 && !p[3]) {
		/* An .ico: its ICONDIR, each entry inside the file. */
		WORD k;
		memcpy(&k, p + 4, 2);
		n = (size_t)6 + 16u * k <= len ? k : -1;
	} else if (len >= 2 && p[0] == 'M' && p[1] == 'Z') {
		struct pe_image *img = pe_parse(buf, len);
		struct groups g = { 0 };
		g.want_id = want_id;
		n = -1;
		if (img && !pe_foreach_resource(img, count_group, &g)) {
			n = g.count;
			if (found)
				*found = g.found;
		}
		pe_free(img);
	} else {
		n = -1;
	}
	free(buf);
	free(unix);
	return n;
}

/* Whether the icon at index (a resource id when negative) is there. */
static int icon_exists(const char *win, int index)
{
	int found;
	int n = icon_count(win, index < 0 ? (DWORD)-index : 0, &found);

	if (n < 0)
		return 0;
	if (index < 0)
		return found || (is_shell32(win, NULL) && -index <= n);
	return index < n;
}

static char *to_utf8(const void *s, int wide)
{
	if (!s)
		return NULL;
	return wide ? utf16_to_utf8_dup(s) : acp_to_utf8(s);
}

/* Copies a UTF-8 string to an A or W buffer of cch characters. */
static void put_text(const char *s, void *buf, int cch, int wide)
{
	if (!buf || cch <= 0)
		return;
	if (wide) {
		if (cp_to_utf16(CP_UTF8, s, -1, buf, cch, 0) < 0)
			((WCHAR *)buf)[0] = 0;
		((WCHAR *)buf)[cch - 1] = 0;
	} else {
		char *a = utf8_to_acp_dup(s);
		snprintf(buf, cch, "%s", a ? a : "");
		free(a);
	}
}

/* ExtractIcon: the count for index -1, else the icon; 1 for a file
 * that is there but can't hold icons. */
static HICON extract_icon(const void *file, UINT index, int wide)
{
	char *win = to_utf8(file, wide);
	HICON r = NULL;

	if (win) {
		int n = icon_count(win, 0, NULL);
		if (index == (UINT)-1)
			r = n > 0 ? (HICON)(ULONG_PTR)n : NULL;
		else if (n == -1)
			r = (HICON)1;
		else if (icon_exists(win, (int)index))
			r = new_icon();
	}
	free(win);
	return r;
}

WINFN(HICON) sh_ExtractIconA(HINSTANCE inst, LPCSTR file, UINT index)
{
	return extract_icon(file, index, 0);
}

WINFN(HICON) sh_ExtractIconW(HINSTANCE inst, LPCWSTR file, UINT index)
{
	return extract_icon(file, index, 1);
}

/*
 * ExtractIconEx: n icons from index into large and small (either may be
 * NULL); with index -1 and both NULL, the count. A negative index is one
 * icon by resource id. Returns how many were extracted.
 */
static UINT extract_icon_ex(const void *file, int index, HICON *large,
			    HICON *small, UINT n, int wide)
{
	char *win = to_utf8(file, wide);
	UINT got = 0;

	if (!win)
		return 0;
	int count = icon_count(win, 0, NULL);
	if (index == -1 && !large && !small) {
		free(win);
		return count > 0 ? (UINT)count : 0;
	}
	if (index < 0)
		n = 1;
	for (UINT i = 0; i < n; i++) {
		int at = index < 0 ? index : index + (int)i;
		if (!icon_exists(win, at))
			break;
		if (large)
			large[i] = new_icon();
		if (small)
			small[i] = new_icon();
		got++;
	}
	free(win);
	return got;
}

WINFN(UINT) sh_ExtractIconExA(LPCSTR file, int index, HICON *large,
			      HICON *small, UINT n)
{
	return extract_icon_ex(file, index, large, small, n, 0);
}

WINFN(UINT) sh_ExtractIconExW(LPCWSTR file, int index, HICON *large,
			      HICON *small, UINT n)
{
	return extract_icon_ex(file, index, large, small, n, 1);
}

/* The default value of an HKEY_CLASSES_ROOT key, as UTF-8 (the user's
 * classes first, then the machine's). */
static char *class_value(const char *sub)
{
	static const char *const roots[] = {
		"HKEY_CURRENT_USER\\Software\\Classes\\",
		"HKEY_LOCAL_MACHINE\\Software\\Classes\\",
	};
	char *r = NULL;

	reg_lock();
	for (int i = 0; !r && i < 2; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s%s", roots[i], sub);
		struct reg_key *k = reg_open(path);
		struct reg_value *v = k ? reg_get(k, "") : NULL;
		if (v && (v->type == REG_SZ || v->type == REG_EXPAND_SZ) &&
		    v->size >= 2) {
			WCHAR *w = malloc(v->size + 2);
			if (w) {
				memcpy(w, v->data, v->size);
				w[v->size / 2] = 0;
				r = utf16_to_utf8_dup(w);
				free(w);
			}
		}
	}
	reg_unlock();
	if (r && strchr(r, '%') && !strstr(r, "%1")) {
		char *e = k32_expand_env(r);
		if (e) {
			free(r);
			r = e;
		}
	}
	return r;
}

/* shell32's own file name, for icon locations in it. */
static char *shell32_path(void)
{
	char *sys = shell_folder_path(0x25);	/* CSIDL_SYSTEM */
	char *r = malloc((sys ? strlen(sys) : 20) + 16);

	if (r)
		sprintf(r, "%s\\shell32.dll", sys ? sys : "C:\\WINDOWS\\system32");
	free(sys);
	return r;
}

/* A "file,index" icon location from DefaultIcon, %1 being win. */
static char *parse_location(const char *spec, const char *win, int *index)
{
	char *s = strdup(spec);
	if (!s)
		return NULL;
	char *comma = strrchr(s, ',');
	*index = 0;
	if (comma) {
		*comma = 0;
		*index = atoi(comma + 1);
	}
	char *f = s;
	while (*f == ' ')
		f++;
	size_t n = strlen(f);
	if (n >= 2 && f[0] == '"' && f[n - 1] == '"') {
		f[n - 1] = 0;
		f++;
	}
	char *r = !strcmp(f, "%1") ? strdup(win) : strdup(f);
	free(s);
	return r;
}

/*
 * Where a file's icon is: a file name (malloc()ed) and index. With
 * known_attrs the file isn't looked at, only its name and attrs; open
 * asks for an open folder's icon.
 */
char *shell_icon_location(const char *win, int known_attrs, DWORD attrs,
			  int open, int *index)
{
	int dir;
	char *unix = path_to_unix(win);
	struct stat st;

	if (known_attrs)
		dir = !!(attrs & FILE_ATTRIBUTE_DIRECTORY);
	else
		dir = unix && !stat(unix, &st) && S_ISDIR(st.st_mode);
	free(unix);
	if (dir) {
		*index = open ? 4 : 3;
		return shell32_path();
	}
	const char *base = base_of(win);
	const char *dot = strrchr(base, '.');
	int program = dot && (!strcasecmp(dot, ".exe") ||
			      !strcasecmp(dot, ".scr"));
	if (!known_attrs && ((dot && !strcasecmp(dot, ".ico")) || program)) {
		if (icon_exists(win, 0)) {
			*index = 0;
			return strdup(win);
		}
	}
	if (program) {
		*index = 2;
		return shell32_path();
	}
	if (dot && dot[1]) {
		char *cls = class_value(dot);
		char *spec = NULL;
		if (cls && *cls) {
			char sub[300];
			snprintf(sub, sizeof(sub), "%s\\DefaultIcon", cls);
			spec = class_value(sub);
		}
		if (!spec) {
			char sub[300];
			snprintf(sub, sizeof(sub), "%s\\DefaultIcon", dot);
			spec = class_value(sub);
		}
		free(cls);
		if (spec && *spec) {
			char *r = parse_location(spec, win, index);
			free(spec);
			if (r)
				return r;
		} else {
			free(spec);
		}
	}
	*index = 0;
	return shell32_path();
}

/* The icon of a virtual folder: My Computer, the Recycle Bin, the
 * desktop, as shell32 numbers them. */
char *shell_icon_csidl(int csidl, int *index)
{
	switch (csidl) {
	case 0x11:	/* CSIDL_DRIVES */
		*index = 15;
		break;
	case 0x0a:	/* CSIDL_BITBUCKET */
		*index = 31;
		break;
	case 0x00:	/* CSIDL_DESKTOP */
		*index = 34;
		break;
	default:
		*index = 3;
		break;
	}
	return shell32_path();
}

/*
 * ExtractAssociatedIcon: the file's own icon at *index, else the one its
 * class has, whose file and index are written back to path (a
 * MAX_PATH buffer).
 */
static HICON associated(void *path, WORD *index, int wide)
{
	char *win = to_utf8(path, wide);
	HICON r = NULL;

	if (!win || !index) {
		free(win);
		return NULL;
	}
	if (icon_exists(win, *index)) {
		r = new_icon();
	} else {
		int at;
		char *loc = shell_icon_location(win, 0, 0, 0, &at);
		if (loc && !icon_exists(loc, at)) {
			free(loc);
			loc = shell32_path();
			at = 0;
		}
		if (loc) {
			put_text(loc, path, MAX_PATH, wide);
			*index = (WORD)at;
			r = new_icon();
		}
		free(loc);
	}
	free(win);
	return r;
}

WINFN(HICON) sh_ExtractAssociatedIconA(HINSTANCE inst, LPSTR path,
				       LPWORD index)
{
	return associated(path, index, 0);
}

WINFN(HICON) sh_ExtractAssociatedIconW(HINSTANCE inst, LPWSTR path,
				       LPWORD index)
{
	return associated(path, index, 1);
}

/* The Ex forms also give the icon's id; here it is its index. */
WINFN(HICON) sh_ExtractAssociatedIconExA(HINSTANCE inst, LPSTR path,
					 LPWORD index, LPWORD id)
{
	HICON r = associated(path, index, 0);
	if (r && id)
		*id = *index;
	return r;
}

WINFN(HICON) sh_ExtractAssociatedIconExW(HINSTANCE inst, LPWSTR path,
					 LPWORD index, LPWORD id)
{
	HICON r = associated(path, index, 1);
	if (r && id)
		*id = *index;
	return r;
}

/* SHDefExtractIcon and IExtractIcon::Extract: S_OK, or S_FALSE when the
 * file has no such icon. */
HRESULT shell_extract_icon(const char *win, int index, HICON *large,
			   HICON *small)
{
	if (!win)
		return E_OUTOFMEMORY;
	if (!icon_exists(win, index))
		return S_FALSE;
	if (large)
		*large = new_icon();
	if (small)
		*small = new_icon();
	return S_OK;
}

static HRESULT def_extract(const void *file, int index, HICON *large,
			   HICON *small, int wide)
{
	char *win = to_utf8(file, wide);
	HRESULT hr = shell_extract_icon(win, index, large, small);
	free(win);
	return hr;
}

WINFN(HRESULT) sh_SHDefExtractIconA(LPCSTR file, int index, UINT flags,
				    HICON *large, HICON *small, UINT size)
{
	return def_extract(file, index, large, small, 0);
}

WINFN(HRESULT) sh_SHDefExtractIconW(LPCWSTR file, int index, UINT flags,
				    HICON *large, HICON *small, UINT size)
{
	return def_extract(file, index, large, small, 1);
}

/* The system image lists, and the icon location of each entry. */
static pthread_mutex_t sys_lock = PTHREAD_MUTEX_INITIALIZER;
static HIMAGELIST sys_large, sys_small;
static struct sys_entry {
	char *file;
	int index;
} *sys_entry;
static int sys_count, sys_cap;

/* With sys_lock held. */
static int sys_add(const char *file, int index)
{
	INT (WINAPI *add)(HIMAGELIST, HICON) =
		builtin_export(&builtin_comctl32, "ImageList_AddIcon");

	for (int i = 0; i < sys_count; i++)
		if (sys_entry[i].index == index &&
		    !strcasecmp(sys_entry[i].file, file))
			return i;
	if (sys_count == sys_cap) {
		int cap = sys_cap ? sys_cap * 2 : 32;
		struct sys_entry *e = realloc(sys_entry, cap * sizeof(*e));
		if (!e)
			return -1;
		sys_entry = e;
		sys_cap = cap;
	}
	char *f = strdup(file);
	HICON icon = new_icon();
	if (!f || add(sys_large, icon) != sys_count ||
	    add(sys_small, icon) != sys_count) {
		free(f);
		return -1;
	}
	sys_entry[sys_count].file = f;
	sys_entry[sys_count].index = index;
	return sys_count++;
}

/* With sys_lock held: makes the lists, shell32's first icons first. */
static int sys_init(void)
{
	HIMAGELIST (WINAPI *create)(INT, INT, UINT, INT, INT) =
		builtin_export(&builtin_comctl32, "ImageList_Create");

	if (sys_large)
		return 0;
	sys_large = create(32, 32, ILC_COLOR32 | ILC_MASK, 16, 16);
	sys_small = create(16, 16, ILC_COLOR32 | ILC_MASK, 16, 16);
	if (!sys_large || !sys_small)
		return -1;
	char *s = shell32_path();
	for (int i = 0; s && i < 5; i++)
		sys_add(s, i);
	free(s);
	return 0;
}

/* The system image list index of an icon location, or -1. */
int shell_sys_index(const char *file, int index)
{
	int r = -1;

	pthread_mutex_lock(&sys_lock);
	if (!sys_init())
		r = sys_add(file, index);
	pthread_mutex_unlock(&sys_lock);
	return r;
}

HIMAGELIST shell_sys_list(int small)
{
	HIMAGELIST r;

	pthread_mutex_lock(&sys_lock);
	sys_init();
	r = small ? sys_small : sys_large;
	pthread_mutex_unlock(&sys_lock);
	return r;
}

HICON shell_new_icon(void)
{
	return new_icon();
}

/* Shell_GetImageLists (ordinal 71): the system image lists. */
WINFN(BOOL) sh_Shell_GetImageLists(HIMAGELIST *large, HIMAGELIST *small)
{
	HIMAGELIST l = shell_sys_list(0), s = shell_sys_list(1);

	if (large)
		*large = l;
	if (small)
		*small = s;
	return l && s;
}

/* FileIconInit (ordinal 660). */
WINFN(BOOL) sh_FileIconInit(BOOL restore)
{
	return shell_sys_list(0) != NULL;
}

/* Shell_GetCachedImageIndex (ordinal 72, Unicode on NT). */
static int cached_index(const void *file, int index, int wide)
{
	char *win = to_utf8(file, wide);
	int r = win ? shell_sys_index(win, index) : -1;
	free(win);
	return r;
}

WINFN(int) sh_Shell_GetCachedImageIndexW(LPCWSTR file, int index, UINT flags)
{
	return cached_index(file, index, 1);
}

WINFN(int) sh_Shell_GetCachedImageIndexA(LPCSTR file, int index, UINT flags)
{
	return cached_index(file, index, 0);
}

#define E(name) { #name, 0, (void *)sh_##name }

const struct builtin_export shell_icon_exports[] = {
	E(ExtractAssociatedIconA), E(ExtractAssociatedIconW),
	E(ExtractAssociatedIconExA), E(ExtractAssociatedIconExW),
	E(ExtractIconA), E(ExtractIconExA), E(ExtractIconExW),
	E(ExtractIconW),
	E(SHDefExtractIconA), E(SHDefExtractIconW),
	{ "Shell_GetImageLists", 71, (void *)sh_Shell_GetImageLists },
	{ "Shell_GetCachedImageIndex", 72,
	  (void *)sh_Shell_GetCachedImageIndexW },
	E(Shell_GetCachedImageIndexA), E(Shell_GetCachedImageIndexW),
	{ "FileIconInit", 660, (void *)sh_FileIconInit },
	BUILTIN_END
};
