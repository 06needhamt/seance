/*
 * Common controls definitions (commctrl.h, prsht.h, and DLLVERSIONINFO
 * from shlwapi.h).
 */
#ifndef VFW_WIN32_COMMCTRL_H
#define VFW_WIN32_COMMCTRL_H

#include "types.h"
#include "wingdi.h"
#include "winuser.h"

/* InitCommonControlsEx. */
typedef struct tagINITCOMMONCONTROLSEX {
	DWORD dwSize;
	DWORD dwICC;
} INITCOMMONCONTROLSEX, *LPINITCOMMONCONTROLSEX;

#define ICC_LISTVIEW_CLASSES   0x1
#define ICC_TREEVIEW_CLASSES   0x2
#define ICC_BAR_CLASSES        0x4
#define ICC_TAB_CLASSES        0x8
#define ICC_UPDOWN_CLASS       0x10
#define ICC_PROGRESS_CLASS     0x20
#define ICC_HOTKEY_CLASS       0x40
#define ICC_ANIMATE_CLASS      0x80
#define ICC_WIN95_CLASSES      0xff
#define ICC_DATE_CLASSES       0x100
#define ICC_USEREX_CLASSES     0x200
#define ICC_COOL_CLASSES       0x400
#define ICC_INTERNET_CLASSES   0x800
#define ICC_PAGESCROLLER_CLASS 0x1000
#define ICC_NATIVEFNTCTL_CLASS 0x2000
#define ICC_STANDARD_CLASSES   0x4000
#define ICC_LINK_CLASS         0x8000

/* Messages and styles common to the controls. */
#define CCM_FIRST            0x2000
#define CCM_SETBKCOLOR       (CCM_FIRST + 1)
#define CCM_SETUNICODEFORMAT (CCM_FIRST + 5)
#define CCM_GETUNICODEFORMAT (CCM_FIRST + 6)
#define CCS_TOP              0x1
#define CCS_NOMOVEY          0x2
#define CCS_BOTTOM           0x3
#define CCS_NORESIZE         0x4
#define CCS_NOPARENTALIGN    0x8
#define CCS_ADJUSTABLE       0x20
#define CCS_NODIVIDER        0x40
#define CCS_VERT             0x80
#define CLR_NONE             0xFFFFFFFF
#define CLR_DEFAULT          0xFF000000
#define HINST_COMMCTRL       ((HINSTANCE)-1)

/* Trackbars. */
#define TBS_AUTOTICKS        0x1
#define TBS_VERT             0x2
#define TBS_HORZ             0x0
#define TBS_TOP              0x4
#define TBS_BOTTOM           0x0
#define TBS_LEFT             0x4
#define TBS_RIGHT            0x0
#define TBS_BOTH             0x8
#define TBS_NOTICKS          0x10
#define TBS_ENABLESELRANGE   0x20
#define TBS_FIXEDLENGTH      0x40
#define TBS_NOTHUMB          0x80
#define TBS_TOOLTIPS         0x100
#define TBS_REVERSED         0x200
#define TBM_GETPOS           (WM_USER)
#define TBM_GETRANGEMIN      (WM_USER + 1)
#define TBM_GETRANGEMAX      (WM_USER + 2)
#define TBM_GETTIC           (WM_USER + 3)
#define TBM_SETTIC           (WM_USER + 4)
#define TBM_SETPOS           (WM_USER + 5)
#define TBM_SETRANGE         (WM_USER + 6)
#define TBM_SETRANGEMIN      (WM_USER + 7)
#define TBM_SETRANGEMAX      (WM_USER + 8)
#define TBM_CLEARTICS        (WM_USER + 9)
#define TBM_SETSEL           (WM_USER + 10)
#define TBM_SETSELSTART      (WM_USER + 11)
#define TBM_SETSELEND        (WM_USER + 12)
#define TBM_GETPTICS         (WM_USER + 14)
#define TBM_GETTICPOS        (WM_USER + 15)
#define TBM_GETNUMTICS       (WM_USER + 16)
#define TBM_GETSELSTART      (WM_USER + 17)
#define TBM_GETSELEND        (WM_USER + 18)
#define TBM_CLEARSEL         (WM_USER + 19)
#define TBM_SETTICFREQ       (WM_USER + 20)
#define TBM_SETPAGESIZE      (WM_USER + 21)
#define TBM_GETPAGESIZE      (WM_USER + 22)
#define TBM_SETLINESIZE      (WM_USER + 23)
#define TBM_GETLINESIZE      (WM_USER + 24)
#define TBM_GETTHUMBRECT     (WM_USER + 25)
#define TBM_GETCHANNELRECT   (WM_USER + 26)
#define TBM_SETTHUMBLENGTH   (WM_USER + 27)
#define TBM_GETTHUMBLENGTH   (WM_USER + 28)
#define TBM_SETTOOLTIPS      (WM_USER + 29)
#define TBM_GETTOOLTIPS      (WM_USER + 30)
#define TBM_SETTIPSIDE       (WM_USER + 31)
#define TBM_SETBUDDY         (WM_USER + 32)
#define TBM_GETBUDDY         (WM_USER + 33)
#define TB_LINEUP            0
#define TB_LINEDOWN          1
#define TB_PAGEUP            2
#define TB_PAGEDOWN          3
#define TB_THUMBPOSITION     4
#define TB_THUMBTRACK        5
#define TB_TOP               6
#define TB_BOTTOM            7
#define TB_ENDTRACK          8

