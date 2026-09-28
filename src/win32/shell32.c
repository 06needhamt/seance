/*
 * shell32.dll, headless: special folders, item ID lists, command lines,
 * and the parts of the shell a codec may touch without needing its UI.
 *
 * - Special folders (SHGetFolderPath and friends) are laid out as on
 *   Windows XP, under drive C:, with the profile named after GetUserName
 *   ($USER): C:\Documents and Settings\<user>\Application Data and so on.
 *   Each is created when first asked for, as a real profile has them all.
 * - Item ID lists are this shell32's own: one item holding a Windows path
 *   (or a virtual folder's CSIDL), allocated with the COM task allocator.
 *   The desktop is the empty list, as on Windows.
 * - With nothing to show anything on, ShellExecute finds no program for
 *   what it is asked to open, SHBrowseForFolder is cancelled, ShellAbout
 *   and the tray icon calls succeed, and there are no icons, dropped
 *   files or appbars. Each says what it was asked on stderr, as
 *   MessageBox does.
 *
 * SHFileOperation, SHCreateDirectoryEx and SHGetFileInfo are in
 * shell_file.c.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"
#include "win32/shell.h"

extern const struct builtin_dll builtin_ole32;
extern const struct builtin_export shell_file_exports[];

/* Folders: where each CSIDL is, from the profile, All Users, or C:. */
enum base { ROOT, PROFILE, ALL_USERS, VIRTUAL };

static const struct folder {
	int csidl;
	enum base base;
	const char *sub;
} folders[] = {
	{ CSIDL_DESKTOP, PROFILE, "Desktop" },
	{ CSIDL_INTERNET, VIRTUAL, NULL },
	{ CSIDL_PROGRAMS, PROFILE, "Start Menu\\Programs" },
	{ CSIDL_CONTROLS, VIRTUAL, NULL },
	{ CSIDL_PRINTERS, VIRTUAL, NULL },
	{ CSIDL_PERSONAL, PROFILE, "My Documents" },
	{ CSIDL_FAVORITES, PROFILE, "Favorites" },
	{ CSIDL_STARTUP, PROFILE, "Start Menu\\Programs\\Startup" },
	{ CSIDL_RECENT, PROFILE, "Recent" },
	{ CSIDL_SENDTO, PROFILE, "SendTo" },
	{ CSIDL_BITBUCKET, VIRTUAL, NULL },
	{ CSIDL_STARTMENU, PROFILE, "Start Menu" },
	{ CSIDL_MYMUSIC, PROFILE, "My Documents\\My Music" },
	{ CSIDL_MYVIDEO, PROFILE, "My Documents\\My Videos" },
	{ CSIDL_DESKTOPDIRECTORY, PROFILE, "Desktop" },
	{ CSIDL_DRIVES, VIRTUAL, NULL },
	{ CSIDL_NETWORK, VIRTUAL, NULL },
	{ CSIDL_NETHOOD, PROFILE, "NetHood" },
	{ CSIDL_FONTS, ROOT, "WINDOWS\\Fonts" },
	{ CSIDL_TEMPLATES, PROFILE, "Templates" },
	{ CSIDL_COMMON_STARTMENU, ALL_USERS, "Start Menu" },
	{ CSIDL_COMMON_PROGRAMS, ALL_USERS, "Start Menu\\Programs" },
	{ CSIDL_COMMON_STARTUP, ALL_USERS, "Start Menu\\Programs\\Startup" },
	{ CSIDL_COMMON_DESKTOPDIRECTORY, ALL_USERS, "Desktop" },
	{ CSIDL_APPDATA, PROFILE, "Application Data" },
	{ CSIDL_PRINTHOOD, PROFILE, "PrintHood" },
	{ CSIDL_LOCAL_APPDATA, PROFILE, "Local Settings\\Application Data" },
	{ CSIDL_ALTSTARTUP, PROFILE, "Start Menu\\Programs\\Startup" },
	{ CSIDL_COMMON_ALTSTARTUP, ALL_USERS, "Start Menu\\Programs\\Startup" },
	{ CSIDL_COMMON_FAVORITES, ALL_USERS, "Favorites" },
	{ CSIDL_INTERNET_CACHE, PROFILE,
	  "Local Settings\\Temporary Internet Files" },
	{ CSIDL_COOKIES, PROFILE, "Cookies" },
	{ CSIDL_HISTORY, PROFILE, "Local Settings\\History" },
	{ CSIDL_COMMON_APPDATA, ALL_USERS, "Application Data" },
	{ CSIDL_WINDOWS, ROOT, "WINDOWS" },
	{ CSIDL_SYSTEM, ROOT, "WINDOWS\\system32" },
	{ CSIDL_PROGRAM_FILES, ROOT, "Program Files" },
	{ CSIDL_MYPICTURES, PROFILE, "My Documents\\My Pictures" },
	{ CSIDL_PROFILE, PROFILE, "" },
	{ CSIDL_SYSTEMX86, ROOT, "WINDOWS\\system32" },
	{ CSIDL_PROGRAM_FILESX86, ROOT, "Program Files" },
	{ CSIDL_PROGRAM_FILES_COMMON, ROOT, "Program Files\\Common Files" },
	{ CSIDL_PROGRAM_FILES_COMMONX86, ROOT, "Program Files\\Common Files" },
	{ CSIDL_COMMON_TEMPLATES, ALL_USERS, "Templates" },
	{ CSIDL_COMMON_DOCUMENTS, ALL_USERS, "Documents" },
	{ CSIDL_COMMON_ADMINTOOLS, ALL_USERS,
	  "Start Menu\\Programs\\Administrative Tools" },
	{ CSIDL_ADMINTOOLS, PROFILE,
	  "Start Menu\\Programs\\Administrative Tools" },
	{ CSIDL_CONNECTIONS, VIRTUAL, NULL },
	{ CSIDL_COMMON_MUSIC, ALL_USERS, "Documents\\My Music" },
	{ CSIDL_COMMON_PICTURES, ALL_USERS, "Documents\\My Pictures" },
	{ CSIDL_COMMON_VIDEO, ALL_USERS, "Documents\\My Videos" },
	{ CSIDL_RESOURCES, ROOT, "WINDOWS\\Resources" },
	{ CSIDL_CDBURN_AREA, PROFILE,
	  "Local Settings\\Application Data\\Microsoft\\CD Burning" },
	{ CSIDL_COMPUTERSNEARME, VIRTUAL, NULL },
};

