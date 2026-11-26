/* SPDX-License-Identifier: GPL-2.0-only */
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

/* DPA_Merge. */
#define DPAM_SORTED          0x1
#define DPAM_NORMAL          0x2
#define DPAM_UNION           0x4
#define DPAM_INTERSECT       0x8
#define DPAMM_MERGE          1
#define DPAMM_DELETE         2
#define DPAMM_INSERT         3
typedef void *(CALLBACK *PFNDPAMERGE)(UINT uMsg, void *pvDest, void *pvSrc,
				      LPARAM lParam);

/* DPA_LoadStream and DPA_SaveStream: one item per callback. */
struct IStream;
typedef struct _DPASTREAMINFO {
	int iPos;
	void *pvItem;
} DPASTREAMINFO;
typedef HRESULT (CALLBACK *PFNDPASTREAM)(DPASTREAMINFO *pinfo,
					 struct IStream *pstream,
					 void *pvInstData);

/* Window subclassing. */
typedef LRESULT (CALLBACK *SUBCLASSPROC)(HWND hWnd, UINT uMsg, WPARAM wParam,
					 LPARAM lParam, UINT_PTR uIdSubclass,
					 DWORD_PTR dwRefData);

/* Notifications (WM_NOTIFY). */
#define NM_FIRST             (0U - 0U)
#define NM_OUTOFMEMORY       (NM_FIRST - 1)
#define NM_CLICK             (NM_FIRST - 2)
#define NM_DBLCLK            (NM_FIRST - 3)
#define NM_RETURN            (NM_FIRST - 4)
#define NM_RCLICK            (NM_FIRST - 5)
#define NM_RDBLCLK           (NM_FIRST - 6)
#define NM_SETFOCUS          (NM_FIRST - 7)
#define NM_KILLFOCUS         (NM_FIRST - 8)
#define NM_CUSTOMDRAW        (NM_FIRST - 12)

#define LPSTR_TEXTCALLBACKA  ((LPSTR)-1)
#define LPSTR_TEXTCALLBACKW  ((LPWSTR)-1)
#define I_IMAGECALLBACK      (-1)
#define I_IMAGENONE          (-2)
#define I_INDENTCALLBACK     (-1)
#define I_CHILDRENCALLBACK   (-1)

/* Header controls. */
#define HDS_HORZ             0x0
#define HDS_BUTTONS          0x2
#define HDS_HOTTRACK         0x4
#define HDS_HIDDEN           0x8
#define HDS_DRAGDROP         0x40
#define HDS_FULLDRAG         0x80
#define HDS_FILTERBAR        0x100

#define HDI_WIDTH            0x1
#define HDI_HEIGHT           HDI_WIDTH
#define HDI_TEXT             0x2
#define HDI_FORMAT           0x4
#define HDI_LPARAM           0x8
#define HDI_BITMAP           0x10
#define HDI_IMAGE            0x20
#define HDI_DI_SETITEM       0x40
#define HDI_ORDER            0x80
#define HDI_FILTER           0x100
#define HDI_STATE            0x200

#define HDF_LEFT             0x0
#define HDF_RIGHT            0x1
#define HDF_CENTER           0x2
#define HDF_JUSTIFYMASK      0x3
#define HDF_RTLREADING       0x4
#define HDF_SORTDOWN         0x200
#define HDF_SORTUP           0x400
#define HDF_IMAGE            0x800
#define HDF_BITMAP_ON_RIGHT  0x1000
#define HDF_BITMAP           0x2000
#define HDF_STRING           0x4000
#define HDF_OWNERDRAW        0x8000

#define HHT_NOWHERE          0x1
#define HHT_ONHEADER         0x2
#define HHT_ONDIVIDER        0x4
#define HHT_ONDIVOPEN        0x8
#define HHT_ABOVE            0x100
#define HHT_BELOW            0x200
#define HHT_TORIGHT          0x400
#define HHT_TOLEFT           0x800

#define HDM_FIRST            0x1200
#define HDM_GETITEMCOUNT     (HDM_FIRST + 0)
#define HDM_INSERTITEMA      (HDM_FIRST + 1)
#define HDM_DELETEITEM       (HDM_FIRST + 2)
#define HDM_GETITEMA         (HDM_FIRST + 3)
#define HDM_SETITEMA         (HDM_FIRST + 4)
#define HDM_LAYOUT           (HDM_FIRST + 5)
#define HDM_HITTEST          (HDM_FIRST + 6)
#define HDM_GETITEMRECT      (HDM_FIRST + 7)
#define HDM_SETIMAGELIST     (HDM_FIRST + 8)
#define HDM_GETIMAGELIST     (HDM_FIRST + 9)
#define HDM_INSERTITEMW      (HDM_FIRST + 10)
#define HDM_GETITEMW         (HDM_FIRST + 11)
#define HDM_SETITEMW         (HDM_FIRST + 12)
#define HDM_ORDERTOINDEX     (HDM_FIRST + 15)
#define HDM_CREATEDRAGIMAGE  (HDM_FIRST + 16)
#define HDM_GETORDERARRAY    (HDM_FIRST + 17)
#define HDM_SETORDERARRAY    (HDM_FIRST + 18)
#define HDM_SETHOTDIVIDER    (HDM_FIRST + 19)
#define HDM_SETBITMAPMARGIN  (HDM_FIRST + 20)
#define HDM_GETBITMAPMARGIN  (HDM_FIRST + 21)
#define HDM_SETFILTERCHANGETIMEOUT (HDM_FIRST + 22)
#define HDM_EDITFILTER       (HDM_FIRST + 23)
#define HDM_CLEARFILTER      (HDM_FIRST + 24)
#define HDM_GETFOCUSEDITEM   (HDM_FIRST + 27)
#define HDM_SETFOCUSEDITEM   (HDM_FIRST + 28)
#define HDM_SETUNICODEFORMAT CCM_SETUNICODEFORMAT
#define HDM_GETUNICODEFORMAT CCM_GETUNICODEFORMAT

