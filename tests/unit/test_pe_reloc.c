/*
 * Base relocation arithmetic, and mapping images at and away from their
 * preferred base.
 */
#include <stdint.h>
#include <string.h>

#include "common/error.h"
#include "loader/pe.h"
#include "test.h"

static void put16(BYTE *p, WORD v)
{
	memcpy(p, &v, 2);
}

static void put32(BYTE *p, DWORD v)
{
	memcpy(p, &v, 4);
}

static WORD get16(const BYTE *p)
{
	WORD v;
	memcpy(&v, p, 2);
	return v;
}

static DWORD get32(const BYTE *p)
{
	DWORD v;
	memcpy(&v, p, 4);
	return v;
}

/* Writes a block header plus entries; returns the bytes written. */
static size_t block(BYTE *out, DWORD va, const WORD *entries, size_t n)
{
	put32(out, va);
	put32(out + 4, 8 + 2 * n);
	for (size_t i = 0; i < n; i++)
		put16(out + 8 + 2 * i, entries[i]);
	return 8 + 2 * n;
}

#define ENTRY(type, off) ((WORD)(((type) << 12) | (off)))

static void test_relocate(void)
{
	static BYTE image[0x3000];
	BYTE relocs[64];
	const DWORD delta = 0x00230010;

	put32(image + 0x1000, 0x10001234);	/* HIGHLOW */
	put16(image + 0x1010, 0x1000);		/* HIGH */
	put16(image + 0x1020, 0x5678);		/* LOW */
	put16(image + 0x2000, 0x1235);		/* HIGHADJ: high half of 0x12348000 */
	put32(image + 0x2ffc, 0xaaaaaaaa);	/* untouched */

	const WORD b1[] = {
		ENTRY(IMAGE_REL_BASED_HIGHLOW, 0x000),
		ENTRY(IMAGE_REL_BASED_HIGH, 0x010),
		ENTRY(IMAGE_REL_BASED_LOW, 0x020),
		ENTRY(IMAGE_REL_BASED_ABSOLUTE, 0),	/* padding */
	};
	const WORD b2[] = {
		ENTRY(IMAGE_REL_BASED_HIGHADJ, 0x000), 0x8000,
	};
	size_t n = block(relocs, 0x1000, b1, 4);
	n += block(relocs + n, 0x2000, b2, 2);

	CHECK_EQ(pe_relocate(image, sizeof(image), relocs, n, delta), 0);
	CHECK_EQ(get32(image + 0x1000), 0x10001234 + delta);
	CHECK_EQ(get16(image + 0x1010), 0x1000 + HIWORD(delta));
	CHECK_EQ(get16(image + 0x1020), (WORD)(0x5678 + LOWORD(delta)));
	CHECK_EQ(get32(image + 0x2ffc), 0xaaaaaaaa);

	/* HIGHADJ: the new high half, combined with the signed low half. */
	DWORD want = 0x12348000 + delta;
	DWORD got = ((DWORD)get16(image + 0x2000) << 16) + (DWORD)(int16_t)0x8000;
	CHECK_EQ(got >> 16, want >> 16);

	/* A zero terminator block ends the list early. */
	BYTE term[16] = { 0 };
	CHECK_EQ(pe_relocate(image, sizeof(image), term, sizeof(term), delta), 0);

	/* Malformed input. */
	BYTE bad[16];
	put32(bad, 0x1000);
	put32(bad + 4, 4);			/* SizeOfBlock < header */
	CHECK_EQ(pe_relocate(image, sizeof(image), bad, 8, delta), -1);
	put32(bad + 4, 64);			/* runs past the data */
	CHECK_EQ(pe_relocate(image, sizeof(image), bad, 16, delta), -1);

	const WORD out_of_range[] = { ENTRY(IMAGE_REL_BASED_HIGHLOW, 0xffe) };
	n = block(relocs, 0x2000, out_of_range, 1);
	CHECK_EQ(pe_relocate(image, sizeof(image), relocs, n, delta), -1);
	CHECK(strstr(vfw_error(), "outside the image") != NULL);

	const WORD unknown[] = { ENTRY(9, 0) };
	n = block(relocs, 0x1000, unknown, 1);
	CHECK_EQ(pe_relocate(image, sizeof(image), relocs, n, delta), -1);
	CHECK(strstr(vfw_error(), "unsupported relocation type 9") != NULL);

	const WORD lone_highadj[] = { ENTRY(IMAGE_REL_BASED_HIGHADJ, 0) };
	n = block(relocs, 0x1000, lone_highadj, 1);
	CHECK_EQ(pe_relocate(image, sizeof(image), relocs, n, delta), -1);
}

