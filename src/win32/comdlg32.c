/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comdlg32.dll, headless: the common dialogs, answered without a GUI.
 *
 * - GetOpenFileName and GetSaveFileName answer with the file named by
 *   $VFW_OPEN_FILE or $VFW_SAVE_FILE (a Windows path, relative ones
 *   resolved against the dialog's initial directory, or an absolute Unix
 *   one); unset, empty or "cancel", they are cancelled, as
 *   SHBrowseForFolder is. The answer is checked as the dialog would
 *   (OFN_FILEMUSTEXIST, OFN_PATHMUSTEXIST), gets lpstrDefExt appended
 *   when it has no extension, and the current directory moves to its
 *   folder unless OFN_NOCHANGEDIR. Hooks and templates are never used.
 * - ChooseColor and ChooseFont answer as if OK were pressed on what they
 *   opened with, as MessageBox answers with its default button.
 * - There are no printers: PrintDlg, PrintDlgEx and PageSetupDlg fail
 *   with PDERR_NODEFAULTPRN, as Windows does with none installed.
 * - FindText and ReplaceText are modeless dialogs, which can't be shown:
 *   they fail, as CreateDialog does.
 *
 * Each says what it was asked on stderr. CommDlgExtendedError is kept
 * per thread.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "win32/codepage.h"
#include "win32/commdlg.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

static _Thread_local DWORD ext_error;

WINFN(DWORD) dlg_CommDlgExtendedError(void)
{
	return ext_error;
}

/* The UTF-8 form of an ANSI or UTF-16 argument (malloc()ed; NULL in, out). */
static char *arg_utf8(const void *s, int wide)
{
	if (!s)
		return NULL;
	return wide ? utf16_to_utf8_dup(s) : acp_to_utf8(s);
}

static const char *or_empty(const char *s)
{
	return s ? s : "";
}

/* File dialogs. */

/* Whether a Windows path names something that exists, or a directory. */
static int win_exists(const char *win, int want_dir)
{
	char *u = path_to_unix(win);
	struct stat st;
	int r = u && !stat(u, &st) && (!want_dir || S_ISDIR(st.st_mode));

	free(u);
	return r;
}

static int is_relative(const char *p)
{
	return !(p[0] && p[1] == ':') && p[0] != '\\' && p[0] != '/';
}

/* The part of a Windows path after its last separator. */
static const char *file_part(const char *p)
{
	const char *f = p;

	for (; *p; p++)
		if (*p == '\\' || *p == '/' || *p == ':')
			f = p + 1;
	return f;
}

/*
 * The dialog's answer as a full Windows path (malloc()ed), or NULL when
 * it is cancelled (ext_error says whether that is a failure).
 */
