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

typedef struct tagMONITORINFO {
	DWORD cbSize;
	RECT rcMonitor;
	RECT rcWork;
	DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;

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
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED  0x0047
#define WM_NCCREATE          0x0081
#define WM_NCDESTROY         0x0082
#define WM_NCCALCSIZE        0x0083
#define WM_INITDIALOG        0x0110
#define WM_COMMAND           0x0111
#define WM_TIMER             0x0113
#define WM_USER              0x0400
#define WM_APP               0x8000

/* Get/SetWindowLong. */
#define GWL_WNDPROC    (-4)
#define GWL_HINSTANCE  (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_ID         (-12)
#define GWL_STYLE      (-16)
#define GWL_EXSTYLE    (-20)
#define GWL_USERDATA   (-21)
#define DWL_MSGRESULT  0
#define DWL_DLGPROC    4
#define DWL_USER       8

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

#endif /* VFW_WIN32_WINUSER_H */