static const struct folder *find_folder(int csidl)
{
	for (size_t i = 0; i < sizeof(folders) / sizeof(*folders); i++)
		if (folders[i].csidl == (csidl & ~CSIDL_FLAG_MASK))
			return &folders[i];
	return NULL;
}

int shell_mkdirs(const char *unix_path)
{
	char tmp[4096];
	struct stat st;

	if (snprintf(tmp, sizeof(tmp), "%s", unix_path) >= (int)sizeof(tmp))
		return -1;
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) && errno != EEXIST)
		return -1;
	return !stat(tmp, &st) && S_ISDIR(st.st_mode) ? 0 : -1;
}

/* The profile's name: GetUserName's, $USER or "user". */
static const char *user_name(void)
{
	const char *u = getenv("USER");
	return u && *u ? u : "user";
}

/*
 * A folder's Windows path (UTF-8, malloc()ed), created if it isn't there.
 * NULL with *hr set if there is none: E_FAIL for a virtual folder,
 * E_INVALIDARG for an unknown CSIDL.
 */
static char *folder_path(int csidl, HRESULT *hr)
{
	const struct folder *f = find_folder(csidl);
	char path[MAX_PATH + 64];

	if (!f || f->base == VIRTUAL) {
		*hr = f ? E_FAIL : E_INVALIDARG;
		return NULL;
	}
	if (f->base == ROOT)
		snprintf(path, sizeof(path), "C:\\%s", f->sub);
	else
		snprintf(path, sizeof(path), "C:\\Documents and Settings\\%s%s%s",
			 f->base == PROFILE ? user_name() : "All Users",
			 *f->sub ? "\\" : "", f->sub);

	char *u = path_to_unix(path);
	if (!u || shell_mkdirs(u)) {
		free(u);
		*hr = E_FAIL;
		return NULL;
	}
	free(u);
	*hr = S_OK;
	return strdup(path);
}

