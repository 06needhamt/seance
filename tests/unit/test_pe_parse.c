/*
 * PE parsing without loading: headers, exports, imports, TLS, resources,
 * version info, and robustness against truncated and corrupted files.
 */
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "loader/pe.h"
#include "test.h"

static const IMAGE_SECTION_HEADER *section_of(const struct pe_image *img,
					      DWORD rva)
{
	for (unsigned i = 0; i < img->nsections; i++) {
		const IMAGE_SECTION_HEADER *s = &img->sections[i];
		if (rva >= s->VirtualAddress &&
		    rva - s->VirtualAddress < s->Misc.VirtualSize)
			return s;
	}
	return NULL;
}

static int in_section(const struct pe_image *img, DWORD rva, const char *name)
{
	const IMAGE_SECTION_HEADER *s = section_of(img, rva);
	return s && !strncmp((const char *)s->Name, name, 8);
}

static void test_bad_input(void)
{
	static const char junk[] =
		"this is not a PE file at all, just text that is long enough "
		"to hold a DOS header";
	BYTE buf[512];

	CHECK(!pe_parse(junk, sizeof(junk)));
	CHECK(strstr(vfw_error(), "no MZ signature") != NULL);
	CHECK(!pe_parse(junk, 3));
	CHECK(strstr(vfw_error(), "too short") != NULL);
	CHECK(!pe_open("/nonexistent/file.dll"));
	CHECK(strstr(vfw_error(), "/nonexistent/file.dll") != NULL);

	/* MZ with e_lfanew pointing past the end. */
	memset(buf, 0, sizeof(buf));
	buf[0] = 'M';
	buf[1] = 'Z';
	buf[60] = 0xf0;
	buf[61] = 0x01;
	CHECK(!pe_parse(buf, sizeof(buf)));
	CHECK(strstr(vfw_error(), "out of range") != NULL);
}

static void test_machine_and_magic(void)
{
	void *buf;
	size_t len;

	CHECK_EQ(vfw_read_file(fixture("pe_basic.dll"), &buf, &len), 0);
	BYTE *p = buf;
	DWORD nt;
	memcpy(&nt, p + 60, 4);

	/* A 64-bit (PE32+) optional header. */
	p[nt + 24] = 0x0b;
	p[nt + 25] = 0x02;
	CHECK(!pe_parse(buf, len));
	CHECK(strstr(vfw_error(), "64-bit") != NULL);
	p[nt + 25] = 0x01;

	/* An x86-64 machine type. */
	p[nt + 4] = 0x64;
	p[nt + 5] = 0x86;
	CHECK(!pe_parse(buf, len));
	CHECK(strstr(vfw_error(), "machine type 0x8664") != NULL);
	p[nt + 4] = 0x4c;
	p[nt + 5] = 0x01;

	struct pe_image *img = pe_parse(buf, len);
	CHECK(img != NULL);
	pe_free(img);
	free(buf);
}

static int count_export(void *ctx, const struct pe_export *e)
{
	(*(int *)ctx)++;
	return 0;
}

