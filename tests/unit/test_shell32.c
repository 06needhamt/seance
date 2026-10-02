/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32, called through its export table: special folders, item ID
 * lists, CommandLineToArgvW, the headless ShellExecute and dialogs,
 * SHCreateDirectoryEx, SHFileOperation and SHGetFileInfo. The Windows
 * world lives in a temporary $VFW_ROOT.
 */
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "test.h"

static struct module *sh, *k32;
static char root[256];

static void *get_from(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "%s has no %s\n", m->name, name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(sh, #name)

/* The Unix path of a C: path, for checking what happened on disk. */
static const char *on_disk(const char *rel)
{
	static char buf[4][1024];
	static int next;
	char *b = buf[next++ % 4];

	snprintf(b, sizeof(buf[0]), "%s/drive_c/%s", root, rel);
	return b;
}

static int exists(const char *rel)
{
	struct stat st;
	return !stat(on_disk(rel), &st);
}

static int is_dir(const char *rel)
{
	struct stat st;
	return !stat(on_disk(rel), &st) && S_ISDIR(st.st_mode);
}

static void write_file(const char *rel, const char *text)
{
	FILE *f = fopen(on_disk(rel), "w");
	CHECK(f != NULL);
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static WCHAR *w16(const char *s)
{
	return utf8_to_utf16_dup(s);
}

static void test_folders(void)
{
	FN(HRESULT, SHGetFolderPathA, HWND, int, HANDLE, DWORD, LPSTR);
	FN(HRESULT, SHGetFolderPathW, HWND, int, HANDLE, DWORD, LPWSTR);
	FN(HRESULT, SHGetFolderPathAndSubDirA, HWND, int, HANDLE, DWORD,
	   LPCSTR, LPSTR);
	FN(BOOL, SHGetSpecialFolderPathA, HWND, LPSTR, int, BOOL);
	char p[MAX_PATH];
	WCHAR w[MAX_PATH];

	CHECK_EQ(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, p), S_OK);
	CHECK_STR(p, "C:\\Documents and Settings\\tester\\Application Data");
	CHECK(is_dir("Documents and Settings/tester/Application Data"));
	CHECK_EQ(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
				  NULL, SHGFP_TYPE_CURRENT, p), S_OK);
	CHECK_STR(p, "C:\\Documents and Settings\\tester\\Local Settings\\"
		     "Application Data");
	CHECK_EQ(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, p), S_OK);
	CHECK_STR(p, "C:\\Documents and Settings\\All Users\\Application Data");
	CHECK_EQ(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, p), S_OK);
	CHECK_STR(p, "C:\\Documents and Settings\\tester");
	CHECK_EQ(SHGetFolderPathA(NULL, CSIDL_SYSTEM, NULL, 0, p), S_OK);
	CHECK_STR(p, "C:\\WINDOWS\\system32");
	CHECK_EQ(SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES_COMMON, NULL, 0, p),
		 S_OK);
	CHECK_STR(p, "C:\\Program Files\\Common Files");
	CHECK(is_dir("Program Files/Common Files"));
	CHECK_EQ(SHGetFolderPathA(NULL, CSIDL_DESKTOP, NULL, 0, p), S_OK);
	CHECK_STR(p, "C:\\Documents and Settings\\tester\\Desktop");

	CHECK_EQ(SHGetFolderPathW(NULL, CSIDL_WINDOWS, NULL, 0, w), S_OK);
	char *u = utf16_to_utf8_dup(w);
	CHECK_STR(u, "C:\\WINDOWS");
	free(u);

	/* Virtual folders have no path; unknown ones are an error. */
	strcpy(p, "x");
	CHECK_EQ(SHGetFolderPathA(NULL, CSIDL_DRIVES, NULL, 0, p), E_FAIL);
	CHECK_STR(p, "");
	CHECK_EQ(SHGetFolderPathA(NULL, 0x7f, NULL, 0, p), E_INVALIDARG);

	CHECK(SHGetSpecialFolderPathA(NULL, p, CSIDL_PERSONAL, FALSE));
	CHECK_STR(p, "C:\\Documents and Settings\\tester\\My Documents");
	CHECK(!SHGetSpecialFolderPathA(NULL, p, CSIDL_BITBUCKET, FALSE));

	/* With a subdirectory: made only with CSIDL_FLAG_CREATE. */
	CHECK_EQ(SHGetFolderPathAndSubDirA(NULL, CSIDL_APPDATA, NULL, 0,
					   "Codec\\Presets", p),
		 HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
	CHECK_EQ(SHGetFolderPathAndSubDirA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE,
					   NULL, 0, "Codec\\Presets", p), S_OK);
	CHECK_STR(p, "C:\\Documents and Settings\\tester\\Application Data\\"
		     "Codec\\Presets");
	CHECK(is_dir("Documents and Settings/tester/Application Data/Codec/"
		     "Presets"));
}

