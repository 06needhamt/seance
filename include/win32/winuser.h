/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * user32 structures and constants (winuser.h): what headless codecs touch
 * - windows and messages, message boxes, system metrics and colours.
 */
#ifndef VFW_WIN32_WINUSER_H
#define VFW_WIN32_WINUSER_H

#include "types.h"

typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR (CALLBACK *DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void (CALLBACK *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);

typedef struct tagWNDCLASSA {
	UINT style;
	WNDPROC lpfnWndProc;
	int cbClsExtra;
	int cbWndExtra;
	HINSTANCE hInstance;
	HICON hIcon;
	HCURSOR hCursor;
	HBRUSH hbrBackground;
	LPCSTR lpszMenuName;
	LPCSTR lpszClassName;
} WNDCLASSA, *PWNDCLASSA, *LPWNDCLASSA;

typedef struct tagWNDCLASSW {
	UINT style;
	WNDPROC lpfnWndProc;
	int cbClsExtra;
	int cbWndExtra;
	HINSTANCE hInstance;
	HICON hIcon;
	HCURSOR hCursor;
	HBRUSH hbrBackground;
	LPCWSTR lpszMenuName;
	LPCWSTR lpszClassName;
} WNDCLASSW, *PWNDCLASSW, *LPWNDCLASSW;

typedef struct tagWNDCLASSEXA {
	UINT cbSize;
	UINT style;
	WNDPROC lpfnWndProc;
	int cbClsExtra;
	int cbWndExtra;
	HINSTANCE hInstance;
	HICON hIcon;
	HCURSOR hCursor;
	HBRUSH hbrBackground;
	LPCSTR lpszMenuName;
	LPCSTR lpszClassName;
	HICON hIconSm;
} WNDCLASSEXA, *PWNDCLASSEXA, *LPWNDCLASSEXA;

typedef struct tagWNDCLASSEXW {
	UINT cbSize;
	UINT style;
	WNDPROC lpfnWndProc;
	int cbClsExtra;
	int cbWndExtra;
	HINSTANCE hInstance;
	HICON hIcon;
	HCURSOR hCursor;
	HBRUSH hbrBackground;
	LPCWSTR lpszMenuName;
	LPCWSTR lpszClassName;
	HICON hIconSm;
} WNDCLASSEXW, *PWNDCLASSEXW, *LPWNDCLASSEXW;

/* Accelerator tables. */
DECLARE_HANDLE(HACCEL);
typedef struct tagACCEL {
	BYTE fVirt;
	WORD key;
	WORD cmd;
} ACCEL, *LPACCEL;
#define FVIRTKEY  0x01
#define FNOINVERT 0x02
#define FSHIFT    0x04
#define FCONTROL  0x08
#define FALT      0x10

#define VK_BACK              0x08
#define VK_TAB               0x09
#define VK_RETURN            0x0D
#define VK_SHIFT   0x10
#define VK_CONTROL 0x11
#define VK_MENU    0x12
#define VK_ESCAPE            0x1B
#define VK_DELETE            0x2E

typedef struct tagMSG {
	HWND hwnd;
	UINT message;
	WPARAM wParam;
	LPARAM lParam;
	DWORD time;
	POINT pt;
} MSG, *PMSG, *LPMSG;

typedef struct tagCREATESTRUCTA {
	LPVOID lpCreateParams;
	HINSTANCE hInstance;
	HMENU hMenu;
	HWND hwndParent;
	int cy;
	int cx;
	int y;
	int x;
	LONG style;
	LPCSTR lpszName;
	LPCSTR lpszClass;
	DWORD dwExStyle;
} CREATESTRUCTA, *LPCREATESTRUCTA;

typedef struct tagCREATESTRUCTW {
	LPVOID lpCreateParams;
	HINSTANCE hInstance;
	HMENU hMenu;
	HWND hwndParent;
	int cy;
	int cx;
	int y;
	int x;
	LONG style;
	LPCWSTR lpszName;
	LPCWSTR lpszClass;
	DWORD dwExStyle;
} CREATESTRUCTW, *LPCREATESTRUCTW;

typedef struct tagPAINTSTRUCT {
	HDC hdc;
	BOOL fErase;
	RECT rcPaint;
	BOOL fRestore;
	BOOL fIncUpdate;
	BYTE rgbReserved[32];
} PAINTSTRUCT, *PPAINTSTRUCT, *LPPAINTSTRUCT;

typedef struct tagUSEROBJECTFLAGS {
	BOOL fInherit;
	BOOL fReserved;
	DWORD dwFlags;
} USEROBJECTFLAGS, *PUSEROBJECTFLAGS;

/* EnumDisplaySettings' mode numbers besides 0, 1, ... */
#define ENUM_CURRENT_SETTINGS  ((DWORD)-1)
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)

