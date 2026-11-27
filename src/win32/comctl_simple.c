/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32's small controls, headless: hot key controls
 * (msctls_hotkey32), animation controls (SysAnimate32), IP address
 * controls (SysIPAddress32), pagers (SysPager) and the native font
 * control (NativeFontCtl), with Windows' messages and notifications.
 * Nothing is drawn and no timer runs.
 *
 * A hot key control takes the key pressed with the Shift, Ctrl and Alt
 * keys held (as WM_KEYDOWN/WM_KEYUP told it), replacing a combination
 * HKM_SETRULES forbids with its default modifiers, and tells its parent
 * with EN_CHANGE. An animation control checks that what ACM_OPEN names
 * (a file, or an "AVI" resource) is an AVI and reads its frame count;
 * ACM_PLAY sends ACN_START, and with a finite repeat count the clip is
 * over at once (ACN_STOP); an endless one plays until ACM_STOP. An IP
 * address control keeps four fields, each blank or within its range,
 * takes digits and dots (WM_CHAR) into the focused field, and sends
 * IPN_FIELDCHANGED when it leaves one and EN_CHANGE when one changes;
 * its window text is the dotted address. A pager asks its parent for its
 * child's size (PGN_CALCSIZE) and moves the child to show the scroll
 * position. The native font control only keeps its font.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"
#include "win32/path.h"

extern const struct builtin_dll builtin_kernel32;
#define KERNEL32(name) builtin_export(&builtin_kernel32, #name)

#define IS_INTRES(p)  (((UINT_PTR)(p) >> 16) == 0)

static UINT control_id(HWND h)
{
	return (UINT)((LONG (WINAPI *)(HWND, int))USER32(GetWindowLongW))(
		h, GWL_ID);
}

/* WM_COMMAND to the parent, with a notification code. */
static void command(HWND h, UINT code)
{
	HWND parent = comctl_parent(h);
	if (parent)
		comctl_send(parent, WM_COMMAND,
			    MAKEWPARAM(control_id(h), code), (LPARAM)h);
}

/* Hot key controls. */

struct hotkey {
	BYTE vk, mods;
	WORD invalid, fallback;
	BOOL shift, ctrl, alt;
	HFONT font;
};

/* The HKCOMB_ bit for a set of modifiers. */
static WORD combination(BYTE mods)
{
	static const WORD bits[8] = {
		HKCOMB_NONE, HKCOMB_S, HKCOMB_C, HKCOMB_SC,
		HKCOMB_A, HKCOMB_SA, HKCOMB_CA, HKCOMB_SCA,
	};
	return bits[mods & (HOTKEYF_SHIFT | HOTKEYF_CONTROL | HOTKEYF_ALT)];
}

static void hotkey_set(HWND h, struct hotkey *s, BYTE vk, BYTE mods)
{
	if (vk == s->vk && mods == s->mods)
		return;
	s->vk = vk;
	s->mods = mods;
	command(h, EN_CHANGE);
}

static void hotkey_key(HWND h, struct hotkey *s, WPARAM vk, LPARAM lp,
		       BOOL down)
{
	switch (vk) {
	case VK_SHIFT:
		s->shift = down;
		return;
	case VK_CONTROL:
		s->ctrl = down;
		return;
	case VK_MENU:
		s->alt = down;
		return;
	}
	if (!down)
		return;
	if ((vk == VK_BACK || vk == VK_DELETE || vk == VK_ESCAPE) &&
	    !s->shift && !s->ctrl && !s->alt) {
		hotkey_set(h, s, 0, 0);
		return;
	}
	BYTE mods = (BYTE)((s->shift ? HOTKEYF_SHIFT : 0) |
			   (s->ctrl ? HOTKEYF_CONTROL : 0) |
			   (s->alt ? HOTKEYF_ALT : 0));
	if (vk && (combination(mods) & s->invalid))
		mods = (BYTE)s->fallback;
	/* An extended key (bit 24 of the key message) keeps HOTKEYF_EXT. */
	if (lp & (1 << 24))
		mods |= HOTKEYF_EXT;
	else
		mods &= (BYTE)~HOTKEYF_EXT;
	hotkey_set(h, s, (BYTE)vk, mods);
}