/* Copies a UTF-8 path out to a MAX_PATH buffer; FALSE if it can't. */
static BOOL path_out(const char *path, void *buf, int wide)
{
	if (!buf)
		return FALSE;
	if (wide) {
		int n = cp_to_utf16(CP_UTF8, path, -1, buf, MAX_PATH, 0);
		if (n < 0)
			((WCHAR *)buf)[0] = 0;
		return n > 0;
	}
	char *a = utf8_to_acp_dup(path);
	BOOL ok = a && strlen(a) < MAX_PATH;
	strcpy(buf, ok ? a : "");
	free(a);
	return ok;
}

static HRESULT get_folder_path(int csidl, const void *sub, int wide,
			       void *buf)
{
	HRESULT hr;
	char *path = folder_path(csidl, &hr);

	if (buf && wide)
		((WCHAR *)buf)[0] = 0;
	else if (buf)
		((char *)buf)[0] = 0;
	if (!path)
		return hr;
	if (sub) {
		char *s = wide ? utf16_to_utf8_dup(sub) : acp_to_utf8(sub);
		char *full = s ? malloc(strlen(path) + strlen(s) + 2) : NULL;
		if (!full) {
			free(s);
			free(path);
			return E_OUTOFMEMORY;
		}
		sprintf(full, "%s\\%s", path, s);
		free(s);
		free(path);
		path = full;
		char *u = path_to_unix(path);
		int bad = !u || ((csidl & CSIDL_FLAG_CREATE) ? shell_mkdirs(u) :
				 access(u, F_OK));
		free(u);
		if (bad) {
			free(path);
			return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
		}
	}
	hr = path_out(path, buf, wide) ? S_OK : E_FAIL;
	free(path);
	return hr;
}

WINFN(HRESULT) sh_SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token,
				   DWORD flags, LPSTR path)
{
	return get_folder_path(csidl, NULL, 0, path);
}

WINFN(HRESULT) sh_SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token,
				   DWORD flags, LPWSTR path)
{
	return get_folder_path(csidl, NULL, 1, path);
}

WINFN(HRESULT) sh_SHGetFolderPathAndSubDirA(HWND hwnd, int csidl,
					    HANDLE token, DWORD flags,
					    LPCSTR sub, LPSTR path)
{
	return get_folder_path(csidl, sub && *sub ? sub : NULL, 0, path);
}

WINFN(HRESULT) sh_SHGetFolderPathAndSubDirW(HWND hwnd, int csidl,
					    HANDLE token, DWORD flags,
					    LPCWSTR sub, LPWSTR path)
{
	return get_folder_path(csidl, sub && *sub ? sub : NULL, 1, path);
}

WINFN(BOOL) sh_SHGetSpecialFolderPathA(HWND hwnd, LPSTR path, int csidl,
				       BOOL create)
{
	return get_folder_path(csidl, NULL, 0, path) == S_OK;
}

WINFN(BOOL) sh_SHGetSpecialFolderPathW(HWND hwnd, LPWSTR path, int csidl,
				       BOOL create)
{
	return get_folder_path(csidl, NULL, 1, path) == S_OK;
}

/*
 * Item ID lists. One item: cb, a four-byte tag, then either a Windows
 * path in UTF-16 with its NUL ("VFWp") or a virtual folder's CSIDL
 * ("VFWv"); then the list's terminating zero cb.
 */
static const BYTE path_tag[4] = { 'V', 'F', 'W', 'p' };
static const BYTE virtual_tag[4] = { 'V', 'F', 'W', 'v' };