#define HDN_FIRST            (0U - 300U)
#define HDN_ITEMCHANGINGA    (HDN_FIRST - 0)
#define HDN_ITEMCHANGEDA     (HDN_FIRST - 1)
#define HDN_ITEMCLICKA       (HDN_FIRST - 2)
#define HDN_ITEMDBLCLICKA    (HDN_FIRST - 3)
#define HDN_DIVIDERDBLCLICKA (HDN_FIRST - 5)
#define HDN_BEGINTRACKA      (HDN_FIRST - 6)
#define HDN_ENDTRACKA        (HDN_FIRST - 7)
#define HDN_TRACKA           (HDN_FIRST - 8)
#define HDN_GETDISPINFOA     (HDN_FIRST - 9)
#define HDN_BEGINDRAG        (HDN_FIRST - 10)
#define HDN_ENDDRAG          (HDN_FIRST - 11)
#define HDN_ITEMCHANGINGW    (HDN_FIRST - 20)
#define HDN_ITEMCHANGEDW     (HDN_FIRST - 21)
#define HDN_ITEMCLICKW       (HDN_FIRST - 22)
#define HDN_ITEMDBLCLICKW    (HDN_FIRST - 23)
#define HDN_DIVIDERDBLCLICKW (HDN_FIRST - 25)
#define HDN_BEGINTRACKW      (HDN_FIRST - 26)
#define HDN_ENDTRACKW        (HDN_FIRST - 27)
#define HDN_TRACKW           (HDN_FIRST - 28)
#define HDN_GETDISPINFOW     (HDN_FIRST - 29)

typedef struct _HD_ITEMA {
	UINT mask;
	int cxy;
	LPSTR pszText;
	HBITMAP hbm;
	int cchTextMax;
	int fmt;
	LPARAM lParam;
	int iImage;
	int iOrder;
	UINT type;
	void *pvFilter;
	UINT state;
} HDITEMA, *LPHDITEMA;

typedef struct _HD_ITEMW {
	UINT mask;
	int cxy;
	LPWSTR pszText;
	HBITMAP hbm;
	int cchTextMax;
	int fmt;
	LPARAM lParam;
	int iImage;
	int iOrder;
	UINT type;
	void *pvFilter;
	UINT state;
} HDITEMW, *LPHDITEMW;

struct tagWINDOWPOS;
typedef struct _HD_LAYOUT {
	RECT *prc;
	struct tagWINDOWPOS *pwpos;
} HDLAYOUT, *LPHDLAYOUT;

typedef struct _HD_HITTESTINFO {
	POINT pt;
	UINT flags;
	int iItem;
} HDHITTESTINFO, *LPHDHITTESTINFO;

typedef struct tagNMHEADERA {
	NMHDR hdr;
	int iItem;
	int iButton;
	HDITEMA *pitem;
} NMHEADERA, *LPNMHEADERA;

typedef struct tagNMHEADERW {
	NMHDR hdr;
	int iItem;
	int iButton;
	HDITEMW *pitem;
} NMHEADERW, *LPNMHEADERW;

/* List views. */
#define LVS_ICON             0x0
#define LVS_REPORT           0x1
#define LVS_SMALLICON        0x2
#define LVS_LIST             0x3
#define LVS_TYPEMASK         0x3
#define LVS_SINGLESEL        0x4
#define LVS_SHOWSELALWAYS    0x8
#define LVS_SORTASCENDING    0x10
#define LVS_SORTDESCENDING   0x20
#define LVS_SHAREIMAGELISTS  0x40
#define LVS_NOLABELWRAP      0x80
#define LVS_AUTOARRANGE      0x100
#define LVS_EDITLABELS       0x200
#define LVS_OWNERDRAWFIXED   0x400
#define LVS_ALIGNLEFT        0x800
#define LVS_OWNERDATA        0x1000
#define LVS_NOSCROLL         0x2000
#define LVS_NOCOLUMNHEADER   0x4000
#define LVS_NOSORTHEADER     0x8000