LRESULT STUB_WINAPI comctl_hotkey_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct hotkey *s = comctl_state(h);

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case HKM_SETHOTKEY:
		s->vk = LOBYTE(wp);
		s->mods = HIBYTE(LOWORD(wp));
		return 0;
	case HKM_GETHOTKEY:
		return MAKEWORD(s->vk, s->mods);
	case HKM_SETRULES:
		s->invalid = (WORD)wp;
		s->fallback = LOWORD(lp);
		return 0;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		hotkey_key(h, s, wp, lp, TRUE);
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		hotkey_key(h, s, wp, lp, FALSE);
		return 0;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		/* Key-ups that went to another window are lost: start over. */
		s->shift = s->ctrl = s->alt = FALSE;
		break;
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}

/* Animation controls. */

struct animate {
	BOOL open, playing;
	DWORD frames;
};

static DWORD rd32(const BYTE *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24;
}

/* An AVI's frame count from its first bytes, or 0 when it isn't one. */
static DWORD avi_frames(const BYTE *p, size_t n)
{
	if (n < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "AVI ", 4))
		return 0;
	for (size_t i = 12; i + 8 + 20 <= n; i++)
		if (!memcmp(p + i, "avih", 4))
			return rd32(p + i + 8 + 16) ? rd32(p + i + 8 + 16) : 1;
	return 1;		/* no header seen: an AVI all the same */
}

static DWORD open_file(const WCHAR *name)
{
	char *a = comctl_w_to_a(name), *unix_path = a ? path_to_unix(a) : NULL;
	BYTE buf[1024];
	size_t n = 0;
	FILE *f = unix_path ? fopen(unix_path, "rb") : NULL;

	if (f) {
		n = fread(buf, 1, sizeof(buf), f);
		fclose(f);
	}
	free(unix_path);
	free(a);
	return f ? avi_frames(buf, n) : 0;
}

static DWORD open_resource(HINSTANCE inst, const WCHAR *name)
{
	HRSRC r = ((HRSRC (WINAPI *)(HMODULE, LPCWSTR, LPCWSTR))
		   KERNEL32(FindResourceW))(inst, name, u"AVI");
	if (!r)
		return 0;
	HGLOBAL g = ((HGLOBAL (WINAPI *)(HMODULE, HRSRC))
		     KERNEL32(LoadResource))(inst, r);
	DWORD size = ((DWORD (WINAPI *)(HMODULE, HRSRC))
		      KERNEL32(SizeofResource))(inst, r);
	const BYTE *p = g ? ((LPVOID (WINAPI *)(HGLOBAL))
			     KERNEL32(LockResource))(g) : NULL;
	return p ? avi_frames(p, size) : 0;
}

/* The parent's ACN_ handlers run synchronously and may destroy the
 * control: s is not used after one unless comctl_alive says so. */
static void animate_stop(HWND h, struct animate *s)
{
	if (!s->playing)
		return;
	s->playing = FALSE;
	command(h, ACN_STOP);
}

static BOOL animate_play(HWND h, struct animate *s, int repeat)
{
	if (!s->open)
		return FALSE;
	animate_stop(h, s);
	/* The ACN_STOP handler may have destroyed or closed the clip. */
	if (!comctl_alive(h, s) || !s->open)
		return FALSE;
	s->playing = TRUE;
	command(h, ACN_START);
	if (repeat != -1 && comctl_alive(h, s))
		animate_stop(h, s);
	return TRUE;
}

LRESULT STUB_WINAPI comctl_animate_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct animate *s = comctl_state(h);

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case ACM_OPENA:
	case ACM_OPENW: {
		animate_stop(h, s);
		if (!comctl_alive(h, s))
			return FALSE;
		s->open = FALSE;
		s->frames = 0;
		if (!lp)
			return TRUE;	/* closing */
		WCHAR *name = NULL;
		const WCHAR *res = (const WCHAR *)lp;
		if (!IS_INTRES(lp)) {
			name = msg == ACM_OPENW ?
			       comctl_w_dup((const WCHAR *)lp) :
			       comctl_a_to_w((const char *)lp);
			res = name;
		}
		/* A NULL module is the one that created the control. */
		HINSTANCE inst = (HINSTANCE)wp;
		if (!inst)
			inst = comctl_instance(h);
		DWORD frames = 0;
		if (inst || IS_INTRES(lp))
			frames = open_resource(inst, res);
		if (!frames && name)
			frames = open_file(name);
		free(name);
		if (!frames)
			return FALSE;
		s->open = TRUE;
		s->frames = frames;
		if (comctl_style(h) & ACS_AUTOPLAY)
			animate_play(h, s, -1);
		return TRUE;
	}
	case ACM_PLAY:
		return animate_play(h, s, (int)wp);
	case ACM_STOP:
		animate_stop(h, s);
		return TRUE;
	case ACM_ISPLAYING:
		return s->playing;
	case WM_NCDESTROY:
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}

