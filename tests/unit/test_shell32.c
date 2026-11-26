/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32, called through its export table: special folders, item ID
 * lists, CommandLineToArgvW, the headless ShellExecute and dialogs,
 * SHCreateDirectoryEx, SHFileOperation, SHGetFileInfo, known folders,
 * IShellFolder, the Recycle Bin and shortcuts. The Windows
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
	/* One on another file system than the bin can't be recycled:
	 * with no one to ask it stays, and goes for good only without
	 * confirmation. */
	char shm[] = "/dev/shm/vfw-rb-XXXXXX";
	struct stat sr, ss;
	if (!stat(root, &sr) && mkdtemp(shm)) {
		char fp[64], zp[80];
		snprintf(fp, sizeof(fp), "%s/x.txt", shm);
		snprintf(zp, sizeof(zp), "Z:%s\\x.txt", shm);
		for (char *c = zp; *c; c++)
			if (*c == '/')
				*c = '\\';
		zp[strlen(zp) + 1] = 0;	/* the list's second NUL */
		FILE *xf = fopen(fp, "w");
		if (xf)
			fclose(xf);
		if (!stat(shm, &ss) && ss.st_dev != sr.st_dev) {
			CHECK_EQ(OP(FO_DELETE, zp, NULL, FOF_ALLOWUNDO), 0);
			CHECK(op.fAnyOperationsAborted);
			CHECK(!access(fp, F_OK));
			CHECK_EQ(OP(FO_DELETE, zp, NULL,
				    FOF_ALLOWUNDO | FOF_NOCONFIRMATION), 0);
			CHECK(!op.fAnyOperationsAborted);
			CHECK(access(fp, F_OK));
		}
		unlink(fp);
		rmdir(shm);
	}
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
	int (CDECL *run)(void) = mod_get_proc(m, "shell_run");
	CHECK(run != NULL);
	if (run)
		CHECK_EQ(run(), 0);
	CHECK(is_dir("Documents and Settings/tester/Application Data/PeShell"));
	CHECK(exists("Documents and Settings/tester/Application Data/PeShell/"
		     "backup.ini"));
	mod_free(m);
}

static struct module *ole;

static void task_free(void *p)
{
	void (WINAPI *f)(void *) = get_from(ole, "CoTaskMemFree");
	f(p);
}

/* A STRRET's text as UTF-8 (malloc()ed), freeing it. */
static char *strret(STRRET *s)
{
	if (s->uType != STRRET_WSTR)
		return strdup("?");
	char *r = utf16_to_utf8_dup(s->u.pOleStr);
	task_free(s->u.pOleStr);
	return r;
}

#define CHECK_NAME(sf, pidl, flags, want) do { \
	STRRET s_; \
	CHECK((sf)->lpVtbl->GetDisplayNameOf((sf), (pidl), (flags), &s_) == S_OK); \
	char *n_ = strret(&s_); \
	CHECK_STR(n_, want); \
	free(n_); \
} while (0)