static LPITEMIDLIST pidl_alloc(const BYTE tag[4], const void *data,
			       size_t len)
{
	size_t item = 2 + 4 + len;
	BYTE *p;

	if (item > 0xffff || !(p = heap_alloc(heap_process(), 0, item + 2)))
		return NULL;
	p[0] = (BYTE)item;
	p[1] = (BYTE)(item >> 8);
	memcpy(p + 2, tag, 4);
	memcpy(p + 6, data, len);
	p[item] = p[item + 1] = 0;
	return (LPITEMIDLIST)p;
}

static LPITEMIDLIST pidl_from_path(const char *path)
{
	WCHAR *w = utf8_to_utf16_dup(path);
	LPITEMIDLIST pidl = w ? pidl_alloc(path_tag, w,
					   (wstrlen(w) + 1) * sizeof(WCHAR)) :
				NULL;
	free(w);
	return pidl;
}

static USHORT item_size(const BYTE *p)
{
	return (USHORT)(p[0] | p[1] << 8);
}

char *shell_pidl_path(LPCITEMIDLIST pidl)
{
	const BYTE *p = (const BYTE *)pidl;
	HRESULT hr;

	if (!p)
		return NULL;
	USHORT cb = item_size(p);
	if (!cb)
		return folder_path(CSIDL_DESKTOPDIRECTORY, &hr);
	if (cb < 8 || memcmp(p + 2, path_tag, 4) || item_size(p + cb))
		return NULL;
	size_t n = (cb - 6) / sizeof(WCHAR);
	WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
	if (!w)
		return NULL;
	memcpy(w, p + 6, n * sizeof(WCHAR));
	w[n] = 0;
	char *path = utf16_to_utf8_dup(w);
	free(w);
	return path;
}

static HRESULT folder_location(int csidl, LPITEMIDLIST *out)
{
	const struct folder *f = find_folder(csidl);
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!f)
		return E_INVALIDARG;
	if ((csidl & ~CSIDL_FLAG_MASK) == CSIDL_DESKTOP) {
		BYTE *p = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 2);
		*out = (LPITEMIDLIST)p;
		return p ? S_OK : E_OUTOFMEMORY;
	}
	if (f->base == VIRTUAL) {
		DWORD c = (DWORD)f->csidl;
		*out = pidl_alloc(virtual_tag, &c, sizeof(c));
		return *out ? S_OK : E_OUTOFMEMORY;
	}
	char *path = folder_path(csidl, &hr);
	if (!path)
		return hr;
	*out = pidl_from_path(path);
	free(path);
	return *out ? S_OK : E_OUTOFMEMORY;
}

WINFN(HRESULT) sh_SHGetFolderLocation(HWND hwnd, int csidl, HANDLE token,
				      DWORD reserved, LPITEMIDLIST *out)
{
	return folder_location(csidl, out);
}

WINFN(HRESULT) sh_SHGetSpecialFolderLocation(HWND hwnd, int csidl,
					     LPITEMIDLIST *out)
{
	return folder_location(csidl, out);
}

WINFN(BOOL) sh_SHGetPathFromIDListA(LPCITEMIDLIST pidl, LPSTR buf)
{
	char *path = shell_pidl_path(pidl);
	BOOL ok = path && path_out(path, buf, 0);

	if (!path && buf)
		buf[0] = 0;
	free(path);
	return ok;
}

WINFN(BOOL) sh_SHGetPathFromIDListW(LPCITEMIDLIST pidl, LPWSTR buf)
{
	char *path = shell_pidl_path(pidl);
	BOOL ok = path && path_out(path, buf, 1);

	if (!path && buf)
		buf[0] = 0;
	free(path);
	return ok;
}

WINFN(LPITEMIDLIST) sh_ILCreateFromPathA(LPCSTR path)
{
	char *u = acp_to_utf8(path);
	LPITEMIDLIST pidl = u ? pidl_from_path(u) : NULL;
	free(u);
	return pidl;
}