static void test_pidls(void)
{
	FN(HRESULT, SHGetSpecialFolderLocation, HWND, int, LPITEMIDLIST *);
	FN(HRESULT, SHGetFolderLocation, HWND, int, HANDLE, DWORD,
	   LPITEMIDLIST *);
	FN(BOOL, SHGetPathFromIDListA, LPCITEMIDLIST, LPSTR);
	FN(BOOL, SHGetPathFromIDListW, LPCITEMIDLIST, LPWSTR);
	FN(LPITEMIDLIST, ILCreateFromPathW, LPCWSTR);
	FN(UINT, ILGetSize, LPCITEMIDLIST);
	FN(LPITEMIDLIST, ILClone, LPCITEMIDLIST);
	FN(void, ILFree, LPITEMIDLIST);
	FN(HRESULT, SHGetMalloc, LPMALLOC *);
	LPITEMIDLIST pidl = NULL;
	char p[MAX_PATH];

	CHECK_EQ(SHGetSpecialFolderLocation(NULL, CSIDL_APPDATA, &pidl), S_OK);
	CHECK(pidl && ILGetSize(pidl) > 2);
	CHECK(SHGetPathFromIDListA(pidl, p));
	CHECK_STR(p, "C:\\Documents and Settings\\tester\\Application Data");
	LPITEMIDLIST copy = ILClone(pidl);
	CHECK(copy && ILGetSize(copy) == ILGetSize(pidl) &&
	      !memcmp(copy, pidl, ILGetSize(pidl)));
	ILFree(copy);
	/* Freed through the task allocator, as callers do. */
	IMalloc *m = NULL;
	CHECK_EQ(SHGetMalloc(&m), S_OK);
	CHECK(m && m->lpVtbl->DidAlloc(m, pidl) == 1);
	if (m)
		m->lpVtbl->Free(m, pidl);

	/* The desktop is the empty list. */
	CHECK_EQ(SHGetFolderLocation(NULL, CSIDL_DESKTOP, NULL, 0, &pidl), S_OK);
	CHECK(pidl && ILGetSize(pidl) == 2);
	CHECK(SHGetPathFromIDListA(pidl, p));
	CHECK_STR(p, "C:\\Documents and Settings\\tester\\Desktop");
	ILFree(pidl);

	/* A virtual folder has a list but no path. */
	CHECK_EQ(SHGetSpecialFolderLocation(NULL, CSIDL_DRIVES, &pidl), S_OK);
	CHECK(pidl != NULL);
	CHECK(!SHGetPathFromIDListA(pidl, p));
	CHECK_STR(p, "");
	ILFree(pidl);
	CHECK_EQ(SHGetSpecialFolderLocation(NULL, 0x7f, &pidl), E_INVALIDARG);
	CHECK(pidl == NULL);

	WCHAR *path = w16("C:\\Some Folder\\f\xc3\xa9.txt");
	pidl = ILCreateFromPathW(path);
	WCHAR back[MAX_PATH];
	CHECK(pidl && SHGetPathFromIDListW(pidl, back));
	CHECK(!memcmp(back, path, (wstrlen(path) + 1) * sizeof(WCHAR)));
	ILFree(pidl);
	free(path);
	ILFree(NULL);
}