/* IP address controls. */

struct ipaddr {
	int field[4];		/* -1: blank */
	int digits[4];		/* typed into each field */
	BOOL selected[4];	/* the field's text is selected: a digit
				 * replaces it */
	BYTE lo[4], hi[4];
	int focus;
	BOOL leaving;		/* IPN_FIELDCHANGED for the focus is out */
	int want;		/* the field to focus once it returns */
	HFONT font;
};

/* The field is left: IPN_FIELDCHANGED, whose handler may change the value
 * (which is then kept to the field's range, blank staying blank) or
 * destroy the control (FALSE). */
static BOOL field_changed(HWND h, struct ipaddr *s, int i)
{
	NMIPADDRESS nm;
	memset(&nm, 0, sizeof(nm));
	nm.iField = i;
	nm.iValue = s->field[i];
	comctl_notify(h, IPN_FIELDCHANGED, &nm.hdr);
	if (!comctl_alive(h, s))
		return FALSE;
	if (nm.iValue < 0)
		s->field[i] = -1;
	else
		s->field[i] = nm.iValue < s->lo[i] ? s->lo[i] :
			      nm.iValue > s->hi[i] ? s->hi[i] : nm.iValue;
	return TRUE;
}

/* Focus field i with its text selected, as Windows does. */
static void ip_focus(struct ipaddr *s, int i)
{
	s->focus = i;
	s->digits[i] = 0;
	s->selected[i] = TRUE;
}

/* Move the focus to field i, telling the parent the old field was left.
 * A focus change asked for from that IPN_FIELDCHANGED handler (an
 * IPM_SETFOCUS, or typing) is recorded instead of leaving the field
 * again, and the newest such request is where the focus goes. FALSE:
 * the handler destroyed the control. */
static BOOL ip_move(HWND h, struct ipaddr *s, int i)
{
	if (s->leaving) {
		s->want = i;
		return TRUE;
	}
	if (i != s->focus) {
		s->leaving = TRUE;
		s->want = i;
		if (!field_changed(h, s, s->focus))
			return FALSE;
		s->leaving = FALSE;
		i = s->want;
	}
	ip_focus(s, i);
	return TRUE;
}

static void ip_char(HWND h, struct ipaddr *s, WCHAR c)
{
	int i = s->focus;

	if (c >= '0' && c <= '9') {
		if (s->selected[i]) {	/* the first digit replaces it */
			s->field[i] = -1;
			s->digits[i] = 0;
			s->selected[i] = FALSE;
		}
		int v = (s->field[i] < 0 ? 0 : s->field[i]) * 10 + (c - '0');
		if (v > s->hi[i])
			v = s->hi[i];
		s->field[i] = v;
		s->digits[i]++;
		command(h, EN_CHANGE);
		if (!comctl_alive(h, s))
			return;
		/* A full field moves on to the next. */
		if (s->digits[i] >= 3 && i < 3)
			ip_move(h, s, i + 1);
		return;
	}
	if (c == '.' && i < 3 && s->digits[i])
		ip_move(h, s, i + 1);
}

static void ip_reset_typing(struct ipaddr *s)
{
	for (int i = 0; i < 4; i++)
		s->digits[i] = 0, s->selected[i] = FALSE;
}

static int ip_text(const struct ipaddr *s, WCHAR *out, int size)
{
	char a[20];
	int n = 0;

	for (int i = 0; i < 4; i++) {
		if (i)
			a[n++] = '.';
		if (s->field[i] >= 0)
			n += snprintf(a + n, sizeof(a) - (size_t)n, "%d",
				      s->field[i]);
	}
	a[n] = 0;
	if (out && size > 0) {
		int k = 0;
		for (; k < n && k < size - 1; k++)
			out[k] = (WCHAR)a[k];
		out[k] = 0;
		return k;
	}
	return n;
}

