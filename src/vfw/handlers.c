/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The FOURCCs a codec DLL may want to be opened under, read from the DLL
 * itself, for one that answers nothing without its own: wnvwinx.dll
 * accepts DRV_OPEN under any handler but answers ICM_GETINFO only as
 * 'WINX', and TRICDC32.DRV opens only under the handlers in a table of
 * its own ('ty2n', 'rtc3', ...).
 *
 * They are looked for in these places, the likelier first:
 *  - the 32-bit immediates its code compares with (cmp eax, imm32 and
 *    cmp r/m32, imm32, as a DRV_OPEN handler checks ICOPEN.fccHandler);
 *  - aligned words standing alone in its data (not part of a longer
 *    string), as a table of them, and an aligned pair of one in upper and
 *    lower case standing alone, as VFCodec.dll (the VFAPI Reader Codec, a
 *    Delphi DLL) keeps 'VIFP' and 'vifp';
 *  - four-character words of its version resource with a digit in them
 *    or in capitals ("On2 VP70 Video Codec");
 *  - unaligned strings of four characters, and the first word of a
 *    string ("VP70 VIDEO FOR WINDOWS CODEC");
 *  - the immediates its code stores or pushes (mov r32, imm32;
 *    mov r/m32, imm32; push imm32), as one that fills in an ICINFO or
 *    builds its output format does.
 * Each is four ASCII letters and digits, a letter first; the stream and
 * driver types ('vidc', 'vids', ...) aren't handlers and are left out.
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

/* Whether b is a, with its letters' case changed (and some letter). */
static int case_pair(const BYTE *a, const BYTE *b)
{
	return memcmp(a, b, 4) && !strncasecmp((const char *)a,
						(const char *)b, 4) &&
	       is_alnum(b[0]) && is_alnum(b[1]) && is_alnum(b[2]) &&
	       is_alnum(b[3]);
}

static unsigned char *add_where;	/* the caller's, or NULL */
static unsigned add_from;

static size_t add(FOURCC *out, size_t n, size_t max, const BYTE *b)
{
	FOURCC f = (FOURCC)b[0] | (FOURCC)b[1] << 8 | (FOURCC)b[2] << 16 |
		   (FOURCC)b[3] << 24;

	for (size_t i = 0; i < n; i++)
		if (out[i] == f)
			return n;
	if (n < max) {
		if (add_where)
			add_where[n] = (unsigned char)add_from;
		out[n++] = f;
	}
	return n;
}

/* Where an r/m32, imm32 form's immediate starts, from its ModRM byte. */
static size_t modrm_immediate(const BYTE *p, size_t left)
{
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

/* Where a cmp's imm32 starts, from the start of the instruction; 0 if none. */
static size_t cmp_immediate(const BYTE *p, size_t left)
{
	if (left >= 5 && p[0] == 0x3d)		/* cmp eax, imm32 */
		return 1;
	if (left < 6 || p[0] != 0x81 || (p[1] & 0x38) != 0x38)
		return 0;			/* not cmp r/m32, imm32 */
	return modrm_immediate(p, left);
}

/* The same for a mov or push of an imm32. */
static size_t mov_immediate(const BYTE *p, size_t left)
{
	if (left >= 5 && ((p[0] >= 0xb8 && p[0] <= 0xbf) || p[0] == 0x68))
		return 1;			/* mov r32, imm32; push imm32 */
	if (left < 6 || p[0] != 0xc7 || (p[1] & 0x38))
		return 0;			/* not mov r/m32, imm32 */
	return modrm_immediate(p, left);
}

struct version_ctx {
	FOURCC *out;
	size_t n, max;
};

/* Its four-character words with a digit, or all in capitals. */
static int version_words(void *ctx, const char *table, const char *key,
			 const char *value)
{
	struct version_ctx *v = ctx;
	const BYTE *p = (const BYTE *)value;

	while (*p) {
		while (*p && !is_alnum(*p))
			p++;
		const BYTE *w = p;
		while (is_alnum(*p))
			p++;
		if (p - w != 4 || !looks_like_fourcc(w))
			continue;
		int digit = 0, caps = 1;
		for (int i = 0; i < 4; i++) {
			digit |= w[i] >= '0' && w[i] <= '9';
			caps &= !(w[i] >= 'a' && w[i] <= 'z');
		}
		if (digit || caps)
			v->n = add(v->out, v->n, v->max, w);
	}
	return 0;
}

/* Whether b (in a section of size bytes, at k) is a string, or begins one. */
static int string_start(const BYTE *b, DWORD k, DWORD size)
{
	if ((k && b[-1]) || k + 5 > size)
		return 0;
	return !b[4] || (b[4] == ' ' && k + 6 <= size && is_printable(b[5]));
}

/*
 * Uncompressed formats a codec compares its input with ('YUY2', 'I420'),
 * which are seldom its own handler (msyuv.dll's 'UYVY', iyuv_32.dll's
 * 'IYUV' are): last, so another candidate is tried first.
 */
static int is_raw(FOURCC f)
{
	static const char raw[][5] = {
		"I420", "IYUV", "YV12", "YUY2", "YUYV", "YVYU", "UYVY", "Y422",
		"UYNV", "Y41P", "YVU9", "YUV9", "Y800", "GREY", "NV12", "NV21",
		"Y12I", "YV16", "I422", "I444", "AYUV", "Y210", "P010", "V210",
	};

	for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); i++)
		if (!strncasecmp((const char *)&f, raw[i], 4))
			return 1;
	return 0;
}