static char *answer_file(int save, const char *title, DWORD flags,
			 const char *initial_dir, const char *def_ext)
{
	const char *fn = save ? "GetSaveFileName" : "GetOpenFileName";
	const char *var = save ? "VFW_SAVE_FILE" : "VFW_OPEN_FILE";
	const char *want = getenv(var);

	if (!want || !*want || !strcasecmp(want, "cancel")) {
		fprintf(stderr, "%s [%s]: (no GUI: cancelled; set %s to "
			"answer)\n", fn, or_empty(title), var);
		return NULL;
	}

	char *name = want[0] == '/' ? path_to_windows(want) : strdup(want);
	if (name && is_relative(name) && initial_dir && *initial_dir) {
		size_t n = strlen(initial_dir);
		char *joined = malloc(n + strlen(name) + 2);
		if (joined)
			sprintf(joined, "%s%s%s", initial_dir,
				initial_dir[n - 1] == '\\' ? "" : "\\", name);
		free(name);
		name = joined;
	}
	char *full = name ? k32_full_path(name) : NULL;
	free(name);
	if (!full || !*file_part(full)) {
		fprintf(stderr, "%s [%s]: %s=%s: not a file name\n", fn,
			or_empty(title), var, want);
		free(full);
		ext_error = FNERR_INVALIDFILENAME;
		return NULL;
	}

	/* The default extension goes on a name without one. */
	if (def_ext && *def_ext && !strchr(file_part(full), '.')) {
		char *with = malloc(strlen(full) + strlen(def_ext) + 2);
		if (!with) {
			free(full);
			ext_error = CDERR_MEMALLOCFAILURE;
			return NULL;
		}
		sprintf(with, "%s.%s", full, def_ext);
		free(full);
		full = with;
	}

	char *dir = strdup(full);
	if (dir) {
		/* Without the separator, except in a root such as C:\. */
		char *f = (char *)file_part(dir);
		if (f - dir > 3)
			f--;
		*f = '\0';
	}
	const char *why = NULL;
	if ((flags & OFN_FILEMUSTEXIST) && !win_exists(full, 0))
		why = "the file does not exist";
	else if ((flags & (OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST)) &&
		 dir && *dir && !win_exists(dir, 1))
		why = "the folder does not exist";
	if (why) {
		fprintf(stderr, "%s [%s]: %s=%s: %s; cancelled\n", fn,
			or_empty(title), var, want, why);
		free(dir);
		free(full);
		return NULL;
	}
	if (!(flags & OFN_NOCHANGEDIR) && dir && *dir)
		path_chdir(dir);
	free(dir);
	fprintf(stderr, "%s [%s]: (no GUI: answering %s, as %s says)\n", fn,
		or_empty(title), full, var);
	return full;
}

/*
 * Puts the answer into an OPENFILENAME (A or W, which differ only in
 * their strings): lpstrFile, the offsets, lpstrFileTitle and Flags.
 * Returns FALSE with ext_error set if lpstrFile is too small.
 */
static BOOL fill_ofn(OPENFILENAMEW *ofn, int wide, const char *full,
		     const char *def_ext)
{
	size_t len, off = 0, ext = 0, unit = wide ? sizeof(WCHAR) : 1;
	void *s = wide ? (void *)utf8_to_utf16_dup(full) :
		  (void *)utf8_to_acp_dup(full);

	if (!s) {
		ext_error = CDERR_MEMALLOCFAILURE;
		return FALSE;
	}
	/* Offsets of the name and its extension, in characters. */
	for (len = 0;; len++) {
		unsigned c = wide ? ((WCHAR *)s)[len] : ((BYTE *)s)[len];
		if (!c)
			break;
		if (c == '\\')
			off = len + 1, ext = 0;
		else if (c == '.')
			ext = len + 1;
	}
	/* A multiple selection ends with a second NUL. */
	size_t need = len + 1 + !!(ofn->Flags & OFN_ALLOWMULTISELECT);
	if (ofn->nMaxFile < need) {
		if (ofn->nMaxFile * unit >= sizeof(WORD))
			memcpy(ofn->lpstrFile, &(WORD){ (WORD)need },
			       sizeof(WORD));
		free(s);
		ext_error = FNERR_BUFFERTOOSMALL;
		return FALSE;
	}
	memset(ofn->lpstrFile, 0, need * unit);
	memcpy(ofn->lpstrFile, s, len * unit);
	ofn->nFileOffset = (WORD)off;
	ofn->nFileExtension = ext > off ? (WORD)ext : 0;

	if (ofn->lpstrFileTitle && ofn->nMaxFileTitle) {
		size_t n = len - off;
		if (n >= ofn->nMaxFileTitle)
			n = ofn->nMaxFileTitle - 1;
		memcpy(ofn->lpstrFileTitle, (BYTE *)s + off * unit, n * unit);
		memset((BYTE *)ofn->lpstrFileTitle + n * unit, 0, unit);
	}

	ofn->Flags &= ~(OFN_READONLY | OFN_EXTENSIONDIFFERENT);
	const char *dot = strrchr(file_part(full), '.');
	if (def_ext && *def_ext && (!dot || strcasecmp(dot + 1, def_ext)))
		ofn->Flags |= OFN_EXTENSIONDIFFERENT;
	free(s);
	return TRUE;
}

