/*
 * comdlg32 through its export table: the file dialogs answering from
 * $VFW_OPEN_FILE and $VFW_SAVE_FILE, GetFileTitle, the colour and font
 * dialogs answering OK, and the printing and find dialogs failing. The
 * Windows world lives in a temporary $VFW_ROOT.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "win32/codepage.h"

static struct module *dlg, *k32;
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
		(ret (WINAPI *)(__VA_ARGS__))get_from(dlg, #name)
#define K32(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(k32, #name)

static const char *on_disk(const char *rel)
{
	static char buf[4][1024];
	static int next;
	char *b = buf[next++ % 4];

	snprintf(b, sizeof(buf[0]), "%s/drive_c/%s", root, rel);
	return b;
}

static void touch(const char *rel)
{
	FILE *f = fopen(on_disk(rel), "w");
	CHECK(f != NULL);
	if (f)
		fclose(f);
}

static void ofn_init(OPENFILENAMEA *ofn, char *buf, DWORD size)
{
	memset(ofn, 0, sizeof(*ofn));
	ofn->lStructSize = sizeof(*ofn);
	ofn->lpstrFile = buf;
	ofn->nMaxFile = size;
	buf[0] = '\0';
}

static const char *cwd(void)
{
	K32(DWORD, GetCurrentDirectoryA, DWORD, LPSTR);
	static char buf[MAX_PATH];

	buf[0] = '\0';
	GetCurrentDirectoryA(sizeof(buf), buf);
	return buf;
}

static void test_open(void)
{
	FN(BOOL, GetOpenFileNameA, LPOPENFILENAMEA);
	FN(BOOL, GetOpenFileNameW, LPOPENFILENAMEW);
	FN(DWORD, CommDlgExtendedError, void);
	K32(BOOL, SetCurrentDirectoryA, LPCSTR);
	OPENFILENAMEA ofn;
	char buf[MAX_PATH], title[16];

	/* Nothing to answer with: cancelled, which isn't an error. */
	unsetenv("VFW_OPEN_FILE");
	ofn_init(&ofn, buf, sizeof(buf));
	CHECK(!GetOpenFileNameA(&ofn));
	CHECK_EQ(CommDlgExtendedError(), 0);
	setenv("VFW_OPEN_FILE", "Cancel", 1);
	CHECK(!GetOpenFileNameA(&ofn));
	CHECK_EQ(CommDlgExtendedError(), 0);
	setenv("VFW_OPEN_FILE", "", 1);
	CHECK(!GetOpenFileNameA(&ofn));

	/* A relative answer is in the initial directory; it must exist. */
	mkdir(on_disk("data"), 0755);
	setenv("VFW_OPEN_FILE", "clip.avi", 1);
	ofn_init(&ofn, buf, sizeof(buf));
	ofn.lpstrInitialDir = "C:\\DATA";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_READONLY | OFN_NOCHANGEDIR;
	CHECK(!GetOpenFileNameA(&ofn));
	CHECK_EQ(CommDlgExtendedError(), 0);
	touch("data/clip.avi");
	ofn.lpstrFileTitle = title;
	ofn.nMaxFileTitle = sizeof(title);
	CHECK(SetCurrentDirectoryA("C:\\"));
	CHECK(GetOpenFileNameA(&ofn));
	CHECK_EQ(CommDlgExtendedError(), 0);
	CHECK_STR(buf, "C:\\DATA\\clip.avi");
	CHECK_EQ(ofn.nFileOffset, 8);
	CHECK_EQ(ofn.nFileExtension, 13);
	CHECK_STR(title, "clip.avi");
	CHECK_EQ(ofn.Flags, OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR);
	CHECK_STR(cwd(), "C:\\");

	/* Without OFN_NOCHANGEDIR the current directory follows. */
	ofn.Flags = OFN_FILEMUSTEXIST;
	CHECK(GetOpenFileNameA(&ofn));
	CHECK_STR(cwd(), "C:\\data");	/* as it is on disk */
	CHECK(SetCurrentDirectoryA("C:\\"));

	/* A file title that doesn't fit is cut short. */
	ofn.nMaxFileTitle = 5;
	CHECK(GetOpenFileNameA(&ofn));
	CHECK_STR(title, "clip");

	/* An absolute Unix path is on Z:. A name without an extension. */
	setenv("VFW_OPEN_FILE", "/tmp/some file", 1);
	ofn_init(&ofn, buf, sizeof(buf));
	ofn.Flags = OFN_NOCHANGEDIR;
	CHECK(GetOpenFileNameA(&ofn));
	CHECK_STR(buf, "Z:\\tmp\\some file");
	CHECK_EQ(ofn.nFileOffset, 7);
	CHECK_EQ(ofn.nFileExtension, 0);

	/* A folder that must exist doesn't. */
	setenv("VFW_OPEN_FILE", "C:\\nowhere\\x.avi", 1);
	ofn.Flags = OFN_PATHMUSTEXIST;
	CHECK(!GetOpenFileNameA(&ofn));
	CHECK_EQ(CommDlgExtendedError(), 0);
	ofn.Flags = 0;
	CHECK(GetOpenFileNameA(&ofn));
	CHECK_STR(buf, "C:\\nowhere\\x.avi");
	CHECK(SetCurrentDirectoryA("C:\\"));

	/* Too small: the size needed is in the first WORD. */
	setenv("VFW_OPEN_FILE", "C:\\data\\clip.avi", 1);
	ofn_init(&ofn, buf, 10);
	memset(buf, 'x', sizeof(buf));
	CHECK(!GetOpenFileNameA(&ofn));
	CHECK_EQ(CommDlgExtendedError(), FNERR_BUFFERTOOSMALL);
	WORD need;
	memcpy(&need, buf, sizeof(need));
	CHECK_EQ(need, 17);
	CHECK_EQ(buf[2], 'x');
	ofn_init(&ofn, buf, 17);
	ofn.Flags = OFN_NOCHANGEDIR;
	CHECK(GetOpenFileNameA(&ofn));
	CHECK_STR(buf, "C:\\data\\clip.avi");

	/* A multiple selection of one ends with two NULs. */
	ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
	CHECK(!GetOpenFileNameA(&ofn));
	CHECK_EQ(CommDlgExtendedError(), FNERR_BUFFERTOOSMALL);
	ofn.nMaxFile = 18;
	memset(buf, 'x', sizeof(buf));
	CHECK(GetOpenFileNameA(&ofn));
	CHECK_STR(buf, "C:\\data\\clip.avi");
	CHECK_EQ(buf[17], '\0');

	/* The structure's size: the full one, or Windows NT 4's. */
	ofn.Flags = OFN_NOCHANGEDIR;
	ofn.lStructSize = 76;
	CHECK(GetOpenFileNameA(&ofn));
	ofn.lStructSize = 80;
	CHECK(!GetOpenFileNameA(&ofn));
	CHECK_EQ(CommDlgExtendedError(), CDERR_STRUCTSIZE);
	CHECK(!GetOpenFileNameA(NULL));
	CHECK_EQ(CommDlgExtendedError(), CDERR_INITIALIZATION);
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFile = NULL;
	CHECK(!GetOpenFileNameA(&ofn));
	CHECK_EQ(CommDlgExtendedError(), CDERR_INITIALIZATION);

	/* The wide one counts characters; é is one here, two in UTF-8. */
	setenv("VFW_OPEN_FILE", "C:\\caf\xc3\xa9\\t\xc3\xa9st.avi", 1);
	OPENFILENAMEW ow;
	WCHAR wbuf[MAX_PATH], wtitle[32];
	memset(&ow, 0, sizeof(ow));
	ow.lStructSize = sizeof(ow);
	ow.lpstrFile = wbuf;
	ow.nMaxFile = MAX_PATH;
	ow.lpstrFileTitle = wtitle;
	ow.nMaxFileTitle = 32;
	ow.Flags = OFN_NOCHANGEDIR;
	CHECK(GetOpenFileNameW(&ow));
	char *u = utf16_to_utf8_dup(wbuf);
	CHECK_STR(u, "C:\\caf\xc3\xa9\\t\xc3\xa9st.avi");
	free(u);
	u = utf16_to_utf8_dup(wtitle);
	CHECK_STR(u, "t\xc3\xa9st.avi");
	free(u);
	CHECK_EQ(ow.nFileOffset, 8);
	CHECK_EQ(ow.nFileExtension, 13);

	/* The ANSI one gets the ANSI code page (é is 0xe9). */
	ofn_init(&ofn, buf, sizeof(buf));
	ofn.Flags = OFN_NOCHANGEDIR;
	CHECK(GetOpenFileNameA(&ofn));
	CHECK_STR(buf, "C:\\caf\xe9\\t\xe9st.avi");
	CHECK_EQ(ofn.nFileExtension, 13);
	unsetenv("VFW_OPEN_FILE");
}