WINFN(LPITEMIDLIST) sh_ILCreateFromPathW(LPCWSTR path)
{
	char *u = utf16_to_utf8_dup(path);
	LPITEMIDLIST pidl = u ? pidl_from_path(u) : NULL;
	free(u);
	return pidl;
}

WINFN(UINT) sh_ILGetSize(LPCITEMIDLIST pidl)
{
	const BYTE *p = (const BYTE *)pidl;
	UINT n = 0;

	if (!p)
		return 0;
	for (USHORT cb; (cb = item_size(p + n)); )
		n += cb;
	return n + 2;
}

WINFN(LPITEMIDLIST) sh_ILClone(LPCITEMIDLIST pidl)
{
	UINT n = sh_ILGetSize(pidl);
	void *p = n ? heap_alloc(heap_process(), 0, n) : NULL;

	if (p)
		memcpy(p, pidl, n);
	return p;
}

WINFN(void) sh_ILFree(LPITEMIDLIST pidl)
{
	heap_free(heap_process(), 0, pidl);
}

/* The COM task allocator, which item ID lists come from. */
WINFN(HRESULT) sh_SHGetMalloc(LPMALLOC *out)
{
	HRESULT (WINAPI *get)(DWORD, LPMALLOC *) =
		builtin_export(&builtin_ole32, "CoGetMalloc");
	return get ? get(1, out) : E_FAIL;
}

/*
 * CommandLineToArgvW, as Windows splits a command line: the program name
 * up to the next space, or between quotes with no escapes; then
 * arguments split at blanks outside quotes, where 2n backslashes before a
 * quote are n and the quote opens or closes, 2n+1 are n and a literal
 * quote, and a third quote in a row is a literal one (after Wine). All in
 * one LocalAlloc block for LocalFree.
 */
static int is_blank(WCHAR c)
{
	return c == ' ' || c == '\t';
}

static int is_space(WCHAR c)
{
	return is_blank(c) || (c >= '\n' && c <= '\r');
}

static int count_args(const WCHAR *s)
{
	int argc = 1, qcount = 0, bcount = 0;

	if (*s == '"') {
		s++;
		while (*s)
			if (*s++ == '"')
				break;
	} else {
		while (*s && !is_space(*s))
			s++;
	}
	while (is_blank(*s))
		s++;
	if (*s)
		argc++;
	while (*s) {
		if (is_blank(*s) && !qcount) {
			while (is_blank(*s))
				s++;
			if (*s)
				argc++;
			bcount = 0;
		} else if (*s == '\\') {
			bcount++;
			s++;
		} else if (*s == '"') {
			if (!(bcount & 1))
				qcount++;
			s++;
			bcount = 0;
			while (*s == '"') {
				qcount++;
				s++;
			}
			qcount %= 3;
			if (qcount == 2)
				qcount = 0;
		} else {
			bcount = 0;
			s++;
		}
	}
	return argc;
}

