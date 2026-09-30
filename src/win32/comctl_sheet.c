/*
 * comctl32: property sheets and task dialogs, headless.
 *
 * - A property sheet page is a copy of the caller's PROPSHEETPAGE (all
 *   dwSize bytes of it, as codecs keep their own data after it), with its
 *   PSPCB_ADDREF and PSPCB_RELEASE callbacks and pcRefParent count kept
 *   as on Windows. PropertySheet can't show the pages' dialogs: it says
 *   so on stderr, destroys the pages as a closed sheet does and answers
 *   0, as if Cancel were pressed (as OleCreatePropertyFrame does). No
 *   page dialog is created, so PSPCB_CREATE and the sheet's callback
 *   never come.
 * - TaskDialog and TaskDialogIndirect print their text and answer with
 *   the default button, as MessageBox does; the default radio button and
 *   verification box are reported as they start. The callback isn't
 *   called: there is no window to send it messages about.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32/codepage.h"
#include "win32/comctl.h"
#include "win32/internal.h"

#define PSP_MAGIC 0x50535048	/* "HPSP" */

struct _PSP {
	DWORD magic;
	int wide;
	/* The copy: at least a whole PROPSHEETPAGEW, zeroed past dwSize. */
	union {
		PROPSHEETPAGEA a;
		PROPSHEETPAGEW w;
		BYTE bytes[sizeof(PROPSHEETPAGEW)];
	} page;
};

static HPROPSHEETPAGE create_page(const void *src, int wide)
{
	const PROPSHEETPAGEW *p = src;

	if (!p || p->dwSize < VFW_PROPSHEETPAGE_V1_SIZE)
		return NULL;
	size_t size = p->dwSize > sizeof(PROPSHEETPAGEW) ? p->dwSize :
		      sizeof(PROPSHEETPAGEW);
	HPROPSHEETPAGE h = comctl_alloc(offsetof(struct _PSP, page) + size);
	if (!h)
		return NULL;
	h->magic = PSP_MAGIC;
	h->wide = wide;
	memcpy(&h->page, p, p->dwSize);

	PROPSHEETPAGEW *c = &h->page.w;
	if ((c->dwFlags & PSP_USEREFPARENT) && c->pcRefParent)
		(*c->pcRefParent)++;
	if ((c->dwFlags & PSP_USECALLBACK) && c->pfnCallback)
		c->pfnCallback(NULL, PSPCB_ADDREF, c);
	return h;
}

WINFN(HPROPSHEETPAGE) ps_CreatePropertySheetPageA(LPCPROPSHEETPAGEA p)
{
	return create_page(p, 0);
}

WINFN(HPROPSHEETPAGE) ps_CreatePropertySheetPageW(LPCPROPSHEETPAGEW p)
{
	return create_page(p, 1);
}

WINFN(BOOL) ps_DestroyPropertySheetPage(HPROPSHEETPAGE h)
{
	if (!h || h->magic != PSP_MAGIC)
		return FALSE;
	PROPSHEETPAGEW *c = &h->page.w;
	if ((c->dwFlags & PSP_USECALLBACK) && c->pfnCallback)
		c->pfnCallback(NULL, PSPCB_RELEASE, c);
	if ((c->dwFlags & PSP_USEREFPARENT) && c->pcRefParent)
		(*c->pcRefParent)--;
	h->magic = 0;
	comctl_free(h);
	return TRUE;
}

/* A string argument that may be a resource id, for printing. */
static char *res_text(const void *s, int wide)
{
	if (!s)
		return strdup("");
	if (((ULONG_PTR)s >> 16) == 0) {
		char buf[16];
		snprintf(buf, sizeof(buf), "#%u", (unsigned)(ULONG_PTR)s);
		return strdup(buf);
	}
	return wide ? utf16_to_utf8_dup(s) : acp_to_utf8(s);
}

static INT_PTR property_sheet(const PROPSHEETHEADERW *h, int wide)
{
	if (!h || h->dwSize < VFW_PROPSHEETHEADER_V1_SIZE)
		return -1;

	/* The pages, created from the array or given as handles. */
	UINT n = h->nPages;
	HPROPSHEETPAGE *pages = calloc(n ? n : 1, sizeof(*pages));
	if (!pages)
		return -1;
	if (h->dwFlags & PSH_PROPSHEETPAGE) {
		const BYTE *p = (const BYTE *)h->ppsp;
		for (UINT i = 0; p && i < n; i++) {
			DWORD size = ((const PROPSHEETPAGEW *)p)->dwSize;
			pages[i] = create_page(p, wide);
			if (size < VFW_PROPSHEETPAGE_V1_SIZE)
				break;
			p += size;
		}
	} else if (h->phpage) {
		memcpy(pages, h->phpage, n * sizeof(*pages));
	}

	char *caption = res_text(h->pszCaption, wide);
	fprintf(stderr, "PropertySheet [%s]: %u page%s\n  (no GUI: %s)\n",
		caption, n, n == 1 ? "" : "s",
		h->dwFlags & PSH_MODELESS ? "not shown" :
		"closed, as with Cancel");
	free(caption);

	for (UINT i = 0; i < n; i++)
		if (pages[i])
			ps_DestroyPropertySheetPage(pages[i]);
	free(pages);
	return 0;
}