#define LVS_EX_GRIDLINES        0x1
#define LVS_EX_SUBITEMIMAGES    0x2
#define LVS_EX_CHECKBOXES       0x4
#define LVS_EX_TRACKSELECT      0x8
#define LVS_EX_HEADERDRAGDROP   0x10
#define LVS_EX_FULLROWSELECT    0x20
#define LVS_EX_ONECLICKACTIVATE 0x40
#define LVS_EX_TWOCLICKACTIVATE 0x80
#define LVS_EX_FLATSB           0x100
#define LVS_EX_INFOTIP          0x400
#define LVS_EX_LABELTIP         0x4000
#define LVS_EX_DOUBLEBUFFER     0x10000

#define LV_VIEW_ICON         0x0
#define LV_VIEW_DETAILS      0x1
#define LV_VIEW_SMALLICON    0x2
#define LV_VIEW_LIST         0x3
#define LV_VIEW_TILE         0x4

#define LVSIL_NORMAL         0
#define LVSIL_SMALL          1
#define LVSIL_STATE          2

#define LVIF_TEXT            0x1
#define LVIF_IMAGE           0x2
#define LVIF_PARAM           0x4
#define LVIF_STATE           0x8
#define LVIF_INDENT          0x10
#define LVIF_GROUPID         0x100
#define LVIF_COLUMNS         0x200
#define LVIF_NORECOMPUTE     0x800

#define LVIS_FOCUSED         0x1
#define LVIS_SELECTED        0x2
#define LVIS_CUT             0x4
#define LVIS_DROPHILITED     0x8
#define LVIS_OVERLAYMASK     0xF00
#define LVIS_STATEIMAGEMASK  0xF000

#define LVNI_ALL             0x0
#define LVNI_FOCUSED         0x1
#define LVNI_SELECTED        0x2
#define LVNI_CUT             0x4
#define LVNI_DROPHILITED     0x8
#define LVNI_ABOVE           0x100
#define LVNI_BELOW           0x200
#define LVNI_TOLEFT          0x400
#define LVNI_TORIGHT         0x800

#define LVFI_PARAM           0x1
#define LVFI_STRING          0x2
#define LVFI_PARTIAL         0x8
#define LVFI_WRAP            0x20
#define LVFI_NEARESTXY       0x40

#define LVIR_BOUNDS          0
#define LVIR_ICON            1
#define LVIR_LABEL           2
#define LVIR_SELECTBOUNDS    3

#define LVHT_NOWHERE         0x1
#define LVHT_ONITEMICON      0x2
#define LVHT_ONITEMLABEL     0x4
#define LVHT_ONITEMSTATEICON 0x8
#define LVHT_ONITEM          (LVHT_ONITEMICON | LVHT_ONITEMLABEL | \
			      LVHT_ONITEMSTATEICON)
#define LVHT_ABOVE           0x8
#define LVHT_BELOW           0x10
#define LVHT_TORIGHT         0x20
#define LVHT_TOLEFT          0x40

#define LVCF_FMT             0x1
#define LVCF_WIDTH           0x2
#define LVCF_TEXT            0x4
#define LVCF_SUBITEM         0x8
#define LVCF_IMAGE           0x10
#define LVCF_ORDER           0x20

#define LVCFMT_LEFT          0x0
#define LVCFMT_RIGHT         0x1
#define LVCFMT_CENTER        0x2
#define LVCFMT_JUSTIFYMASK   0x3
#define LVCFMT_IMAGE         0x800
#define LVCFMT_BITMAP_ON_RIGHT 0x1000
#define LVCFMT_COL_HAS_IMAGES 0x8000

#define LVSCW_AUTOSIZE       (-1)
#define LVSCW_AUTOSIZE_USEHEADER (-2)

#define LVA_DEFAULT          0x0
#define LVA_ALIGNLEFT        0x1
#define LVA_ALIGNTOP         0x2
#define LVA_SNAPTOGRID       0x5