WINFN(LPWSTR *) sh_CommandLineToArgvW(LPCWSTR cmd, int *nargs)
{
	WCHAR self[MAX_PATH];

	if (!nargs) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	if (!cmd || !*cmd) {
		/* The program's own path. */
		DWORD (WINAPI *name)(HMODULE, LPWSTR, DWORD) =
			builtin_export(builtin_find("kernel32.dll"),
				       "GetModuleFileNameW");
		if (!name || !name(NULL, self, MAX_PATH))
			self[0] = 0;
		size_t n = wstrlen(self);
		LPWSTR *argv = global_alloc(0, 2 * sizeof(LPWSTR) +
					    (n + 1) * sizeof(WCHAR));
		if (!argv)
			return NULL;
		argv[0] = (WCHAR *)(argv + 2);
		memcpy(argv[0], self, (n + 1) * sizeof(WCHAR));
		argv[1] = NULL;
		*nargs = 1;
		return argv;
	}

	int argc = count_args(cmd);
	size_t len = wstrlen(cmd);
	LPWSTR *argv = global_alloc(0, (size_t)(argc + 1) * sizeof(LPWSTR) +
				    (len + 1) * sizeof(WCHAR));
	if (!argv)
		return NULL;
	WCHAR *d = (WCHAR *)(argv + argc + 1);
	const WCHAR *s;
	memcpy(d, cmd, (len + 1) * sizeof(WCHAR));
	argv[0] = d;
	argc = 1;

	if (*d == '"') {
		s = d + 1;
		while (*s) {
			if (*s == '"') {
				s++;
				break;
			}
			*d++ = *s++;
		}
	} else {
		while (*d && !is_space(*d))
			d++;
		s = d;
		if (*s)
			s++;
	}
	*d++ = 0;
	while (is_blank(*s))
		s++;
	if (!*s) {
		argv[argc] = NULL;
		*nargs = argc;
		return argv;
	}

	argv[argc++] = d;
	int qcount = 0, bcount = 0;
	while (*s) {
		if (is_blank(*s) && !qcount) {
			*d++ = 0;
			bcount = 0;
			do
				s++;
			while (is_blank(*s));
			if (*s)
				argv[argc++] = d;
		} else if (*s == '\\') {
			*d++ = *s++;
			bcount++;
		} else if (*s == '"') {
			if (!(bcount & 1)) {
				d -= bcount / 2;
				qcount++;
			} else {
				d = d - bcount / 2 - 1;
				*d++ = '"';
			}
			s++;
			bcount = 0;
			while (*s == '"') {
				if (++qcount == 3) {
					*d++ = '"';
					qcount = 0;
				}
				s++;
			}
			if (qcount == 2)
				qcount = 0;
		} else {
			*d++ = *s++;
			bcount = 0;
		}
	}
	*d = 0;
	argv[argc] = NULL;
	*nargs = argc;
	return argv;
}

/* ShellExecute and friends: there is nothing to open anything with. */

static char *dup_text(const void *s, int wide)
{
	if (!s)
		return NULL;
	return wide ? utf16_to_utf8_dup(s) : acp_to_utf8(s);
}

/* "http://...", "mailto:...": a scheme of two or more characters. */
static int is_url(const char *s)
{
	size_t n = 0;

	while ((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= 'A' && s[n] <= 'Z') ||
	       (n && ((s[n] >= '0' && s[n] <= '9') || s[n] == '+' ||
		      s[n] == '-' || s[n] == '.')))
		n++;
	return n >= 2 && s[n] == ':';
}

/* SE_ERR_FNF for a file that isn't there, else SE_ERR_NOASSOC. */
static UINT_PTR shell_execute(const void *verb, const void *file,
			      const void *params, int wide)
{
	char *v = dup_text(verb, wide), *f = dup_text(file, wide);
	char *p = dup_text(params, wide);
	UINT_PTR r = SE_ERR_NOASSOC;

	fprintf(stderr, "ShellExecute [%s]: %s%s%s\n  (no GUI: nothing to "
		"open it with)\n", v ? v : "open", f ? f : "",
		p && *p ? " " : "", p ? p : "");
	if (f && !is_url(f)) {
		char *u = path_to_unix(f);
		if (!u || access(u, F_OK))
			r = SE_ERR_FNF;
		free(u);
	}
	win_set_error(r == SE_ERR_FNF ? ERROR_FILE_NOT_FOUND :
		      ERROR_NO_ASSOCIATION);
	free(v);
	free(f);
	free(p);
	return r;
}

WINFN(HINSTANCE) sh_ShellExecuteA(HWND hwnd, LPCSTR verb, LPCSTR file,
				  LPCSTR params, LPCSTR dir, INT show)
{
	return (HINSTANCE)shell_execute(verb, file, params, 0);
}

WINFN(HINSTANCE) sh_ShellExecuteW(HWND hwnd, LPCWSTR verb, LPCWSTR file,
				  LPCWSTR params, LPCWSTR dir, INT show)
{
	return (HINSTANCE)shell_execute(verb, file, params, 1);
}