static void test_exports(void)
{
	struct pe_image *img = pe_open(fixture("pe_basic.dll"));
	struct pe_export e;
	int count = 0;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK(img->base == NULL);
	CHECK_EQ(img->nt.OptionalHeader.ImageBase, 0x10000000);
	CHECK_STR(pe_export_dll_name(img), "PE_BASIC.DLL");
	CHECK_EQ(pe_foreach_export(img, count_export, &count), 0);
	CHECK_EQ(count, 7);

	/* Ordinals from pe_basic.def. */
	CHECK_EQ(pe_find_export(img, "basic_add", 0, &e), 0);
	CHECK_EQ(e.ordinal, 1);
	CHECK(e.forwarder == NULL);
	CHECK(in_section(img, e.rva, ".text"));
	CHECK_EQ(pe_find_export(img, "basic_mul", 1, &e), 0);	/* hint right */
	CHECK_EQ(e.ordinal, 2);
	CHECK_EQ(pe_find_export(img, "basic_mul", 5, &e), 0);	/* hint wrong */
	CHECK_EQ(e.ordinal, 2);
	CHECK_EQ(pe_find_export(img, "basic_on_detach", 0, &e), 0);
	CHECK_EQ(e.ordinal, 12);

	/* A data export lives in .data. */
	CHECK_EQ(pe_find_export(img, "basic_data", 0, &e), 0);
	CHECK_EQ(e.ordinal, 8);
	CHECK(in_section(img, e.rva, ".data"));

	/* NONAME: reachable by ordinal only. */
	CHECK_EQ(pe_find_export(img, "basic_secret", 0, &e), -1);
	CHECK(strstr(vfw_error(), "no export named basic_secret") != NULL);
	CHECK_EQ(pe_find_export_ordinal(img, 7, &e), 0);
	CHECK(e.name == NULL);
	CHECK(in_section(img, e.rva, ".text"));

	/* Ordinal lookups also report the name. */
	CHECK_EQ(pe_find_export_ordinal(img, 10, &e), 0);
	CHECK_STR(e.name, "basic_attach_count");

	/* Gaps in the ordinal range, and out-of-range ordinals. */
	CHECK_EQ(pe_find_export_ordinal(img, 3, &e), -1);
	CHECK_EQ(pe_find_export_ordinal(img, 0, &e), -1);
	CHECK_EQ(pe_find_export_ordinal(img, 13, &e), -1);
	pe_free(img);

	img = pe_open(fixture("pe_dep.dll"));
	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_EQ(pe_find_export(img, "fwd_add", 0, &e), 0);
	CHECK_EQ(e.ordinal, 3);
	CHECK_STR(e.forwarder, "PE_BASIC.basic_add");
	CHECK_EQ(pe_find_export(img, "dep_compute", 0, &e), 0);
	CHECK(e.forwarder == NULL);
	pe_free(img);

	/* No export table at all. */
	img = pe_open(fixture("pe_missing.dll"));
	if (img) {
		CHECK_EQ(pe_find_export(img, "anything", 0, &e), -1);
		CHECK_EQ(pe_find_export(img, "missing_call", 0, &e), 0);
		pe_free(img);
	}
}

struct import_list {
	char text[16][64];
	int n;
};

static int list_import(void *ctx, const struct pe_import *imp)
{
	struct import_list *l = ctx;

	if (l->n < 16) {
		if (imp->name)
			snprintf(l->text[l->n], 64, "%s!%s", imp->dll, imp->name);
		else
			snprintf(l->text[l->n], 64, "%s!#%u", imp->dll,
				 imp->ordinal);
	}
	l->n++;
	return 0;
}

static int stop_import(void *ctx, const struct pe_import *imp)
{
	(*(int *)ctx)++;
	return -1;
}

static void test_imports(void)
{
	struct pe_image *img = pe_open(fixture("pe_dep.dll"));
	struct import_list l = { .n = 0 };
	int calls = 0;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_EQ(pe_foreach_import(img, list_import, &l), 0);
	CHECK_EQ(l.n, 4);
	CHECK_STR(l.text[0], "PE_BASIC.DLL!basic_add");
	CHECK_STR(l.text[1], "PE_BASIC.DLL!basic_data");
	CHECK_STR(l.text[2], "PE_BASIC.DLL!basic_mul");
	CHECK_STR(l.text[3], "PE_BASIC.DLL!#7");

	/* The callback can stop the walk. */
	CHECK_EQ(pe_foreach_import(img, stop_import, &calls), -1);
	CHECK_EQ(calls, 1);

	/* Binding needs a mapped image. */
	CHECK_EQ(pe_bind_imports(img, NULL, NULL), -1);
	pe_free(img);

	img = pe_open(fixture("pe_basic.dll"));
	l.n = 0;
	CHECK_EQ(pe_foreach_import(img, list_import, &l), 0);
	CHECK_EQ(l.n, 0);
	CHECK_EQ(pe_foreach_delay_import(img, list_import, &l), 0);
	CHECK_EQ(l.n, 0);
	pe_free(img);
}

static int count_tls(void *ctx, DWORD rva)
{
	(*(int *)ctx)++;
	return 0;
}