WINFN(INT_PTR) ps_PropertySheetA(LPCPROPSHEETHEADERA h)
{
	return property_sheet((const PROPSHEETHEADERW *)h, 0);
}

WINFN(INT_PTR) ps_PropertySheetW(LPCPROPSHEETHEADERW h)
{
	return property_sheet(h, 1);
}

/* Task dialogs. */

static const struct {
	int flag, id;
	const char *name;
} common[] = {
	{ TDCBF_OK_BUTTON, IDOK, "OK" },
	{ TDCBF_YES_BUTTON, IDYES, "Yes" },
	{ TDCBF_NO_BUTTON, IDNO, "No" },
	{ TDCBF_RETRY_BUTTON, IDRETRY, "Retry" },
	{ TDCBF_CANCEL_BUTTON, IDCANCEL, "Cancel" },
	{ TDCBF_CLOSE_BUTTON, IDCLOSE, "Close" },
};

/* The buttons in the order shown: custom ones, then the common ones. */
static int button_at(const TASKDIALOGCONFIG *c, int k, char **name)
{
	int n = 0;

	for (UINT i = 0; c->pButtons && i < c->cButtons; i++, n++)
		if (n == k) {
			*name = res_text(c->pButtons[i].pszButtonText, 1);
			return c->pButtons[i].nButtonID;
		}
	int flags = c->dwCommonButtons;
	if (!flags && !(c->pButtons && c->cButtons))
		flags = TDCBF_OK_BUTTON;
	for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++)
		if ((flags & common[i].flag) && n++ == k) {
			*name = strdup(common[i].name);
			return common[i].id;
		}
	return 0;
}

WINFN(HRESULT) ps_TaskDialogIndirect(const TASKDIALOGCONFIG *c, int *button,
				     int *radio, BOOL *verified)
{
	if (!c || c->cbSize != sizeof(*c))
		return E_INVALIDARG;

	/* The default button, or the first. */
	char *name = NULL;
	int answer = button_at(c, 0, &name);
	for (int k = 0;; k++) {
		char *n = NULL;
		int id = button_at(c, k, &n);
		if (!id) {
			free(n);
			break;
		}
		if (id == c->nDefaultButton) {
			free(name);
			name = n;
			answer = id;
			break;
		}
		free(n);
	}

	int chosen = 0;
	if (c->pRadioButtons && c->cRadioButtons &&
	    !(c->dwFlags & TDF_NO_DEFAULT_RADIO_BUTTON)) {
		chosen = c->pRadioButtons[0].nButtonID;
		for (UINT i = 0; i < c->cRadioButtons; i++)
			if (c->pRadioButtons[i].nButtonID == c->nDefaultRadioButton)
				chosen = c->nDefaultRadioButton;
	}

	char *title = res_text(c->pszWindowTitle, 1);
	char *main = res_text(c->pszMainInstruction, 1);
	char *content = res_text(c->pszContent, 1);
	fprintf(stderr, "TaskDialog [%s]: %s%s%s\n  (no GUI: answering %s)\n",
		title, main, *main && *content ? "\n  " : "", content,
		name ? name : "?");
	free(title);
	free(main);
	free(content);
	free(name);

	if (button)
		*button = answer;
	if (radio)
		*radio = chosen;
	if (verified)
		*verified = !!(c->dwFlags & TDF_VERIFICATION_FLAG_CHECKED);
	return S_OK;
}

WINFN(HRESULT) ps_TaskDialog(HWND owner, HINSTANCE inst, LPCWSTR title,
			     LPCWSTR main, LPCWSTR content, int buttons,
			     LPCWSTR icon, int *button)
{
	TASKDIALOGCONFIG c;

	memset(&c, 0, sizeof(c));
	c.cbSize = sizeof(c);
	c.hwndParent = owner;
	c.hInstance = inst;
	c.dwCommonButtons = buttons;
	c.pszWindowTitle = title;
	c.pszMainIcon = icon;
	c.pszMainInstruction = main;
	c.pszContent = content;
	return ps_TaskDialogIndirect(&c, button, NULL, NULL);
}

#define E(name, ord) { #name, ord, (void *)ps_##name }

const struct builtin_export comctl_sheet_exports[] = {
	{ "CreatePropertySheetPage", 0, (void *)ps_CreatePropertySheetPageA },
	E(CreatePropertySheetPageA, 0),
	E(CreatePropertySheetPageW, 0),
	E(DestroyPropertySheetPage, 0),
	{ "PropertySheet", 0, (void *)ps_PropertySheetA },
	E(PropertySheetA, 0),
	E(PropertySheetW, 0),
	E(TaskDialog, 344),
	E(TaskDialogIndirect, 345),
	BUILTIN_END
};