#define LVM_FIRST            0x1000
#define LVM_GETBKCOLOR       (LVM_FIRST + 0)
#define LVM_SETBKCOLOR       (LVM_FIRST + 1)
#define LVM_GETIMAGELIST     (LVM_FIRST + 2)
#define LVM_SETIMAGELIST     (LVM_FIRST + 3)
#define LVM_GETITEMCOUNT     (LVM_FIRST + 4)
#define LVM_GETITEMA         (LVM_FIRST + 5)
#define LVM_SETITEMA         (LVM_FIRST + 6)
#define LVM_INSERTITEMA      (LVM_FIRST + 7)
#define LVM_DELETEITEM       (LVM_FIRST + 8)
#define LVM_DELETEALLITEMS   (LVM_FIRST + 9)
#define LVM_GETCALLBACKMASK  (LVM_FIRST + 10)
#define LVM_SETCALLBACKMASK  (LVM_FIRST + 11)
#define LVM_GETNEXTITEM      (LVM_FIRST + 12)
#define LVM_FINDITEMA        (LVM_FIRST + 13)
#define LVM_GETITEMRECT      (LVM_FIRST + 14)
#define LVM_SETITEMPOSITION  (LVM_FIRST + 15)
#define LVM_GETITEMPOSITION  (LVM_FIRST + 16)
#define LVM_GETSTRINGWIDTHA  (LVM_FIRST + 17)
#define LVM_HITTEST          (LVM_FIRST + 18)
#define LVM_ENSUREVISIBLE    (LVM_FIRST + 19)
#define LVM_SCROLL           (LVM_FIRST + 20)
#define LVM_REDRAWITEMS      (LVM_FIRST + 21)
#define LVM_ARRANGE          (LVM_FIRST + 22)
#define LVM_EDITLABELA       (LVM_FIRST + 23)
#define LVM_GETEDITCONTROL   (LVM_FIRST + 24)
#define LVM_GETCOLUMNA       (LVM_FIRST + 25)
#define LVM_SETCOLUMNA       (LVM_FIRST + 26)
#define LVM_INSERTCOLUMNA    (LVM_FIRST + 27)
#define LVM_DELETECOLUMN     (LVM_FIRST + 28)
#define LVM_GETCOLUMNWIDTH   (LVM_FIRST + 29)
#define LVM_SETCOLUMNWIDTH   (LVM_FIRST + 30)
#define LVM_GETHEADER        (LVM_FIRST + 31)
#define LVM_CREATEDRAGIMAGE  (LVM_FIRST + 33)
#define LVM_GETVIEWRECT      (LVM_FIRST + 34)
#define LVM_GETTEXTCOLOR     (LVM_FIRST + 35)
#define LVM_SETTEXTCOLOR     (LVM_FIRST + 36)
#define LVM_GETTEXTBKCOLOR   (LVM_FIRST + 37)
#define LVM_SETTEXTBKCOLOR   (LVM_FIRST + 38)
#define LVM_GETTOPINDEX      (LVM_FIRST + 39)
#define LVM_GETCOUNTPERPAGE  (LVM_FIRST + 40)
#define LVM_GETORIGIN        (LVM_FIRST + 41)
#define LVM_UPDATE           (LVM_FIRST + 42)
#define LVM_SETITEMSTATE     (LVM_FIRST + 43)
#define LVM_GETITEMSTATE     (LVM_FIRST + 44)
#define LVM_GETITEMTEXTA     (LVM_FIRST + 45)
#define LVM_SETITEMTEXTA     (LVM_FIRST + 46)
#define LVM_SETITEMCOUNT     (LVM_FIRST + 47)
#define LVM_SORTITEMS        (LVM_FIRST + 48)
#define LVM_SETITEMPOSITION32 (LVM_FIRST + 49)
#define LVM_GETSELECTEDCOUNT (LVM_FIRST + 50)
#define LVM_GETITEMSPACING   (LVM_FIRST + 51)
#define LVM_GETISEARCHSTRINGA (LVM_FIRST + 52)
#define LVM_SETICONSPACING   (LVM_FIRST + 53)
#define LVM_SETEXTENDEDLISTVIEWSTYLE (LVM_FIRST + 54)
#define LVM_GETEXTENDEDLISTVIEWSTYLE (LVM_FIRST + 55)
#define LVM_GETSUBITEMRECT   (LVM_FIRST + 56)
#define LVM_SUBITEMHITTEST   (LVM_FIRST + 57)
#define LVM_SETCOLUMNORDERARRAY (LVM_FIRST + 58)
#define LVM_GETCOLUMNORDERARRAY (LVM_FIRST + 59)
#define LVM_SETHOTITEM       (LVM_FIRST + 60)
#define LVM_GETHOTITEM       (LVM_FIRST + 61)
#define LVM_APPROXIMATEVIEWRECT (LVM_FIRST + 64)
#define LVM_GETSELECTIONMARK (LVM_FIRST + 66)
#define LVM_SETSELECTIONMARK (LVM_FIRST + 67)
#define LVM_SETHOVERTIME     (LVM_FIRST + 71)
#define LVM_GETHOVERTIME     (LVM_FIRST + 72)
#define LVM_SETTOOLTIPS      (LVM_FIRST + 74)
#define LVM_GETITEMW         (LVM_FIRST + 75)
#define LVM_SETITEMW         (LVM_FIRST + 76)
#define LVM_INSERTITEMW      (LVM_FIRST + 77)
#define LVM_GETTOOLTIPS      (LVM_FIRST + 78)
#define LVM_SORTITEMSEX      (LVM_FIRST + 81)
#define LVM_FINDITEMW        (LVM_FIRST + 83)
#define LVM_GETSTRINGWIDTHW  (LVM_FIRST + 87)
#define LVM_GETCOLUMNW       (LVM_FIRST + 95)
#define LVM_SETCOLUMNW       (LVM_FIRST + 96)
#define LVM_INSERTCOLUMNW    (LVM_FIRST + 97)
#define LVM_GETITEMTEXTW     (LVM_FIRST + 115)
#define LVM_SETITEMTEXTW     (LVM_FIRST + 116)
#define LVM_GETISEARCHSTRINGW (LVM_FIRST + 117)
#define LVM_EDITLABELW       (LVM_FIRST + 118)
#define LVM_SETVIEW          (LVM_FIRST + 142)
#define LVM_GETVIEW          (LVM_FIRST + 143)
#define LVM_SETUNICODEFORMAT CCM_SETUNICODEFORMAT
#define LVM_GETUNICODEFORMAT CCM_GETUNICODEFORMAT