/* Progress bars. */
typedef struct {
	int iLow;
	int iHigh;
} PBRANGE, *PPBRANGE;

#define PBS_SMOOTH           0x1
#define PBS_VERTICAL         0x4
#define PBS_MARQUEE          0x8
#define PBM_SETRANGE         (WM_USER + 1)
#define PBM_SETPOS           (WM_USER + 2)
#define PBM_DELTAPOS         (WM_USER + 3)
#define PBM_SETSTEP          (WM_USER + 4)
#define PBM_STEPIT           (WM_USER + 5)
#define PBM_SETRANGE32       (WM_USER + 6)
#define PBM_GETRANGE         (WM_USER + 7)
#define PBM_GETPOS           (WM_USER + 8)
#define PBM_SETBARCOLOR      (WM_USER + 9)
#define PBM_SETBKCOLOR       CCM_SETBKCOLOR
#define PBM_SETMARQUEE       (WM_USER + 10)
#define PBM_GETSTEP          (WM_USER + 13)
#define PBM_GETBKCOLOR       (WM_USER + 14)
#define PBM_GETBARCOLOR      (WM_USER + 15)
#define PBM_SETSTATE         (WM_USER + 16)
#define PBM_GETSTATE         (WM_USER + 17)
#define PBST_NORMAL          1
#define PBST_ERROR           2
#define PBST_PAUSED          3

/* Up-down controls. */
typedef struct _UDACCEL {
	UINT nSec;
	UINT nInc;
} UDACCEL, *LPUDACCEL;

#define UD_MAXVAL            0x7fff
#define UD_MINVAL            (-UD_MAXVAL)
#define UDS_WRAP             0x1
#define UDS_SETBUDDYINT      0x2
#define UDS_ALIGNRIGHT       0x4
#define UDS_ALIGNLEFT        0x8
#define UDS_AUTOBUDDY        0x10
#define UDS_ARROWKEYS        0x20
#define UDS_HORZ             0x40
#define UDS_NOTHOUSANDS      0x80
#define UDS_HOTTRACK         0x100
#define UDM_SETRANGE         (WM_USER + 101)
#define UDM_GETRANGE         (WM_USER + 102)
#define UDM_SETPOS           (WM_USER + 103)
#define UDM_GETPOS           (WM_USER + 104)
#define UDM_SETBUDDY         (WM_USER + 105)
#define UDM_GETBUDDY         (WM_USER + 106)
#define UDM_SETACCEL         (WM_USER + 107)
#define UDM_GETACCEL         (WM_USER + 108)
#define UDM_SETBASE          (WM_USER + 109)
#define UDM_GETBASE          (WM_USER + 110)
#define UDM_SETRANGE32       (WM_USER + 111)
#define UDM_GETRANGE32       (WM_USER + 112)
#define UDM_SETPOS32         (WM_USER + 113)
#define UDM_GETPOS32         (WM_USER + 114)

