/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The FOURCCs a codec DLL may want to be opened under, read from the DLL
 * itself, for one that answers nothing without its own: wnvwinx.dll
 * accepts DRV_OPEN under any handler but answers ICM_GETINFO only as
 * 'WINX', and TRICDC32.DRV opens only under the handlers in a table of
 * its own ('ty2n', 'rtc3', ...).
 *
 * Two places hold them: the 32-bit immediates its code compares with
 * (cmp eax, imm32 and cmp r/m32, imm32, as a DRV_OPEN handler checks
 * ICOPEN.fccHandler), and aligned words standing alone in its data (not
 * part of a longer string), as a table of them. Each is four ASCII
 * letters and digits, a letter first; the stream and driver types
 * ('vidc', 'vids', ...) aren't handlers and are left out.
 */
#include <string.h>
#include <strings.h>

#include "loader/pe.h"
#include "vfw/registry.h"

static int is_alnum(BYTE c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
	       (c >= 'a' && c <= 'z');
}

static int is_printable(BYTE c)
{
	return c >= 0x20 && c < 0x7f;
}

static int looks_like_fourcc(const BYTE *b)
{
	static const char *const types[] = {
		"vidc", "audc", "vids", "auds", "draw", "txts", "mids",
	};

	if (!((b[0] >= 'A' && b[0] <= 'Z') || (b[0] >= 'a' && b[0] <= 'z')) ||
	    !is_alnum(b[1]) || !is_alnum(b[2]) || !is_alnum(b[3]))
		return 0;
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
		if (!strncasecmp((const char *)b, types[i], 4))
			return 0;
	return 1;
}

static size_t add(FOURCC *out, size_t n, size_t max, const BYTE *b)
{
	FOURCC f = (FOURCC)b[0] | (FOURCC)b[1] << 8 | (FOURCC)b[2] << 16 |
		   (FOURCC)b[3] << 24;

	for (size_t i = 0; i < n; i++)
		if (out[i] == f)
			return n;
	if (n < max)
		out[n++] = f;
	return n;
}

/* Where a cmp's imm32 starts, from the start of the instruction; 0 if none. */
static size_t cmp_immediate(const BYTE *p, size_t left)
{
	if (left >= 5 && p[0] == 0x3d)		/* cmp eax, imm32 */
		return 1;
	if (left < 6 || p[0] != 0x81 || (p[1] & 0x38) != 0x38)
		return 0;			/* not cmp r/m32, imm32 */
	int mod = p[1] >> 6, rm = p[1] & 7;
	size_t len = 2;
	if (mod != 3 && rm == 4) {		/* a SIB byte */
		if ((p[2] & 7) == 5 && mod == 0)
			len += 4;
		len++;
	} else if (mod == 0 && rm == 5) {
		len += 4;
	}
	if (mod == 1)
		len += 1;
	else if (mod == 2)
		len += 4;
	return len + 4 <= left ? len : 0;
}

size_t vfw_dll_handlers(const char *path, FOURCC *out, size_t max)
{
	struct pe_image *img = pe_open(path);
	size_t n = 0;

	if (!img)
		return 0;
	/* The code's comparisons first: they are the likelier. */
	for (int pass = 0; pass < 2; pass++)
		for (unsigned i = 0; i < img->nsections; i++) {
			const IMAGE_SECTION_HEADER *s = &img->sections[i];
			DWORD size = s->SizeOfRawData;
			if (s->Misc.VirtualSize && s->Misc.VirtualSize < size)
				size = s->Misc.VirtualSize;
			if (s->PointerToRawData > img->file_len ||
			    size > img->file_len - s->PointerToRawData)
				continue;
			const BYTE *p = img->file + s->PointerToRawData;
			int code = !!(s->Characteristics &
				      (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE));
			if (pass == 0 && code) {
				for (DWORD k = 0; k < size; k++) {
					size_t at = cmp_immediate(p + k, size - k);
					if (at && looks_like_fourcc(p + k + at))
						n = add(out, n, max, p + k + at);
				}
			} else if (pass == 1 && !code &&
				   (s->Characteristics &
				    IMAGE_SCN_CNT_INITIALIZED_DATA)) {
				for (DWORD k = 0; k + 4 <= size; k += 4)
					if (looks_like_fourcc(p + k) &&
					    !(k && is_printable(p[k - 1])) &&
					    !(k + 4 < size && is_printable(p[k + 4])))
						n = add(out, n, max, p + k);
			}
		}
	pe_free(img);
	return n;
}