static void test_tls(void)
{
	struct pe_image *img = pe_open(fixture("pe_tls.dll"));
	int n = 0;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_EQ(pe_foreach_tls_callback(img, count_tls, &n), 0);
	CHECK_EQ(n, 1);
	pe_free(img);

	img = pe_open(fixture("pe_basic.dll"));
	n = 0;
	CHECK_EQ(pe_foreach_tls_callback(img, count_tls, &n), 0);
	CHECK_EQ(n, 0);
	pe_free(img);
}

struct version_seen {
	int description, product;
};

static int check_version(void *ctx, const char *table, const char *key,
			 const char *value)
{
	struct version_seen *v = ctx;

	CHECK_STR(table, "040904b0");
	if (!strcmp(key, "FileDescription")) {
		CHECK_STR(value, "vfw-harness PE test fixture");
		v->description = 1;
	}
	if (!strcmp(key, "ProductName")) {
		CHECK_STR(value, "vfw-harness");
		v->product = 1;
	}
	return 0;
}

struct res_types {
	int string, version, named;
};

static int list_resource(void *ctx, const struct pe_resource *r)
{
	struct res_types *t = ctx;

	if (r->type_id == RT_STRING)
		t->string++;
	if (r->type_id == RT_VERSION)
		t->version++;
	if (r->type_name && !strcmp(r->type_name, "CODECDATA")) {
		CHECK_STR(r->name_name, "SETTINGS");
		t->named++;
	}
	return 0;
}

static void test_resources(void)
{
	struct pe_image *img = pe_open(fixture("pe_basic.dll"));
	struct pe_resource r;
	struct res_types types = { 0, 0, 0 };
	struct version_seen seen = { 0, 0 };
	VS_FIXEDFILEINFO fixed;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_EQ(pe_foreach_resource(img, list_resource, &types), 0);
	CHECK_EQ(types.string, 1);
	CHECK_EQ(types.version, 1);
	CHECK_EQ(types.named, 1);

	/* Strings 0-15 are in string table block 1. */
	CHECK_EQ(pe_find_resource(img, RT_STRING, 1, PE_RES_ANY_LANG, &r), 0);
	CHECK(r.size > 0);
	CHECK(pe_ptr(img, r.data_rva, r.size) != NULL);
	CHECK_EQ(pe_find_resource(img, RT_STRING, 2, PE_RES_ANY_LANG, &r), -1);
	CHECK_EQ(pe_find_resource(img, RT_BITMAP, 1, PE_RES_ANY_LANG, &r), -1);

	/* Language fallback: exact, then neutral, then the first one. */
	CHECK_EQ(pe_find_resource(img, RT_STRING, 1, 0x409, &r), 0);
	CHECK_EQ(pe_find_resource(img, RT_STRING, 1, 0x407, &r), 0);

	/* String names match without regard to case. */
	CHECK_EQ(pe_find_resource(img, (pe_resid)"codecdata",
				  (pe_resid)"Settings", PE_RES_ANY_LANG, &r), 0);
	const char *data = pe_ptr(img, r.data_rva, r.size);
	CHECK(data && r.size >= 14 && !memcmp(data, "codec settings", 14));
	CHECK_EQ(pe_find_resource(img, (pe_resid)"CODECDATA",
				  (pe_resid)"OTHER", PE_RES_ANY_LANG, &r), -1);

	CHECK_EQ(pe_version_info(img, &fixed, check_version, &seen), 0);
	CHECK(seen.description && seen.product);
	CHECK_EQ(fixed.dwSignature, VS_FFI_SIGNATURE);
	CHECK_EQ(fixed.dwFileVersionMS, 0x00010002);
	CHECK_EQ(fixed.dwFileVersionLS, 0x00030004);
	CHECK_EQ(fixed.dwProductVersionMS, 0x00010002);
	pe_free(img);

	img = pe_open(fixture("pe_dep.dll"));
	CHECK_EQ(pe_foreach_resource(img, list_resource, &types), 0);
	CHECK_EQ(pe_version_info(img, NULL, NULL, NULL), -1);
	pe_free(img);
}

/* Walks everything, touching every string the parser hands out. */
static size_t sink;

static int touch_export(void *c, const struct pe_export *e)
{
	sink += e->name ? strlen(e->name) : 0;
	sink += e->forwarder ? strlen(e->forwarder) : 0;
	return 0;
}