#define LVN_FIRST            (0U - 100U)
#define LVN_ITEMCHANGING     (LVN_FIRST - 0)
#define LVN_ITEMCHANGED      (LVN_FIRST - 1)
#define LVN_INSERTITEM       (LVN_FIRST - 2)
#define LVN_DELETEITEM       (LVN_FIRST - 3)
#define LVN_DELETEALLITEMS   (LVN_FIRST - 4)
#define LVN_BEGINLABELEDITA  (LVN_FIRST - 5)
#define LVN_ENDLABELEDITA    (LVN_FIRST - 6)
#define LVN_COLUMNCLICK      (LVN_FIRST - 8)
#define LVN_BEGINDRAG        (LVN_FIRST - 9)
#define LVN_BEGINRDRAG       (LVN_FIRST - 11)
#define LVN_ODCACHEHINT      (LVN_FIRST - 13)
#define LVN_ITEMACTIVATE     (LVN_FIRST - 14)
#define LVN_ODSTATECHANGED   (LVN_FIRST - 15)
#define LVN_HOTTRACK         (LVN_FIRST - 21)
#define LVN_GETDISPINFOA     (LVN_FIRST - 50)
#define LVN_SETDISPINFOA     (LVN_FIRST - 51)
#define LVN_ODFINDITEMA      (LVN_FIRST - 52)
#define LVN_KEYDOWN          (LVN_FIRST - 55)
#define LVN_MARQUEEBEGIN     (LVN_FIRST - 56)
#define LVN_BEGINLABELEDITW  (LVN_FIRST - 75)
#define LVN_ENDLABELEDITW    (LVN_FIRST - 76)
#define LVN_GETDISPINFOW     (LVN_FIRST - 77)
#define LVN_SETDISPINFOW     (LVN_FIRST - 78)
#define LVN_ODFINDITEMW      (LVN_FIRST - 79)

typedef struct tagLVITEMA {
	UINT mask;
	int iItem;
	int iSubItem;
	UINT state;
	UINT stateMask;
	LPSTR pszText;
	int cchTextMax;
	int iImage;
	LPARAM lParam;
	int iIndent;
	int iGroupId;
	UINT cColumns;
	UINT *puColumns;
	int *piColFmt;
	int iGroup;
} LVITEMA, *LPLVITEMA;

typedef struct tagLVITEMW {
	UINT mask;
	int iItem;
	int iSubItem;
	UINT state;
	UINT stateMask;
	LPWSTR pszText;
	int cchTextMax;
	int iImage;
	LPARAM lParam;
	int iIndent;
	int iGroupId;
	UINT cColumns;
	UINT *puColumns;
	int *piColFmt;
	int iGroup;
} LVITEMW, *LPLVITEMW;

typedef struct tagLVCOLUMNA {
	UINT mask;
	int fmt;
	int cx;
	LPSTR pszText;
	int cchTextMax;
	int iSubItem;
	int iImage;
	int iOrder;
	int cxMin;
	int cxDefault;
	int cxIdeal;
} LVCOLUMNA, *LPLVCOLUMNA;

typedef struct tagLVCOLUMNW {
	UINT mask;
	int fmt;
	int cx;
	LPWSTR pszText;
	int cchTextMax;
	int iSubItem;
	int iImage;
	int iOrder;
	int cxMin;
	int cxDefault;
	int cxIdeal;
} LVCOLUMNW, *LPLVCOLUMNW;

typedef struct tagLVFINDINFOA {
	UINT flags;
	LPCSTR psz;
	LPARAM lParam;
	POINT pt;
	UINT vkDirection;
} LVFINDINFOA, *LPFINDINFOA;

typedef struct tagLVFINDINFOW {
	UINT flags;
	LPCWSTR psz;
	LPARAM lParam;
	POINT pt;
	UINT vkDirection;
} LVFINDINFOW, *LPFINDINFOW;

typedef struct tagLVHITTESTINFO {
	POINT pt;
	UINT flags;
	int iItem;
	int iSubItem;
	int iGroup;
} LVHITTESTINFO, *LPLVHITTESTINFO;

typedef struct tagNMLISTVIEW {
	NMHDR hdr;
	int iItem;
	int iSubItem;
	UINT uNewState;
	UINT uOldState;
	UINT uChanged;
	POINT ptAction;
	LPARAM lParam;
} NMLISTVIEW, *LPNMLISTVIEW;

typedef struct tagLVDISPINFO {
	NMHDR hdr;
	LVITEMA item;
} NMLVDISPINFOA, *LPNMLVDISPINFOA;

typedef struct tagLVDISPINFOW {
	NMHDR hdr;
	LVITEMW item;
} NMLVDISPINFOW, *LPNMLVDISPINFOW;

typedef int (CALLBACK *PFNLVCOMPARE)(LPARAM, LPARAM, LPARAM);