static void ip_parse(struct ipaddr *s, const WCHAR *t)
{
	for (int i = 0; i < 4; i++)
		s->field[i] = -1;
	for (int i = 0; t && *t && i < 4; i++) {
		int v = -1;
		while (*t >= '0' && *t <= '9') {
			v = (v < 0 ? 0 : v) * 10 + (*t++ - '0');
			if (v > 255)
				v = 255;
		}
		if (v >= 0)
			s->field[i] = v < s->lo[i] ? s->lo[i] :
				      v > s->hi[i] ? s->hi[i] : v;
		while (*t && *t != '.')
			t++;
		if (*t == '.')
			t++;
	}
}

LRESULT STUB_WINAPI comctl_ipaddr_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct ipaddr *s = comctl_state(h);

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		for (int i = 0; i < 4; i++) {
			s->field[i] = -1;
			s->hi[i] = 255;
		}
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case IPM_CLEARADDRESS:
		for (int i = 0; i < 4; i++)
			s->field[i] = -1, s->digits[i] = 0;
		return 0;
	case IPM_SETADDRESS:
		ip_reset_typing(s);
		for (int i = 0; i < 4; i++)
			s->field[i] = (int)(((DWORD)lp >> (24 - 8 * i)) & 0xff);
		return TRUE;
	case IPM_GETADDRESS: {
		DWORD v = 0;
		int n = 0;
		for (int i = 0; i < 4; i++) {
			if (s->field[i] >= 0) {
				v |= (DWORD)s->field[i] << (24 - 8 * i);
				n++;
			}
		}
		if (lp)
			*(DWORD *)lp = v;
		return n;
	}
	case IPM_SETRANGE: {
		int i = (int)wp;
		BYTE lo = LOBYTE(lp), hi = HIBYTE(LOWORD(lp));
		if (i < 0 || i > 3 || lo > hi)
			return FALSE;
		s->lo[i] = lo;
		s->hi[i] = hi;
		if (s->field[i] >= 0 && s->field[i] < lo)
			s->field[i] = lo;
		if (s->field[i] > hi)
			s->field[i] = hi;
		return TRUE;
	}
	case IPM_SETFOCUS: {
		int i = (int)wp;
		if (i < 0 || i > 3) {	/* the first blank field, else 0 */
			for (i = 0; i < 4 && s->field[i] >= 0; i++)
				;
			if (i == 4)
				i = 0;
		}
		ip_move(h, s, i);
		return 0;
	}
	case IPM_ISBLANK:
		for (int i = 0; i < 4; i++)
			if (s->field[i] >= 0)
				return FALSE;
		return TRUE;
	case WM_CHAR:
		ip_char(h, s, (WCHAR)wp);
		return 0;
	case WM_SETTEXT:
		ip_reset_typing(s);
		ip_parse(s, (const WCHAR *)lp);
		return TRUE;
	case WM_GETTEXT:
		return ip_text(s, (WCHAR *)lp, (int)wp);
	case WM_GETTEXTLENGTH:
		return ip_text(s, NULL, 0);
	case WM_SETFONT:
		s->font = (HFONT)wp;
		return 0;
	case WM_GETFONT:
		return (LRESULT)s->font;
	case WM_NCDESTROY:
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}

/* Pagers. */

struct pager {
	HWND child;
	int pos, extent, border, button;
	COLORREF bk;
	BOOL forward;
	unsigned calc_gen;	/* bumped by each PGN_CALCSIZE round */
};

static int pager_room(HWND h, const struct pager *s)
{
	RECT rc;
	comctl_client_rect(h, &rc);
	int room = (comctl_style(h) & PGS_HORZ) ? rc.right - rc.left :
						 rc.bottom - rc.top;
	return room - 2 * s->border;
}

static int pager_max(HWND h, const struct pager *s)
{
	int m = s->extent - pager_room(h, s);
	return m > 0 ? m : 0;
}

static void pager_place(HWND h, struct pager *s)
{
	RECT rc;
	if (!s->child)
		return;
	if (s->pos > pager_max(h, s))
		s->pos = pager_max(h, s);
	if (s->pos < 0)
		s->pos = 0;
	comctl_client_rect(h, &rc);
	if (comctl_style(h) & PGS_HORZ)
		comctl_move(s->child, s->border - s->pos, 0, s->extent,
			    rc.bottom - rc.top);
	else
		comctl_move(s->child, 0, s->border - s->pos,
			    rc.right - rc.left, s->extent);
}