static void test_save(void)
{
	FN(BOOL, GetSaveFileNameA, LPOPENFILENAMEA);
	FN(DWORD, CommDlgExtendedError, void);
	OPENFILENAMEA ofn;
	char buf[MAX_PATH];

	unsetenv("VFW_SAVE_FILE");
	setenv("VFW_OPEN_FILE", "C:\\data\\clip.avi", 1);
	ofn_init(&ofn, buf, sizeof(buf));
	CHECK(!GetSaveFileNameA(&ofn));		/* not the open dialog's */
	CHECK_EQ(CommDlgExtendedError(), 0);
	unsetenv("VFW_OPEN_FILE");

	/* The default extension goes on a name without one. */
	mkdir(on_disk("out"), 0755);
	setenv("VFW_SAVE_FILE", "C:\\out\\stats", 1);
	ofn.lpstrDefExt = "txt";
	ofn.Flags = OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT |
		    OFN_EXTENSIONDIFFERENT;
	CHECK(GetSaveFileNameA(&ofn));
	CHECK_STR(buf, "C:\\out\\stats.txt");
	CHECK_EQ(ofn.nFileOffset, 7);
	CHECK_EQ(ofn.nFileExtension, 13);
	CHECK_EQ(ofn.Flags, OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT);

	/* Another extension stays, and is flagged. */
	setenv("VFW_SAVE_FILE", "C:\\out\\stats.log", 1);
	CHECK(GetSaveFileNameA(&ofn));
	CHECK_STR(buf, "C:\\out\\stats.log");
	CHECK(ofn.Flags & OFN_EXTENSIONDIFFERENT);
	setenv("VFW_SAVE_FILE", "C:\\out\\stats.TXT", 1);
	CHECK(GetSaveFileNameA(&ofn));
	CHECK(!(ofn.Flags & OFN_EXTENSIONDIFFERENT));

	/* The file needn't exist; its folder must if the flags say so. */
	setenv("VFW_SAVE_FILE", "C:\\missing\\stats", 1);
	ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
	CHECK(!GetSaveFileNameA(&ofn));
	setenv("VFW_SAVE_FILE", "C:\\out\\", 1);
	ofn.Flags = OFN_NOCHANGEDIR;
	CHECK(!GetSaveFileNameA(&ofn));		/* a folder, not a file */
	CHECK_EQ(CommDlgExtendedError(), FNERR_INVALIDFILENAME);

	/* A file in the root: the current directory moves to C:\. */
	K32(BOOL, SetCurrentDirectoryA, LPCSTR);
	CHECK(SetCurrentDirectoryA("C:\\out"));
	setenv("VFW_SAVE_FILE", "C:\\top.txt", 1);
	ofn.Flags = OFN_PATHMUSTEXIST;
	CHECK(GetSaveFileNameA(&ofn));
	CHECK_STR(buf, "C:\\top.txt");
	CHECK_STR(cwd(), "C:\\");
	unsetenv("VFW_SAVE_FILE");
}