static int touch_import(void *c, const struct pe_import *i)
{
	sink += strlen(i->dll) + (i->name ? strlen(i->name) : 0);
	return 0;
}

static int touch_tls(void *c, DWORD rva)
{
	sink += rva;
	return 0;
}

static int touch_resource(void *c, const struct pe_resource *r)
{
	sink += r->type_name ? strlen(r->type_name) : 0;
	sink += r->name_name ? strlen(r->name_name) : 0;
	return 0;
}

static int touch_version(void *c, const char *t, const char *k, const char *v)
{
	sink += strlen(t) + strlen(k) + strlen(v);
	return 0;
}

static void exercise(const struct pe_image *img)
{
	struct pe_export e;
	struct pe_resource r;
	VS_FIXEDFILEINFO fixed;

	pe_export_dll_name(img);
	pe_foreach_export(img, touch_export, NULL);
	pe_find_export(img, "basic_add", 3, &e);
	pe_find_export_ordinal(img, 7, &e);
	pe_foreach_import(img, touch_import, NULL);
	pe_foreach_delay_import(img, touch_import, NULL);
	pe_foreach_tls_callback(img, touch_tls, NULL);
	pe_foreach_resource(img, touch_resource, NULL);
	pe_find_resource(img, (pe_resid)"CODECDATA", (pe_resid)"SETTINGS",
			 0x409, &r);
	pe_version_info(img, &fixed, touch_version, NULL);
}

static DWORD rng_state = 0x12345678;

static DWORD rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void test_truncation_and_corruption(const char *name)
{
	void *orig;
	size_t len;

	CHECK_EQ(vfw_read_file(fixture(name), &orig, &len), 0);

	/*
	 * A truncated file parses exactly when every section's raw data is
	 * still there (mingw appends a symbol table that is not needed).
	 */
	struct pe_image *full = pe_parse(orig, len);
	size_t need = 0;
	CHECK(full != NULL);
	for (unsigned i = 0; full && i < full->nsections; i++) {
		const IMAGE_SECTION_HEADER *s = &full->sections[i];
		size_t end = (size_t)s->PointerToRawData + s->SizeOfRawData;
		if (s->SizeOfRawData && end > need)
			need = end;
	}
	pe_free(full);
	CHECK(need > 0 && need <= len);

	int wrong = 0;
	for (size_t n = 0; n < len; n++) {
		struct pe_image *img = pe_parse(orig, n);
		if (!!img != (n >= need))
			wrong++;
		if (img) {
			exercise(img);
			pe_free(img);
		}
	}
	CHECK_EQ(wrong, 0);

	/* Random corruption; whatever still parses must walk safely. */
	BYTE *buf = malloc(len);
	int survived = 0, mapped = 0;
	for (int iter = 0; iter < 3000; iter++) {
		memcpy(buf, orig, len);
		int flips = 1 + rng() % 4;
		for (int f = 0; f < flips; f++) {
			/* Mostly hit the headers and directories. */
			size_t at = rng() % (iter % 2 ? len : (len < 1024 ? len : 1024));
			buf[at] = rng();
		}
		struct pe_image *img = pe_parse(buf, len);
		if (img) {
			survived++;
			exercise(img);
			/*
			 * Some get mapped and relocated too (never run), and
			 * walked again through the mapped image.
			 */
			if (iter % 8 == 0 &&
			    !pe_map(img, iter % 16 ? 0 : PE_MAP_FORCE_RELOC)) {
				mapped++;
				exercise(img);
				pe_tls_init(img);
			}
			pe_free(img);
		}
	}
	CHECK(survived > 0);	/* the walkers really ran on damaged images */
	CHECK(mapped > 0);
	free(buf);
	free(orig);
}

int main(void)
{
	test_bad_input();
	test_machine_and_magic();
	test_exports();
	test_imports();
	test_tls();
	test_resources();
	test_truncation_and_corruption("pe_basic.dll");
	test_truncation_and_corruption("pe_dep.dll");
	test_truncation_and_corruption("pe_tls.dll");
	test_truncation_and_corruption("testcodec.dll");
	return test_summary("test_pe_parse");
}
