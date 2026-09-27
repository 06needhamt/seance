/*
 * user32 internals shared between its source files.
 */
#ifndef VFW_WIN32_USER_H
#define VFW_WIN32_USER_H

#include "win32.h"

/* The pretend screen (the same as gdi.h's). */
#define USER_SCREEN_WIDTH  1024
#define USER_SCREEN_HEIGHT 768

/* Whether hwnd is a live window (the desktop included). */
int user_is_window(HWND hwnd);

/* The desktop window. */
HWND user_desktop(void);

/* A window's client rectangle ({0, 0, width, height}); 0 if no window. */
int user_client_rect(HWND hwnd, RECT *rc);

/* Milliseconds since boot (GetTickCount). */
DWORD user_ticks(void);

#endif /* VFW_WIN32_USER_H */
