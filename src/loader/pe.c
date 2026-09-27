/*
 * PE32 parsing, mapping, relocation, import/export/TLS/resource walking.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/error.h"
#include "common/unicode.h"
#include "loader/pe.h"
#include "runtime/crash.h"
#include "runtime/teb.h"

#define MAX_SECTIONS     96		/* the PE/COFF limit */
#define MAX_IMAGE_SIZE   (512u << 20)
#define MAX_DESCRIPTORS  4096
#define MAX_THUNKS       65536
#define MAX_EXPORTS      65536
#define MAX_TLS_CALLBACKS 1024
#define IMAGE_ALIGN      0x10000	/* Windows maps images on 64K boundaries */

#define FAIL(...) do { vfw_set_error(__VA_ARGS__); return -1; } while (0)

static DWORD rd32(const void *p)
{
	DWORD v;
	memcpy(&v, p, 4);
	return v;
}

static WORD rd16(const void *p)
{
	WORD v;
	memcpy(&v, p, 2);
	return v;
}

static void wr32(void *p, DWORD v)
{
	memcpy(p, &v, 4);
}

static void wr16(void *p, WORD v)
{
	memcpy(p, &v, 2);
}

/* off + len <= total, without overflow. */
static int range_ok(size_t off, size_t len, size_t total)
{
	return off <= total && len <= total - off;
}

static DWORD section_vsize(const IMAGE_SECTION_HEADER *s)
{
	return s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
}

static void section_name(const IMAGE_SECTION_HEADER *s, char out[9])
{
	memcpy(out, s->Name, 8);
	out[8] = '\0';
}

static const IMAGE_OPTIONAL_HEADER32 *opt(const struct pe_image *img)
{
	return &img->nt.OptionalHeader;
}

struct pe_image *pe_parse(const void *buf, size_t len)
{
	const BYTE *p = buf;
	IMAGE_DOS_HEADER dos;
	IMAGE_FILE_HEADER fh;
	DWORD sig;

	if (len < sizeof(dos)) {
		vfw_set_error("not a PE file: too short");
		return NULL;
	}
	memcpy(&dos, p, sizeof(dos));
	if (dos.e_magic != IMAGE_DOS_SIGNATURE) {
		vfw_set_error("not a PE file: no MZ signature");
		return NULL;
	}
	if (dos.e_lfanew < 0 ||
	    !range_ok(dos.e_lfanew, 4 + sizeof(fh), len)) {
		vfw_set_error("not a PE file: PE header offset out of range");
		return NULL;
	}
	size_t nt_off = dos.e_lfanew;
	sig = rd32(p + nt_off);
	if (sig != IMAGE_NT_SIGNATURE) {
		vfw_set_error("not a PE file: no PE signature");
		return NULL;
	}
	memcpy(&fh, p + nt_off + 4, sizeof(fh));
	if (fh.Machine != IMAGE_FILE_MACHINE_I386) {
		vfw_set_error("unsupported machine type 0x%04x (only i386 is supported)",
			      fh.Machine);
		return NULL;
	}

	size_t opt_off = nt_off + 4 + sizeof(fh);
	size_t opt_min = offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
	if (fh.SizeOfOptionalHeader < opt_min ||
	    !range_ok(opt_off, fh.SizeOfOptionalHeader, len)) {
		vfw_set_error("bad PE file: optional header truncated");
		return NULL;
	}
	WORD magic = rd16(p + opt_off);
	if (magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
		vfw_set_error(magic == 0x20b ?
			      "unsupported image: 64-bit (PE32+)" :
			      "bad PE file: optional header magic 0x%x", magic);
		return NULL;
	}

	struct pe_image *img = calloc(1, sizeof(*img));
	if (!img) {
		vfw_set_error("out of memory");
		return NULL;
	}
	img->nt.Signature = sig;
	img->nt.FileHeader = fh;
	IMAGE_OPTIONAL_HEADER32 *o = &img->nt.OptionalHeader;
	size_t opt_copy = fh.SizeOfOptionalHeader < sizeof(*o) ?
			  fh.SizeOfOptionalHeader : sizeof(*o);
	memcpy(o, p + opt_off, opt_copy);

	/* Only trust the directories the header says it has room for. */
	size_t ndirs = (fh.SizeOfOptionalHeader - opt_min) /
		       sizeof(IMAGE_DATA_DIRECTORY);
	if (ndirs > o->NumberOfRvaAndSizes)
		ndirs = o->NumberOfRvaAndSizes;
	for (size_t i = ndirs; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; i++)
		o->DataDirectory[i] = (IMAGE_DATA_DIRECTORY){ 0, 0 };

	if (o->SizeOfImage == 0 || o->SizeOfImage > MAX_IMAGE_SIZE) {
		vfw_set_error("bad PE file: SizeOfImage 0x%x", o->SizeOfImage);
		goto fail;
	}
	if (o->SectionAlignment == 0 || o->FileAlignment == 0) {
		vfw_set_error("bad PE file: zero section or file alignment");
		goto fail;
	}
	if (o->SizeOfHeaders > o->SizeOfImage) {
		vfw_set_error("bad PE file: SizeOfHeaders larger than the image");
		goto fail;
	}
	if (o->AddressOfEntryPoint >= o->SizeOfImage) {
		vfw_set_error("bad PE file: entry point outside the image");
		goto fail;
	}

	img->nsections = fh.NumberOfSections;
	if (img->nsections > MAX_SECTIONS) {
		vfw_set_error("bad PE file: %u sections", img->nsections);
		goto fail;
	}
	size_t sec_off = opt_off + fh.SizeOfOptionalHeader;
	size_t sec_len = img->nsections * sizeof(IMAGE_SECTION_HEADER);
	if (!range_ok(sec_off, sec_len, len)) {
		vfw_set_error("bad PE file: section table truncated");
		goto fail;
	}
	img->sections = malloc(sec_len ? sec_len : 1);
	if (!img->sections) {
		vfw_set_error("out of memory");
		goto fail;
	}
	memcpy(img->sections, p + sec_off, sec_len);