WINFN(BOOL) sh_ShellExecuteExA(SHELLEXECUTEINFOA *info)
{
	if (!info || info->cbSize != sizeof(*info)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	info->hInstApp = (HINSTANCE)shell_execute(info->lpVerb, info->lpFile,
						  info->lpParameters, 0);
	info->hProcess = NULL;
	return FALSE;
}

WINFN(BOOL) sh_ShellExecuteExW(SHELLEXECUTEINFOW *info)
{
	if (!info || info->cbSize != sizeof(*info)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	info->hInstApp = (HINSTANCE)shell_execute(info->lpVerb, info->lpFile,
						  info->lpParameters, 1);
	info->hProcess = NULL;
	return FALSE;
}

/*
 * No program is associated with anything: SE_ERR_FNF for a file (in dir,
 * when relative) that isn't there, else SE_ERR_NOASSOC.
 */
static UINT_PTR find_executable(const void *file, const void *dir, int wide)
{
	char *f = dup_text(file, wide), *d = dup_text(dir, wide);
	UINT_PTR r = SE_ERR_NOASSOC;

	if (f && d && *d && f[0] != '\\' && f[0] != '/' &&
	    !(f[0] && f[1] == ':')) {
		size_t n = strlen(d);
		char *j = malloc(n + strlen(f) + 2);
		if (j) {
			memcpy(j, d, n);
			if (d[n - 1] != '\\' && d[n - 1] != '/')
				j[n++] = '\\';
			strcpy(j + n, f);
		}
		free(f);
		f = j;
	}
	char *u = f ? path_to_unix(f) : NULL;
	if (!u || access(u, F_OK))
		r = SE_ERR_FNF;
	free(u);
	free(f);
	free(d);
	return r;
}

WINFN(HINSTANCE) sh_FindExecutableA(LPCSTR file, LPCSTR dir, LPSTR result)
{
	if (result)
		result[0] = 0;
	return (HINSTANCE)find_executable(file, dir, 0);
}

WINFN(HINSTANCE) sh_FindExecutableW(LPCWSTR file, LPCWSTR dir, LPWSTR result)
{
	if (result)
		result[0] = 0;
	return (HINSTANCE)find_executable(file, dir, 1);
}

/* The folder browser, cancelled, as a user closing it would. */
static LPITEMIDLIST browse(const void *title, int wide)
{
	char *t = dup_text(title, wide);

	fprintf(stderr, "SHBrowseForFolder [%s]\n  (no GUI: cancelled)\n",
		t ? t : "");
	free(t);
	return NULL;
}

WINFN(LPITEMIDLIST) sh_SHBrowseForFolderA(BROWSEINFOA *bi)
{
	if (bi && bi->pszDisplayName)
		bi->pszDisplayName[0] = 0;
	return browse(bi ? bi->lpszTitle : NULL, 0);
}

WINFN(LPITEMIDLIST) sh_SHBrowseForFolderW(BROWSEINFOW *bi)
{
	if (bi && bi->pszDisplayName)
		bi->pszDisplayName[0] = 0;
	return browse(bi ? bi->lpszTitle : NULL, 1);
}

/* The About box: shown (on stderr) and closed. */
static BOOL shell_about(const void *app, const void *other, int wide)
{
	char *a = dup_text(app, wide), *o = dup_text(other, wide);

	fprintf(stderr, "ShellAbout [%s]: %s\n  (no GUI: answering OK)\n",
		a ? a : "", o ? o : "");
	free(a);
	free(o);
	return TRUE;
}

WINFN(BOOL) sh_ShellAboutA(HWND hwnd, LPCSTR app, LPCSTR other, HICON icon)
{
	return shell_about(app, other, 0);
}

WINFN(BOOL) sh_ShellAboutW(HWND hwnd, LPCWSTR app, LPCWSTR other, HICON icon)
{
	return shell_about(app, other, 1);
}

/* No taskbar: tray icons are accepted, and there are no appbars. */
WINFN(BOOL) sh_Shell_NotifyIconA(DWORD msg, void *data) { return TRUE; }
WINFN(BOOL) sh_Shell_NotifyIconW(DWORD msg, void *data) { return TRUE; }
WINFN(UINT_PTR) sh_SHAppBarMessage(DWORD msg, void *data) { return 0; }

/* No icons in any file. */
WINFN(HICON) sh_ExtractIconA(HINSTANCE inst, LPCSTR file, UINT index)
{
	return NULL;
}

WINFN(HICON) sh_ExtractIconW(HINSTANCE inst, LPCWSTR file, UINT index)
{
	return NULL;
}

WINFN(UINT) sh_ExtractIconExA(LPCSTR file, int index, HICON *large,
			      HICON *small, UINT n)
{
	return 0;
}

WINFN(UINT) sh_ExtractIconExW(LPCWSTR file, int index, HICON *large,
			      HICON *small, UINT n)
{
	return 0;
}

WINFN(HICON) sh_ExtractAssociatedIconA(HINSTANCE inst, LPSTR path,
				       LPWORD index)
{
	return NULL;
}

WINFN(HICON) sh_ExtractAssociatedIconW(HINSTANCE inst, LPWSTR path,
				       LPWORD index)
{
	return NULL;
}

/* Nothing is ever dropped. */
WINFN(void) sh_DragAcceptFiles(HWND hwnd, BOOL accept) { }
WINFN(void) sh_DragFinish(HDROP drop) { }

WINFN(UINT) sh_DragQueryFileA(HDROP drop, UINT i, LPSTR buf, UINT n)
{
	if (buf && n)
		buf[0] = 0;
	return 0;
}

WINFN(UINT) sh_DragQueryFileW(HDROP drop, UINT i, LPWSTR buf, UINT n)
{
	if (buf && n)
		buf[0] = 0;
	return 0;
}

WINFN(BOOL) sh_DragQueryPoint(HDROP drop, POINT *pt) { return FALSE; }

/* Nobody to tell about changes or recent documents. */
WINFN(void) sh_SHChangeNotify(LONG event, UINT flags, LPCVOID a, LPCVOID b) { }
WINFN(void) sh_SHAddToRecentDocs(UINT flags, LPCVOID p) { }

/* As on XP, where users were administrators by default. */
WINFN(BOOL) sh_IsUserAnAdmin(void) { return TRUE; }

#define E(name) { #name, 0, (void *)sh_##name }

static const struct builtin_export exports[] = {
	E(CommandLineToArgvW),
	E(DragAcceptFiles), E(DragFinish), E(DragQueryFileA),
	E(DragQueryFileW), E(DragQueryPoint),
	E(ExtractAssociatedIconA), E(ExtractAssociatedIconW),
	E(ExtractIconA), E(ExtractIconExA), E(ExtractIconExW),
	E(ExtractIconW),
	E(FindExecutableA), E(FindExecutableW),
	E(ILClone), E(ILCreateFromPathA), E(ILCreateFromPathW), E(ILFree),
	E(ILGetSize), E(IsUserAnAdmin),
	E(SHAddToRecentDocs), E(SHAppBarMessage),
	E(SHBrowseForFolderA), E(SHBrowseForFolderW), E(SHChangeNotify),
	E(SHGetFolderLocation), E(SHGetFolderPathA),
	E(SHGetFolderPathAndSubDirA), E(SHGetFolderPathAndSubDirW),
	E(SHGetFolderPathW), E(SHGetMalloc),
	E(SHGetPathFromIDListA), E(SHGetPathFromIDListW),
	E(SHGetSpecialFolderLocation), E(SHGetSpecialFolderPathA),
	E(SHGetSpecialFolderPathW),
	E(ShellAboutA), E(ShellAboutW),
	E(ShellExecuteA), E(ShellExecuteExA), E(ShellExecuteExW),
	E(ShellExecuteW),
	E(Shell_NotifyIconA), E(Shell_NotifyIconW),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = {
	exports, shell_file_exports, NULL
};

const struct builtin_dll builtin_shell32 = { "shell32.dll", NULL, tables, 0 };
