/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Common dialog definitions (commdlg.h, cderr.h). On 32-bit Windows the
 * structures in commdlg.h are byte-packed (pshpack1.h), which moves
 * PRINTDLG's fields after its WORDs off their natural alignment; the ABI
 * check holds these to the same.
 */
#ifndef VFW_WIN32_COMMDLG_H
#define VFW_WIN32_COMMDLG_H

#include "types.h"
#include "wingdi.h"
#include "commctrl.h"

typedef UINT_PTR (CALLBACK *LPOFNHOOKPROC)(HWND, UINT, WPARAM, LPARAM);
typedef UINT_PTR (CALLBACK *LPCCHOOKPROC)(HWND, UINT, WPARAM, LPARAM);
typedef UINT_PTR (CALLBACK *LPFRHOOKPROC)(HWND, UINT, WPARAM, LPARAM);
typedef UINT_PTR (CALLBACK *LPCFHOOKPROC)(HWND, UINT, WPARAM, LPARAM);
typedef UINT_PTR (CALLBACK *LPPRINTHOOKPROC)(HWND, UINT, WPARAM, LPARAM);
typedef UINT_PTR (CALLBACK *LPSETUPHOOKPROC)(HWND, UINT, WPARAM, LPARAM);
typedef UINT_PTR (CALLBACK *LPPAGEPAINTHOOK)(HWND, UINT, WPARAM, LPARAM);
typedef UINT_PTR (CALLBACK *LPPAGESETUPHOOK)(HWND, UINT, WPARAM, LPARAM);

#pragma pack(push, 1)

typedef struct tagOFNA {
	DWORD lStructSize;
	HWND hwndOwner;
	HINSTANCE hInstance;
	LPCSTR lpstrFilter;
	LPSTR lpstrCustomFilter;
	DWORD nMaxCustFilter;
	DWORD nFilterIndex;
	LPSTR lpstrFile;
	DWORD nMaxFile;
	LPSTR lpstrFileTitle;
	DWORD nMaxFileTitle;
	LPCSTR lpstrInitialDir;
	LPCSTR lpstrTitle;
	DWORD Flags;
	WORD nFileOffset;
	WORD nFileExtension;
	LPCSTR lpstrDefExt;
	LPARAM lCustData;
	LPOFNHOOKPROC lpfnHook;
	LPCSTR lpTemplateName;
	void *pvReserved;
	DWORD dwReserved;
	DWORD FlagsEx;
} OPENFILENAMEA, *LPOPENFILENAMEA;

typedef struct tagOFNW {
	DWORD lStructSize;
	HWND hwndOwner;
	HINSTANCE hInstance;
	LPCWSTR lpstrFilter;
	LPWSTR lpstrCustomFilter;
	DWORD nMaxCustFilter;
	DWORD nFilterIndex;
	LPWSTR lpstrFile;
	DWORD nMaxFile;
	LPWSTR lpstrFileTitle;
	DWORD nMaxFileTitle;
	LPCWSTR lpstrInitialDir;
	LPCWSTR lpstrTitle;
	DWORD Flags;
	WORD nFileOffset;
	WORD nFileExtension;
	LPCWSTR lpstrDefExt;
	LPARAM lCustData;
	LPOFNHOOKPROC lpfnHook;
	LPCWSTR lpTemplateName;
	void *pvReserved;
	DWORD dwReserved;
	DWORD FlagsEx;
} OPENFILENAMEW, *LPOPENFILENAMEW;

/* The size of the structure before Windows 2000 (up to lpTemplateName). */
#define VFW_OPENFILENAME_SIZE_VERSION_400 76

typedef struct tagCHOOSECOLORA {
	DWORD lStructSize;
	HWND hwndOwner;
	HWND hInstance;
	COLORREF rgbResult;
	COLORREF *lpCustColors;
	DWORD Flags;
	LPARAM lCustData;
	LPCCHOOKPROC lpfnHook;
	LPCSTR lpTemplateName;
} CHOOSECOLORA, *LPCHOOSECOLORA;

typedef struct tagCHOOSECOLORW {
	DWORD lStructSize;
	HWND hwndOwner;
	HWND hInstance;
	COLORREF rgbResult;
	COLORREF *lpCustColors;
	DWORD Flags;
	LPARAM lCustData;
	LPCCHOOKPROC lpfnHook;
	LPCWSTR lpTemplateName;
} CHOOSECOLORW, *LPCHOOSECOLORW;

typedef struct tagFINDREPLACEA {
	DWORD lStructSize;
	HWND hwndOwner;
	HINSTANCE hInstance;
	DWORD Flags;
	LPSTR lpstrFindWhat;
	LPSTR lpstrReplaceWith;
	WORD wFindWhatLen;
	WORD wReplaceWithLen;
	LPARAM lCustData;
	LPFRHOOKPROC lpfnHook;
	LPCSTR lpTemplateName;
} FINDREPLACEA, *LPFINDREPLACEA;