/* Tree views. */
struct _TREEITEM;
typedef struct _TREEITEM *HTREEITEM;

#define TVS_HASBUTTONS       0x1
#define TVS_HASLINES         0x2
#define TVS_LINESATROOT      0x4
#define TVS_EDITLABELS       0x8
#define TVS_DISABLEDRAGDROP  0x10
#define TVS_SHOWSELALWAYS    0x20
#define TVS_RTLREADING       0x40
#define TVS_NOTOOLTIPS       0x80
#define TVS_CHECKBOXES       0x100
#define TVS_TRACKSELECT      0x200
#define TVS_SINGLEEXPAND     0x400
#define TVS_INFOTIP          0x800
#define TVS_FULLROWSELECT    0x1000
#define TVS_NOSCROLL         0x2000
#define TVS_NONEVENHEIGHT    0x4000
#define TVS_NOHSCROLL        0x8000

#define TVIF_TEXT            0x1
#define TVIF_IMAGE           0x2
#define TVIF_PARAM           0x4
#define TVIF_STATE           0x8
#define TVIF_HANDLE          0x10
#define TVIF_SELECTEDIMAGE   0x20
#define TVIF_CHILDREN        0x40
#define TVIF_INTEGRAL        0x80
#define TVIF_STATEEX         0x100
#define TVIF_EXPANDEDIMAGE   0x200

#define TVIS_SELECTED        0x2
#define TVIS_CUT             0x4
#define TVIS_DROPHILITED     0x8
#define TVIS_BOLD            0x10
#define TVIS_EXPANDED        0x20
#define TVIS_EXPANDEDONCE    0x40
#define TVIS_EXPANDPARTIAL   0x80
#define TVIS_OVERLAYMASK     0xF00
#define TVIS_STATEIMAGEMASK  0xF000
#define TVIS_USERMASK        0xF000

#define TVI_ROOT             ((HTREEITEM)(ULONG_PTR)-0x10000)
#define TVI_FIRST            ((HTREEITEM)(ULONG_PTR)-0x0FFFF)
#define TVI_LAST             ((HTREEITEM)(ULONG_PTR)-0x0FFFE)
#define TVI_SORT             ((HTREEITEM)(ULONG_PTR)-0x0FFFD)

#define TVE_COLLAPSE         0x1
#define TVE_EXPAND           0x2
#define TVE_TOGGLE           0x3
#define TVE_EXPANDPARTIAL    0x4000
#define TVE_COLLAPSERESET    0x8000

#define TVGN_ROOT            0x0
#define TVGN_NEXT            0x1
#define TVGN_PREVIOUS        0x2
#define TVGN_PARENT          0x3
#define TVGN_CHILD           0x4
#define TVGN_FIRSTVISIBLE    0x5
#define TVGN_NEXTVISIBLE     0x6
#define TVGN_PREVIOUSVISIBLE 0x7
#define TVGN_DROPHILITE      0x8
#define TVGN_CARET           0x9
#define TVGN_LASTVISIBLE     0xA

#define TVSIL_NORMAL         0
#define TVSIL_STATE          2

#define TVHT_NOWHERE         0x1
#define TVHT_ONITEMICON      0x2
#define TVHT_ONITEMLABEL     0x4
#define TVHT_ONITEMINDENT    0x8
#define TVHT_ONITEMBUTTON    0x10
#define TVHT_ONITEMRIGHT     0x20
#define TVHT_ONITEMSTATEICON 0x40
#define TVHT_ONITEM          (TVHT_ONITEMICON | TVHT_ONITEMLABEL | \
			      TVHT_ONITEMSTATEICON)
#define TVHT_ABOVE           0x100
#define TVHT_BELOW           0x200
#define TVHT_TORIGHT         0x400
#define TVHT_TOLEFT          0x800

#define TVC_UNKNOWN          0x0
#define TVC_BYMOUSE          0x1
#define TVC_BYKEYBOARD       0x2