/* argv[1..] of a CommandLineToArgvW split, joined with '|'. */
static void split(const char *cmd, int *argc, char *out, size_t size)
{
	FN(LPWSTR *, CommandLineToArgvW, LPCWSTR, int *);
	HLOCAL (WINAPI *local_free)(HLOCAL) = get_from(k32, "LocalFree");
	WCHAR *w = w16(cmd);
	LPWSTR *argv = CommandLineToArgvW(w, argc);

	out[0] = 0;
	CHECK(argv != NULL);
	for (int i = 0; argv && i < *argc; i++) {
		char *a = utf16_to_utf8_dup(argv[i]);
		size_t n = strlen(out);
		snprintf(out + n, size - n, "%s%s", i ? "|" : "", a ? a : "?");
		free(a);
	}
	if (argv)
		CHECK(argv[*argc] == NULL);
	local_free(argv);
	free(w);
}

static void test_argv(void)
{
	FN(LPWSTR *, CommandLineToArgvW, LPCWSTR, int *);
	static const struct {
		const char *cmd;
		int argc;
		const char *args;
	} cases[] = {
		{ "prog \"a b c\" d e", 4, "prog|a b c|d|e" },
		{ "prog \"ab\\\"c\" \"\\\\\" d", 4, "prog|ab\"c|\\|d" },
		{ "prog a\\\\\\b d\"e f\"g h", 4, "prog|a\\\\\\b|de fg|h" },
		{ "prog a\\\\\\\"b c d", 4, "prog|a\\\"b|c|d" },
		{ "prog a\\\\\\\\\"b c\" d e", 4, "prog|a\\\\b c|d|e" },
		/* The program name: quotes with no escapes, or to a space. */
		{ "\"C:\\Program Files\\x.exe\" -q", 2,
		  "C:\\Program Files\\x.exe|-q" },
		{ "C:\\x\\\"y z", 2, "C:\\x\\\"y|z" },
		{ "prog   ", 1, "prog" },
		{ "prog\t\"\"", 2, "prog|" },
		{ "prog \"\"\"x\"\"\"", 2, "prog|\"x\"" },
	};
	char out[512];
	int argc;

	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		argc = -1;
		split(cases[i].cmd, &argc, out, sizeof(out));
		if (argc != cases[i].argc || strcmp(out, cases[i].args)) {
			fprintf(stderr, "CommandLineToArgvW(%s) = %d: %s\n",
				cases[i].cmd, argc, out);
			CHECK(0);
		}
	}

	/* Empty: the program's own path. */
	WCHAR empty[1] = { 0 };
	LPWSTR *argv = CommandLineToArgvW(empty, &argc);
	CHECK(argv && argc == 1 && argv[0][0] && !argv[1]);
	HLOCAL (WINAPI *local_free)(HLOCAL) = get_from(k32, "LocalFree");
	local_free(argv);
	CHECK(!CommandLineToArgvW(empty, NULL));
}