typedef struct tagMONITORINFO {
	DWORD cbSize;
	RECT rcMonitor;
	RECT rcWork;
	DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;

/* DrawText. */
#define DT_TOP        0x0000
#define DT_LEFT       0x0000
#define DT_CENTER     0x0001
#define DT_RIGHT      0x0002
#define DT_VCENTER    0x0004
#define DT_BOTTOM     0x0008
#define DT_WORDBREAK  0x0010
#define DT_SINGLELINE 0x0020
#define DT_EXPANDTABS 0x0040
#define DT_NOCLIP     0x0100
#define DT_CALCRECT   0x0400
#define DT_NOPREFIX   0x0800

/* MessageBox. */
#define MB_OK                0x00000000
#define MB_OKCANCEL          0x00000001
#define MB_ABORTRETRYIGNORE  0x00000002
#define MB_YESNOCANCEL       0x00000003
#define MB_YESNO             0x00000004
#define MB_RETRYCANCEL       0x00000005
#define MB_CANCELTRYCONTINUE 0x00000006
#define MB_ICONHAND          0x00000010
#define MB_ICONQUESTION      0x00000020
#define MB_ICONEXCLAMATION   0x00000030
#define MB_ICONASTERISK      0x00000040
#define MB_DEFBUTTON1        0x00000000
#define MB_DEFBUTTON2        0x00000100
#define MB_DEFBUTTON3        0x00000200
#define MB_DEFBUTTON4        0x00000300
#define MB_TYPEMASK          0x0000000f
#define MB_ICONMASK          0x000000f0
#define MB_DEFMASK           0x00000f00

#define IDOK       1
#define IDCANCEL   2
#define IDABORT    3
#define IDRETRY    4
#define IDIGNORE   5
#define IDYES      6
#define IDNO       7
#define IDTRYAGAIN 10
#define IDCONTINUE 11

/* GetSystemMetrics. */
#define SM_CXSCREEN          0
#define SM_CYSCREEN          1
#define SM_CXVSCROLL         2
#define SM_CYHSCROLL         3
#define SM_CYCAPTION         4
#define SM_CXBORDER          5
#define SM_CYBORDER          6
#define SM_CXDLGFRAME        7
#define SM_CYDLGFRAME        8
#define SM_CYVTHUMB          9
#define SM_CXHTHUMB          10
#define SM_CXICON            11
#define SM_CYICON            12
#define SM_CXCURSOR          13
#define SM_CYCURSOR          14
#define SM_CYMENU            15
#define SM_CXFULLSCREEN      16
#define SM_CYFULLSCREEN      17
#define SM_CYKANJIWINDOW     18
#define SM_MOUSEPRESENT      19
#define SM_CYVSCROLL         20
#define SM_CXHSCROLL         21
#define SM_DEBUG             22
#define SM_SWAPBUTTON        23
#define SM_CXMIN             28
#define SM_CYMIN             29
#define SM_CXSIZE            30
#define SM_CYSIZE            31
#define SM_CXFRAME           32
#define SM_CYFRAME           33
#define SM_CXMINTRACK        34
#define SM_CYMINTRACK        35
#define SM_CXDOUBLECLK       36
#define SM_CYDOUBLECLK       37
#define SM_CMOUSEBUTTONS     43
#define SM_CXSMICON          49
#define SM_CYSMICON          50
#define SM_CXMAXIMIZED       61
#define SM_CYMAXIMIZED       62
#define SM_NETWORK           63
#define SM_CLEANBOOT         67
#define SM_XVIRTUALSCREEN    76
#define SM_YVIRTUALSCREEN    77
#define SM_CXVIRTUALSCREEN   78
#define SM_CYVIRTUALSCREEN   79
#define SM_CMONITORS         80
#define SM_SAMEDISPLAYFORMAT 81
#define SM_REMOTESESSION     0x1000

/* SendMessageTimeout flags. */
#define SMTO_NORMAL             0x0000
#define SMTO_BLOCK              0x0001
#define SMTO_ABORTIFHUNG        0x0002
#define SMTO_NOTIMEOUTIFNOTHUNG 0x0008
#define SMTO_ERRORONEXIT        0x0020

/* Window messages. */
#define WM_NULL              0x0000
#define WM_CREATE            0x0001
#define WM_DESTROY           0x0002
#define WM_MOVE              0x0003
#define WM_SIZE              0x0005
#define WM_ACTIVATE          0x0006
#define WM_SETFOCUS          0x0007
#define WM_KILLFOCUS         0x0008
#define WM_ENABLE            0x000a
#define WM_SETTEXT           0x000c
#define WM_GETTEXT           0x000d
#define WM_GETTEXTLENGTH     0x000e
#define WM_PAINT             0x000f
#define WM_CLOSE             0x0010
#define WM_QUIT              0x0012
#define WM_ERASEBKGND        0x0014
#define WM_SHOWWINDOW        0x0018
#define WM_GETMINMAXINFO     0x0024
#define WM_SETFONT           0x0030
#define WM_GETFONT           0x0031
#define WM_NOTIFY            0x004E
#define WM_NOTIFYFORMAT      0x0055
#define WM_MOUSEMOVE         0x0200
#define WM_LBUTTONDOWN       0x0201
#define WM_LBUTTONUP         0x0202
#define WM_RBUTTONDOWN       0x0204
#define WM_RBUTTONUP         0x0205
#define WM_MBUTTONDOWN       0x0207
#define WM_MBUTTONUP         0x0208
#define MK_LBUTTON           0x0001
#define MK_RBUTTON           0x0002
#define MK_SHIFT             0x0004
#define MK_CONTROL           0x0008
#define MK_MBUTTON           0x0010

/* Combo box messages (which ComboBoxEx answers). */
#define CB_ERR               (-1)
#define CB_ERRSPACE          (-2)
#define CB_ADDSTRING         0x0143
#define CB_DELETESTRING      0x0144
#define CB_GETCOUNT          0x0146
#define CB_GETCURSEL         0x0147
#define CB_GETLBTEXT         0x0148
#define CB_GETLBTEXTLEN      0x0149
#define CB_INSERTSTRING      0x014A
#define CB_RESETCONTENT      0x014B
#define CB_FINDSTRING        0x014C
#define CB_SETCURSEL         0x014E
#define CB_SHOWDROPDOWN      0x014F
#define CB_GETITEMDATA       0x0150
#define CB_SETITEMDATA       0x0151
#define CB_GETDROPPEDSTATE   0x0157
#define CB_FINDSTRINGEXACT   0x0158
#define CBN_SELCHANGE        1
#define EN_CHANGE            0x0300
#define CBN_DROPDOWN         7
#define CBN_CLOSEUP          8
#define WM_STYLECHANGING     0x007C
#define WM_STYLECHANGED      0x007D
#define NFR_ANSI             1
#define NFR_UNICODE          2
#define NF_QUERY             3
#define NF_REQUERY           4
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED  0x0047
#define WM_NCCREATE          0x0081
#define WM_NCDESTROY         0x0082
#define WM_NCCALCSIZE        0x0083
#define WM_INITDIALOG        0x0110
#define WM_COMMAND           0x0111
#define WM_KEYDOWN           0x0100
#define WM_KEYUP             0x0101
#define WM_CHAR              0x0102
#define WM_SYSKEYDOWN        0x0104
#define WM_SYSKEYUP          0x0105
#define WM_SYSCHAR           0x0106
#define WM_SYSCOMMAND        0x0112
#define WM_RENDERFORMAT      0x0305
#define WM_RENDERALLFORMATS  0x0306
#define WM_TIMER             0x0113
#define WM_USER              0x0400
#define WM_APP               0x8000

/*
 * Get/SetWindowLong, and the Ptr forms' indices for the pointer-sized
 * values. Win64 has only the latter for those: a LONG can't hold them.
 */
#if !defined(__x86_64__)
#define GWL_WNDPROC    (-4)
#define GWL_HINSTANCE  (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_USERDATA   (-21)
#define DWL_MSGRESULT  0
#define DWL_DLGPROC    4
#define DWL_USER       8
#endif
#define GWL_ID         (-12)
#define GWL_STYLE      (-16)
#define GWL_EXSTYLE    (-20)
#define GWLP_WNDPROC    (-4)
#define GWLP_HINSTANCE  (-6)
#define GWLP_HWNDPARENT (-8)
#define GWLP_ID         (-12)
#define GWLP_USERDATA   (-21)
#define GCLP_MENUNAME      (-8)
#define GCLP_HBRBACKGROUND (-10)
#define GCLP_HCURSOR       (-12)
#define GCLP_HICON         (-14)
#define GCLP_HMODULE       (-16)
#define GCL_CBWNDEXTRA     (-18)
#define GCL_CBCLSEXTRA     (-20)
#define GCLP_WNDPROC       (-24)
#define GCL_STYLE          (-26)
#define GCW_ATOM           (-32)
#define GCLP_HICONSM       (-34)
#define DWLP_MSGRESULT  0
#define DWLP_DLGPROC    ((LONG_PTR)DWLP_MSGRESULT + (LONG_PTR)sizeof(LRESULT))
#define DWLP_USER       ((LONG_PTR)DWLP_DLGPROC + (LONG_PTR)sizeof(void *))

/* Window styles. */
#define WS_OVERLAPPED       0x00000000
#define WS_POPUP            0x80000000
#define WS_CHILD            0x40000000
#define WS_MINIMIZE         0x20000000
#define WS_VISIBLE          0x10000000
#define WS_DISABLED         0x08000000
#define WS_CAPTION          0x00c00000
#define WS_SYSMENU          0x00080000
#define WS_OVERLAPPEDWINDOW 0x00cf0000
#define WS_VSCROLL          0x00200000
#define WS_HSCROLL          0x00100000

/* Scroll bars. */
#define SB_HORZ 0
#define SB_VERT 1
#define SB_CTL  2
#define SB_BOTH 3

#define SIF_RANGE           0x0001
#define SIF_PAGE            0x0002
#define SIF_POS             0x0004
#define SIF_DISABLENOSCROLL 0x0008
#define SIF_TRACKPOS        0x0010
#define SIF_ALL             (SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS)

#define ESB_ENABLE_BOTH  0x0000
#define ESB_DISABLE_LTUP 0x0001
#define ESB_DISABLE_RTDN 0x0002
#define ESB_DISABLE_BOTH 0x0003

typedef struct tagSCROLLINFO {
	UINT cbSize;
	UINT fMask;
	int nMin;
	int nMax;
	UINT nPage;
	int nPos;
	int nTrackPos;
} SCROLLINFO, *LPSCROLLINFO;
typedef const SCROLLINFO *LPCSCROLLINFO;

#define CW_USEDEFAULT ((int)0x80000000)

#define HWND_DESKTOP   ((HWND)0)
#define HWND_BROADCAST ((HWND)0xffff)
#define HWND_MESSAGE   ((HWND)-3)

/* ShowWindow. */
#define SW_HIDE           0
#define SW_SHOWNORMAL     1
#define SW_SHOWMINIMIZED  2
#define SW_SHOWMAXIMIZED  3
#define SW_SHOWNOACTIVATE 4
#define SW_MINIMIZE       6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA         8
#define SW_SHOW           5

/* SetWindowPos. */
#define SWP_NOSIZE       0x0001
#define SWP_NOMOVE       0x0002
#define SWP_NOZORDER     0x0004
#define SWP_NOACTIVATE   0x0010
#define SWP_SHOWWINDOW   0x0040
#define SWP_HIDEWINDOW   0x0080

/* Monitors. */
#define MONITOR_DEFAULTTONULL    0
#define MONITOR_DEFAULTTOPRIMARY 1
#define MONITOR_DEFAULTTONEAREST 2
#define MONITORINFOF_PRIMARY     1

/* SystemParametersInfo. */
#define SPI_GETWORKAREA 0x0030

/* LoadImage. */
#define IMAGE_BITMAP        0
#define IMAGE_ICON          1
#define IMAGE_CURSOR        2
#define LR_LOADFROMFILE     0x0010
#define LR_CREATEDIBSECTION 0x2000

/* PeekMessage. */
#define PM_NOREMOVE 0x0000
#define PM_REMOVE   0x0001
#define PM_NOYIELD  0x0002

/* Dialog buttons. */
#define BM_GETCHECK       0x00f0
#define BM_SETCHECK       0x00f1
#define BST_UNCHECKED     0
#define BST_CHECKED       1
#define BST_INDETERMINATE 2

/* GetSysColor. */
#define COLOR_SCROLLBAR               0
#define COLOR_BACKGROUND              1
#define COLOR_ACTIVECAPTION           2
#define COLOR_INACTIVECAPTION         3
#define COLOR_MENU                    4
#define COLOR_WINDOW                  5
#define COLOR_WINDOWFRAME             6
#define COLOR_MENUTEXT                7
#define COLOR_WINDOWTEXT              8
#define COLOR_CAPTIONTEXT             9
#define COLOR_ACTIVEBORDER            10
#define COLOR_INACTIVEBORDER          11
#define COLOR_APPWORKSPACE            12
#define COLOR_HIGHLIGHT               13
#define COLOR_HIGHLIGHTTEXT           14
#define COLOR_BTNFACE                 15
#define COLOR_BTNSHADOW               16
#define COLOR_GRAYTEXT                17
#define COLOR_BTNTEXT                 18
#define COLOR_INACTIVECAPTIONTEXT     19
#define COLOR_BTNHIGHLIGHT            20
#define COLOR_3DDKSHADOW              21
#define COLOR_3DLIGHT                 22
#define COLOR_INFOTEXT                23
#define COLOR_INFOBK                  24
#define COLOR_HOTLIGHT                26
#define COLOR_GRADIENTACTIVECAPTION   27
#define COLOR_GRADIENTINACTIVECAPTION 28
#define COLOR_MENUHILIGHT             29
#define COLOR_MENUBAR                 30

/* GetUserObjectInformation. */
#define UOI_FLAGS   1
#define UOI_NAME    2
#define UOI_TYPE    3
#define WSF_VISIBLE 0x0001

/* WM_WINDOWPOSCHANGING/CHANGED, and HDM_LAYOUT's answer. */
typedef struct tagWINDOWPOS {
	HWND hwnd;
	HWND hwndInsertAfter;
	int x;
	int y;
	int cx;
	int cy;
	UINT flags;
} WINDOWPOS, *LPWINDOWPOS, *PWINDOWPOS;

/* WM_NOTIFY's header, and _TrackMouseEvent's request. */
typedef struct tagNMHDR {
	HWND hwndFrom;
	UINT_PTR idFrom;
	UINT code;
} NMHDR, *LPNMHDR;

typedef struct tagTRACKMOUSEEVENT {
	DWORD cbSize;
	DWORD dwFlags;
	HWND hwndTrack;
	DWORD dwHoverTime;
} TRACKMOUSEEVENT, *LPTRACKMOUSEEVENT;

#define CS_GLOBALCLASS 0x4000
#define IDCLOSE        8

/* Dropped files. */
#define CF_HDROP           15
#define WM_DROPFILES       0x0233
#define WS_EX_ACCEPTFILES  0x00000010L

/* Menu items. */
#define MIIM_STATE      0x00000001
#define MIIM_ID         0x00000002
#define MIIM_SUBMENU    0x00000004
#define MIIM_TYPE       0x00000010
#define MIIM_STRING     0x00000040
#define MIIM_FTYPE      0x00000100
#define MFT_STRING      0x00000000
#define MFT_SEPARATOR   0x00000800
#define MFS_DEFAULT     0x00001000

typedef struct tagMENUITEMINFOW {
	UINT cbSize;
	UINT fMask;
	UINT fType;
	UINT fState;
	UINT wID;
	HMENU hSubMenu;
	HBITMAP hbmpChecked;
	HBITMAP hbmpUnchecked;
	ULONG_PTR dwItemData;
	LPWSTR dwTypeData;
	UINT cch;
	HBITMAP hbmpItem;
} MENUITEMINFOW, *LPMENUITEMINFOW;


/* Dialogs and the standard controls (u32_dialog.c). */
#define DLGWINDOWEXTRA           30
#define VK_END                   0x23
#define VK_UP                    0x26
#define VK_DOWN                  0x28
#define VK_F4                    0x73
#define VK_HOME                  0x24
#define VK_LEFT                  0x25
#define VK_RIGHT                 0x27
#define WS_TABSTOP               0x00010000L
#define WS_BORDER                0x00800000L
#define WS_THICKFRAME            0x00040000L
#define WS_GROUP                 0x00020000L
#define WS_EX_NOPARENTNOTIFY     0x00000004L
#define GW_HWNDFIRST             0
#define GW_HWNDLAST              1
#define GW_HWNDNEXT              2
#define GW_HWNDPREV              3
#define GW_OWNER                 4
#define GW_CHILD                 5
#define GW_ENABLEDPOPUP          6
#define MAKEINTRESOURCEW(i)      ((LPWSTR)(ULONG_PTR)(WORD)(i))
#define MAKEINTRESOURCEA(i)      ((LPSTR)(ULONG_PTR)(WORD)(i))
#define DS_ABSALIGN              0x01
#define DS_SYSMODAL              0x02
#define DS_3DLOOK                0x0004
#define DS_FIXEDSYS              0x0008
#define DS_NOFAILCREATE          0x0010
#define DS_LOCALEDIT             0x20
#define DS_SETFONT               0x40
#define DS_MODALFRAME            0x80
#define DS_NOIDLEMSG             0x100
#define DS_SETFOREGROUND         0x200
#define DS_CONTROL               0x0400
#define DS_CENTER                0x0800
#define DS_CENTERMOUSE           0x1000
#define DS_CONTEXTHELP           0x2000
#define DS_SHELLFONT             (DS_SETFONT | DS_FIXEDSYS)
#define DM_GETDEFID              (WM_USER+0)
#define DM_SETDEFID              (WM_USER+1)
#define DM_REPOSITION            (WM_USER+2)
#define DC_HASDEFID              0x534B
#define WM_NEXTDLGCTL            0x0028
#define WM_VKEYTOITEM            0x002E
#define WM_CHARTOITEM            0x002F
#define WM_QUERYDRAGICON         0x0037
#define WM_COMPAREITEM           0x0039
#define WM_GETDLGCODE            0x0087
#define WM_CTLCOLORMSGBOX        0x0132
#define WM_CTLCOLOREDIT          0x0133
#define WM_CTLCOLORLISTBOX       0x0134
#define WM_CTLCOLORBTN           0x0135
#define WM_CTLCOLORDLG           0x0136
#define WM_CTLCOLORSCROLLBAR     0x0137
#define WM_CTLCOLORSTATIC        0x0138
#define DLGC_WANTARROWS          0x0001
#define DLGC_WANTTAB             0x0002
#define DLGC_WANTALLKEYS         0x0004
#define DLGC_WANTMESSAGE         0x0004
#define DLGC_HASSETSEL           0x0008
#define DLGC_DEFPUSHBUTTON       0x0010
#define DLGC_UNDEFPUSHBUTTON     0x0020
#define DLGC_RADIOBUTTON         0x0040
#define DLGC_WANTCHARS           0x0080
#define DLGC_STATIC              0x0100
#define DLGC_BUTTON              0x2000
#define IDHELP                   9
#define BS_PUSHBUTTON            0x00000000L
#define BS_DEFPUSHBUTTON         0x00000001L
#define BS_CHECKBOX              0x00000002L
#define BS_AUTOCHECKBOX          0x00000003L
#define BS_RADIOBUTTON           0x00000004L
#define BS_3STATE                0x00000005L
#define BS_AUTO3STATE            0x00000006L
#define BS_GROUPBOX              0x00000007L
#define BS_USERBUTTON            0x00000008L
#define BS_AUTORADIOBUTTON       0x00000009L
#define BS_PUSHBOX               0x0000000AL
#define BS_OWNERDRAW             0x0000000BL
#define BS_TYPEMASK              0x0000000FL
#define BM_GETSTATE              0x00F2
#define BM_SETSTATE              0x00F3
#define BM_SETSTYLE              0x00F4
#define BM_CLICK                 0x00F5
#define BM_GETIMAGE              0x00F6
#define BM_SETIMAGE              0x00F7
#define BST_PUSHED               0x0004
#define BST_FOCUS                0x0008
#define BN_CLICKED               0
#define ES_LEFT                  0x0000L
#define ES_CENTER                0x0001L
#define ES_RIGHT                 0x0002L
#define ES_MULTILINE             0x0004L
#define ES_UPPERCASE             0x0008L
#define ES_LOWERCASE             0x0010L
#define ES_PASSWORD              0x0020L
#define ES_AUTOVSCROLL           0x0040L
#define ES_AUTOHSCROLL           0x0080L
#define ES_NOHIDESEL             0x0100L
#define ES_READONLY              0x0800L
#define ES_WANTRETURN            0x1000L
#define ES_NUMBER                0x2000L
#define EM_GETSEL                0x00B0
#define EM_SETSEL                0x00B1
#define EM_GETMODIFY             0x00B8
#define EM_SETMODIFY             0x00B9
#define EM_GETLINECOUNT          0x00BA
#define EM_LINELENGTH            0x00C1
#define EM_REPLACESEL            0x00C2
#define EM_LIMITTEXT             0x00C5
#define EM_SETLIMITTEXT          EM_LIMITTEXT
#define EM_CANUNDO               0x00C6
#define EM_UNDO                  0x00C7
#define EM_SETPASSWORDCHAR       0x00CC
#define EM_EMPTYUNDOBUFFER       0x00CD
#define EM_SETREADONLY           0x00CF
#define EM_GETPASSWORDCHAR       0x00D2
#define EM_GETLIMITTEXT          0x00D5
#define EN_SETFOCUS              0x0100
#define EN_KILLFOCUS             0x0200
#define EN_UPDATE                0x0400
#define EN_MAXTEXT               0x0501
#define LB_OKAY                  0
#define LB_ERR                   (-1)
#define LB_ERRSPACE              (-2)
#define LB_ADDSTRING             0x0180
#define LB_INSERTSTRING          0x0181
#define LB_DELETESTRING          0x0182
#define LB_RESETCONTENT          0x0184
#define LB_SETSEL                0x0185
#define LB_SETCURSEL             0x0186
#define LB_GETSEL                0x0187
#define LB_GETCURSEL             0x0188
#define LB_GETTEXT               0x0189
#define LB_GETTEXTLEN            0x018A
#define LB_GETCOUNT              0x018B
#define LB_SELECTSTRING          0x018C
#define LB_GETTOPINDEX           0x018E
#define LB_FINDSTRING            0x018F
#define LB_GETSELCOUNT           0x0190
#define LB_GETSELITEMS           0x0191
#define LB_SETTABSTOPS           0x0192
#define LB_SETHORIZONTALEXTENT   0x0194
#define LB_SETCOLUMNWIDTH        0x0195
#define LB_SETTOPINDEX           0x0197
#define LB_GETITEMDATA           0x0199
#define LB_SETITEMDATA           0x019A
#define LB_SELITEMRANGE          0x019B
#define LB_SETANCHORINDEX        0x019C
#define LB_GETANCHORINDEX        0x019D
#define LB_SETCARETINDEX         0x019E
#define LB_GETCARETINDEX         0x019F
#define LB_SETITEMHEIGHT         0x01A0
#define LB_GETITEMHEIGHT         0x01A1
#define LB_FINDSTRINGEXACT       0x01A2
#define LB_INITSTORAGE           0x01A8
#define LBS_NOTIFY               0x0001L
#define LBS_SORT                 0x0002L
#define LBS_NOREDRAW             0x0004L
#define LBS_MULTIPLESEL          0x0008L
#define LBS_OWNERDRAWFIXED       0x0010L
#define LBS_OWNERDRAWVARIABLE    0x0020L
#define LBS_HASSTRINGS           0x0040L
#define LBS_USETABSTOPS          0x0080L
#define LBS_NOINTEGRALHEIGHT     0x0100L
#define LBS_MULTICOLUMN          0x0200L
#define LBS_WANTKEYBOARDINPUT    0x0400L
#define LBS_EXTENDEDSEL          0x0800L
#define LBS_DISABLENOSCROLL      0x1000L
#define LBS_NODATA               0x2000L
#define LBS_NOSEL                0x4000L
#define LBN_ERRSPACE             (-2)
#define LBN_SELCHANGE            1
#define LBN_DBLCLK               2
#define LBN_SELCANCEL            3
#define LBN_SETFOCUS             4
#define LBN_KILLFOCUS            5
#define CB_OKAY                  0
#define CB_GETEDITSEL            0x0140
#define CB_LIMITTEXT             0x0141
#define CB_SETEDITSEL            0x0142
#define CB_SELECTSTRING          0x014D
#define CB_SETITEMHEIGHT         0x0153
#define CB_GETITEMHEIGHT         0x0154
#define CB_SETEXTENDEDUI         0x0155
#define CB_GETEXTENDEDUI         0x0156
#define CB_GETTOPINDEX           0x015B
#define CB_SETTOPINDEX           0x015C
#define CB_GETDROPPEDWIDTH       0x015F
#define CB_SETDROPPEDWIDTH       0x0160
#define CB_INITSTORAGE           0x0161
#define CBS_SIMPLE               0x0001L
#define CBS_DROPDOWN             0x0002L
#define CBS_DROPDOWNLIST         0x0003L
#define CBS_OWNERDRAWFIXED       0x0010L
#define CBS_OWNERDRAWVARIABLE    0x0020L
#define CBS_AUTOHSCROLL          0x0040L
#define CBS_SORT                 0x0100L
#define CBS_HASSTRINGS           0x0200L
#define CBN_ERRSPACE             (-1)
#define CBN_DBLCLK               2
#define CBN_SETFOCUS             3
#define CBN_KILLFOCUS            4
#define CBN_EDITCHANGE           5
#define CBN_EDITUPDATE           6
#define CBN_SELENDOK             9
#define CBN_SELENDCANCEL         10
#define STM_SETICON              0x0170
#define STM_GETICON              0x0171
#define STM_SETIMAGE             0x0172
#define STM_GETIMAGE             0x0173
#define SS_LEFT                  0x00000000L
#define SS_NOPREFIX              0x00000080L
#define SS_CENTER                0x00000001L
#define SS_RIGHT                 0x00000002L
#define SS_ICON                  0x00000003L
#define SS_BITMAP                0x0000000EL
#define SS_TYPEMASK              0x0000001FL
#define SS_NOTIFY                0x00000100L
#define STN_CLICKED              0

#pragma pack(push, 2)
typedef struct {
	DWORD style;
	DWORD dwExtendedStyle;
	WORD cdit;
	short x, y, cx, cy;
} DLGTEMPLATE, *LPDLGTEMPLATEA, *LPDLGTEMPLATEW;
typedef const DLGTEMPLATE *LPCDLGTEMPLATEA, *LPCDLGTEMPLATEW;

typedef struct {
	DWORD style;
	DWORD dwExtendedStyle;
	short x, y, cx, cy;
	WORD id;
} DLGITEMTEMPLATE;
#pragma pack(pop)

#endif /* VFW_WIN32_WINUSER_H */