static BOOL file_dialog(OPENFILENAMEW *ofn, int wide, int save)
{
	ext_error = 0;
	if (!ofn) {
		ext_error = CDERR_INITIALIZATION;
		return FALSE;
	}
	if (ofn->lStructSize != sizeof(OPENFILENAMEW) &&
	    ofn->lStructSize != VFW_OPENFILENAME_SIZE_VERSION_400) {
		ext_error = CDERR_STRUCTSIZE;
		return FALSE;
	}
	if (!ofn->lpstrFile) {
		ext_error = CDERR_INITIALIZATION;
		return FALSE;
	}

	char *title = arg_utf8(ofn->lpstrTitle, wide);
	char *dir = arg_utf8(ofn->lpstrInitialDir, wide);
	char *def_ext = arg_utf8(ofn->lpstrDefExt, wide);
	char *full = answer_file(save, title, ofn->Flags, dir, def_ext);
	BOOL ok = full && fill_ofn(ofn, wide, full, def_ext);

	free(full);
	free(def_ext);
	free(dir);
	free(title);
	return ok;
}

WINFN(BOOL) dlg_GetOpenFileNameA(LPOPENFILENAMEA ofn)
{
	return file_dialog((OPENFILENAMEW *)ofn, 0, 0);
}

WINFN(BOOL) dlg_GetOpenFileNameW(LPOPENFILENAMEW ofn)
{
	return file_dialog(ofn, 1, 0);
}

WINFN(BOOL) dlg_GetSaveFileNameA(LPOPENFILENAMEA ofn)
{
	return file_dialog((OPENFILENAMEW *)ofn, 0, 1);
}

WINFN(BOOL) dlg_GetSaveFileNameW(LPOPENFILENAMEW ofn)
{
	return file_dialog(ofn, 1, 1);
}

/*
 * GetFileTitle: the name after the last separator. 0 on success, the size
 * needed (with the NUL) if buf is too small, -1 for a name that isn't a
 * file's.
 */
static short file_title(const void *file, int wide, void *buf, WORD size)
{
	size_t unit = wide ? sizeof(WCHAR) : 1, len = 0, start = 0;

	if (!file)
		return -1;
	for (;; len++) {
		unsigned c = wide ? ((const WCHAR *)file)[len] :
			     ((const BYTE *)file)[len];
		if (!c)
			break;
		if (c == '*' || c == '?' || c == '[' || c == ']' || c == '<' ||
		    c == '>' || c == '|' || c == '"')
			return -1;
		if (c == '\\' || c == '/' || c == ':')
			start = len + 1;
	}
	if (!len || start == len)
		return -1;
	size_t need = len - start + 1;
	if (!buf || size < need)
		return (short)need;
	memcpy(buf, (const BYTE *)file + start * unit, need * unit);
	return 0;
}

WINFN(short) dlg_GetFileTitleA(LPCSTR file, LPSTR buf, WORD size)
{
	return file_title(file, 0, buf, size);
}

WINFN(short) dlg_GetFileTitleW(LPCWSTR file, LPWSTR buf, WORD size)
{
	return file_title(file, 1, buf, size);
}

/* Colours and fonts: answered as if OK were pressed. */

static BOOL choose_color(CHOOSECOLORW *cc, int wide)
{
	ext_error = 0;
	if (!cc) {
		ext_error = CDERR_INITIALIZATION;
		return FALSE;
	}
	if (cc->lStructSize != sizeof(*cc)) {
		ext_error = CDERR_STRUCTSIZE;
		return FALSE;
	}
	if (!(cc->Flags & CC_RGBINIT))
		cc->rgbResult = RGB(0, 0, 0);
	fprintf(stderr, "ChooseColor: (no GUI: answering #%02x%02x%02x)\n",
		GetRValue(cc->rgbResult), GetGValue(cc->rgbResult),
		GetBValue(cc->rgbResult));
	return TRUE;
}

WINFN(BOOL) dlg_ChooseColorA(LPCHOOSECOLORA cc)
{
	return choose_color((CHOOSECOLORW *)cc, 0);
}