	for (unsigned i = 0; i < img->nsections; i++) {
		const IMAGE_SECTION_HEADER *s = &img->sections[i];
		char name[9];

		section_name(s, name);
		if (!range_ok(s->VirtualAddress, section_vsize(s), o->SizeOfImage)) {
			vfw_set_error("bad PE file: section '%s' lies outside the image",
				      name);
			goto fail;
		}
		if (s->SizeOfRawData &&
		    !range_ok(s->PointerToRawData, s->SizeOfRawData, len)) {
			vfw_set_error("bad PE file: section '%s' raw data runs past the end of the file",
				      name);
			goto fail;
		}
	}

	img->file = malloc(len);
	if (!img->file) {
		vfw_set_error("out of memory");
		goto fail;
	}
	memcpy(img->file, buf, len);
	img->file_len = len;
	return img;

fail:
	pe_free(img);
	return NULL;
}

struct pe_image *pe_open(const char *path)
{
	void *buf;
	size_t len;

	if (vfw_read_file(path, &buf, &len))
		return NULL;
	struct pe_image *img = pe_parse(buf, len);
	free(buf);
	if (!img) {
		char msg[512];
		snprintf(msg, sizeof(msg), "%s", vfw_error());
		vfw_set_error("%s: %s", path, msg);
	}
	return img;
}

void pe_free(struct pe_image *img)
{
	if (!img)
		return;
	if (img->has_tls)
		teb_static_tls_remove(img->tls_index);
	if (img->base)
		munmap(img->base, img->map_len);
	free(img->sections);
	free(img->file);
	free(img);
}

/*
 * The bytes at rva and how many follow it contiguously. A mapped image is
 * flat; a parsed one is the headers plus each section's raw data.
 */
static const BYTE *pe_span(const struct pe_image *img, DWORD rva, DWORD *avail)
{
	if (img->base) {
		if (rva >= opt(img)->SizeOfImage)
			return NULL;
		*avail = opt(img)->SizeOfImage - rva;
		return img->base + rva;
	}

	DWORD hdr = opt(img)->SizeOfHeaders;
	if (hdr > img->file_len)
		hdr = img->file_len;
	if (rva < hdr) {
		*avail = hdr - rva;
		return img->file + rva;
	}
	for (unsigned i = 0; i < img->nsections; i++) {
		const IMAGE_SECTION_HEADER *s = &img->sections[i];
		DWORD raw = s->SizeOfRawData;
		if (raw > section_vsize(s))
			raw = section_vsize(s);
		if (rva >= s->VirtualAddress && rva - s->VirtualAddress < raw) {
			DWORD off = rva - s->VirtualAddress;
			*avail = raw - off;
			return img->file + s->PointerToRawData + off;
		}
	}
	return NULL;
}

const void *pe_ptr(const struct pe_image *img, DWORD rva, DWORD len)
{
	DWORD avail;
	const BYTE *p = pe_span(img, rva, &avail);

	return p && len <= avail ? p : NULL;
}

const char *pe_str(const struct pe_image *img, DWORD rva)
{
	DWORD avail;
	const BYTE *p = pe_span(img, rva, &avail);

	return p && memchr(p, 0, avail) ? (const char *)p : NULL;
}

const IMAGE_DATA_DIRECTORY *pe_dir(const struct pe_image *img, int index)
{
	const IMAGE_DATA_DIRECTORY *d = &opt(img)->DataDirectory[index];

	return d->VirtualAddress && d->Size ? d : NULL;
}

/* Current load address of the image, or its preferred one if unmapped. */
static DWORD image_base(const struct pe_image *img)
{
	return img->base ? (DWORD)(uintptr_t)img->base : opt(img)->ImageBase;
}

static DWORD va_to_rva(const struct pe_image *img, DWORD va)
{
	return va - image_base(img);
}

/* Mapping and relocation. */

static void *map_at(void *want, size_t len)
{
	void *p = mmap(want, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p != MAP_FAILED && p != want) {
		/* A kernel without MAP_FIXED_NOREPLACE treats it as a hint. */
		munmap(p, len);
		return MAP_FAILED;
	}
	return p;
}