/* Status bars. */
#define SBARS_SIZEGRIP       0x100
#define SBARS_TOOLTIPS       0x800
#define SBT_NOBORDERS        0x100
#define SBT_POPOUT           0x200
#define SBT_RTLREADING       0x400
#define SBT_NOTABPARSING     0x800
#define SBT_OWNERDRAW        0x1000
#define SB_SETTEXTA          (WM_USER + 1)
#define SB_GETTEXTA          (WM_USER + 2)
#define SB_GETTEXTLENGTHA    (WM_USER + 3)
#define SB_SETPARTS          (WM_USER + 4)
#define SB_GETPARTS          (WM_USER + 6)
#define SB_GETBORDERS        (WM_USER + 7)
#define SB_SETMINHEIGHT      (WM_USER + 8)
#define SB_SIMPLE            (WM_USER + 9)
#define SB_GETRECT           (WM_USER + 10)
#define SB_SETTEXTW          (WM_USER + 11)
#define SB_GETTEXTLENGTHW    (WM_USER + 12)
#define SB_GETTEXTW          (WM_USER + 13)
#define SB_ISSIMPLE          (WM_USER + 14)
#define SB_SETICON           (WM_USER + 15)
#define SB_GETICON           (WM_USER + 20)
#define SB_SIMPLEID          0xff

/* Toolbars (CreateToolbarEx) and CreateMappedBitmap. */
typedef struct _TBBUTTON {
	int iBitmap;
	int idCommand;
	BYTE fsState;
	BYTE fsStyle;
	BYTE bReserved[2];
	DWORD_PTR dwData;
	INT_PTR iString;
} TBBUTTON, *PTBBUTTON, *LPTBBUTTON;
typedef const TBBUTTON *LPCTBBUTTON;

typedef struct _COLORMAP {
	COLORREF from;
	COLORREF to;
} COLORMAP, *LPCOLORMAP;

#define CMB_MASKED           0x2
#define TB_BUTTONSTRUCTSIZE  (WM_USER + 30)
#define TB_ADDBITMAP         (WM_USER + 19)
#define TB_ADDBUTTONSA       (WM_USER + 20)
#define TB_SETBITMAPSIZE     (WM_USER + 32)
#define TB_SETBUTTONSIZE     (WM_USER + 31)

/* Image lists. */
struct _IMAGELIST;
typedef struct _IMAGELIST *HIMAGELIST;

typedef struct _IMAGEINFO {
	HBITMAP hbmImage;
	HBITMAP hbmMask;
	int Unused1;
	int Unused2;
	RECT rcImage;
} IMAGEINFO, *LPIMAGEINFO;

typedef struct _IMAGELISTDRAWPARAMS {
	DWORD cbSize;
	HIMAGELIST himl;
	int i;
	HDC hdcDst;
	int x;
	int y;
	int cx;
	int cy;
	int xBitmap;
	int yBitmap;
	COLORREF rgbBk;
	COLORREF rgbFg;
	UINT fStyle;
	DWORD dwRop;
	DWORD fState;
	DWORD Frame;
	COLORREF crEffect;
} IMAGELISTDRAWPARAMS, *LPIMAGELISTDRAWPARAMS;

/* The size before Windows XP (up to dwRop). */
#define VFW_IMAGELISTDRAWPARAMS_V3_SIZE 56