static void test_file_title(void)
{
	FN(short, GetFileTitleA, LPCSTR, LPSTR, WORD);
	FN(short, GetFileTitleW, LPCWSTR, LPWSTR, WORD);
	char buf[16];
	WCHAR wbuf[16];

	CHECK_EQ(GetFileTitleA("C:\\dir\\name.txt", buf, sizeof(buf)), 0);
	CHECK_STR(buf, "name.txt");
	CHECK_EQ(GetFileTitleA("C:name.txt", buf, sizeof(buf)), 0);
	CHECK_STR(buf, "name.txt");
	CHECK_EQ(GetFileTitleA("a/b", buf, sizeof(buf)), 0);
	CHECK_STR(buf, "b");
	CHECK_EQ(GetFileTitleA("plain", buf, sizeof(buf)), 0);
	CHECK_STR(buf, "plain");
	CHECK_EQ(GetFileTitleA("C:\\dir\\name.txt", buf, 8), 9);
	CHECK_EQ(GetFileTitleA("C:\\dir\\name.txt", NULL, 0), 9);
	CHECK_EQ(GetFileTitleA("C:\\dir\\", buf, sizeof(buf)), -1);
	CHECK_EQ(GetFileTitleA("C:\\*.txt", buf, sizeof(buf)), -1);
	CHECK_EQ(GetFileTitleA("", buf, sizeof(buf)), -1);
	CHECK_EQ(GetFileTitleA(NULL, buf, sizeof(buf)), -1);

	WCHAR *in = utf8_to_utf16_dup("C:\\x\\\xc3\xa9t\xc3\xa9.bmp");
	CHECK_EQ(GetFileTitleW(in, wbuf, 8), 0);
	char *u = utf16_to_utf8_dup(wbuf);
	CHECK_STR(u, "\xc3\xa9t\xc3\xa9.bmp");
	free(u);
	CHECK_EQ(GetFileTitleW(in, wbuf, 7), 8);
	free(in);
}