static void demote_raw(FOURCC *out, unsigned char *where, size_t n)
{
	FOURCC f[n ? n : 1];
	unsigned char w[n ? n : 1];
	size_t k = 0;

	for (int raw = 0; raw < 2; raw++)
		for (size_t i = 0; i < n; i++)
			if (is_raw(out[i]) == raw) {
				f[k] = out[i];
				w[k++] = where ? where[i] : 0;
			}
	memcpy(out, f, n * sizeof(*out));
	if (where)
		memcpy(where, w, n);
}

size_t vfw_dll_handlers(const char *path, FOURCC *out, unsigned char *where,
			size_t max)
{
	struct pe_image *img = pe_open(path);
	size_t n = 0;

	if (!img)
		return 0;
	add_where = where;
	for (int pass = 0; pass < 5; pass++) {
		if (pass == 2) {
			struct version_ctx v = { out, n, max };
			add_from = VFW_FOURCC_FROM_VERSION;
			pe_version_info(img, NULL, version_words, &v);
			n = v.n;
			continue;
		}
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
			int data = !code && (s->Characteristics &
					     IMAGE_SCN_CNT_INITIALIZED_DATA);
			if ((pass == 0 || pass == 4) && code) {
				add_from = pass ? VFW_FOURCC_FROM_MOV :
					   VFW_FOURCC_FROM_CMP;
				for (DWORD k = 0; k < size; k++) {
					size_t at = pass ?
						mov_immediate(p + k, size - k) :
						cmp_immediate(p + k, size - k);
					if (at && looks_like_fourcc(p + k + at))
						n = add(out, n, max, p + k + at);
				}
			} else if (pass == 1 && data) {
				for (DWORD k = 0; k + 4 <= size; k += 4) {
					if (!looks_like_fourcc(p + k) ||
					    (k && is_printable(p[k - 1])))
						continue;
					if (!(k + 4 < size && is_printable(p[k + 4]))) {
						add_from = VFW_FOURCC_FROM_TABLE;
						n = add(out, n, max, p + k);
					} else if (k + 8 <= size &&
						   case_pair(p + k, p + k + 4) &&
						   !(k + 8 < size &&
						     is_printable(p[k + 8]))) {
						add_from = VFW_FOURCC_FROM_PAIR;
						n = add(out, n, max, p + k);
					}
				}
			} else if (pass == 3 && data) {
				add_from = VFW_FOURCC_FROM_STRING;
				for (DWORD k = 0; k + 4 <= size; k++)
					if (looks_like_fourcc(p + k) &&
					    string_start(p + k, k, size))
						n = add(out, n, max, p + k);
			}
		}
	}
	add_where = NULL;
	pe_free(img);
	demote_raw(out, where, n);
	return n;
}

const char *vfw_fourcc_from_name(unsigned from)
{
	static const char *const names[] = {
		[VFW_FOURCC_FROM_CMP] = "compared with in its code",
		[VFW_FOURCC_FROM_TABLE] = "a word in its data",
		[VFW_FOURCC_FROM_PAIR] = "an upper and lower case pair in its data",
		[VFW_FOURCC_FROM_VERSION] = "in its version resource",
		[VFW_FOURCC_FROM_STRING] = "a string in its data",
		[VFW_FOURCC_FROM_MOV] = "stored or pushed by its code",
	};

	return from < sizeof(names) / sizeof(names[0]) ? names[from] : "?";
}