#define ILC_MASK             0x1
#define ILC_COLOR            0x0
#define ILC_COLORDDB         0xfe
#define ILC_COLOR4           0x4
#define ILC_COLOR8           0x8
#define ILC_COLOR16          0x10
#define ILC_COLOR24          0x18
#define ILC_COLOR32          0x20
#define ILC_PALETTE          0x800
#define ILC_MIRROR           0x2000
#define ILC_PERITEMMIRROR    0x8000
#define ILD_NORMAL           0x0
#define ILD_TRANSPARENT      0x1
#define ILD_BLEND25          0x2
#define ILD_BLEND50          0x4
#define ILD_MASK             0x10
#define ILD_IMAGE            0x20
#define ILD_ROP              0x40
#define ILD_OVERLAYMASK      0xf00
#define ILD_PRESERVEALPHA    0x1000
#define ILD_SCALE            0x2000
#define ILD_DPISCALE         0x4000
#define ILCF_MOVE            0x0
#define ILCF_SWAP            0x1

/* Dynamic arrays. */
struct _DSA;
struct _DPA;
typedef struct _DSA *HDSA;
typedef struct _DPA *HDPA;
typedef int (CALLBACK *PFNDAENUMCALLBACK)(void *p, void *pData);
typedef int (CALLBACK *PFNDACOMPARE)(void *p1, void *p2, LPARAM lParam);
typedef PFNDAENUMCALLBACK PFNDPAENUMCALLBACK;
typedef PFNDAENUMCALLBACK PFNDSAENUMCALLBACK;
typedef PFNDACOMPARE PFNDPACOMPARE;

#define DA_LAST              0x7fffffff
#define DA_ERR               (-1)
#define DPAS_SORTED          0x1
#define DPAS_INSERTBEFORE    0x2
#define DPAS_INSERTAFTER     0x4

/* Window subclassing. */
typedef LRESULT (CALLBACK *SUBCLASSPROC)(HWND hWnd, UINT uMsg, WPARAM wParam,
					 LPARAM lParam, UINT_PTR uIdSubclass,
					 DWORD_PTR dwRefData);

/* Notifications (WM_NOTIFY). */
#define NM_FIRST             (0U - 0U)
#define NM_OUTOFMEMORY       (NM_FIRST - 1)
#define NM_CLICK             (NM_FIRST - 2)

/* Property sheets (prsht.h). */
struct _PSP;
typedef struct _PSP *HPROPSHEETPAGE;
struct _PROPSHEETPAGEA;
struct _PROPSHEETPAGEW;
typedef UINT (CALLBACK *LPFNPSPCALLBACKA)(HWND hwnd, UINT uMsg,
					  struct _PROPSHEETPAGEA *ppsp);
typedef UINT (CALLBACK *LPFNPSPCALLBACKW)(HWND hwnd, UINT uMsg,
					  struct _PROPSHEETPAGEW *ppsp);
typedef int (CALLBACK *PFNPROPSHEETCALLBACK)(HWND, UINT, LPARAM);

typedef struct _PROPSHEETPAGEA {
	DWORD dwSize;
	DWORD dwFlags;
	HINSTANCE hInstance;
	union {
		LPCSTR pszTemplate;
		const void *pResource;
	};
	union {
		HICON hIcon;
		LPCSTR pszIcon;
	};
	LPCSTR pszTitle;
	DLGPROC pfnDlgProc;
	LPARAM lParam;
	LPFNPSPCALLBACKA pfnCallback;
	UINT *pcRefParent;
	LPCSTR pszHeaderTitle;
	LPCSTR pszHeaderSubTitle;
	HANDLE hActCtx;
} PROPSHEETPAGEA, *LPPROPSHEETPAGEA;
typedef const PROPSHEETPAGEA *LPCPROPSHEETPAGEA;

typedef struct _PROPSHEETPAGEW {
	DWORD dwSize;
	DWORD dwFlags;
	HINSTANCE hInstance;
	union {
		LPCWSTR pszTemplate;
		const void *pResource;
	};
	union {
		HICON hIcon;
		LPCWSTR pszIcon;
	};
	LPCWSTR pszTitle;
	DLGPROC pfnDlgProc;
	LPARAM lParam;
	LPFNPSPCALLBACKW pfnCallback;
	UINT *pcRefParent;
	LPCWSTR pszHeaderTitle;
	LPCWSTR pszHeaderSubTitle;
	HANDLE hActCtx;
} PROPSHEETPAGEW, *LPPROPSHEETPAGEW;
typedef const PROPSHEETPAGEW *LPCPROPSHEETPAGEW;