static void test_color_font(void)
{
	FN(BOOL, ChooseColorA, LPCHOOSECOLORA);
	FN(BOOL, ChooseFontA, LPCHOOSEFONTA);
	FN(BOOL, ChooseFontW, LPCHOOSEFONTW);
	FN(DWORD, CommDlgExtendedError, void);
	COLORREF custom[16] = { 0 };
	CHOOSECOLORA cc;

	memset(&cc, 0, sizeof(cc));
	cc.lStructSize = sizeof(cc);
	cc.lpCustColors = custom;
	cc.rgbResult = RGB(1, 2, 3);
	cc.Flags = CC_RGBINIT;
	CHECK(ChooseColorA(&cc));
	CHECK_EQ(cc.rgbResult, RGB(1, 2, 3));
	cc.Flags = 0;
	CHECK(ChooseColorA(&cc));
	CHECK_EQ(cc.rgbResult, 0);
	cc.lStructSize = 4;
	CHECK(!ChooseColorA(&cc));
	CHECK_EQ(CommDlgExtendedError(), CDERR_STRUCTSIZE);

	/* With nothing to start from: 8-point MS Sans Serif. */
	LOGFONTA lf;
	CHOOSEFONTA cf;
	memset(&lf, 0x55, sizeof(lf));
	memset(&cf, 0, sizeof(cf));
	cf.lStructSize = sizeof(cf);
	cf.lpLogFont = &lf;
	cf.Flags = CF_SCREENFONTS;
	CHECK(ChooseFontA(&cf));
	CHECK_EQ(CommDlgExtendedError(), 0);
	CHECK_STR(lf.lfFaceName, "MS Sans Serif");
	CHECK_EQ(lf.lfHeight, -11);
	CHECK_EQ(lf.lfWeight, FW_NORMAL);
	CHECK_EQ(lf.lfItalic, 0);
	CHECK_EQ(cf.iPointSize, 80);
	CHECK_EQ(cf.nFontType, SCREEN_FONTTYPE | REGULAR_FONTTYPE);

	/* Or from the caller's font. */
	strcpy(lf.lfFaceName, "Arial");
	lf.lfHeight = -16;
	lf.lfWeight = FW_BOLD;
	lf.lfItalic = 1;
	cf.Flags = CF_INITTOLOGFONTSTRUCT;
	CHECK(ChooseFontA(&cf));
	CHECK_STR(lf.lfFaceName, "Arial");
	CHECK_EQ(cf.iPointSize, 120);
	CHECK_EQ(cf.nFontType,
		 SCREEN_FONTTYPE | BOLD_FONTTYPE | ITALIC_FONTTYPE);

	cf.Flags = CF_LIMITSIZE;
	cf.nSizeMin = 10;
	cf.nSizeMax = 8;
	CHECK(!ChooseFontA(&cf));
	CHECK_EQ(CommDlgExtendedError(), CFERR_MAXLESSTHANMIN);
	cf.lpLogFont = NULL;
	cf.Flags = 0;
	CHECK(!ChooseFontA(&cf));
	CHECK_EQ(CommDlgExtendedError(), CDERR_INITIALIZATION);

	LOGFONTW lw;
	CHOOSEFONTW cw;
	memset(&cw, 0, sizeof(cw));
	cw.lStructSize = sizeof(cw);
	cw.lpLogFont = &lw;
	CHECK(ChooseFontW(&cw));
	char *u = utf16_to_utf8_dup(lw.lfFaceName);
	CHECK_STR(u, "MS Sans Serif");
	free(u);
}