typedef struct tagFINDREPLACEW {
	DWORD lStructSize;
	HWND hwndOwner;
	HINSTANCE hInstance;
	DWORD Flags;
	LPWSTR lpstrFindWhat;
	LPWSTR lpstrReplaceWith;
	WORD wFindWhatLen;
	WORD wReplaceWithLen;
	LPARAM lCustData;
	LPFRHOOKPROC lpfnHook;
	LPCWSTR lpTemplateName;
} FINDREPLACEW, *LPFINDREPLACEW;

typedef struct tagCHOOSEFONTA {
	DWORD lStructSize;
	HWND hwndOwner;
	HDC hDC;
	LPLOGFONTA lpLogFont;
	INT iPointSize;
	DWORD Flags;
	COLORREF rgbColors;
	LPARAM lCustData;
	LPCFHOOKPROC lpfnHook;
	LPCSTR lpTemplateName;
	HINSTANCE hInstance;
	LPSTR lpszStyle;
	WORD nFontType;
	WORD ___MISSING_ALIGNMENT__;
	INT nSizeMin;
	INT nSizeMax;
} CHOOSEFONTA, *LPCHOOSEFONTA;

typedef struct tagCHOOSEFONTW {
	DWORD lStructSize;
	HWND hwndOwner;
	HDC hDC;
	LPLOGFONTW lpLogFont;
	INT iPointSize;
	DWORD Flags;
	COLORREF rgbColors;
	LPARAM lCustData;
	LPCFHOOKPROC lpfnHook;
	LPCWSTR lpTemplateName;
	HINSTANCE hInstance;
	LPWSTR lpszStyle;
	WORD nFontType;
	WORD ___MISSING_ALIGNMENT__;
	INT nSizeMin;
	INT nSizeMax;
} CHOOSEFONTW, *LPCHOOSEFONTW;

typedef struct tagPDA {
	DWORD lStructSize;
	HWND hwndOwner;
	HGLOBAL hDevMode;
	HGLOBAL hDevNames;
	HDC hDC;
	DWORD Flags;
	WORD nFromPage;
	WORD nToPage;
	WORD nMinPage;
	WORD nMaxPage;
	WORD nCopies;
	HINSTANCE hInstance;
	LPARAM lCustData;
	LPPRINTHOOKPROC lpfnPrintHook;
	LPSETUPHOOKPROC lpfnSetupHook;
	LPCSTR lpPrintTemplateName;
	LPCSTR lpSetupTemplateName;
	HGLOBAL hPrintTemplate;
	HGLOBAL hSetupTemplate;
} PRINTDLGA, *LPPRINTDLGA;

typedef struct tagPDW {
	DWORD lStructSize;
	HWND hwndOwner;
	HGLOBAL hDevMode;
	HGLOBAL hDevNames;
	HDC hDC;
	DWORD Flags;
	WORD nFromPage;
	WORD nToPage;
	WORD nMinPage;
	WORD nMaxPage;
	WORD nCopies;
	HINSTANCE hInstance;
	LPARAM lCustData;
	LPPRINTHOOKPROC lpfnPrintHook;
	LPSETUPHOOKPROC lpfnSetupHook;
	LPCWSTR lpPrintTemplateName;
	LPCWSTR lpSetupTemplateName;
	HGLOBAL hPrintTemplate;
	HGLOBAL hSetupTemplate;
} PRINTDLGW, *LPPRINTDLGW;

typedef struct tagPRINTPAGERANGE {
	DWORD nFromPage;
	DWORD nToPage;
} PRINTPAGERANGE, *LPPRINTPAGERANGE;

typedef struct tagPDEXA {
	DWORD lStructSize;
	HWND hwndOwner;
	HGLOBAL hDevMode;
	HGLOBAL hDevNames;
	HDC hDC;
	DWORD Flags;
	DWORD Flags2;
	DWORD ExclusionFlags;
	DWORD nPageRanges;
	DWORD nMaxPageRanges;
	LPPRINTPAGERANGE lpPageRanges;
	DWORD nMinPage;
	DWORD nMaxPage;
	DWORD nCopies;
	HINSTANCE hInstance;
	LPCSTR lpPrintTemplateName;
	void *lpCallback;
	DWORD nPropertyPages;
	HPROPSHEETPAGE *lphPropertyPages;
	DWORD nStartPage;
	DWORD dwResultAction;
} PRINTDLGEXA, *LPPRINTDLGEXA;