/* The size of the first version of PROPSHEETPAGE (up to pcRefParent). */
#define VFW_PROPSHEETPAGE_V1_SIZE 40

typedef struct _PROPSHEETHEADERA {
	DWORD dwSize;
	DWORD dwFlags;
	HWND hwndParent;
	HINSTANCE hInstance;
	union {
		HICON hIcon;
		LPCSTR pszIcon;
	};
	LPCSTR pszCaption;
	UINT nPages;
	union {
		UINT nStartPage;
		LPCSTR pStartPage;
	};
	union {
		LPCPROPSHEETPAGEA ppsp;
		HPROPSHEETPAGE *phpage;
	};
	PFNPROPSHEETCALLBACK pfnCallback;
	union {
		HBITMAP hbmWatermark;
		LPCSTR pszbmWatermark;
	};
	HPALETTE hplWatermark;
	union {
		HBITMAP hbmHeader;
		LPCSTR pszbmHeader;
	};
} PROPSHEETHEADERA, *LPPROPSHEETHEADERA;
typedef const PROPSHEETHEADERA *LPCPROPSHEETHEADERA;

typedef struct _PROPSHEETHEADERW {
	DWORD dwSize;
	DWORD dwFlags;
	HWND hwndParent;
	HINSTANCE hInstance;
	union {
		HICON hIcon;
		LPCWSTR pszIcon;
	};
	LPCWSTR pszCaption;
	UINT nPages;
	union {
		UINT nStartPage;
		LPCWSTR pStartPage;
	};
	union {
		LPCPROPSHEETPAGEW ppsp;
		HPROPSHEETPAGE *phpage;
	};
	PFNPROPSHEETCALLBACK pfnCallback;
	union {
		HBITMAP hbmWatermark;
		LPCWSTR pszbmWatermark;
	};
	HPALETTE hplWatermark;
	union {
		HBITMAP hbmHeader;
		LPCWSTR pszbmHeader;
	};
} PROPSHEETHEADERW, *LPPROPSHEETHEADERW;
typedef const PROPSHEETHEADERW *LPCPROPSHEETHEADERW;

/* The size of the first version of PROPSHEETHEADER (up to pfnCallback). */
#define VFW_PROPSHEETHEADER_V1_SIZE 40

#define PSP_DLGINDIRECT      0x1
#define PSP_USEHICON         0x2
#define PSP_USEICONID        0x4
#define PSP_USETITLE         0x8
#define PSP_RTLREADING       0x10
#define PSP_HASHELP          0x20
#define PSP_USEREFPARENT     0x40
#define PSP_USECALLBACK      0x80
#define PSP_PREMATURE        0x400
#define PSPCB_ADDREF         0
#define PSPCB_RELEASE        1
#define PSPCB_CREATE         2
#define PSH_DEFAULT          0x0
#define PSH_PROPTITLE        0x1
#define PSH_USEHICON         0x2
#define PSH_USEICONID        0x4
#define PSH_PROPSHEETPAGE    0x8
#define PSH_WIZARDHASFINISH  0x10
#define PSH_WIZARD           0x20
#define PSH_USEPSTARTPAGE    0x40
#define PSH_NOAPPLYNOW       0x80
#define PSH_USECALLBACK      0x100
#define PSH_HASHELP          0x200
#define PSH_MODELESS         0x400
#define PSH_RTLREADING       0x800
#define PSH_WIZARD97         0x01000000
#define PSCB_INITIALIZED     1
#define PSCB_PRECREATE       2
#define ID_PSRESTARTWINDOWS  0x2
#define ID_PSREBOOTSYSTEM    0x3

/* Task dialogs. */
typedef HRESULT (CALLBACK *PFTASKDIALOGCALLBACK)(HWND hwnd, UINT msg,
						 WPARAM wParam, LPARAM lParam,
						 LONG_PTR lpRefData);