WINFN(BOOL) dlg_ChooseColorW(LPCHOOSECOLORW cc)
{
	return choose_color(cc, 1);
}

/* The screen is 96 dpi. */
#define LOGPIXELS 96

static BOOL choose_font(CHOOSEFONTW *cf, int wide)
{
	ext_error = 0;
	if (!cf || !cf->lpLogFont) {
		ext_error = CDERR_INITIALIZATION;
		return FALSE;
	}
	if (cf->lStructSize != sizeof(*cf)) {
		ext_error = CDERR_STRUCTSIZE;
		return FALSE;
	}
	if ((cf->Flags & CF_LIMITSIZE) && cf->nSizeMax < cf->nSizeMin) {
		ext_error = CFERR_MAXLESSTHANMIN;
		return FALSE;
	}

	/* LOGFONTA and LOGFONTW differ only in lfFaceName, which ends them. */
	LOGFONTW *lf = cf->lpLogFont;
	if (!(cf->Flags & CF_INITTOLOGFONTSTRUCT)) {
		static const char face[] = "MS Sans Serif";
		memset(lf, 0, wide ? sizeof(LOGFONTW) : sizeof(LOGFONTA));
		lf->lfHeight = -(8 * LOGPIXELS + 36) / 72;	/* 8 points */
		lf->lfWeight = FW_NORMAL;
		lf->lfCharSet = ANSI_CHARSET;
		lf->lfOutPrecision = OUT_DEFAULT_PRECIS;
		lf->lfClipPrecision = CLIP_DEFAULT_PRECIS;
		lf->lfQuality = DEFAULT_QUALITY;
		lf->lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
		for (size_t i = 0; i < sizeof(face); i++) {
			if (wide)
				lf->lfFaceName[i] = (BYTE)face[i];
			else
				((LOGFONTA *)lf)->lfFaceName[i] = face[i];
		}
	}
	/* The size list's entry: whole points. */
	LONG h = lf->lfHeight < 0 ? -lf->lfHeight : lf->lfHeight;
	cf->iPointSize = (INT)((h * 72 + LOGPIXELS / 2) / LOGPIXELS) * 10;
	cf->nFontType = SCREEN_FONTTYPE |
			(lf->lfWeight >= FW_BOLD ? BOLD_FONTTYPE :
			 REGULAR_FONTTYPE) |
			(lf->lfItalic ? ITALIC_FONTTYPE : 0);

	char *face = wide ? utf16_to_utf8_dup(lf->lfFaceName) :
		     acp_to_utf8(((LOGFONTA *)lf)->lfFaceName);
	fprintf(stderr, "ChooseFont: (no GUI: answering %s, %d points)\n",
		or_empty(face), cf->iPointSize / 10);
	free(face);
	return TRUE;
}

WINFN(BOOL) dlg_ChooseFontA(LPCHOOSEFONTA cf)
{
	return choose_font((CHOOSEFONTW *)cf, 0);
}

WINFN(BOOL) dlg_ChooseFontW(LPCHOOSEFONTW cf)
{
	return choose_font(cf, 1);
}

/* Printing: there are no printers. */

static BOOL no_printer(const char *fn, const void *p, DWORD size, DWORD want)
{
	ext_error = 0;
	if (!p) {
		ext_error = CDERR_INITIALIZATION;
		return FALSE;
	}
	if (size != want) {
		ext_error = CDERR_STRUCTSIZE;
		return FALSE;
	}
	fprintf(stderr, "%s: (no GUI and no printers: failing)\n", fn);
	ext_error = PDERR_NODEFAULTPRN;
	return FALSE;
}

WINFN(BOOL) dlg_PrintDlgA(LPPRINTDLGA pd)
{
	return no_printer("PrintDlg", pd, pd ? pd->lStructSize : 0,
			  sizeof(*pd));
}

WINFN(BOOL) dlg_PrintDlgW(LPPRINTDLGW pd)
{
	return no_printer("PrintDlg", pd, pd ? pd->lStructSize : 0,
			  sizeof(*pd));
}