typedef struct tagPDEXW {
	DWORD lStructSize;
	HWND hwndOwner;
	HGLOBAL hDevMode;
	HGLOBAL hDevNames;
	HDC hDC;
	DWORD Flags;
	DWORD Flags2;
	DWORD ExclusionFlags;
	DWORD nPageRanges;
	DWORD nMaxPageRanges;
	LPPRINTPAGERANGE lpPageRanges;
	DWORD nMinPage;
	DWORD nMaxPage;
	DWORD nCopies;
	HINSTANCE hInstance;
	LPCWSTR lpPrintTemplateName;
	void *lpCallback;
	DWORD nPropertyPages;
	HPROPSHEETPAGE *lphPropertyPages;
	DWORD nStartPage;
	DWORD dwResultAction;
} PRINTDLGEXW, *LPPRINTDLGEXW;

typedef struct tagPSDA {
	DWORD lStructSize;
	HWND hwndOwner;
	HGLOBAL hDevMode;
	HGLOBAL hDevNames;
	DWORD Flags;
	POINT ptPaperSize;
	RECT rtMinMargin;
	RECT rtMargin;
	HINSTANCE hInstance;
	LPARAM lCustData;
	LPPAGESETUPHOOK lpfnPageSetupHook;
	LPPAGEPAINTHOOK lpfnPagePaintHook;
	LPCSTR lpPageSetupTemplateName;
	HGLOBAL hPageSetupTemplate;
} PAGESETUPDLGA, *LPPAGESETUPDLGA;

typedef struct tagPSDW {
	DWORD lStructSize;
	HWND hwndOwner;
	HGLOBAL hDevMode;
	HGLOBAL hDevNames;
	DWORD Flags;
	POINT ptPaperSize;
	RECT rtMinMargin;
	RECT rtMargin;
	HINSTANCE hInstance;
	LPARAM lCustData;
	LPPAGESETUPHOOK lpfnPageSetupHook;
	LPPAGEPAINTHOOK lpfnPagePaintHook;
	LPCWSTR lpPageSetupTemplateName;
	HGLOBAL hPageSetupTemplate;
} PAGESETUPDLGW, *LPPAGESETUPDLGW;

#pragma pack(pop)

/* OPENFILENAME flags. */
#define OFN_READONLY             0x1
#define OFN_OVERWRITEPROMPT      0x2
#define OFN_HIDEREADONLY         0x4
#define OFN_NOCHANGEDIR          0x8
#define OFN_SHOWHELP             0x10
#define OFN_ENABLEHOOK           0x20
#define OFN_ENABLETEMPLATE       0x40
#define OFN_ENABLETEMPLATEHANDLE 0x80
#define OFN_NOVALIDATE           0x100
#define OFN_ALLOWMULTISELECT     0x200
#define OFN_EXTENSIONDIFFERENT   0x400
#define OFN_PATHMUSTEXIST        0x800
#define OFN_FILEMUSTEXIST        0x1000
#define OFN_CREATEPROMPT         0x2000
#define OFN_SHAREAWARE           0x4000
#define OFN_NOREADONLYRETURN     0x8000
#define OFN_NOTESTFILECREATE     0x10000
#define OFN_NONETWORKBUTTON      0x20000
#define OFN_NOLONGNAMES          0x40000
#define OFN_EXPLORER             0x80000
#define OFN_NODEREFERENCELINKS   0x100000
#define OFN_LONGNAMES            0x200000
#define OFN_ENABLEINCLUDENOTIFY  0x400000
#define OFN_ENABLESIZING         0x800000
#define OFN_DONTADDTORECENT      0x2000000
#define OFN_FORCESHOWHIDDEN      0x10000000

/* CHOOSECOLOR flags. */
#define CC_RGBINIT               0x1
#define CC_FULLOPEN              0x2
#define CC_PREVENTFULLOPEN       0x4
#define CC_SHOWHELP              0x8
#define CC_ENABLEHOOK            0x10
#define CC_ENABLETEMPLATE        0x20
#define CC_ENABLETEMPLATEHANDLE  0x40
#define CC_SOLIDCOLOR            0x80
#define CC_ANYCOLOR              0x100

/* FINDREPLACE flags. */
#define FR_DOWN                  0x1
#define FR_WHOLEWORD             0x2
#define FR_MATCHCASE             0x4
#define FR_FINDNEXT              0x8
#define FR_REPLACE               0x10
#define FR_REPLACEALL            0x20
#define FR_DIALOGTERM            0x40
#define FR_SHOWHELP              0x80
#define FR_ENABLEHOOK            0x100
#define FR_ENABLETEMPLATE        0x200
#define FR_NOUPDOWN              0x400
#define FR_NOMATCHCASE           0x800
#define FR_NOWHOLEWORD           0x1000
#define FR_ENABLETEMPLATEHANDLE  0x2000
#define FR_HIDEUPDOWN            0x4000
#define FR_HIDEMATCHCASE         0x8000
#define FR_HIDEWHOLEWORD         0x10000