typedef int (*int_fn)(void);
typedef const char *(*str_fn)(void);

static void *export_addr(struct pe_image *img, const char *name)
{
	struct pe_export e;

	if (pe_find_export(img, name, 0, &e))
		return NULL;
	return img->base + e.rva;
}

/* Maps an import-free fixture and makes it runnable. */
static struct pe_image *map_fixture(const char *name, unsigned flags)
{
	struct pe_image *img = pe_open(fixture(name));

	if (!img)
		return NULL;
	if (pe_map(img, flags) || pe_protect(img)) {
		pe_free(img);
		return NULL;
	}
	return img;
}

static DWORD mapped_image_base(const struct pe_image *img)
{
	const IMAGE_DOS_HEADER *dos = (const void *)img->base;
	const IMAGE_NT_HEADERS32 *nt = (const void *)(img->base + dos->e_lfanew);
	return nt->OptionalHeader.ImageBase;
}

static void check_reloc_fixture(struct pe_image *img)
{
	int_fn check = (int_fn)export_addr(img, "reloc_check");
	str_fn message = (str_fn)export_addr(img, "reloc_message");

	CHECK(check && message);
	if (!check || !message)
		return;
	CHECK_EQ(check(), 84);
	const char *msg = message();
	CHECK((const BYTE *)msg >= img->base &&
	      (const BYTE *)msg < img->base + img->map_len);
	CHECK_STR(msg, "relocated");
}

static void test_map(void)
{
	/* The preferred base is free, so no relocation is needed. */
	struct pe_image *basic = map_fixture("pe_basic.dll", 0);
	CHECK(basic != NULL);
	if (!basic)
		return;
	CHECK_EQ((uintptr_t)basic->base, 0x10000000);
	CHECK_EQ(basic->relocated, 0);
	int_fn add = (int_fn)export_addr(basic, "basic_add");
	CHECK(add != NULL);

	/* pe_reloc wants the same base, so it has to move. */
	struct pe_image *img = map_fixture("pe_reloc.dll", 0);
	CHECK(img != NULL);
	if (img) {
		CHECK(img->relocated);
		CHECK((uintptr_t)img->base != 0x10000000);
		CHECK_EQ((uintptr_t)img->base % 0x10000, 0);	/* 64K aligned */
		CHECK_EQ(mapped_image_base(img), (DWORD)(uintptr_t)img->base);
		check_reloc_fixture(img);
		pe_free(img);
	}

	/* Without relocations it cannot move. */
	img = pe_open(fixture("pe_noreloc.dll"));
	CHECK(img != NULL);
	if (img) {
		CHECK_EQ(pe_map(img, 0), -1);
		CHECK(strstr(vfw_error(), "no relocations") != NULL);
		CHECK(img->base == NULL);
		pe_free(img);
	}
	pe_free(basic);

	/* With the base free again, pe_noreloc loads where it wants. */
	img = map_fixture("pe_noreloc.dll", 0);
	CHECK(img != NULL);
	if (img) {
		CHECK_EQ((uintptr_t)img->base, 0x10000000);
		check_reloc_fixture(img);
		pe_free(img);
	}

	/* Forced relocation: moves even though the base is free. */
	img = map_fixture("pe_reloc.dll", PE_MAP_FORCE_RELOC);
	CHECK(img != NULL);
	if (img) {
		CHECK(img->relocated);
		CHECK((uintptr_t)img->base != 0x10000000);
		check_reloc_fixture(img);
		pe_free(img);
	}

	img = pe_open(fixture("pe_noreloc.dll"));
	if (img) {
		CHECK_EQ(pe_map(img, PE_MAP_FORCE_RELOC), -1);
		pe_free(img);
	}

	/* Mapping twice is an error. */
	img = map_fixture("pe_reloc.dll", 0);
	if (img) {
		CHECK_EQ(pe_map(img, 0), -1);
		pe_free(img);
	}
}

int main(void)
{
	test_relocate();
	test_map();
	return test_summary("test_pe_reloc");
}
