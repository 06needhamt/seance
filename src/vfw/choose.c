/*
 * ICCompressorChoose without its dialog. The harness has no GUI, so, as
 * MessageBox answers with its default button, this answers as if OK were
 * pressed on the dialog's initial selection: the compressor pc already
 * names, if the list would show it, else the first entry, "Full Frames
 * (Uncompressed)". $VFW_COMPRESSOR chooses instead: a FOURCC ("TST0",
 * "vidc.TST0"), "uncompressed", or "cancel".
 *
 * The list, and what is filled in, follow Wine's dialog: every
 * compressor of the type, only those accepting the input format pvIn
 * unless ICMF_CHOOSE_ALLCOMPRESSORS is set; the chosen one opened in
 * pc->hic (NULL for Full Frames), with the state from an earlier choice
 * of the same compressor restored, and dropped for any other. Where the
 * dialog's quality slider and keyframe box would start from the codec's
 * defaults, so do lQ and lKey when pc left them unset.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/error.h"
#include "vfw/icm.h"
#include "vfw/registry.h"

static int same_fourcc(FOURCC a, FOURCC b)
{
	for (int i = 0; i < 4; i++, a >>= 8, b >>= 8)
		if (toupper(a & 0xff) != toupper(b & 0xff))
			return 0;
	return 1;
}

static const char *fourcc_str(FOURCC f, char buf[5])
{
	for (int i = 0; i < 4; i++, f >>= 8)
		buf[i] = (char)(f & 0xff);
	buf[4] = '\0';
	return buf;
}

/*
 * The compressor for *handler if the dialog would list it, opened, with
 * *handler set to its own spelling (the list shows each codec's ICInfo);
 * NULL with vfw_error() if not.
 */
static HIC open_listed(DWORD type, FOURCC *handler, UINT flags, void *in)
{
	ICINFO info;
	char buf[5];

	if (!ic_info(type, *handler, &info)) {
		vfw_set_error("no compressor '%s'", fourcc_str(*handler, buf));
		return NULL;
	}
	*handler = info.fccHandler;
	HIC hic = ic_open(type, *handler, ICMODE_COMPRESS);
	if (!hic) {
		vfw_set_error("'%s' doesn't open to compress",
			      fourcc_str(*handler, buf));
		return NULL;
	}
	if (in && !(flags & ICMF_CHOOSE_ALLCOMPRESSORS) &&
	    ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)in, 0) != ICERR_OK) {
		ic_close(hic);
		vfw_set_error("'%s' doesn't take the input format",
			      fourcc_str(*handler, buf));
		return NULL;
	}
	return hic;
}

BOOL STUB_WINAPI ic_compressor_choose(HWND hwnd, UINT flags, LPVOID in,
				      LPVOID data, COMPVARS *pc, LPSTR title)
{
	const char *want = getenv("VFW_COMPRESSOR");
	char buf[5];
	HIC hic = NULL;
	FOURCC handler = comptypeDIB;

	if (!pc || pc->cbSize != (LONG)sizeof(COMPVARS))
		return FALSE;

	COMPVARS cv = *pc;
	if (!(cv.dwFlags & ICMF_COMPVARS_VALID)) {
		cv.dwFlags = 0;
		cv.fccType = cv.fccHandler = 0;
		cv.hic = NULL;
		cv.lpbiIn = cv.lpbiOut = NULL;
		cv.lpBitsOut = cv.lpBitsPrev = cv.lpState = NULL;
		cv.cbState = 0;
		cv.lQ = ICQUALITY_DEFAULT;
		cv.lKey = -1;
		cv.lDataRate = 300;	/* KB/s, the dialog's default */
	}
	if (!cv.fccType)
		cv.fccType = ICTYPE_VIDEO;

	if (want && *want) {
		FOURCC type;
		if (!strcasecmp(want, "cancel")) {
			fprintf(stderr, "ICCompressorChoose [%s]: (no GUI: "
				"cancelled, as VFW_COMPRESSOR says)\n",
				title ? title : "");
			return FALSE;
		}
		if (strcasecmp(want, "uncompressed") &&
		    (vfw_parse_fourcc(want, &type, &handler) ||
		     !(hic = open_listed(cv.fccType, &handler, flags, in)))) {
			fprintf(stderr, "ICCompressorChoose [%s]: VFW_COMPRESSOR="
				"%s: %s\n", title ? title : "", want,
				vfw_parse_fourcc(want, &type, &handler) ?
				"not a FOURCC, \"uncompressed\" or \"cancel\"" :
				vfw_error());
			return FALSE;
		}
	} else if (cv.fccHandler && !same_fourcc(cv.fccHandler, comptypeDIB)) {
		/* Not listed: the selection falls back to Full Frames. */
		FOURCC f = cv.fccHandler;
		if ((hic = open_listed(cv.fccType, &f, flags, in)))
			handler = f;
	}
	if (!hic)
		handler = comptypeDIB;

	if (hic) {
		/* The state of an earlier choice of the same compressor. */
		if ((pc->dwFlags & ICMF_COMPVARS_VALID) && cv.lpState &&
		    cv.cbState > 0 && same_fourcc(cv.fccHandler, handler))
			ic_send(hic, ICM_SETSTATE, (DWORD_PTR)cv.lpState,
				(DWORD_PTR)cv.cbState);
		DWORD v = 0;
		if (cv.lQ == ICQUALITY_DEFAULT &&
		    ic_send(hic, ICM_GETDEFAULTQUALITY, (DWORD_PTR)&v,
			    sizeof(v)) ==
		    ICERR_OK)
			cv.lQ = (LONG)v;
		v = 0;
		if (cv.lKey < 0)
			cv.lKey = ic_send(hic, ICM_GETDEFAULTKEYFRAMERATE,
					  (DWORD_PTR)&v, sizeof(v)) == ICERR_OK ?
				  (LONG)v : 0;
	}

	/*
	 * State belongs to the compressor that made it: kept only when the
	 * same one is chosen again, so a sequence started on the new choice
	 * isn't handed another codec's settings.
	 */
	if (!hic || !same_fourcc(cv.fccHandler, handler)) {
		cv.lpState = NULL;
		cv.cbState = 0;
	}

	/* An earlier choice's compressor gives way to this one. */
	if ((pc->dwFlags & ICMF_COMPVARS_VALID) && pc->hic)
		ic_close(pc->hic);

	fprintf(stderr, "ICCompressorChoose [%s]: (no GUI: choosing %s)\n",
		title ? title : "",
		hic ? fourcc_str(handler, buf) : "Full Frames (Uncompressed)");
	cv.hic = hic;
	cv.fccHandler = handler;
	cv.dwFlags |= ICMF_COMPVARS_VALID;
	*pc = cv;
	return TRUE;
}