static void pager_recalc(HWND h, struct pager *s)
{
	RECT rc;
	NMPGCALCSIZE nm;
	BOOL horz = (comctl_style(h) & PGS_HORZ) != 0;

	comctl_client_rect(h, &rc);
	memset(&nm, 0, sizeof(nm));
	nm.dwFlag = horz ? PGF_CALCWIDTH : PGF_CALCHEIGHT;
	nm.iWidth = rc.right - rc.left;
	nm.iHeight = rc.bottom - rc.top;
	if (s->child) {
		RECT cr;
		comctl_client_rect(s->child, &cr);
		nm.iWidth = cr.right - cr.left;
		nm.iHeight = cr.bottom - cr.top;
	}
	/* A PGM_SETCHILD or PGM_RECALCSIZE from the handler starts a newer
	 * round, whose result stands; this one's is then stale. */
	unsigned gen = ++s->calc_gen;
	comctl_notify(h, PGN_CALCSIZE, &nm.hdr);
	if (!comctl_alive(h, s) || gen != s->calc_gen)
		return;
	s->extent = horz ? nm.iWidth : nm.iHeight;
	pager_place(h, s);
}

LRESULT STUB_WINAPI comctl_pager_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	struct pager *s = comctl_state(h);

	if (msg == WM_NCCREATE) {
		if (!(s = calloc(1, sizeof(*s))))
			return FALSE;
		s->button = 12;
		s->bk = ((DWORD (WINAPI *)(int))USER32(GetSysColor))(
			COLOR_BTNFACE);
		comctl_set_state(h, s);
	}
	if (!s)
		return comctl_def(h, msg, wp, lp);
	switch (msg) {
	case PGM_SETCHILD:
		s->child = (HWND)lp;
		s->pos = 0;
		pager_recalc(h, s);
		return 0;
	case PGM_RECALCSIZE:
		pager_recalc(h, s);
		return 0;
	case PGM_FORWARDMOUSE:
		s->forward = (BOOL)wp;
		return 0;
	case PGM_SETBKCOLOR: {
		COLORREF old = s->bk;
		s->bk = (COLORREF)lp;
		return old;
	}
	case PGM_GETBKCOLOR:
		return s->bk;
	case PGM_SETBORDER: {
		int old = s->border;
		s->border = (int)lp > 0 ? (int)lp : 0;
		pager_place(h, s);
		return old;
	}
	case PGM_GETBORDER:
		return s->border;
	case PGM_SETPOS:
		s->pos = (int)lp;
		pager_place(h, s);
		return 0;
	case PGM_GETPOS:
		return s->pos;
	case PGM_SETBUTTONSIZE: {
		int old = s->button;
		s->button = (int)lp > 0 ? (int)lp : s->button;
		return old;
	}
	case PGM_GETBUTTONSIZE:
		return s->button;
	case PGM_GETBUTTONSTATE:	/* the button is in lParam */
		if (lp == PGB_TOPORLEFT)
			return s->pos > 0 ? PGF_NORMAL : PGF_INVISIBLE;
		if (lp == PGB_BOTTOMORRIGHT)
			return s->pos < pager_max(h, s) ? PGF_NORMAL :
							  PGF_INVISIBLE;
		return PGF_INVISIBLE;
	case WM_SIZE:
		pager_place(h, s);
		break;
	case WM_MOUSEMOVE:
		/* PGM_FORWARDMOUSE: to the child, in its coordinates. */
		if (s->forward && s->child) {
			BOOL horz = (comctl_style(h) & PGS_HORZ) != 0;
			int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
			int off = s->border - s->pos;
			if (horz)
				x -= off;
			else
				y -= off;
			comctl_send(s->child, WM_MOUSEMOVE, wp,
				    MAKELPARAM(x, y));
			return 0;
		}
		break;
	case WM_NCDESTROY:
		free(s);
		comctl_set_state(h, NULL);
		break;
	}
	return comctl_def(h, msg, wp, lp);
}

/* The native font control. */

LRESULT STUB_WINAPI comctl_nativefont_proc(HWND h, UINT msg, WPARAM wp,
				    LPARAM lp)
{
	switch (msg) {
	case WM_SETFONT:
		comctl_set_state(h, (void *)wp);
		return 0;
	case WM_GETFONT:
		return (LRESULT)comctl_state(h);
	}
	return comctl_def(h, msg, wp, lp);
}