static void test_print_find(void)
{
	FN(BOOL, PrintDlgA, LPPRINTDLGA);
	FN(BOOL, PrintDlgW, LPPRINTDLGW);
	FN(HRESULT, PrintDlgExA, LPPRINTDLGEXA);
	FN(BOOL, PageSetupDlgA, LPPAGESETUPDLGA);
	FN(HWND, FindTextA, LPFINDREPLACEA);
	FN(HWND, ReplaceTextW, LPFINDREPLACEW);
	FN(DWORD, CommDlgExtendedError, void);

	/* The packed layouts, as on 32-bit Windows. */
	CHECK_EQ(sizeof(PRINTDLGA), 66);
	CHECK_EQ(sizeof(OPENFILENAMEA), 88);
	CHECK_EQ(sizeof(CHOOSEFONTA), 60);

	PRINTDLGA pd;
	memset(&pd, 0, sizeof(pd));
	pd.lStructSize = sizeof(pd);
	pd.Flags = PD_RETURNDEFAULT;
	CHECK(!PrintDlgA(&pd));
	CHECK_EQ(CommDlgExtendedError(), PDERR_NODEFAULTPRN);
	pd.lStructSize = 68;
	CHECK(!PrintDlgA(&pd));
	CHECK_EQ(CommDlgExtendedError(), CDERR_STRUCTSIZE);
	PRINTDLGW pw;
	memset(&pw, 0, sizeof(pw));
	pw.lStructSize = sizeof(pw);
	CHECK(!PrintDlgW(&pw));
	CHECK_EQ(CommDlgExtendedError(), PDERR_NODEFAULTPRN);

	PRINTDLGEXA px;
	memset(&px, 0, sizeof(px));
	px.lStructSize = sizeof(px);
	px.dwResultAction = PD_RESULT_PRINT;
	CHECK_EQ(PrintDlgExA(&px), E_FAIL);
	CHECK_EQ(px.dwResultAction, PD_RESULT_CANCEL);
	CHECK_EQ(CommDlgExtendedError(), PDERR_NODEFAULTPRN);
	px.lStructSize = 8;
	CHECK_EQ(PrintDlgExA(&px), E_INVALIDARG);
	CHECK_EQ(CommDlgExtendedError(), CDERR_STRUCTSIZE);

	PAGESETUPDLGA ps;
	memset(&ps, 0, sizeof(ps));
	ps.lStructSize = sizeof(ps);
	CHECK(!PageSetupDlgA(&ps));
	CHECK_EQ(CommDlgExtendedError(), PDERR_NODEFAULTPRN);

	char what[32] = "needle";
	FINDREPLACEA fr;
	memset(&fr, 0, sizeof(fr));
	fr.lStructSize = sizeof(fr);
	fr.lpstrFindWhat = what;
	fr.wFindWhatLen = sizeof(what);
	CHECK(FindTextA(&fr) == NULL);
	CHECK_EQ(CommDlgExtendedError(), CDERR_DIALOGFAILURE);
	fr.wFindWhatLen = 0;
	CHECK(FindTextA(&fr) == NULL);
	CHECK_EQ(CommDlgExtendedError(), FRERR_BUFFERLENGTHZERO);

	WCHAR find[8] = { 'a', 0 }, with[8] = { 'b', 0 };
	FINDREPLACEW rw;
	memset(&rw, 0, sizeof(rw));
	rw.lStructSize = sizeof(rw);
	rw.lpstrFindWhat = find;
	rw.wFindWhatLen = 8;
	CHECK(ReplaceTextW(&rw) == NULL);	/* nothing to replace with */
	CHECK_EQ(CommDlgExtendedError(), FRERR_BUFFERLENGTHZERO);
	rw.lpstrReplaceWith = with;
	rw.wReplaceWithLen = 8;
	CHECK(ReplaceTextW(&rw) == NULL);
	CHECK_EQ(CommDlgExtendedError(), CDERR_DIALOGFAILURE);
}

static void test_misc(void)
{
	FN(HRESULT, DllGetClassObject, REFCLSID, REFIID, void **);
	FN(HRESULT, DllRegisterServer, void);
	static const GUID any = { 1, 2, 3, { 4 } };
	void *out = &out;

	CHECK_EQ(DllGetClassObject(&any, &any, &out), CLASS_E_CLASSNOTAVAILABLE);
	CHECK(out == NULL);
	CHECK_EQ(DllRegisterServer(), S_OK);
}

int main(void)
{
	const char *tmpdir = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/vfw-comdlg-XXXXXX",
		 tmpdir && *tmpdir ? tmpdir : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", root, 1);
	setenv("VFW_REGISTRY", "", 1);
	char c[300];
	snprintf(c, sizeof(c), "%s/drive_c", root);
	mkdir(c, 0755);

	CHECK_EQ(runtime_enter(), 0);
	dlg = mod_load("comdlg32.dll", NULL);
	k32 = mod_load("kernel32.dll", NULL);
	CHECK(dlg && dlg->builtin && !dlg->builtin->fallback && k32);
	if (!dlg || !k32)
		return test_summary("test_comdlg32");

	test_open();
	test_save();
	test_file_title();
	test_color_font();
	test_print_find();
	test_misc();

	mod_free(k32);
	mod_free(dlg);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", root);
	return test_summary("test_comdlg32");
}