static void test_ui(void)
{
	FN(HINSTANCE, ShellExecuteA, HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
	FN(BOOL, ShellExecuteExA, SHELLEXECUTEINFOA *);
	FN(HINSTANCE, FindExecutableA, LPCSTR, LPCSTR, LPSTR);
	FN(LPITEMIDLIST, SHBrowseForFolderA, BROWSEINFOA *);
	FN(BOOL, ShellAboutA, HWND, LPCSTR, LPCSTR, HICON);
	FN(BOOL, Shell_NotifyIconA, DWORD, void *);
	FN(HICON, ExtractIconA, HINSTANCE, LPCSTR, UINT);
	FN(UINT, DragQueryFileA, HDROP, UINT, LPSTR, UINT);

	write_file("readme.txt", "hello");
	CHECK_EQ((UINT_PTR)ShellExecuteA(NULL, "open", "http://example.com/",
					 NULL, NULL, 1), SE_ERR_NOASSOC);
	CHECK_EQ((UINT_PTR)ShellExecuteA(NULL, NULL, "mailto:a@b.c", NULL,
					 NULL, 1), SE_ERR_NOASSOC);
	CHECK_EQ((UINT_PTR)ShellExecuteA(NULL, "open", "C:\\readme.txt", NULL,
					 NULL, 1), SE_ERR_NOASSOC);
	CHECK_EQ(win_get_error(), ERROR_NO_ASSOCIATION);
	CHECK_EQ((UINT_PTR)ShellExecuteA(NULL, "open", "C:\\nope.txt", "-x",
					 NULL, 1), SE_ERR_FNF);
	CHECK_EQ(win_get_error(), ERROR_FILE_NOT_FOUND);

	SHELLEXECUTEINFOA sei;
	memset(&sei, 0, sizeof(sei));
	sei.cbSize = sizeof(sei);
	sei.lpFile = "http://example.com/";
	sei.hProcess = (HANDLE)1;
	CHECK(!ShellExecuteExA(&sei));
	CHECK((UINT_PTR)sei.hInstApp == SE_ERR_NOASSOC && !sei.hProcess);
	sei.cbSize = 4;
	CHECK(!ShellExecuteExA(&sei));
	CHECK_EQ(win_get_error(), ERROR_INVALID_PARAMETER);

	char exe[MAX_PATH] = "x";
	CHECK_EQ((UINT_PTR)FindExecutableA("C:\\readme.txt", NULL, exe),
		 SE_ERR_NOASSOC);
	CHECK_STR(exe, "");
	CHECK_EQ((UINT_PTR)FindExecutableA("C:\\nope.txt", NULL, exe),
		 SE_ERR_FNF);
	CHECK_EQ((UINT_PTR)FindExecutableA("readme.txt", "C:\\", exe),
		 SE_ERR_NOASSOC);
	CHECK_EQ((UINT_PTR)FindExecutableA("readme.txt", "C:\\made", exe),
		 SE_ERR_FNF);

	char name[MAX_PATH] = "x";
	BROWSEINFOA bi;
	memset(&bi, 0, sizeof(bi));
	bi.pszDisplayName = name;
	bi.lpszTitle = "Pick a folder";
	CHECK(SHBrowseForFolderA(&bi) == NULL);
	CHECK_STR(name, "");
	CHECK(ShellAboutA(NULL, "Codec", "About it", NULL));
	CHECK(Shell_NotifyIconA(0, NULL));
	CHECK(ExtractIconA(NULL, "C:\\readme.txt", 0) == NULL);
	CHECK_EQ(DragQueryFileA(NULL, 0xffffffff, NULL, 0), 0);
}

static void test_create_directory(void)
{
	FN(int, SHCreateDirectoryExA, HWND, LPCSTR, void *);
	FN(int, SHCreateDirectory, HWND, LPCWSTR);

	CHECK_EQ(SHCreateDirectoryExA(NULL, "C:\\made\\by\\shell", NULL),
		 ERROR_SUCCESS);
	CHECK(is_dir("made/by/shell"));
	CHECK_EQ(SHCreateDirectoryExA(NULL, "C:\\made\\by\\shell", NULL),
		 ERROR_ALREADY_EXISTS);
	CHECK_EQ(SHCreateDirectoryExA(NULL, "C:\\readme.txt", NULL),
		 ERROR_FILE_EXISTS);
	CHECK_EQ(SHCreateDirectoryExA(NULL, "relative\\dir", NULL),
		 ERROR_BAD_PATHNAME);
	WCHAR *w = w16("C:\\made\\wide");
	CHECK_EQ(SHCreateDirectory(NULL, w), ERROR_SUCCESS);
	CHECK(is_dir("made/wide"));
	free(w);
}

static void test_file_operation(void)
{
	FN(int, SHFileOperationA, SHFILEOPSTRUCTA *);
	FN(int, SHFileOperationW, SHFILEOPSTRUCTW *);
	SHFILEOPSTRUCTA op;

	mkdir(on_disk("src"), 0755);
	mkdir(on_disk("src/sub"), 0755);
	write_file("src/one.txt", "1");
	write_file("src/two.TXT", "22");
	write_file("src/sub/three.dat", "333");

#define OP(func, from, to, fl) \
	(memset(&op, 0, sizeof(op)), op.wFunc = (func), op.pFrom = (from), \
	 op.pTo = (to), op.fFlags = (fl), op.fAnyOperationsAborted = TRUE, \
	 SHFileOperationA(&op))

	/* Wildcards, matched ignoring case, into a new directory. */
	CHECK_EQ(OP(FO_COPY, "C:\\src\\*.txt\0", "C:\\dst\0",
		    FOF_NOCONFIRMATION | FOF_SILENT), 0);
	CHECK(!op.fAnyOperationsAborted);
	CHECK(exists("dst/one.txt") && exists("dst/two.TXT") &&
	      !exists("dst/sub"));
	/* A directory to a new name: the whole tree. */
	CHECK_EQ(OP(FO_COPY, "C:\\src\0", "C:\\copy\0", 0), 0);
	CHECK(exists("copy/sub/three.dat") && exists("copy/one.txt"));
	/* Several sources into an existing directory, replacing. */
	CHECK_EQ(OP(FO_COPY, "C:\\src\\one.txt\0C:\\src\\sub\0", "C:\\dst\0",
		    0), 0);
	CHECK(exists("dst/sub/three.dat"));
	/* Only files, with a wildcard. */
	CHECK_EQ(OP(FO_COPY, "C:\\src\\*.*\0", "C:\\files\0", FOF_FILESONLY),
		 0);
	CHECK(exists("files/one.txt") && !exists("files/sub"));
	/* A collision renamed, twice. */
	CHECK_EQ(OP(FO_COPY, "C:\\src\\one.txt\0", "C:\\dst\0",
		    FOF_RENAMEONCOLLISION), 0);
	CHECK_EQ(OP(FO_COPY, "C:\\src\\one.txt\0", "C:\\dst\0",
		    FOF_RENAMEONCOLLISION), 0);
	CHECK(exists("dst/Copy of one.txt") && exists("dst/Copy (2) of one.txt"));
	/* Each source to its own destination. */
	CHECK_EQ(OP(FO_COPY, "C:\\src\\one.txt\0C:\\src\\two.TXT\0",
		    "C:\\a.txt\0C:\\b.txt\0", FOF_MULTIDESTFILES), 0);
	CHECK(exists("a.txt") && exists("b.txt"));
	/* Not into its own subtree. */
	CHECK(OP(FO_COPY, "C:\\src\0", "C:\\src\\sub\\inner\0", 0) != 0);
	CHECK(!exists("src/sub/inner"));

	/* A file bigger than one read, and a copy cut short by a full disk. */
	FILE *f = fopen(on_disk("big.bin"), "wb");
	CHECK(f != NULL);
	for (int i = 0; f && i < 150000; i++)
		fputc(i * 7, f);
	if (f)
		fclose(f);
	CHECK_EQ(OP(FO_COPY, "C:\\big.bin\0", "C:\\big2.bin\0", 0), 0);
	struct stat st;
	CHECK(!stat(on_disk("big2.bin"), &st) && st.st_size == 150000);
	struct rlimit old, lim;
	void (*xfsz)(int) = signal(SIGXFSZ, SIG_IGN);
	CHECK(!getrlimit(RLIMIT_FSIZE, &old));
	lim = old;
	lim.rlim_cur = 70000;	/* the second write stops short */
	CHECK(!setrlimit(RLIMIT_FSIZE, &lim));
	CHECK_EQ(OP(FO_COPY, "C:\\big.bin\0", "C:\\big3.bin\0", 0),
		 ERROR_DISK_FULL);
	CHECK(!setrlimit(RLIMIT_FSIZE, &old));
	signal(SIGXFSZ, xfsz);

	/* Move, rename, delete. */
	CHECK_EQ(OP(FO_MOVE, "C:\\dst\\two.TXT\0", "C:\\moved.txt\0", 0), 0);
	CHECK(!exists("dst/two.TXT") && exists("moved.txt"));
	CHECK_EQ(OP(FO_MOVE, "C:\\copy\\sub\0", "C:\\made\0", 0), 0);
	CHECK(!exists("copy/sub") && exists("made/sub/three.dat"));
	CHECK_EQ(OP(FO_RENAME, "C:\\moved.txt\0", "C:\\renamed.txt\0", 0), 0);
	CHECK(!exists("moved.txt") && exists("renamed.txt"));
	CHECK_EQ(OP(FO_RENAME, "C:\\renamed.txt\0", "C:\\a.txt\0", 0),
		 ERROR_ALREADY_EXISTS);
	CHECK_EQ(OP(FO_DELETE, "C:\\copy\0C:\\a.txt\0", NULL, FOF_ALLOWUNDO), 0);
	CHECK(!exists("copy") && !exists("a.txt"));
	CHECK_EQ(OP(FO_DELETE, "C:\\no such thing\0", NULL, 0),
		 ERROR_FILE_NOT_FOUND);
	CHECK_EQ(OP(FO_DELETE, "C:\\files\\*\0", NULL, 0), 0);
	CHECK(is_dir("files") && !exists("files/one.txt"));
	/* Without recursion a directory isn't entered: only an empty one goes. */
	mkdir(on_disk("keep"), 0755);
	mkdir(on_disk("keep/inner"), 0755);
	write_file("keep/inner/x.txt", "x");
	CHECK_EQ(OP(FO_DELETE, "C:\\keep\0", NULL, FOF_NORECURSION),
		 ERROR_DIR_NOT_EMPTY);
	CHECK(exists("keep/inner/x.txt"));
	CHECK_EQ(OP(FO_DELETE, "C:\\keep\\inner\\x.txt\0", NULL,
		    FOF_NORECURSION), 0);
	CHECK_EQ(OP(FO_DELETE, "C:\\keep\\inner\0", NULL, FOF_NORECURSION),
		 0);
	CHECK(is_dir("keep") && !exists("keep/inner"));
	CHECK(OP(9, "C:\\src\0", NULL, 0) != 0);
#undef OP

	SHFILEOPSTRUCTW wop;
	WCHAR from[] = { 'C', ':', '\\', 'b', '.', 't', 'x', 't', 0, 0 };
	WCHAR to[] = { 'C', ':', '\\', 0x20ac, '.', 't', 'x', 't', 0, 0 };
	memset(&wop, 0, sizeof(wop));
	wop.wFunc = FO_MOVE;
	wop.pFrom = from;
	wop.pTo = to;
	CHECK_EQ(SHFileOperationW(&wop), 0);
	CHECK(!exists("b.txt") && exists("\xe2\x82\xac.txt"));
}

static void test_file_info(void)
{
	FN(DWORD_PTR, SHGetFileInfoA, LPCSTR, DWORD, SHFILEINFOA *, UINT, UINT);
	FN(DWORD_PTR, SHGetFileInfoW, LPCWSTR, DWORD, SHFILEINFOW *, UINT, UINT);
	FN(LPITEMIDLIST, ILCreateFromPathW, LPCWSTR);
	FN(void, ILFree, LPITEMIDLIST);
	SHFILEINFOA fi;
	const UINT all = SHGFI_ATTRIBUTES | SHGFI_DISPLAYNAME | SHGFI_TYPENAME |
			 SHGFI_ICON;

	memset(&fi, 0xcc, sizeof(fi));
	CHECK(SHGetFileInfoA("C:\\src", 0, &fi, sizeof(fi), all));
	CHECK(fi.dwAttributes & SFGAO_FOLDER);
	CHECK(fi.dwAttributes & SFGAO_FILESYSTEM);
	CHECK_STR(fi.szDisplayName, "src");
	CHECK_STR(fi.szTypeName, "File Folder");
	CHECK(fi.hIcon == NULL && fi.iIcon == 0);

	CHECK(SHGetFileInfoA("C:\\src\\one.txt", 0, &fi, sizeof(fi), all));
	CHECK(!(fi.dwAttributes & SFGAO_FOLDER) &&
	      (fi.dwAttributes & SFGAO_STREAM));
	CHECK_STR(fi.szDisplayName, "one.txt");
	CHECK_STR(fi.szTypeName, "TXT File");

	/* Not on disk: only with SHGFI_USEFILEATTRIBUTES. */
	CHECK(!SHGetFileInfoA("C:\\clip.avi", 0, &fi, sizeof(fi), all));
	CHECK(SHGetFileInfoA("C:\\clip.avi", FILE_ATTRIBUTE_NORMAL, &fi,
			     sizeof(fi), SHGFI_TYPENAME |
			     SHGFI_USEFILEATTRIBUTES));
	CHECK_STR(fi.szTypeName, "AVI File");
	CHECK(SHGetFileInfoA("C:\\codec.dll", FILE_ATTRIBUTE_NORMAL, &fi,
			     sizeof(fi), SHGFI_TYPENAME |
			     SHGFI_USEFILEATTRIBUTES));
	CHECK_STR(fi.szTypeName, "Application Extension");
	CHECK(!SHGetFileInfoA("C:\\src\\one.txt", 0, &fi, 10, all));

	/* By item ID list, and wide. */
	WCHAR *w = w16("C:\\src\\sub");
	LPITEMIDLIST pidl = ILCreateFromPathW(w);
	SHFILEINFOW wfi;
	CHECK(SHGetFileInfoW((LPCWSTR)pidl, 0, &wfi, sizeof(wfi),
			     SHGFI_PIDL | SHGFI_DISPLAYNAME | SHGFI_TYPENAME));
	char *name = utf16_to_utf8_dup(wfi.szDisplayName);
	char *type = utf16_to_utf8_dup(wfi.szTypeName);
	CHECK_STR(name, "sub");
	CHECK_STR(type, "File Folder");
	free(name);
	free(type);
	ILFree(pidl);
	free(w);

	/* SHGFI_EXETYPE: a DLL and a text file are not programs. */
	const char *dir = getenv("VFW_FIXTURES");
	char dll[1024];
	snprintf(dll, sizeof(dll), "%s/testcodec.dll", dir ? dir : ".");
	if (dir && !access(dll, R_OK)) {
		snprintf(dll, sizeof(dll), "Z:%s/testcodec.dll", dir);
		for (char *c = dll; *c; c++)
			if (*c == '/')
				*c = '\\';
		CHECK_EQ(SHGetFileInfoA(dll, 0, NULL, 0, SHGFI_EXETYPE), 0);
	}
	CHECK_EQ(SHGetFileInfoA("C:\\src\\one.txt", 0, NULL, 0, SHGFI_EXETYPE), 0);
}

/* pe_shell.dll: the same from Win32 code, through its imports. */
static void test_fixture(void)
{
	struct module *m = mod_load(fixture("pe_shell.dll"), NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	int (*run)(void) = mod_get_proc(m, "shell_run");
	CHECK(run != NULL);
	if (run)
		CHECK_EQ(run(), 0);
	CHECK(is_dir("Documents and Settings/tester/Application Data/PeShell"));
	CHECK(exists("Documents and Settings/tester/Application Data/PeShell/"
		     "backup.ini"));
	mod_free(m);
}

int main(void)
{
	const char *tmpdir = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/vfw-shell-XXXXXX",
		 tmpdir && *tmpdir ? tmpdir : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", root, 1);
	setenv("VFW_REGISTRY", "", 1);
	setenv("USER", "tester", 1);

	CHECK_EQ(runtime_enter(), 0);
	sh = mod_load("shell32.dll", NULL);
	k32 = mod_load("kernel32.dll", NULL);
	CHECK(sh && sh->builtin && !sh->builtin->fallback && k32);
	if (!sh || !k32)
		return test_summary("test_shell32");

	test_folders();
	test_pidls();
	test_argv();
	test_ui();
	test_create_directory();
	test_file_operation();
	test_file_info();
	test_fixture();

	mod_free(k32);
	mod_free(sh);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", root);
	return test_summary("test_shell32");
}