#pragma pack(push, 1)

typedef struct _TASKDIALOG_BUTTON {
	int nButtonID;
	LPCWSTR pszButtonText;
} TASKDIALOG_BUTTON;

typedef struct _TASKDIALOGCONFIG {
	UINT cbSize;
	HWND hwndParent;
	HINSTANCE hInstance;
	int dwFlags;
	int dwCommonButtons;
	LPCWSTR pszWindowTitle;
	union {
		HICON hMainIcon;
		LPCWSTR pszMainIcon;
	};
	LPCWSTR pszMainInstruction;
	LPCWSTR pszContent;
	UINT cButtons;
	const TASKDIALOG_BUTTON *pButtons;
	int nDefaultButton;
	UINT cRadioButtons;
	const TASKDIALOG_BUTTON *pRadioButtons;
	int nDefaultRadioButton;
	LPCWSTR pszVerificationText;
	LPCWSTR pszExpandedInformation;
	LPCWSTR pszExpandedControlText;
	LPCWSTR pszCollapsedControlText;
	union {
		HICON hFooterIcon;
		LPCWSTR pszFooterIcon;
	};
	LPCWSTR pszFooter;
	PFTASKDIALOGCALLBACK pfCallback;
	LONG_PTR lpCallbackData;
	UINT cxWidth;
} TASKDIALOGCONFIG;

#pragma pack(pop)

#define TDF_ENABLE_HYPERLINKS          0x1
#define TDF_USE_HICON_MAIN             0x2
#define TDF_USE_HICON_FOOTER           0x4
#define TDF_ALLOW_DIALOG_CANCELLATION  0x8
#define TDF_USE_COMMAND_LINKS          0x10
#define TDF_USE_COMMAND_LINKS_NO_ICON  0x20
#define TDF_EXPAND_FOOTER_AREA         0x40
#define TDF_EXPANDED_BY_DEFAULT        0x80
#define TDF_VERIFICATION_FLAG_CHECKED  0x100
#define TDF_SHOW_PROGRESS_BAR          0x200
#define TDF_SHOW_MARQUEE_PROGRESS_BAR  0x400
#define TDF_CALLBACK_TIMER             0x800
#define TDF_POSITION_RELATIVE_TO_WINDOW 0x1000
#define TDF_RTL_LAYOUT                 0x2000
#define TDF_NO_DEFAULT_RADIO_BUTTON    0x4000
#define TDF_CAN_BE_MINIMIZED           0x8000
#define TDF_SIZE_TO_CONTENT            0x1000000
#define TDCBF_OK_BUTTON                0x1
#define TDCBF_YES_BUTTON               0x2
#define TDCBF_NO_BUTTON                0x4
#define TDCBF_CANCEL_BUTTON            0x8
#define TDCBF_RETRY_BUTTON             0x10
#define TDCBF_CLOSE_BUTTON             0x20
#define TDN_CREATED                    0
#define TDN_BUTTON_CLICKED             2
#define TDN_DESTROYED                  5
#define TDN_DIALOG_CONSTRUCTED         7

/* _TrackMouseEvent. */
#define TME_HOVER            0x1
#define TME_LEAVE            0x2
#define TME_NONCLIENT        0x10
#define TME_QUERY            0x40000000
#define TME_CANCEL           0x80000000

/* LoadIconMetric. */
#define LIM_SMALL            0
#define LIM_LARGE            1

/* DllGetVersion (shlwapi.h). */
typedef struct _DLLVERSIONINFO {
	DWORD cbSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformID;
} DLLVERSIONINFO;

typedef struct _DLLVERSIONINFO2 {
	DLLVERSIONINFO info1;
	DWORD dwFlags;
	ULONGLONG ullVersion;
} DLLVERSIONINFO2;

#define DLLVER_PLATFORM_WINDOWS 0x1
#define DLLVER_PLATFORM_NT      0x2

#endif /* VFW_WIN32_COMMCTRL_H */