#define TV_FIRST             0x1100
#define TVM_INSERTITEMA      (TV_FIRST + 0)
#define TVM_DELETEITEM       (TV_FIRST + 1)
#define TVM_EXPAND           (TV_FIRST + 2)
#define TVM_GETITEMRECT      (TV_FIRST + 4)
#define TVM_GETCOUNT         (TV_FIRST + 5)
#define TVM_GETINDENT        (TV_FIRST + 6)
#define TVM_SETINDENT        (TV_FIRST + 7)
#define TVM_GETIMAGELIST     (TV_FIRST + 8)
#define TVM_SETIMAGELIST     (TV_FIRST + 9)
#define TVM_GETNEXTITEM      (TV_FIRST + 10)
#define TVM_SELECTITEM       (TV_FIRST + 11)
#define TVM_GETITEMA         (TV_FIRST + 12)
#define TVM_SETITEMA         (TV_FIRST + 13)
#define TVM_EDITLABELA       (TV_FIRST + 14)
#define TVM_GETEDITCONTROL   (TV_FIRST + 15)
#define TVM_GETVISIBLECOUNT  (TV_FIRST + 16)
#define TVM_HITTEST          (TV_FIRST + 17)
#define TVM_CREATEDRAGIMAGE  (TV_FIRST + 18)
#define TVM_SORTCHILDREN     (TV_FIRST + 19)
#define TVM_ENSUREVISIBLE    (TV_FIRST + 20)
#define TVM_SORTCHILDRENCB   (TV_FIRST + 21)
#define TVM_ENDEDITLABELNOW  (TV_FIRST + 22)
#define TVM_GETISEARCHSTRINGA (TV_FIRST + 23)
#define TVM_SETTOOLTIPS      (TV_FIRST + 24)
#define TVM_GETTOOLTIPS      (TV_FIRST + 25)
#define TVM_SETINSERTMARK    (TV_FIRST + 26)
#define TVM_SETITEMHEIGHT    (TV_FIRST + 27)
#define TVM_GETITEMHEIGHT    (TV_FIRST + 28)
#define TVM_SETBKCOLOR       (TV_FIRST + 29)
#define TVM_SETTEXTCOLOR     (TV_FIRST + 30)
#define TVM_GETBKCOLOR       (TV_FIRST + 31)
#define TVM_GETTEXTCOLOR     (TV_FIRST + 32)
#define TVM_SETSCROLLTIME    (TV_FIRST + 33)
#define TVM_GETSCROLLTIME    (TV_FIRST + 34)
#define TVM_SETINSERTMARKCOLOR (TV_FIRST + 37)
#define TVM_GETINSERTMARKCOLOR (TV_FIRST + 38)
#define TVM_GETITEMSTATE     (TV_FIRST + 39)
#define TVM_SETLINECOLOR     (TV_FIRST + 40)
#define TVM_GETLINECOLOR     (TV_FIRST + 41)
#define TVM_SETEXTENDEDSTYLE (TV_FIRST + 44)
#define TVM_GETEXTENDEDSTYLE (TV_FIRST + 45)
#define TVM_INSERTITEMW      (TV_FIRST + 50)
#define TVM_GETITEMW         (TV_FIRST + 62)
#define TVM_SETITEMW         (TV_FIRST + 63)
#define TVM_GETISEARCHSTRINGW (TV_FIRST + 64)
#define TVM_EDITLABELW       (TV_FIRST + 65)
#define TVM_SETUNICODEFORMAT CCM_SETUNICODEFORMAT
#define TVM_GETUNICODEFORMAT CCM_GETUNICODEFORMAT

#define TVN_FIRST            (0U - 400U)
#define TVN_SELCHANGINGA     (TVN_FIRST - 1)
#define TVN_SELCHANGEDA      (TVN_FIRST - 2)
#define TVN_GETDISPINFOA     (TVN_FIRST - 3)
#define TVN_SETDISPINFOA     (TVN_FIRST - 4)
#define TVN_ITEMEXPANDINGA   (TVN_FIRST - 5)
#define TVN_ITEMEXPANDEDA    (TVN_FIRST - 6)
#define TVN_BEGINDRAGA       (TVN_FIRST - 7)
#define TVN_BEGINRDRAGA      (TVN_FIRST - 8)
#define TVN_DELETEITEMA      (TVN_FIRST - 9)
#define TVN_BEGINLABELEDITA  (TVN_FIRST - 10)
#define TVN_ENDLABELEDITA    (TVN_FIRST - 11)
#define TVN_KEYDOWN          (TVN_FIRST - 12)
#define TVN_SINGLEEXPAND     (TVN_FIRST - 15)
#define TVN_SELCHANGINGW     (TVN_FIRST - 50)
#define TVN_SELCHANGEDW      (TVN_FIRST - 51)
#define TVN_GETDISPINFOW     (TVN_FIRST - 52)
#define TVN_SETDISPINFOW     (TVN_FIRST - 53)
#define TVN_ITEMEXPANDINGW   (TVN_FIRST - 54)
#define TVN_ITEMEXPANDEDW    (TVN_FIRST - 55)
#define TVN_BEGINDRAGW       (TVN_FIRST - 56)
#define TVN_BEGINRDRAGW      (TVN_FIRST - 57)
#define TVN_DELETEITEMW      (TVN_FIRST - 58)
#define TVN_BEGINLABELEDITW  (TVN_FIRST - 59)
#define TVN_ENDLABELEDITW    (TVN_FIRST - 60)

typedef struct tagTVITEMA {
	UINT mask;
	HTREEITEM hItem;
	UINT state;
	UINT stateMask;
	LPSTR pszText;
	int cchTextMax;
	int iImage;
	int iSelectedImage;
	int cChildren;
	LPARAM lParam;
} TVITEMA, *LPTVITEMA;

typedef struct tagTVITEMW {
	UINT mask;
	HTREEITEM hItem;
	UINT state;
	UINT stateMask;
	LPWSTR pszText;
	int cchTextMax;
	int iImage;
	int iSelectedImage;
	int cChildren;
	LPARAM lParam;
} TVITEMW, *LPTVITEMW;