static void test_known_folders(void)
{
	FN(HRESULT, SHGetKnownFolderPath, REFKNOWNFOLDERID, DWORD, HANDLE,
	   PWSTR *);
	FN(HRESULT, SHGetFolderPathEx, REFKNOWNFOLDERID, DWORD, HANDLE, LPWSTR,
	   UINT);
	FN(HRESULT, SHGetKnownFolderIDList, REFKNOWNFOLDERID, DWORD, HANDLE,
	   LPITEMIDLIST *);
	FN(BOOL, SHGetPathFromIDListA, LPCITEMIDLIST, LPSTR);
	FN(void, ILFree, LPITEMIDLIST);
	PWSTR w = NULL;

	CHECK(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &w) == S_OK);
	char *s = w ? utf16_to_utf8_dup(w) : NULL;
	CHECK_STR(s, "C:\\Documents and Settings\\tester\\Application Data");
	free(s);
	task_free(w);
	CHECK(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &w) == S_OK);
	s = w ? utf16_to_utf8_dup(w) : NULL;
	CHECK_STR(s, "C:\\Documents and Settings\\tester\\Downloads");
	CHECK(is_dir("Documents and Settings/tester/Downloads"));
	free(s);
	task_free(w);
	/* The Default User's profile isn't the current user's. */
	w = NULL;
	CHECK(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, (HANDLE)(LONG_PTR)-1,
				   &w) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
	CHECK(w == NULL);
	/* No path for a virtual folder; an unknown one isn't found. */
	CHECK(SHGetKnownFolderPath(&FOLDERID_ComputerFolder, 0, NULL, &w) ==
	      E_INVALIDARG && !w);
	CHECK(SHGetKnownFolderPath(&IID_IUnknown, 0, NULL, &w) ==
	      HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
	WCHAR buf[MAX_PATH];
	CHECK(SHGetFolderPathEx(&FOLDERID_System, 0, NULL, buf, MAX_PATH) == S_OK);
	s = utf16_to_utf8_dup(buf);
	CHECK_STR(s, "C:\\WINDOWS\\system32");
	free(s);
	CHECK(SHGetFolderPathEx(&FOLDERID_System, 0, NULL, buf, 5) ==
	      HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
	LPITEMIDLIST pidl = NULL;
	char path[MAX_PATH];
	CHECK(SHGetKnownFolderIDList(&FOLDERID_Documents, 0, NULL, &pidl) == S_OK);
	CHECK(pidl && SHGetPathFromIDListA(pidl, path));
	CHECK_STR(path, "C:\\Documents and Settings\\tester\\My Documents");
	ILFree(pidl);
}

static void test_namespace(void)
{
	FN(HRESULT, SHGetDesktopFolder, IShellFolder **);
	FN(HRESULT, SHBindToParent, LPCITEMIDLIST, REFIID, void **,
	   LPCITEMIDLIST *);
	FN(HRESULT, SHParseDisplayName, LPCWSTR, IBindCtx *, LPITEMIDLIST *,
	   ULONG, ULONG *);
	FN(BOOL, SHGetPathFromIDListA, LPCITEMIDLIST, LPSTR);
	FN(LPITEMIDLIST, ILCombine, LPCITEMIDLIST, LPCITEMIDLIST);
	FN(LPITEMIDLIST, ILFindLastID, LPCITEMIDLIST);
	FN(BOOL, ILRemoveLastID, LPITEMIDLIST);
	FN(BOOL, ILIsEqual, LPCITEMIDLIST, LPCITEMIDLIST);
	FN(BOOL, ILIsParent, LPCITEMIDLIST, LPCITEMIDLIST, BOOL);
	FN(LPITEMIDLIST, ILGetNext, LPCITEMIDLIST);
	FN(UINT, ILGetSize, LPCITEMIDLIST);
	FN(LPITEMIDLIST, ILCreateFromPathA, LPCSTR);
	FN(void, ILFree, LPITEMIDLIST);
	IShellFolder *desk = NULL, *dir = NULL, *drives = NULL;
	char path[MAX_PATH];

	mkdir(on_disk("ns"), 0755);
	mkdir(on_disk("ns/sub"), 0755);
	write_file("ns/b.txt", "b");
	write_file("ns/a.lnk", "a");
	write_file("ns/.hidden", "h");

	CHECK(SHGetDesktopFolder(&desk) == S_OK && desk);
	if (!desk)
		return;
	/* Parse a path: one item, its path. */
	WCHAR *name = w16("C:\\ns\\");
	LPITEMIDLIST ns = NULL;
	ULONG eaten = 0, attrs = SFGAO_FOLDER | SFGAO_STREAM;
	CHECK(desk->lpVtbl->ParseDisplayName(desk, NULL, NULL, name, &eaten, &ns,
					     &attrs) == S_OK);
	CHECK(eaten == 6 && attrs == SFGAO_FOLDER);
	free(name);
	CHECK(ns && SHGetPathFromIDListA(ns, path));
	CHECK_STR(path, "C:\\ns");
	name = w16("C:\\nothing-here");
	LPITEMIDLIST none = (void *)1;
	CHECK(desk->lpVtbl->ParseDisplayName(desk, NULL, NULL, name, NULL, &none,
					     NULL) ==
	      HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && !none);
	free(name);

	/* Bind to it and list it: folders, then files; hidden ones only
	 * when asked. */
	CHECK(desk->lpVtbl->BindToObject(desk, ns, NULL, &IID_IShellFolder,
					 (void **)&dir) == S_OK && dir);
	if (dir) {
		IEnumIDList *en = NULL;
		LPITEMIDLIST item[8];
		ULONG got = 0;
		CHECK(dir->lpVtbl->EnumObjects(dir, NULL, SHCONTF_FOLDERS |
					       SHCONTF_NONFOLDERS, &en) == S_OK);
		CHECK(en && en->lpVtbl->Next(en, 8, item, &got) == S_FALSE);
		CHECK_EQ(got, 3);
		int sub = -1, b = -1, lnk = -1;
		for (ULONG i = 0; i < got; i++) {
			STRRET s;
			dir->lpVtbl->GetDisplayNameOf(dir, item[i], SHGDN_INFOLDER, &s);
			char *n = strret(&s);
			if (!strcmp(n, "sub")) sub = (int)i;
			if (!strcmp(n, "b.txt")) b = (int)i;
			if (!strcmp(n, "a.lnk")) lnk = (int)i;
			free(n);
		}
		CHECK(sub >= 0 && b >= 0 && lnk >= 0);
		if (sub >= 0 && b >= 0 && lnk >= 0) {
			CHECK_NAME(dir, item[b], SHGDN_FORPARSING, "C:\\ns\\b.txt");
			CHECK_NAME(dir, item[b], SHGDN_FORPARSING | SHGDN_INFOLDER,
				   "b.txt");
			/* Folders sort first, then names. */
			CHECK((SHORT)dir->lpVtbl->CompareIDs(dir, 0, item[sub],
							     item[b]) == -1);
			CHECK((SHORT)dir->lpVtbl->CompareIDs(dir, 0, item[b],
							     item[lnk]) == 1);
			CHECK((SHORT)dir->lpVtbl->CompareIDs(dir, 0, item[b],
							     item[b]) == 0);
			ULONG a = 0xffffffff;
			LPCITEMIDLIST one[1] = { item[sub] };
			CHECK(dir->lpVtbl->GetAttributesOf(dir, 1, one, &a) == S_OK);
			CHECK((a & SFGAO_FOLDER) && (a & SFGAO_FILESYSTEM) &&
			      !(a & SFGAO_STREAM));
			a = 0xffffffff;
			one[0] = item[lnk];
			CHECK(dir->lpVtbl->GetAttributesOf(dir, 1, one, &a) == S_OK);
			CHECK((a & SFGAO_LINK) && (a & SFGAO_STREAM) &&
			      !(a & SFGAO_FOLDER));
			LPCITEMIDLIST two[2] = { item[sub], item[b] };
			a = SFGAO_FOLDER | SFGAO_FILESYSTEM;
			CHECK(dir->lpVtbl->GetAttributesOf(dir, 2, two, &a) == S_OK);
			CHECK_EQ(a, SFGAO_FILESYSTEM);
			/* Rename. */
			name = w16("c.txt");
			LPITEMIDLIST renamed = NULL;
			CHECK(dir->lpVtbl->SetNameOf(dir, NULL, item[b], name, 0,
						     &renamed) == S_OK);
			free(name);
			CHECK(exists("ns/c.txt") && !exists("ns/b.txt"));
			CHECK(renamed && SHGetPathFromIDListA(renamed, path));
			CHECK_STR(path, "C:\\ns\\c.txt");
			ILFree(renamed);
			/* Not over another link to the same file: both names
			 * stay, and the rename fails. */
			char c_path[512];
			snprintf(c_path, sizeof(c_path), "%s", on_disk("ns/c.txt"));
			CHECK_EQ(link(c_path, on_disk("ns/d.txt")), 0);
			LPITEMIDLIST ci = NULL;
			name = w16("c.txt");
			CHECK(dir->lpVtbl->ParseDisplayName(dir, NULL, NULL, name,
							    NULL, &ci, NULL) ==
			      S_OK);
			free(name);
			name = w16("d.txt");
			renamed = NULL;
			CHECK(ci && dir->lpVtbl->SetNameOf(dir, NULL, ci, name, 0,
							   &renamed) ==
			      HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
			free(name);
			ILFree(ci);
			CHECK(!renamed && exists("ns/c.txt") &&
			      exists("ns/d.txt"));
			unlink(on_disk("ns/d.txt"));
			/* A combined list stands for its last item. */
			LPITEMIDLIST full = ILCombine(ns, item[sub]);
			CHECK(full && ILGetSize(full) == ILGetSize(ns) +
			      ILGetSize(item[sub]) - 2);
			CHECK(full && SHGetPathFromIDListA(full, path));
			CHECK_STR(path, "C:\\ns\\sub");
			CHECK(ILGetNext(full) == ILFindLastID(full));
			CHECK(ILIsEqual(ILFindLastID(full), item[sub]));
			CHECK(ILIsParent(ns, full, TRUE) && !ILIsParent(full, ns, FALSE));
			/* Its parent: the directory, and the item. */
			IShellFolder *parent = NULL;
			LPCITEMIDLIST last = NULL;
			CHECK(SHBindToParent(full, &IID_IShellFolder, (void **)&parent,
					     &last) == S_OK && parent);
			CHECK(last == ILFindLastID(full));
			if (parent) {
				CHECK_NAME(parent, last, SHGDN_INFOLDER, "sub");
				parent->lpVtbl->Release(parent);
			}
			CHECK(ILRemoveLastID(full) && ILIsEqual(full, ns));
			ILFree(full);
		}
		for (ULONG i = 0; i < got; i++)
			ILFree(item[i]);
		CHECK(en->lpVtbl->Reset(en) == S_OK);
		IEnumIDList *c = NULL;
		CHECK(en->lpVtbl->Clone(en, &c) == S_OK && c);
		CHECK(c && c->lpVtbl->Skip(c, 9) == S_FALSE);
		if (c)
			c->lpVtbl->Release(c);
		en->lpVtbl->Release(en);
		CHECK(dir->lpVtbl->EnumObjects(dir, NULL, SHCONTF_NONFOLDERS |
					       SHCONTF_INCLUDEHIDDEN, &en) == S_OK);
		got = 0;
		en->lpVtbl->Next(en, 8, item, &got);
		CHECK_EQ(got, 3);	/* c.txt, a.lnk, .hidden */
		for (ULONG i = 0; i < got; i++)
			ILFree(item[i]);
		en->lpVtbl->Release(en);
		/* A file is no folder. */
		LPITEMIDLIST file = ILCreateFromPathA("C:\\ns\\c.txt");
		IShellFolder *bad = (void *)1;
		CHECK(dir->lpVtbl->BindToObject(dir, file, NULL, &IID_IShellFolder,
						(void **)&bad) == E_INVALIDARG &&
		      !bad);
		ILFree(file);
		dir->lpVtbl->Release(dir);
	}
	ILFree(ns);

	/* The desktop holds My Computer and the Recycle Bin; My Computer
	 * holds drive C:. */
	IEnumIDList *en = NULL;
	LPITEMIDLIST item[2];
	ULONG got = 0;
	CHECK(desk->lpVtbl->EnumObjects(desk, NULL, SHCONTF_FOLDERS, &en) == S_OK);
	CHECK(en->lpVtbl->Next(en, 2, item, &got) == S_OK && got == 2);
	CHECK_NAME(desk, item[0], SHGDN_NORMAL, "My Computer");
	CHECK_NAME(desk, item[0], SHGDN_FORPARSING,
		   "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}");
	CHECK_NAME(desk, item[1], SHGDN_NORMAL, "Recycle Bin");
	CHECK(!SHGetPathFromIDListA(item[0], path));
	CHECK(desk->lpVtbl->BindToObject(desk, item[0], NULL, &IID_IShellFolder,
					 (void **)&drives) == S_OK && drives);
	ILFree(item[0]);
	ILFree(item[1]);
	en->lpVtbl->Release(en);
	if (drives) {
		CHECK(drives->lpVtbl->EnumObjects(drives, NULL, SHCONTF_FOLDERS,
						  &en) == S_OK);
		int c = 0;
		LPITEMIDLIST d;
		while (en->lpVtbl->Next(en, 1, &d, NULL) == S_OK) {
			STRRET s;
			drives->lpVtbl->GetDisplayNameOf(drives, d, SHGDN_FORPARSING, &s);
			char *n = strret(&s);
			if (!strcmp(n, "C:\\")) {
				c = 1;
				CHECK_NAME(drives, d, SHGDN_NORMAL, "Local Disk (C:)");
			}
			free(n);
			ILFree(d);
		}
		CHECK(c);
		en->lpVtbl->Release(en);
		drives->lpVtbl->Release(drives);
	}
	/* "::{CLSID}" names, through SHParseDisplayName. */
	name = w16("::{645FF040-5081-101B-9F08-00AA002F954E}");
	LPITEMIDLIST bin = NULL;
	ULONG sfgao = 0;
	CHECK(SHParseDisplayName(name, NULL, &bin, SFGAO_FOLDER, &sfgao) == S_OK);
	CHECK(bin && sfgao == SFGAO_FOLDER);
	free(name);
	if (bin)
		CHECK_NAME(desk, bin, SHGDN_NORMAL, "Recycle Bin");
	ILFree(bin);
	desk->lpVtbl->Release(desk);
}

static void test_recycle_bin(void)
{
	FN(int, SHFileOperationA, SHFILEOPSTRUCTA *);
	FN(HRESULT, SHQueryRecycleBinA, LPCSTR, LPSHQUERYRBINFO);
	FN(HRESULT, SHEmptyRecycleBinA, HWND, LPCSTR, DWORD);
	FN(HRESULT, SHGetDesktopFolder, IShellFolder **);
	FN(void, ILFree, LPITEMIDLIST);

	SHQUERYRBINFO info = { sizeof(info), -1, -1 };
	/* Empty what test_file_operation() put there. */
	CHECK(SHEmptyRecycleBinA(NULL, NULL, SHERB_NOCONFIRMATION) == S_OK);
	CHECK(SHQueryRecycleBinA("C:\\", &info) == S_OK);
	CHECK(info.i64Size == 0 && info.i64NumItems == 0);
	write_file("junk.txt", "12345");
	SHFILEOPSTRUCTA op = { 0 };
	op.wFunc = FO_DELETE;
	op.pFrom = "C:\\junk.txt\0";
	op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
	CHECK_EQ(SHFileOperationA(&op), 0);
	CHECK(!exists("junk.txt"));
	CHECK(SHQueryRecycleBinA(NULL, &info) == S_OK);
	CHECK(info.i64Size == 5 && info.i64NumItems == 1);
	CHECK(SHQueryRecycleBinA("D:\\", &info) == S_OK && !info.i64NumItems);
	info.cbSize = 4;
	CHECK(SHQueryRecycleBinA(NULL, &info) == E_INVALIDARG);

	/* The bin lists it by its old name. */
	IShellFolder *desk = NULL, *bin = NULL;
	CHECK(SHGetDesktopFolder(&desk) == S_OK);
	WCHAR *name = w16("::{645FF040-5081-101B-9F08-00AA002F954E}");
	LPITEMIDLIST pidl = NULL;
	CHECK(desk->lpVtbl->ParseDisplayName(desk, NULL, NULL, name, NULL, &pidl,
					     NULL) == S_OK);
	free(name);
	CHECK(desk->lpVtbl->BindToObject(desk, pidl, NULL, &IID_IShellFolder,
					 (void **)&bin) == S_OK && bin);
	ILFree(pidl);
	if (bin) {
		IEnumIDList *en = NULL;
		LPITEMIDLIST item;
		CHECK(bin->lpVtbl->EnumObjects(bin, NULL, SHCONTF_FOLDERS |
					       SHCONTF_NONFOLDERS, &en) == S_OK);
		CHECK(en->lpVtbl->Next(en, 1, &item, NULL) == S_OK);
		CHECK_NAME(bin, item, SHGDN_NORMAL, "junk.txt");
		ILFree(item);
		CHECK(en->lpVtbl->Next(en, 1, &item, NULL) == S_FALSE);
		en->lpVtbl->Release(en);
		bin->lpVtbl->Release(bin);
	}
	desk->lpVtbl->Release(desk);

	CHECK(SHEmptyRecycleBinA(NULL, NULL, SHERB_NOCONFIRMATION) == S_OK);
	info.cbSize = sizeof(info);
	CHECK(SHQueryRecycleBinA(NULL, &info) == S_OK && !info.i64NumItems &&
	      !info.i64Size);
}

static void test_shell_link(void)
{
	HRESULT (WINAPI *init)(void *) = get_from(ole, "CoInitialize");
	void (WINAPI *uninit)(void) = get_from(ole, "CoUninitialize");
	HRESULT (WINAPI *create)(REFCLSID, IUnknown *, DWORD, REFIID, void **) =
		get_from(ole, "CoCreateInstance");
	IShellLinkA *la = NULL;
	IShellLinkW *lw = NULL;
	IPersistFile *pf = NULL;
	char buf[MAX_PATH];
	int idx = 0;

	CHECK(SUCCEEDED(init(NULL)));
	CHECK(create(&CLSID_ShellLink, NULL, 1, &IID_IShellLinkA, (void **)&la) ==
	      S_OK && la);
	if (!la)
		return;
	write_file("target.exe", "MZ");
	CHECK(la->lpVtbl->SetPath(la, "C:\\target.exe") == S_OK);
	CHECK(la->lpVtbl->SetPath(la, "C:\\nowhere.exe") == S_FALSE);
	CHECK(la->lpVtbl->SetPath(la, "C:\\target.exe") == S_OK);
	CHECK(la->lpVtbl->SetArguments(la, "-x \"y z\"") == S_OK);
	CHECK(la->lpVtbl->SetDescription(la, "A target") == S_OK);
	CHECK(la->lpVtbl->SetWorkingDirectory(la, "C:\\") == S_OK);
	CHECK(la->lpVtbl->SetIconLocation(la, "C:\\target.exe", 2) == S_OK);
	CHECK(la->lpVtbl->SetHotkey(la, 0x0241) == S_OK);
	CHECK(la->lpVtbl->SetShowCmd(la, 3) == S_OK);
	CHECK(la->lpVtbl->QueryInterface(la, &IID_IPersistFile, (void **)&pf) ==
	      S_OK && pf);
	CHECK(pf->lpVtbl->IsDirty(pf) == S_OK);
	WCHAR *file = w16("C:\\link.lnk");
	CHECK(pf->lpVtbl->Save(pf, file, TRUE) == S_OK);
	CHECK(pf->lpVtbl->IsDirty(pf) == S_FALSE);
	pf->lpVtbl->Release(pf);
	la->lpVtbl->Release(la);

	/* The file: the header, LinkInfo with the path, then the strings. */
	FILE *f = fopen(on_disk("link.lnk"), "rb");
	unsigned char data[1024];
	size_t n = f ? fread(data, 1, sizeof(data), f) : 0;
	if (f)
		fclose(f);
	CHECK(n > 0x4c && data[0] == 0x4c && data[4] == 0x01 && data[5] == 0x14 &&
	      data[6] == 0x02 && data[0x14] == 0xf6);
	CHECK(memmem(data, n, "C:\\target.exe", 14) != NULL);

	/* Read back, through IShellLinkW. */
	CHECK(create(&CLSID_ShellLink, NULL, 1, &IID_IPersistFile, (void **)&pf) ==
	      S_OK && pf);
	if (pf) {
		CHECK(pf->lpVtbl->Load(pf, file, 0) == S_OK);
		CHECK(pf->lpVtbl->QueryInterface(pf, &IID_IShellLinkW,
						 (void **)&lw) == S_OK);
		LPOLESTR cur = NULL;
		CHECK(pf->lpVtbl->GetCurFile(pf, &cur) == S_OK && cur);
		task_free(cur);
		pf->lpVtbl->Release(pf);
	}
	if (lw) {
		WCHAR w[MAX_PATH];
		WIN32_FIND_DATAW fd;
		CHECK(lw->lpVtbl->GetPath(lw, w, MAX_PATH, &fd, 0) == S_OK);
		char *s = utf16_to_utf8_dup(w);
		CHECK_STR(s, "C:\\target.exe");
		free(s);
		s = utf16_to_utf8_dup(fd.cFileName);
		CHECK_STR(s, "target.exe");
		CHECK_EQ(fd.nFileSizeLow, 2);
		free(s);
		CHECK(lw->lpVtbl->GetArguments(lw, w, MAX_PATH) == S_OK);
		s = utf16_to_utf8_dup(w);
		CHECK_STR(s, "-x \"y z\"");
		free(s);
		CHECK(lw->lpVtbl->QueryInterface(lw, &IID_IShellLinkA,
						 (void **)&la) == S_OK);
		lw->lpVtbl->Release(lw);
	}
	if (la) {
		WORD key = 0;
		int show = 0;
		CHECK(la->lpVtbl->GetDescription(la, buf, MAX_PATH) == S_OK);
		CHECK_STR(buf, "A target");
		CHECK(la->lpVtbl->GetWorkingDirectory(la, buf, MAX_PATH) == S_OK);
		CHECK_STR(buf, "C:\\");
		CHECK(la->lpVtbl->GetIconLocation(la, buf, MAX_PATH, &idx) == S_OK);
		CHECK(!strcmp(buf, "C:\\target.exe") && idx == 2);
		CHECK(la->lpVtbl->GetHotkey(la, &key) == S_OK && key == 0x0241);
		CHECK(la->lpVtbl->GetShowCmd(la, &show) == S_OK && show == 3);
		/* Truncated to the buffer. */
		CHECK(la->lpVtbl->GetPath(la, buf, 5, NULL, 0) == S_OK);
		CHECK_STR(buf, "C:\\t");
		CHECK(la->lpVtbl->Resolve(la, NULL, SLR_NO_UI) == S_OK);
		LPITEMIDLIST pidl = NULL;
		CHECK(la->lpVtbl->GetIDList(la, &pidl) == S_OK && pidl);
		FN(BOOL, SHGetPathFromIDListA, LPCITEMIDLIST, LPSTR);
		FN(void, ILFree, LPITEMIDLIST);
		CHECK(SHGetPathFromIDListA(pidl, buf));
		CHECK_STR(buf, "C:\\target.exe");
		ILFree(pidl);
		/* A stream round trip. */
		IPersistStream *ps = NULL;
		CHECK(la->lpVtbl->QueryInterface(la, &IID_IPersistStream,
						 (void **)&ps) == S_OK);
		ULARGE_INTEGER max;
		CHECK(ps->lpVtbl->GetSizeMax(ps, &max) == S_OK &&
		      max.QuadPart == n);
		ps->lpVtbl->Release(ps);
		la->lpVtbl->Release(la);
	}
	/* Not a link. */
	write_file("bad.lnk", "not a shortcut at all, no header here......");
	CHECK(create(&CLSID_ShellLink, NULL, 1, &IID_IPersistFile, (void **)&pf) ==
	      S_OK);
	WCHAR *badf = w16("C:\\bad.lnk"), *missing = w16("C:\\missing.lnk");
	CHECK(pf->lpVtbl->Load(pf, badf, 0) == E_FAIL);
	CHECK(pf->lpVtbl->Load(pf, missing, 0) ==
	      HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
	pf->lpVtbl->Release(pf);
	free(badf);
	free(missing);
	free(file);
	uninit();
}

/* Relative paths, saving copies, Unicode targets, the default prompt. */
static void test_shell_link_more(void)
{
	HRESULT (WINAPI *init)(void *) = get_from(ole, "CoInitialize");
	void (WINAPI *uninit)(void) = get_from(ole, "CoUninitialize");
	HRESULT (WINAPI *create)(REFCLSID, IUnknown *, DWORD, REFIID, void **) =
		get_from(ole, "CoCreateInstance");
	IShellLinkW *lw = NULL;
	IPersistFile *pf = NULL;
	WCHAR w[MAX_PATH];

	CHECK(SUCCEEDED(init(NULL)));
	CHECK(create(&CLSID_ShellLink, NULL, 1, &IID_IShellLinkW, (void **)&lw) ==
	      S_OK && lw);
	if (!lw)
		return;
	CHECK(lw->lpVtbl->QueryInterface(lw, &IID_IPersistFile, (void **)&pf) ==
	      S_OK && pf);

	/* A new link has no file: S_FALSE, with the default prompt. */
	LPOLESTR cur = NULL;
	CHECK(pf->lpVtbl->GetCurFile(pf, &cur) == S_FALSE && cur);
	if (cur) {
		char *c = utf16_to_utf8_dup(cur);
		CHECK_STR(c, "*.lnk");
		free(c);
		task_free(cur);
	}

	/* A target the ANSI code page can't spell survives a reload. */
	mkdir(on_disk("old"), 0755);
	write_file("old/\xe6\x97\xa5.exe", "MZ");
	WCHAR *t = w16("C:\\old\\\xe6\x97\xa5.exe");
	WCHAR *lnk = w16("C:\\old\\run.lnk");
	CHECK(lw->lpVtbl->SetPath(lw, t) == S_OK);
	/* SetRelativePath takes the link's own name. */
	CHECK(lw->lpVtbl->SetRelativePath(lw, lnk, 0) == S_OK);
	CHECK(pf->lpVtbl->Save(pf, lnk, TRUE) == S_OK);

	/* Saving a copy leaves the current file and dirty flag alone. */
	WCHAR *d = w16("edited");
	CHECK(lw->lpVtbl->SetDescription(lw, d) == S_OK);
	free(d);
	WCHAR *copy = w16("C:\\copy.lnk");
	CHECK(pf->lpVtbl->Save(pf, copy, FALSE) == S_OK);
	CHECK(pf->lpVtbl->IsDirty(pf) == S_OK);
	CHECK(pf->lpVtbl->GetCurFile(pf, &cur) == S_OK && cur);
	if (cur) {
		char *c = utf16_to_utf8_dup(cur);
		CHECK_STR(c, "C:\\old\\run.lnk");
		free(c);
		task_free(cur);
	}
	free(copy);
	pf->lpVtbl->Release(pf);
	lw->lpVtbl->Release(lw);
	lw = NULL;
	pf = NULL;
	/* The copy holds what was saved. */
	copy = w16("C:\\copy.lnk");
	CHECK(create(&CLSID_ShellLink, NULL, 1, &IID_IPersistFile, (void **)&pf) ==
	      S_OK && pf);
	if (pf) {
		CHECK(pf->lpVtbl->Load(pf, copy, 0) == S_OK);
		CHECK(pf->lpVtbl->QueryInterface(pf, &IID_IShellLinkW,
						 (void **)&lw) == S_OK);
		pf->lpVtbl->Release(pf);
		pf = NULL;
	}
	if (lw) {
		CHECK(lw->lpVtbl->GetDescription(lw, w, MAX_PATH) == S_OK);
		char *c = utf16_to_utf8_dup(w);
		CHECK_STR(c, "edited");
		free(c);
		lw->lpVtbl->Release(lw);
		lw = NULL;
	}
	free(copy);

	/* Both moved to C:\new: Resolve finds the target beside the link. */
	mkdir(on_disk("new"), 0755);
	CHECK(!rename(on_disk("old/\xe6\x97\xa5.exe"),
		      on_disk("new/\xe6\x97\xa5.exe")));
	CHECK(!rename(on_disk("old/run.lnk"), on_disk("new/run.lnk")));
	/* Loaded by a name with forward slashes; and the link file is
	 * private, which a save keeps it. */
	WCHAR *nlnk = w16("C:/new/run.lnk");
	CHECK(!chmod(on_disk("new/run.lnk"), 0600));
	CHECK(create(&CLSID_ShellLink, NULL, 1, &IID_IPersistFile, (void **)&pf) ==
	      S_OK && pf);
	if (pf) {
		CHECK(pf->lpVtbl->Load(pf, nlnk, 0) == S_OK);
		CHECK(pf->lpVtbl->QueryInterface(pf, &IID_IShellLinkW,
						 (void **)&lw) == S_OK);
	}
	if (lw) {
		CHECK(lw->lpVtbl->GetPath(lw, w, MAX_PATH, NULL, 0) == S_OK);
		char *c = utf16_to_utf8_dup(w);
		CHECK_STR(c, "C:\\old\\\xe6\x97\xa5.exe");
		free(c);
		CHECK(lw->lpVtbl->Resolve(lw, NULL, SLR_NO_UI) == S_OK);
		CHECK(lw->lpVtbl->GetPath(lw, w, MAX_PATH, NULL, 0) == S_OK);
		c = utf16_to_utf8_dup(w);
		CHECK_STR(c, "C:\\new\\\xe6\x97\xa5.exe");
		free(c);
		/* The ID list names the new place too. */
		LPITEMIDLIST pidl = NULL;
		FN(BOOL, SHGetPathFromIDListW, LPCITEMIDLIST, LPWSTR);
		FN(void, ILFree, LPITEMIDLIST);
		CHECK(lw->lpVtbl->GetIDList(lw, &pidl) == S_OK && pidl);
		if (pidl) {
			CHECK(SHGetPathFromIDListW(pidl, w));
			c = utf16_to_utf8_dup(w);
			CHECK_STR(c, "C:\\new\\\xe6\x97\xa5.exe");
			free(c);
			ILFree(pidl);
		}
		if (pf) {
			struct stat st;
			CHECK(pf->lpVtbl->Save(pf, NULL, TRUE) == S_OK);
			CHECK(!stat(on_disk("new/run.lnk"), &st) &&
			      (st.st_mode & 0777) == 0600);
		}
		lw->lpVtbl->Release(lw);
	}
	if (pf)
		pf->lpVtbl->Release(pf);
	free(nlnk);
	free(t);
	free(lnk);
	uninit();
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
	ole = mod_load("ole32.dll", NULL);
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
	test_known_folders();
	test_namespace();
	test_recycle_bin();
	test_shell_link();
	test_shell_link_more();
	mod_free(ole);

	mod_free(k32);
	mod_free(sh);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", root);
	return test_summary("test_shell32");
}