/* Maps len bytes anywhere, aligned like a Windows image (64K). */
static void *map_anywhere(size_t len)
{
	size_t total = len + IMAGE_ALIGN;
	BYTE *p = mmap(NULL, total, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return MAP_FAILED;

	uintptr_t start = ((uintptr_t)p + IMAGE_ALIGN - 1) &
			  ~(uintptr_t)(IMAGE_ALIGN - 1);
	if (start > (uintptr_t)p)
		munmap(p, start - (uintptr_t)p);
	size_t tail = (uintptr_t)p + total - (start + len);
	if (tail)
		munmap((void *)(start + len), tail);
	return (void *)start;
}

int pe_map(struct pe_image *img, unsigned flags)
{
	const IMAGE_OPTIONAL_HEADER32 *o = opt(img);
	size_t page = sysconf(_SC_PAGESIZE);
	size_t len = (o->SizeOfImage + page - 1) & ~(page - 1);
	void *want = (void *)(uintptr_t)o->ImageBase;
	void *base = MAP_FAILED;

	if (img->base)
		FAIL("image is already mapped");

	if (!(flags & PE_MAP_FORCE_RELOC))
		base = map_at(want, len);
	if (base == MAP_FAILED) {
		base = map_anywhere(len);
		if (base == want) {
			/* Forced relocation, but the preferred base was free. */
			void *other = map_anywhere(len);
			munmap(base, len);
			base = other;
		}
	}
	if (base == MAP_FAILED)
		FAIL("cannot map %zu bytes: %s", len, strerror(errno));

	img->base = base;
	img->map_len = len;

	size_t hdr = o->SizeOfHeaders < img->file_len ?
		     o->SizeOfHeaders : img->file_len;
	memcpy(img->base, img->file, hdr);
	for (unsigned i = 0; i < img->nsections; i++) {
		const IMAGE_SECTION_HEADER *s = &img->sections[i];
		DWORD raw = s->SizeOfRawData;
		if (raw > section_vsize(s))
			raw = section_vsize(s);
		/* PointerToRawData means nothing (and isn't checked) without data. */
		if (raw)
			memcpy(img->base + s->VirtualAddress,
			       img->file + s->PointerToRawData, raw);
	}

	DWORD delta = (DWORD)(uintptr_t)base - o->ImageBase;
	if (!delta)
		return 0;

	img->relocated = 1;
	const IMAGE_DATA_DIRECTORY *d = pe_dir(img, IMAGE_DIRECTORY_ENTRY_BASERELOC);
	const BYTE *relocs = d ? pe_ptr(img, d->VirtualAddress, d->Size) : NULL;
	if (!d || (img->nt.FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)) {
		vfw_set_error("image has no relocations and cannot be loaded "
			      "away from its preferred base 0x%08x", o->ImageBase);
		goto fail;
	}
	if (!relocs) {
		vfw_set_error("bad PE file: relocation directory out of range");
		goto fail;
	}
	if (pe_relocate(img->base, o->SizeOfImage, relocs, d->Size, delta))
		goto fail;

	/* Like Windows, record the actual base in the mapped header. */
	size_t ib_off = ((const IMAGE_DOS_HEADER *)img->file)->e_lfanew + 4 +
			sizeof(IMAGE_FILE_HEADER) +
			offsetof(IMAGE_OPTIONAL_HEADER32, ImageBase);
	if (range_ok(ib_off, 4, hdr))
		wr32(img->base + ib_off, (DWORD)(uintptr_t)base);
	return 0;

fail:
	munmap(img->base, img->map_len);
	img->base = NULL;
	img->map_len = 0;
	img->relocated = 0;
	return -1;
}

int pe_relocate(BYTE *image, size_t image_len, const BYTE *relocs,
		size_t relocs_len, DWORD delta)
{
	size_t pos = 0;

	while (pos + sizeof(IMAGE_BASE_RELOCATION) <= relocs_len) {
		IMAGE_BASE_RELOCATION blk;

		memcpy(&blk, relocs + pos, sizeof(blk));
		if (blk.VirtualAddress == 0 && blk.SizeOfBlock == 0)
			break;
		if (blk.SizeOfBlock < sizeof(blk) ||
		    blk.SizeOfBlock > relocs_len - pos)
			FAIL("bad relocation block at offset 0x%zx", pos);

		const BYTE *ent = relocs + pos + sizeof(blk);
		size_t n = (blk.SizeOfBlock - sizeof(blk)) / 2;
		for (size_t i = 0; i < n; i++) {
			WORD e = rd16(ent + 2 * i);
			unsigned type = e >> 12;
			size_t rva = (size_t)blk.VirtualAddress + (e & 0xfff);
			size_t width = type == IMAGE_REL_BASED_HIGHLOW ? 4 : 2;

			if (type == IMAGE_REL_BASED_ABSOLUTE)
				continue;
			if (!range_ok(rva, width, image_len))
				FAIL("relocation target 0x%zx outside the image", rva);

			BYTE *t = image + rva;
			switch (type) {
			case IMAGE_REL_BASED_HIGHLOW:
				wr32(t, rd32(t) + delta);
				break;
			case IMAGE_REL_BASED_HIGH:
				wr16(t, rd16(t) + HIWORD(delta));
				break;
			case IMAGE_REL_BASED_LOW:
				wr16(t, rd16(t) + LOWORD(delta));
				break;
			case IMAGE_REL_BASED_HIGHADJ: {
				/* The next entry holds the low half of the value. */
				if (++i >= n)
					FAIL("truncated HIGHADJ relocation at 0x%zx", rva);
				DWORD v = ((DWORD)rd16(t) << 16) +
					  (DWORD)(int32_t)(int16_t)rd16(ent + 2 * i);
				v += delta;
				wr16(t, HIWORD(v + 0x8000));
				break;
			}
			default:
				FAIL("unsupported relocation type %u at 0x%zx",
				     type, rva);
			}
		}
		pos += blk.SizeOfBlock;
	}
	return 0;
}

int pe_protect(struct pe_image *img)
{
	const IMAGE_OPTIONAL_HEADER32 *o = opt(img);
	size_t page = sysconf(_SC_PAGESIZE);
	int exec_data = !(o->DllCharacteristics & IMAGE_DLLCHARACTERISTICS_NX_COMPAT);

	if (!img->base)
		FAIL("image is not mapped");

	/* Sections that share pages cannot have their own protections. */
	if (o->SectionAlignment < page) {
		if (mprotect(img->base, img->map_len,
			     PROT_READ | PROT_WRITE | PROT_EXEC))
			FAIL("mprotect: %s", strerror(errno));
		return 0;
	}

	if (mprotect(img->base, img->map_len, PROT_READ))
		FAIL("mprotect: %s", strerror(errno));
	for (unsigned i = 0; i < img->nsections; i++) {
		const IMAGE_SECTION_HEADER *s = &img->sections[i];
		DWORD c = s->Characteristics;
		size_t start = s->VirtualAddress & ~(page - 1);
		size_t end = (size_t)s->VirtualAddress + section_vsize(s);
		int prot = PROT_READ;

		end = (end + page - 1) & ~(page - 1);
		if (end > img->map_len)
			end = img->map_len;
		if (end <= start)
			continue;
		if (c & IMAGE_SCN_MEM_WRITE)
			prot |= PROT_WRITE;
		if ((c & IMAGE_SCN_MEM_EXECUTE) || exec_data)
			prot |= PROT_EXEC;
		if (mprotect(img->base + start, end - start, prot))
			FAIL("mprotect: %s", strerror(errno));
	}
	return 0;
}

/* Imports. */

/* Walks one import name table (ILT/INT). */
static int walk_thunks(const struct pe_image *img, const char *dll,
		       DWORD lookup_rva, DWORD iat_rva, int va_based,
		       int delayed, pe_import_fn fn, void *ctx)
{
	for (DWORD i = 0; ; i++) {
		if (i >= MAX_THUNKS)
			FAIL("%s: too many imports", dll);
		const BYTE *t = pe_ptr(img, lookup_rva + 4 * i, 4);
		if (!t)
			FAIL("%s: import table runs off the image", dll);
		DWORD v = rd32(t);
		if (!v)
			return 0;

		struct pe_import imp = {
			.dll = dll,
			.iat_rva = iat_rva + 4 * i,
			.delayed = delayed,
		};
		if (v & IMAGE_ORDINAL_FLAG32) {
			imp.ordinal = IMAGE_ORDINAL32(v);
		} else {
			DWORD rva = va_based ? va_to_rva(img, v) : v;
			const BYTE *hint = pe_ptr(img, rva, 2);
			imp.name = pe_str(img, rva + 2);
			if (!hint || !imp.name)
				FAIL("%s: import name out of range", dll);
			imp.ordinal = rd16(hint);
		}
		if (fn(ctx, &imp))
			return -1;
	}
}

int pe_foreach_import(const struct pe_image *img, pe_import_fn fn, void *ctx)
{
	const IMAGE_DATA_DIRECTORY *d = pe_dir(img, IMAGE_DIRECTORY_ENTRY_IMPORT);

	if (!d)
		return 0;
	for (DWORD n = 0; ; n++) {
		IMAGE_IMPORT_DESCRIPTOR desc;
		const void *p = pe_ptr(img, d->VirtualAddress + n * sizeof(desc),
				       sizeof(desc));

		if (n >= MAX_DESCRIPTORS || !p)
			FAIL("bad PE file: import directory runs off the image");
		memcpy(&desc, p, sizeof(desc));
		if (!desc.Name && !desc.FirstThunk)
			return 0;

		const char *dll = pe_str(img, desc.Name);
		if (!dll)
			FAIL("bad PE file: import DLL name out of range");
		/* Old linkers leave OriginalFirstThunk zero; use the IAT. */
		DWORD lookup = desc.OriginalFirstThunk ? desc.OriginalFirstThunk :
			       desc.FirstThunk;
		if (walk_thunks(img, dll, lookup, desc.FirstThunk, 0, 0, fn, ctx))
			return -1;
	}
}

int pe_foreach_delay_import(const struct pe_image *img, pe_import_fn fn,
			    void *ctx)
{
	const IMAGE_DATA_DIRECTORY *d =
		pe_dir(img, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);

	if (!d)
		return 0;
	for (DWORD n = 0; ; n++) {
		IMAGE_DELAYLOAD_DESCRIPTOR desc;
		const void *p = pe_ptr(img, d->VirtualAddress + n * sizeof(desc),
				       sizeof(desc));

		if (n >= MAX_DESCRIPTORS || !p)
			FAIL("bad PE file: delay import directory runs off the image");
		memcpy(&desc, p, sizeof(desc));
		if (!desc.DllNameRVA)
			return 0;

		/* VC6-era descriptors hold VAs rather than RVAs. */
		int va = !(desc.Attributes.AllAttributes & IMAGE_DELAYLOAD_RVA_BASED);
		DWORD fix = va ? image_base(img) : 0;
		const char *dll = pe_str(img, desc.DllNameRVA - fix);
		if (!dll)
			FAIL("bad PE file: delay import DLL name out of range");
		if (walk_thunks(img, dll, desc.ImportNameTableRVA - fix,
				desc.ImportAddressTableRVA - fix, va, 1, fn, ctx))
			return -1;
	}
}

struct bind_ctx {
	struct pe_image *img;
	pe_resolve_fn resolve;
	void *ctx;
};

static int bind_one(void *c, const struct pe_import *imp)
{
	struct bind_ctx *b = c;
	void *addr = b->resolve(b->ctx, imp);

	if (!addr)
		return -1;
	BYTE *slot = (BYTE *)pe_ptr(b->img, imp->iat_rva, 4);
	if (!slot)
		FAIL("%s: import address table out of range", imp->dll);
	wr32(slot, (DWORD)(uintptr_t)addr);
	return 0;
}

int pe_bind_imports(struct pe_image *img, pe_resolve_fn resolve, void *ctx)
{
	struct bind_ctx b = { img, resolve, ctx };

	if (!img->base)
		FAIL("image is not mapped");
	return pe_foreach_import(img, bind_one, &b);
}

/* Exports. */

struct export_tables {
	IMAGE_EXPORT_DIRECTORY ed;
	DWORD dir_rva, dir_size;
	const BYTE *funcs;	/* DWORD[NumberOfFunctions] */
	const BYTE *names;	/* DWORD[NumberOfNames] */
	const BYTE *ords;	/* WORD[NumberOfNames] */
};

/* 1 if the image has exports, 0 if not, -1 on a malformed directory. */
static int export_tables(const struct pe_image *img, struct export_tables *t)
{
	const IMAGE_DATA_DIRECTORY *d = pe_dir(img, IMAGE_DIRECTORY_ENTRY_EXPORT);
	const void *p;

	if (!d)
		return 0;
	p = pe_ptr(img, d->VirtualAddress, sizeof(t->ed));
	if (!p)
		FAIL("bad PE file: export directory out of range");
	memcpy(&t->ed, p, sizeof(t->ed));
	t->dir_rva = d->VirtualAddress;
	t->dir_size = d->Size;

	if (t->ed.NumberOfFunctions > MAX_EXPORTS ||
	    t->ed.NumberOfNames > MAX_EXPORTS)
		FAIL("bad PE file: too many exports");
	t->funcs = pe_ptr(img, t->ed.AddressOfFunctions,
			  t->ed.NumberOfFunctions * 4);
	t->names = pe_ptr(img, t->ed.AddressOfNames, t->ed.NumberOfNames * 4);
	t->ords = pe_ptr(img, t->ed.AddressOfNameOrdinals,
			 t->ed.NumberOfNames * 2);
	if ((t->ed.NumberOfFunctions && !t->funcs) ||
	    (t->ed.NumberOfNames && (!t->names || !t->ords)))
		FAIL("bad PE file: export tables out of range");
	return 1;
}

static const char *export_name(const struct pe_image *img,
			       const struct export_tables *t, DWORD i)
{
	return pe_str(img, rd32(t->names + 4 * i));
}

/* Fills *out for export table index idx; -1 if that slot is unused. */
static int fill_export(const struct pe_image *img,
		       const struct export_tables *t, DWORD idx,
		       const char *name, struct pe_export *out)
{
	if (idx >= t->ed.NumberOfFunctions)
		FAIL("export index %u out of range", idx);
	DWORD rva = rd32(t->funcs + 4 * idx);
	if (!rva)
		FAIL("export ordinal %u is unused", t->ed.Base + idx);

	out->ordinal = t->ed.Base + idx;
	out->name = name;
	out->rva = rva;
	out->forwarder = NULL;
	if (rva >= t->dir_rva && rva - t->dir_rva < t->dir_size) {
		out->forwarder = pe_str(img, rva);
		if (!out->forwarder)
			FAIL("bad PE file: forwarder string out of range");
	}
	return 0;
}

const char *pe_export_dll_name(const struct pe_image *img)
{
	struct export_tables t;

	return export_tables(img, &t) == 1 ? pe_str(img, t.ed.Name) : NULL;
}

int pe_foreach_export(const struct pe_image *img, pe_export_fn fn, void *ctx)
{
	struct export_tables t;
	int r = export_tables(img, &t);

	if (r <= 0)
		return r;

	/* Map each function index to its first name, if it has one. */
	const char **names = calloc(t.ed.NumberOfFunctions + 1, sizeof(*names));
	if (!names)
		FAIL("out of memory");
	for (DWORD i = 0; i < t.ed.NumberOfNames; i++) {
		WORD idx = rd16(t.ords + 2 * i);
		if (idx < t.ed.NumberOfFunctions && !names[idx])
			names[idx] = export_name(img, &t, i);
	}

	r = 0;
	for (DWORD idx = 0; idx < t.ed.NumberOfFunctions && !r; idx++) {
		struct pe_export exp;
		if (!rd32(t.funcs + 4 * idx))
			continue;
		r = fill_export(img, &t, idx, names[idx], &exp);
		if (!r && fn(ctx, &exp))
			r = -1;
	}
	free(names);
	return r;
}

int pe_find_export(const struct pe_image *img, const char *name, WORD hint,
		   struct pe_export *out)
{
	struct export_tables t;
	int r = export_tables(img, &t);
	DWORD n;

	if (r <= 0) {
		if (r == 0)
			vfw_set_error("no export named %s (no export table)", name);
		return -1;
	}
	n = t.ed.NumberOfNames;

	/* The hint is usually the right slot in the name table. */
	if (hint < n) {
		const char *s = export_name(img, &t, hint);
		if (s && !strcmp(s, name))
			return fill_export(img, &t, rd16(t.ords + 2 * hint), s, out);
	}

	/* The table is sorted; fall back to a scan for linkers that don't. */
	DWORD lo = 0, hi = n;
	while (lo < hi) {
		DWORD mid = lo + (hi - lo) / 2;
		const char *s = export_name(img, &t, mid);
		if (!s)
			break;
		int c = strcmp(name, s);
		if (!c)
			return fill_export(img, &t, rd16(t.ords + 2 * mid), s, out);
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	for (DWORD i = 0; i < n; i++) {
		const char *s = export_name(img, &t, i);
		if (s && !strcmp(s, name))
			return fill_export(img, &t, rd16(t.ords + 2 * i), s, out);
	}
	FAIL("no export named %s", name);
}

int pe_find_export_ordinal(const struct pe_image *img, WORD ordinal,
			   struct pe_export *out)
{
	struct export_tables t;
	int r = export_tables(img, &t);

	if (r <= 0) {
		if (r == 0)
			vfw_set_error("no export with ordinal %u (no export table)",
				      ordinal);
		return -1;
	}
	if (ordinal < t.ed.Base || ordinal - t.ed.Base >= t.ed.NumberOfFunctions)
		FAIL("no export with ordinal %u", ordinal);

	DWORD idx = ordinal - t.ed.Base;
	const char *name = NULL;
	for (DWORD i = 0; i < t.ed.NumberOfNames && !name; i++)
		if (rd16(t.ords + 2 * i) == idx)
			name = export_name(img, &t, i);
	if (!rd32(t.funcs + 4 * idx))
		FAIL("no export with ordinal %u", ordinal);
	return fill_export(img, &t, idx, name, out);
}

/* TLS. */

/* 1 and *out if the image has a TLS directory, 0 if not, -1 if malformed. */
static int tls_dir(const struct pe_image *img, IMAGE_TLS_DIRECTORY32 *out)
{
	const IMAGE_DATA_DIRECTORY *d = pe_dir(img, IMAGE_DIRECTORY_ENTRY_TLS);
	const void *p;

	if (!d)
		return 0;
	p = pe_ptr(img, d->VirtualAddress, sizeof(*out));
	if (!p)
		FAIL("bad PE file: TLS directory out of range");
	memcpy(out, p, sizeof(*out));
	return 1;
}

int pe_foreach_tls_callback(const struct pe_image *img, pe_tls_fn fn,
			    void *ctx)
{
	IMAGE_TLS_DIRECTORY32 tls;
	int r = tls_dir(img, &tls);

	if (r <= 0 || !tls.AddressOfCallBacks)
		return r < 0 ? -1 : 0;

	DWORD arr = va_to_rva(img, tls.AddressOfCallBacks);
	for (DWORD i = 0; i < MAX_TLS_CALLBACKS; i++) {
		const void *p = pe_ptr(img, arr + 4 * i, 4);
		if (!p)
			FAIL("bad PE file: TLS callback array out of range");
		DWORD va = rd32(p);
		if (!va)
			return 0;
		DWORD rva = va_to_rva(img, va);
		if (rva >= opt(img)->SizeOfImage)
			FAIL("bad PE file: TLS callback outside the image");
		if (fn(ctx, rva))
			return -1;
	}
	FAIL("bad PE file: too many TLS callbacks");
}

static const char *dll_reason_name(UINT reason, char buf[24])
{
	switch (reason) {
	case DLL_PROCESS_ATTACH: return "DLL_PROCESS_ATTACH";
	case DLL_PROCESS_DETACH: return "DLL_PROCESS_DETACH";
	case DLL_THREAD_ATTACH:  return "DLL_THREAD_ATTACH";
	case DLL_THREAD_DETACH:  return "DLL_THREAD_DETACH";
	}
	snprintf(buf, 24, "%u", reason);
	return buf;
}

struct tls_call {
	struct pe_image *img;
	DWORD reason;
};

static int tls_check(void *c, DWORD rva)
{
	return 0;
}

static int call_tls_callback(void *c, DWORD rva)
{
	struct tls_call *t = c;
	PIMAGE_TLS_CALLBACK cb = (PIMAGE_TLS_CALLBACK)(t->img->base + rva);
	struct crash_frame saved;

	crash_enter("TLS callback", (const void *)cb, t->reason, dll_reason_name,
		    &saved);
	cb(t->img->base, t->reason, NULL);
	crash_leave(&saved);
	return 0;
}

void pe_tls_notify(struct pe_image *img, DWORD reason)
{
	struct tls_call t = { img, reason };

	if (img->has_tls && img->base)
		pe_foreach_tls_callback(img, call_tls_callback, &t);
}

int pe_tls_init(struct pe_image *img)
{
	IMAGE_TLS_DIRECTORY32 tls;
	int r = tls_dir(img, &tls);

	if (r <= 0)
		return r;
	if (!img->base)
		FAIL("image is not mapped");

	/* Check the whole callback list now, before anything runs. */
	if (pe_foreach_tls_callback(img, tls_check, NULL))
		return -1;

	/* The template: raw data from Start to End, then zero fill. */
	const void *tmpl = NULL;
	size_t size = 0;
	if (tls.EndAddressOfRawData > tls.StartAddressOfRawData) {
		size = tls.EndAddressOfRawData - tls.StartAddressOfRawData;
		tmpl = pe_ptr(img, va_to_rva(img, tls.StartAddressOfRawData), size);
		if (!tmpl)
			FAIL("bad PE file: TLS data out of range");
	} else if (tls.EndAddressOfRawData < tls.StartAddressOfRawData) {
		FAIL("bad PE file: TLS data ends before it starts");
	}
	DWORD index = teb_static_tls_add(tmpl, size, tls.SizeOfZeroFill);
	if (index == 0xffffffff)
		return -1;
	img->has_tls = 1;
	img->tls_index = index;
	if (tls.AddressOfIndex) {
		BYTE *p = (BYTE *)pe_ptr(img, va_to_rva(img, tls.AddressOfIndex), 4);
		if (!p)
			FAIL("bad PE file: TLS index out of range");
		wr32(p, img->tls_index);
	}
	return 0;
}

BOOL pe_call_entry(struct pe_image *img, DWORD reason)
{
	typedef BOOL (WINAPI *entry_fn)(HINSTANCE, DWORD, LPVOID);
	DWORD ep = opt(img)->AddressOfEntryPoint;

	if (!ep || !img->base)
		return TRUE;
	entry_fn entry = (entry_fn)(img->base + ep);
	struct crash_frame saved;
	crash_enter("DllMain", (const void *)entry, reason, dll_reason_name, &saved);
	BOOL r = entry((HINSTANCE)img->base, reason, NULL);
	crash_leave(&saved);
	return r;
}

/* Resources. */

struct res_walk {
	const struct pe_image *img;
	DWORD root;		/* RVA of the resource directory */
	DWORD size;
};

/* The entries of the directory at offset off, or NULL. */
static const BYTE *res_entries(const struct res_walk *w, DWORD off,
			       unsigned *count)
{
	const IMAGE_RESOURCE_DIRECTORY *dir;
	IMAGE_RESOURCE_DIRECTORY d;

	if (off >= w->size)
		return NULL;
	dir = pe_ptr(w->img, w->root + off, sizeof(d));
	if (!dir)
		return NULL;
	memcpy(&d, dir, sizeof(d));
	*count = d.NumberOfNamedEntries + d.NumberOfIdEntries;
	return pe_ptr(w->img, w->root + off + sizeof(d),
		      *count * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY));
}

/* The UTF-16 name of a named entry: a length-prefixed string. */
static const WCHAR *res_name(const struct res_walk *w, DWORD name, WORD *len)
{
	DWORD off = name & ~IMAGE_RESOURCE_NAME_IS_STRING;
	const BYTE *p = off < w->size ? pe_ptr(w->img, w->root + off, 2) : NULL;

	if (!p)
		return NULL;
	*len = rd16(p);
	return pe_ptr(w->img, w->root + off + 2, *len * 2u) ?
	       (const WCHAR *)(p + 2) : NULL;
}

/* Converts an entry's id or name for pe_resource; returns 0 or -1. */
static int res_ident(const struct res_walk *w, DWORD name, DWORD *id,
		     const char **str, char *buf, size_t bufsz)
{
	if (!(name & IMAGE_RESOURCE_NAME_IS_STRING)) {
		*id = name & 0xffff;
		*str = NULL;
		return 0;
	}
	WORD len;
	const WCHAR *s = res_name(w, name, &len);
	if (!s)
		FAIL("bad PE file: resource name out of range");
	WCHAR tmp[256];
	if (len > 255)
		len = 255;
	memcpy(tmp, s, len * 2u);
	utf16_to_utf8(tmp, len, buf, bufsz);
	*id = 0;
	*str = buf;
	return 0;
}

static int res_match(const struct res_walk *w, DWORD name, pe_resid want)
{
	if (want <= 0xffff)
		return !(name & IMAGE_RESOURCE_NAME_IS_STRING) &&
		       (name & 0xffff) == want;
	if (!(name & IMAGE_RESOURCE_NAME_IS_STRING))
		return 0;
	WORD len;
	const WCHAR *s = res_name(w, name, &len);
	if (!s)
		return 0;
	WCHAR tmp[256];
	if (len > 255)
		return 0;
	memcpy(tmp, s, len * 2u);
	return !utf16_ascii_casecmp(tmp, len, (const char *)want);
}

static int res_init(const struct pe_image *img, struct res_walk *w)
{
	const IMAGE_DATA_DIRECTORY *d = pe_dir(img, IMAGE_DIRECTORY_ENTRY_RESOURCE);

	if (!d)
		return 0;
	w->img = img;
	w->root = d->VirtualAddress;
	w->size = d->Size;
	return 1;
}

static void res_entry(const BYTE *entries, unsigned i, DWORD *name, DWORD *data)
{
	*name = rd32(entries + 8 * i);
	*data = rd32(entries + 8 * i + 4);
}

static int res_leaf(const struct res_walk *w, DWORD data,
		    struct pe_resource *out)
{
	IMAGE_RESOURCE_DATA_ENTRY de;
	const void *p;

	if ((data & IMAGE_RESOURCE_DATA_IS_DIRECTORY) || data >= w->size ||
	    !(p = pe_ptr(w->img, w->root + data, sizeof(de))))
		FAIL("bad PE file: resource data entry out of range");
	memcpy(&de, p, sizeof(de));
	out->entry_rva = w->root + data;
	out->data_rva = de.OffsetToData;
	out->size = de.Size;
	out->codepage = de.CodePage;
	return 0;
}

int pe_foreach_resource(const struct pe_image *img, pe_resource_fn fn,
			void *ctx)
{
	struct res_walk w;
	const BYTE *types, *names, *langs;
	unsigned ntypes, nnames, nlangs;
	char tbuf[256], nbuf[256];

	if (!res_init(img, &w))
		return 0;
	if (!(types = res_entries(&w, 0, &ntypes)))
		FAIL("bad PE file: resource directory out of range");

	for (unsigned ti = 0; ti < ntypes; ti++) {
		DWORD tname, tdata;
		res_entry(types, ti, &tname, &tdata);
		if (!(tdata & IMAGE_RESOURCE_DATA_IS_DIRECTORY) ||
		    !(names = res_entries(&w, tdata & ~IMAGE_RESOURCE_DATA_IS_DIRECTORY,
					  &nnames)))
			FAIL("bad PE file: resource type directory out of range");

		for (unsigned ni = 0; ni < nnames; ni++) {
			DWORD nname, ndata;
			res_entry(names, ni, &nname, &ndata);
			if (!(ndata & IMAGE_RESOURCE_DATA_IS_DIRECTORY) ||
			    !(langs = res_entries(&w, ndata & ~IMAGE_RESOURCE_DATA_IS_DIRECTORY,
						  &nlangs)))
				FAIL("bad PE file: resource name directory out of range");

			for (unsigned li = 0; li < nlangs; li++) {
				struct pe_resource r;
				DWORD lname, ldata;

				res_entry(langs, li, &lname, &ldata);
				if (res_ident(&w, tname, &r.type_id, &r.type_name,
					      tbuf, sizeof(tbuf)) ||
				    res_ident(&w, nname, &r.name_id, &r.name_name,
					      nbuf, sizeof(nbuf)) ||
				    res_leaf(&w, ldata, &r))
					return -1;
				r.lang = lname & 0xffff;
				if (fn(ctx, &r))
					return -1;
			}
		}
	}
	return 0;
}

/* Finds the subdirectory (level 1 or 2) matching want; 0 means any. */
static const BYTE *res_find_dir(const struct res_walk *w, const BYTE *entries,
				unsigned count, pe_resid want, unsigned *sub_count,
				DWORD *name)
{
	for (unsigned i = 0; i < count; i++) {
		DWORD n, data;
		res_entry(entries, i, &n, &data);
		if (want && !res_match(w, n, want))
			continue;
		if (!(data & IMAGE_RESOURCE_DATA_IS_DIRECTORY))
			return NULL;
		*name = n;
		return res_entries(w, data & ~IMAGE_RESOURCE_DATA_IS_DIRECTORY,
				   sub_count);
	}
	return NULL;
}

int pe_find_resource(const struct pe_image *img, pe_resid type,
		     pe_resid name, DWORD lang, struct pe_resource *out)
{
	struct res_walk w;
	const BYTE *types, *names, *langs;
	unsigned ntypes, nnames, nlangs;
	DWORD tname, nname;

	if (!res_init(img, &w) || !(types = res_entries(&w, 0, &ntypes)) ||
	    !(names = res_find_dir(&w, types, ntypes, type, &nnames, &tname)) ||
	    !(langs = res_find_dir(&w, names, nnames, name, &nlangs, &nname)) ||
	    !nlangs)
		FAIL("resource not found");

	/* Exact language, then neutral, then whatever comes first. */
	unsigned pick = 0;
	int found = 0;
	for (int pass = 0; pass < 2 && !found && lang != PE_RES_ANY_LANG; pass++) {
		DWORD want = pass == 0 ? lang : 0;
		for (unsigned i = 0; i < nlangs; i++) {
			DWORD l, d;
			res_entry(langs, i, &l, &d);
			if ((l & 0xffff) == want) {
				pick = i;
				found = 1;
				break;
			}
		}
	}

	DWORD lname, ldata;
	res_entry(langs, pick, &lname, &ldata);
	memset(out, 0, sizeof(*out));
	out->type_id = tname & IMAGE_RESOURCE_NAME_IS_STRING ? 0 : tname & 0xffff;
	out->name_id = nname & IMAGE_RESOURCE_NAME_IS_STRING ? 0 : nname & 0xffff;
	out->lang = lname & 0xffff;
	return res_leaf(&w, ldata, out);
}

/* Version resources: a tree of { wLength, wValueLength, wType, szKey, Value, Children }. */

struct ver_node {
	const WCHAR *key;
	size_t keylen;
	const BYTE *value;
	size_t value_size;
	const BYTE *children, *end;
};

static const BYTE *align4(const BYTE *base, const BYTE *p)
{
	return base + (((p - base) + 3) & ~(size_t)3);
}

static int ver_node(const BYTE *base, const BYTE *p, const BYTE *limit,
		    struct ver_node *n)
{
	if (limit - p < 6)
		return -1;
	WORD len = rd16(p), vlen = rd16(p + 2), type = rd16(p + 4);
	if (len < 6 || len > limit - p)
		return -1;
	n->end = p + len;

	const BYTE *k = p + 6;
	n->key = (const WCHAR *)k;
	n->keylen = 0;
	while (k + 2 <= n->end && rd16(k)) {
		k += 2;
		n->keylen++;
	}
	if (k + 2 > n->end)
		return -1;

	const BYTE *v = align4(base, k + 2);
	if (v > n->end)
		v = n->end;
	n->value = v;
	n->value_size = type == 1 ? vlen * 2u : vlen;	/* text is in WCHARs */
	if (n->value_size > (size_t)(n->end - v))
		n->value_size = n->end - v;
	n->children = align4(base, v + n->value_size);
	if (n->children > n->end)
		n->children = n->end;
	return 0;
}

/* Copies a node's key to UTF-8. */
static void ver_key(const struct ver_node *n, char *out, size_t outsz)
{
	WCHAR tmp[128];
	size_t len = n->keylen < 128 ? n->keylen : 127;

	memcpy(tmp, n->key, len * 2);
	utf16_to_utf8(tmp, len, out, outsz);
}

int pe_version_info(const struct pe_image *img, VS_FIXEDFILEINFO *fixed,
		    pe_version_fn fn, void *ctx)
{
	struct pe_resource res;
	struct ver_node root, sfi, table, str;
	const BYTE *data;
	char key[128];

	if (pe_find_resource(img, RT_VERSION, 0, PE_RES_ANY_LANG, &res))
		FAIL("no version resource");
	data = pe_ptr(img, res.data_rva, res.size);
	if (!data || ver_node(data, data, data + res.size, &root))
		FAIL("bad version resource");

	if (fixed) {
		memset(fixed, 0, sizeof(*fixed));
		if (root.value_size >= sizeof(*fixed) &&
		    rd32(root.value) == VS_FFI_SIGNATURE)
			memcpy(fixed, root.value, sizeof(*fixed));
	}
	if (!fn)
		return 0;

	for (const BYTE *p = root.children; p < root.end;
	     p = align4(data, sfi.end)) {
		if (ver_node(data, p, root.end, &sfi))
			break;
		ver_key(&sfi, key, sizeof(key));
		if (strcmp(key, "StringFileInfo"))
			continue;

		for (const BYTE *q = sfi.children; q < sfi.end;
		     q = align4(data, table.end)) {
			char tname[32];
			if (ver_node(data, q, sfi.end, &table))
				break;
			ver_key(&table, tname, sizeof(tname));

			for (const BYTE *s = table.children; s < table.end;
			     s = align4(data, str.end)) {
				char value[1024];
				WCHAR tmp[512];
				size_t n;

				if (ver_node(data, s, table.end, &str))
					break;
				ver_key(&str, key, sizeof(key));
				n = str.value_size / 2;
				if (n > 511)
					n = 511;
				memcpy(tmp, str.value, n * 2);
				utf16_to_utf8(tmp, n, value, sizeof(value));
				if (fn(ctx, tname, key, value))
					return -1;
			}
		}
	}
	return 0;
}