typedef struct tagTVITEMEXA {
	UINT mask;
	HTREEITEM hItem;
	UINT state;
	UINT stateMask;
	LPSTR pszText;
	int cchTextMax;
	int iImage;
	int iSelectedImage;
	int cChildren;
	LPARAM lParam;
	int iIntegral;
	UINT uStateEx;
	HWND hwnd;
	int iExpandedImage;
	int iReserved;
} TVITEMEXA, *LPTVITEMEXA;

typedef struct tagTVITEMEXW {
	UINT mask;
	HTREEITEM hItem;
	UINT state;
	UINT stateMask;
	LPWSTR pszText;
	int cchTextMax;
	int iImage;
	int iSelectedImage;
	int cChildren;
	LPARAM lParam;
	int iIntegral;
	UINT uStateEx;
	HWND hwnd;
	int iExpandedImage;
	int iReserved;
} TVITEMEXW, *LPTVITEMEXW;

typedef struct tagTVINSERTSTRUCTA {
	HTREEITEM hParent;
	HTREEITEM hInsertAfter;
	union {
		TVITEMEXA itemex;
		TVITEMA item;
	};
} TVINSERTSTRUCTA, *LPTVINSERTSTRUCTA;

typedef struct tagTVINSERTSTRUCTW {
	HTREEITEM hParent;
	HTREEITEM hInsertAfter;
	union {
		TVITEMEXW itemex;
		TVITEMW item;
	};
} TVINSERTSTRUCTW, *LPTVINSERTSTRUCTW;

typedef struct tagNMTREEVIEWA {
	NMHDR hdr;
	UINT action;
	TVITEMA itemOld;
	TVITEMA itemNew;
	POINT ptDrag;
} NMTREEVIEWA, *LPNMTREEVIEWA;

typedef struct tagNMTREEVIEWW {
	NMHDR hdr;
	UINT action;
	TVITEMW itemOld;
	TVITEMW itemNew;
	POINT ptDrag;
} NMTREEVIEWW, *LPNMTREEVIEWW;

typedef struct tagTVDISPINFOA {
	NMHDR hdr;
	TVITEMA item;
} NMTVDISPINFOA, *LPNMTVDISPINFOA;

typedef struct tagTVDISPINFOW {
	NMHDR hdr;
	TVITEMW item;
} NMTVDISPINFOW, *LPNMTVDISPINFOW;

typedef int (CALLBACK *PFNTVCOMPARE)(LPARAM, LPARAM, LPARAM);

typedef struct tagTVSORTCB {
	HTREEITEM hParent;
	PFNTVCOMPARE lpfnCompare;
	LPARAM lParam;
} TVSORTCB, *LPTVSORTCB;

typedef struct tagTVHITTESTINFO {
	POINT pt;
	UINT flags;
	HTREEITEM hItem;
} TVHITTESTINFO, *LPTVHITTESTINFO;


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


/* Flat scroll bars. */
#define WSB_PROP_CYVSCROLL 0x00000001
#define WSB_PROP_CXHSCROLL 0x00000002
#define WSB_PROP_CYHSCROLL 0x00000004
#define WSB_PROP_CXVSCROLL 0x00000008
#define WSB_PROP_CXHTHUMB  0x00000010
#define WSB_PROP_CYVTHUMB  0x00000020
#define WSB_PROP_VBKGCOLOR 0x00000040
#define WSB_PROP_HBKGCOLOR 0x00000080
#define WSB_PROP_VSTYLE    0x00000100
#define WSB_PROP_HSTYLE    0x00000200
#define WSB_PROP_WINSTYLE  0x00000400
#define WSB_PROP_PALETTE   0x00000800
#define WSB_PROP_MASK      0x00000fff

#define FSB_REGULAR_MODE 0
#define FSB_ENCARTA_MODE 1
#define FSB_FLAT_MODE    2

/* MRU lists. */
#define MRU_STRING   0x0000
#define MRU_BINARY   0x0001
#define MRU_CACHEWRITE 0x0002

typedef int (CALLBACK *MRUSTRINGCMPPROCA)(LPCSTR, LPCSTR);
typedef int (CALLBACK *MRUSTRINGCMPPROCW)(LPCWSTR, LPCWSTR);
typedef int (CALLBACK *MRUBINARYCMPPROC)(LPCVOID, LPCVOID, DWORD);

typedef struct tagMRUINFOA {
	DWORD cbSize;
	UINT uMax;
	UINT fFlags;
	HKEY hKey;
	LPCSTR lpszSubKey;
	union {
		MRUSTRINGCMPPROCA string_cmpfn;
		MRUBINARYCMPPROC binary_cmpfn;
	} u;
} MRUINFOA, *LPMRUINFOA;

typedef struct tagMRUINFOW {
	DWORD cbSize;
	UINT uMax;
	UINT fFlags;
	HKEY hKey;
	LPCWSTR lpszSubKey;
	union {
		MRUSTRINGCMPPROCW string_cmpfn;
		MRUBINARYCMPPROC binary_cmpfn;
	} u;
} MRUINFOW, *LPMRUINFOW;

#endif /* VFW_WIN32_COMMCTRL_H */