WINFN(BOOL) dlg_PageSetupDlgA(LPPAGESETUPDLGA psd)
{
	return no_printer("PageSetupDlg", psd, psd ? psd->lStructSize : 0,
			  sizeof(*psd));
}

WINFN(BOOL) dlg_PageSetupDlgW(LPPAGESETUPDLGW psd)
{
	return no_printer("PageSetupDlg", psd, psd ? psd->lStructSize : 0,
			  sizeof(*psd));
}

static HRESULT print_ex(PRINTDLGEXW *pd)
{
	if (!pd || pd->lStructSize != sizeof(*pd)) {
		ext_error = pd ? CDERR_STRUCTSIZE : CDERR_INITIALIZATION;
		return E_INVALIDARG;
	}
	no_printer("PrintDlgEx", pd, pd->lStructSize, sizeof(*pd));
	pd->dwResultAction = PD_RESULT_CANCEL;
	return E_FAIL;
}

WINFN(HRESULT) dlg_PrintDlgExA(LPPRINTDLGEXA pd)
{
	return print_ex((PRINTDLGEXW *)pd);
}

WINFN(HRESULT) dlg_PrintDlgExW(LPPRINTDLGEXW pd)
{
	return print_ex(pd);
}

/* Find and replace: modeless dialogs, which can't be shown. */

static HWND find_replace(const char *fn, FINDREPLACEW *fr, int replace)
{
	ext_error = 0;
	if (!fr) {
		ext_error = CDERR_INITIALIZATION;
		return NULL;
	}
	if (fr->lStructSize != sizeof(*fr)) {
		ext_error = CDERR_STRUCTSIZE;
		return NULL;
	}
	if (!fr->lpstrFindWhat || !fr->wFindWhatLen ||
	    (replace && (!fr->lpstrReplaceWith || !fr->wReplaceWithLen))) {
		ext_error = FRERR_BUFFERLENGTHZERO;
		return NULL;
	}
	vfw_warn("%s: a DLL tried to open a dialog box, but there is no GUI; "
		 "it fails", fn);
	ext_error = CDERR_DIALOGFAILURE;
	return NULL;
}

WINFN(HWND) dlg_FindTextA(LPFINDREPLACEA fr)
{
	return find_replace("FindTextA", (FINDREPLACEW *)fr, 0);
}

WINFN(HWND) dlg_FindTextW(LPFINDREPLACEW fr)
{
	return find_replace("FindTextW", fr, 0);
}

WINFN(HWND) dlg_ReplaceTextA(LPFINDREPLACEA fr)
{
	return find_replace("ReplaceTextA", (FINDREPLACEW *)fr, 1);
}

WINFN(HWND) dlg_ReplaceTextW(LPFINDREPLACEW fr)
{
	return find_replace("ReplaceTextW", fr, 1);
}

/* COM registration: comdlg32 has no classes a codec could want. */

WINFN(HRESULT) dlg_DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	if (out)
		*out = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

WINFN(HRESULT) dlg_DllRegisterServer(void) { return S_OK; }
WINFN(HRESULT) dlg_DllUnregisterServer(void) { return S_OK; }

#define E(name) { #name, 0, (void *)dlg_##name }

static const struct builtin_export exports[] = {
	E(ChooseColorA),
	E(ChooseColorW),
	E(ChooseFontA),
	E(ChooseFontW),
	E(CommDlgExtendedError),
	E(DllGetClassObject),
	E(DllRegisterServer),
	E(DllUnregisterServer),
	E(FindTextA),
	E(FindTextW),
	E(GetFileTitleA),
	E(GetFileTitleW),
	E(GetOpenFileNameA),
	E(GetOpenFileNameW),
	E(GetSaveFileNameA),
	E(GetSaveFileNameW),
	E(PageSetupDlgA),
	E(PageSetupDlgW),
	E(PrintDlgA),
	E(PrintDlgExA),
	E(PrintDlgExW),
	E(PrintDlgW),
	E(ReplaceTextA),
	E(ReplaceTextW),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_comdlg32 = { "comdlg32.dll", NULL, tables, 0 };