/* CHOOSEFONT flags and font types. */
#define CF_SCREENFONTS           0x1
#define CF_PRINTERFONTS          0x2
#define CF_BOTH                  0x3
#define CF_SHOWHELP              0x4
#define CF_ENABLEHOOK            0x8
#define CF_ENABLETEMPLATE        0x10
#define CF_ENABLETEMPLATEHANDLE  0x20
#define CF_INITTOLOGFONTSTRUCT   0x40
#define CF_USESTYLE              0x80
#define CF_EFFECTS               0x100
#define CF_APPLY                 0x200
#define CF_ANSIONLY              0x400
#define CF_NOVECTORFONTS         0x800
#define CF_NOSIMULATIONS         0x1000
#define CF_LIMITSIZE             0x2000
#define CF_FIXEDPITCHONLY        0x4000
#define CF_WYSIWYG               0x8000
#define CF_FORCEFONTEXIST        0x10000
#define CF_SCALABLEONLY          0x20000
#define CF_TTONLY                0x40000
#define CF_NOFACESEL             0x80000
#define CF_NOSTYLESEL            0x100000
#define CF_NOSIZESEL             0x200000
#define SIMULATED_FONTTYPE       0x8000
#define PRINTER_FONTTYPE         0x4000
#define SCREEN_FONTTYPE          0x2000
#define BOLD_FONTTYPE            0x100
#define ITALIC_FONTTYPE          0x200
#define REGULAR_FONTTYPE         0x400

/* PRINTDLG(EX) flags and results. */
#define PD_ALLPAGES              0x0
#define PD_SELECTION             0x1
#define PD_PAGENUMS              0x2
#define PD_NOSELECTION           0x4
#define PD_NOPAGENUMS            0x8
#define PD_COLLATE               0x10
#define PD_PRINTTOFILE           0x20
#define PD_PRINTSETUP            0x40
#define PD_NOWARNING             0x80
#define PD_RETURNDC              0x100
#define PD_RETURNIC              0x200
#define PD_RETURNDEFAULT         0x400
#define PD_SHOWHELP              0x800
#define PD_ENABLEPRINTHOOK       0x1000
#define PD_ENABLESETUPHOOK       0x2000
#define PD_RESULT_CANCEL         0
#define PD_RESULT_PRINT          1
#define PD_RESULT_APPLY          2

/* PAGESETUPDLG flags. */
#define PSD_MARGINS              0x2
#define PSD_NOWARNING            0x80
#define PSD_RETURNDEFAULT        0x400

/* CommDlgExtendedError codes (cderr.h). */
#define CDERR_DIALOGFAILURE      0xFFFF
#define CDERR_GENERALCODES       0x0000
#define CDERR_STRUCTSIZE         0x0001
#define CDERR_INITIALIZATION     0x0002
#define CDERR_NOTEMPLATE         0x0003
#define CDERR_NOHINSTANCE        0x0004
#define CDERR_LOADSTRFAILURE     0x0005
#define CDERR_FINDRESFAILURE     0x0006
#define CDERR_LOADRESFAILURE     0x0007
#define CDERR_LOCKRESFAILURE     0x0008
#define CDERR_MEMALLOCFAILURE    0x0009
#define CDERR_MEMLOCKFAILURE     0x000A
#define CDERR_NOHOOK             0x000B
#define CDERR_REGISTERMSGFAIL    0x000C
#define PDERR_PRINTERCODES       0x1000
#define PDERR_SETUPFAILURE       0x1001
#define PDERR_PARSEFAILURE       0x1002
#define PDERR_RETDEFFAILURE      0x1003
#define PDERR_LOADDRVFAILURE     0x1004
#define PDERR_GETDEVMODEFAIL     0x1005
#define PDERR_INITFAILURE        0x1006
#define PDERR_NODEVICES          0x1007
#define PDERR_NODEFAULTPRN       0x1008
#define PDERR_DNDMMISMATCH       0x1009
#define PDERR_CREATEICFAILURE    0x100A
#define PDERR_PRINTERNOTFOUND    0x100B
#define PDERR_DEFAULTDIFFERENT   0x100C
#define CFERR_CHOOSEFONTCODES    0x2000
#define CFERR_NOFONTS            0x2001
#define CFERR_MAXLESSTHANMIN     0x2002
#define FNERR_FILENAMECODES      0x3000
#define FNERR_SUBCLASSFAILURE    0x3001
#define FNERR_INVALIDFILENAME    0x3002
#define FNERR_BUFFERTOOSMALL     0x3003
#define FRERR_FINDREPLACECODES   0x4000
#define FRERR_BUFFERLENGTHZERO   0x4001
#define CCERR_CHOOSECOLORCODES   0x5000

#endif /* VFW_WIN32_COMMDLG_H */
