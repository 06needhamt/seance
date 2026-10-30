/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance info <dll>: describe a PE image without loading or running it.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "commands.h"
#include "dshow/discover.h"
#include "common/error.h"
#include "loader/pe.h"
#include "runtime/builtin.h"

struct flag_name {
	DWORD bit;
	const char *name;
};

static const struct flag_name file_flags[] = {
	{ IMAGE_FILE_RELOCS_STRIPPED, "RELOCS_STRIPPED" },
	{ IMAGE_FILE_EXECUTABLE_IMAGE, "EXECUTABLE_IMAGE" },
	{ IMAGE_FILE_LARGE_ADDRESS_AWARE, "LARGE_ADDRESS_AWARE" },
	{ IMAGE_FILE_32BIT_MACHINE, "32BIT_MACHINE" },
	{ IMAGE_FILE_DEBUG_STRIPPED, "DEBUG_STRIPPED" },
	{ IMAGE_FILE_DLL, "DLL" },
	{ 0, NULL }
};

static const struct flag_name dll_flags[] = {
	{ IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, "DYNAMIC_BASE" },
	{ IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY, "FORCE_INTEGRITY" },
	{ IMAGE_DLLCHARACTERISTICS_NX_COMPAT, "NX_COMPAT" },
	{ IMAGE_DLLCHARACTERISTICS_NO_ISOLATION, "NO_ISOLATION" },
	{ IMAGE_DLLCHARACTERISTICS_NO_SEH, "NO_SEH" },
	{ IMAGE_DLLCHARACTERISTICS_NO_BIND, "NO_BIND" },
	{ IMAGE_DLLCHARACTERISTICS_WDM_DRIVER, "WDM_DRIVER" },
	{ IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE, "TERMINAL_SERVER_AWARE" },
	{ 0, NULL }
};

static const char *const resource_types[] = {
	[RT_CURSOR] = "RT_CURSOR", [RT_BITMAP] = "RT_BITMAP",
	[RT_ICON] = "RT_ICON", [RT_MENU] = "RT_MENU",
	[RT_DIALOG] = "RT_DIALOG", [RT_STRING] = "RT_STRING",
	[RT_FONTDIR] = "RT_FONTDIR", [RT_FONT] = "RT_FONT",
	[RT_ACCELERATOR] = "RT_ACCELERATOR", [RT_RCDATA] = "RT_RCDATA",
	[RT_MESSAGETABLE] = "RT_MESSAGETABLE",
	[RT_GROUP_CURSOR] = "RT_GROUP_CURSOR",
	[RT_GROUP_ICON] = "RT_GROUP_ICON", [RT_VERSION] = "RT_VERSION",
	[RT_DLGINCLUDE] = "RT_DLGINCLUDE", [RT_PLUGPLAY] = "RT_PLUGPLAY",
	[RT_VXD] = "RT_VXD", [RT_ANICURSOR] = "RT_ANICURSOR",
	[RT_ANIICON] = "RT_ANIICON", [RT_HTML] = "RT_HTML",
	[RT_MANIFEST] = "RT_MANIFEST",
};

static int status;

static void report_error(void)
{
	printf("  error: %s\n", vfw_error());
	status = 1;
}

static void print_flags(DWORD value, const struct flag_name *names)
{
	int any = 0;

	for (const struct flag_name *f = names; f->name; f++) {
		if (value & f->bit) {
			printf(" %s", f->name);
			any = 1;
		}
	}
	if (!any)
		printf(" none");
	printf("\n");
}

static const char *subsystem_name(WORD s)
{
	switch (s) {
	case IMAGE_SUBSYSTEM_NATIVE: return "native";
	case IMAGE_SUBSYSTEM_WINDOWS_GUI: return "Windows GUI";
	case IMAGE_SUBSYSTEM_WINDOWS_CUI: return "Windows console";
	default: return "other";
	}
}

static void print_header(const char *path, const struct pe_image *img)
{
	const IMAGE_FILE_HEADER *fh = &img->nt.FileHeader;
	const IMAGE_OPTIONAL_HEADER32 *o = &img->nt.OptionalHeader;
	time_t ts = fh->TimeDateStamp;
	char when[64] = "";

	strftime(when, sizeof(when), " (%Y-%m-%d %H:%M:%S UTC)", gmtime(&ts));
	printf("file:            %s\n", path);
	printf("type:            %s, i386\n",
	       fh->Characteristics & IMAGE_FILE_DLL ? "DLL" : "executable");
	struct pe_export e;
	if (!pe_find_export(img, "DriverProc", 0, &e))
		printf("kind:            VfW codec, ACM driver or other driver "
		       "(DriverProc)\n");
	else if (dshow_is_server(img))
		printf("kind:            COM server, perhaps DirectShow filters "
		       "or DMOs (seance register --dry-run lists them)\n");
	printf("timestamp:       0x%08x%s\n", fh->TimeDateStamp, when);
	printf("linker:          %u.%u\n", o->MajorLinkerVersion,
	       o->MinorLinkerVersion);
	printf("image base:      0x%08x\n", o->ImageBase);
	printf("image size:      0x%08x\n", o->SizeOfImage);
	printf("entry point:     0x%08x\n", o->AddressOfEntryPoint);
	printf("subsystem:       %s (%u), OS %u.%u\n", subsystem_name(o->Subsystem),
	       o->Subsystem, o->MajorOperatingSystemVersion,
	       o->MinorOperatingSystemVersion);
	printf("characteristics:");
	print_flags(fh->Characteristics, file_flags);
	printf("dll flags:      ");
	print_flags(o->DllCharacteristics, dll_flags);
}

static void print_sections(const struct pe_image *img)
{
	printf("\nsections (%u):\n", img->nsections);
	printf("  %-8s  %-10s  %-10s  %-10s  flags\n", "name", "address",
	       "vsize", "raw size");
	for (unsigned i = 0; i < img->nsections; i++) {
		const IMAGE_SECTION_HEADER *s = &img->sections[i];
		DWORD c = s->Characteristics;
		char name[9];

		memcpy(name, s->Name, 8);
		name[8] = '\0';
		printf("  %-8s  0x%08x  0x%08x  0x%08x  %c%c%c%s%s%s\n", name,
		       s->VirtualAddress, s->Misc.VirtualSize, s->SizeOfRawData,
		       c & IMAGE_SCN_MEM_READ ? 'r' : '-',
		       c & IMAGE_SCN_MEM_WRITE ? 'w' : '-',
		       c & IMAGE_SCN_MEM_EXECUTE ? 'x' : '-',
		       c & IMAGE_SCN_CNT_CODE ? " code" : "",
		       c & IMAGE_SCN_CNT_INITIALIZED_DATA ? " data" : "",
		       c & IMAGE_SCN_CNT_UNINITIALIZED_DATA ? " bss" : "");
	}
}

static int print_export(void *ctx, const struct pe_export *e)
{
	int *count = ctx;
	const char *name = e->name ? e->name : "(ordinal only)";

	if (e->forwarder)
		printf("  %7u  %-10s  %s -> %s\n", e->ordinal, "forwarder",
		       name, e->forwarder);
	else
		printf("  %7u  0x%08x  %s\n", e->ordinal, e->rva, name);
	(*count)++;
	return 0;
}

static int count_export(void *ctx, const struct pe_export *e)
{
	(*(int *)ctx)++;
	return 0;
}

static void print_exports(const struct pe_image *img)
{
	int count = 0;
	const char *dll = pe_export_dll_name(img);

	if (pe_foreach_export(img, count_export, &count)) {
		printf("\nexports:\n");
		report_error();
		return;
	}
	printf("\nexports (%d)", count);
	if (dll)
		printf(", module name %s", dll);
	printf(":\n");
	if (!count)
		return;
	printf("  %7s  %-10s  name\n", "ordinal", "rva");
	count = 0;
	pe_foreach_export(img, print_export, &count);
}

struct import_state {
	const char *dll;	/* DLL of the previous import */
	const struct builtin_dll *builtin;
	int count, missing;
};

static int print_import(void *ctx, const struct pe_import *imp)
{
	struct import_state *st = ctx;

	if (!st->dll || strcmp(st->dll, imp->dll)) {
		st->builtin = builtin_find(imp->dll);
		printf("  %s%s\n", imp->dll,
		       st->builtin ? " (built-in)" : " (loaded from disk)");
	}
	st->dll = imp->dll;

	/* Whether the harness implements it; only known for built-in DLLs. */
	const char *note = "";
	if (st->builtin &&
	    !(imp->name ? builtin_export(st->builtin, imp->name) :
			  builtin_export_ordinal(st->builtin, imp->ordinal))) {
		note = "  [not implemented]";
		st->missing++;
	}
	if (imp->name)
		printf("    %s%s\n", imp->name, note);
	else
		printf("    #%u%s\n", imp->ordinal, note);
	st->count++;
	return 0;
}

static void print_imports(const struct pe_image *img, int delayed)
{
	struct import_state st = { NULL, NULL, 0, 0 };
	int r;

	printf("\n%s:\n", delayed ? "delay imports" : "imports");
	r = delayed ? pe_foreach_delay_import(img, print_import, &st) :
		      pe_foreach_import(img, print_import, &st);
	if (r)
		report_error();
	else if (!st.count)
		printf("  none\n");
	else if (st.missing)
		printf("  %d of %d not implemented by the harness\n",
		       st.missing, st.count);
	const char *thunk = delayed ? NULL : pe_win9x_thunk(img);
	if (thunk)
		printf("  %s: a Windows 9x DLL that thunks to 16-bit code, "
		       "which doesn't run here\n", thunk);
}

static int print_tls_callback(void *ctx, DWORD rva)
{
	printf("  callback at 0x%08x\n", rva);
	(*(int *)ctx)++;
	return 0;
}

static void print_tls(const struct pe_image *img)
{
	int count = 0;

	if (!pe_dir(img, IMAGE_DIRECTORY_ENTRY_TLS))
		return;
	printf("\ntls:\n");
	if (pe_foreach_tls_callback(img, print_tls_callback, &count))
		report_error();
	else if (!count)
		printf("  no callbacks\n");
}

#define MAX_RES_TYPES 64

struct res_count {
	DWORD id;
	char name[64];
	unsigned count;
	DWORD bytes;
};

struct res_state {
	struct res_count types[MAX_RES_TYPES];
	unsigned ntypes;
};

static int count_resource(void *ctx, const struct pe_resource *r)
{
	struct res_state *st = ctx;
	struct res_count *t = NULL;

	for (unsigned i = 0; i < st->ntypes && !t; i++)
		if (st->types[i].id == r->type_id &&
		    (r->type_id || !strcmp(st->types[i].name, r->type_name)))
			t = &st->types[i];
	if (!t) {
		if (st->ntypes == MAX_RES_TYPES)
			return 0;
		t = &st->types[st->ntypes++];
		t->id = r->type_id;
		snprintf(t->name, sizeof(t->name), "%s",
			 r->type_name ? r->type_name : "");
	}
	t->count++;
	t->bytes += r->size;
	return 0;
}

static int print_version_string(void *ctx, const char *table,
				const char *key, const char *value)
{
	printf("  %-18s %s\n", key, value);
	return 0;
}

static void print_version_number(const char *label, DWORD ms, DWORD ls)
{
	printf("  %-18s %u.%u.%u.%u\n", label, HIWORD(ms), LOWORD(ms),
	       HIWORD(ls), LOWORD(ls));
}

static void print_resources(const struct pe_image *img)
{
	struct res_state st = { .ntypes = 0 };
	VS_FIXEDFILEINFO fixed;

	if (!pe_dir(img, IMAGE_DIRECTORY_ENTRY_RESOURCE))
		return;
	printf("\nresources:\n");
	if (pe_foreach_resource(img, count_resource, &st)) {
		report_error();
		return;
	}
	for (unsigned i = 0; i < st.ntypes; i++) {
		const struct res_count *t = &st.types[i];
		const char *name = t->name;
		char buf[32];

		if (t->id) {
			if (t->id < sizeof(resource_types) / sizeof(resource_types[0]) &&
			    resource_types[t->id])
				name = resource_types[t->id];
			else {
				snprintf(buf, sizeof(buf), "type %u", t->id);
				name = buf;
			}
		}
		printf("  %-18s %u (%u bytes)\n", name, t->count, t->bytes);
	}

	if (pe_find_resource(img, RT_VERSION, 0, PE_RES_ANY_LANG,
			     &(struct pe_resource){ 0 }))
		return;
	printf("\nversion:\n");
	if (pe_version_info(img, &fixed, print_version_string, NULL)) {
		report_error();
		return;
	}
	if (fixed.dwSignature == VS_FFI_SIGNATURE) {
		print_version_number("file version", fixed.dwFileVersionMS,
				     fixed.dwFileVersionLS);
		print_version_number("product version", fixed.dwProductVersionMS,
				     fixed.dwProductVersionLS);
	}
}

int cmd_info(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: seance info <dll>\n");
		return 2;
	}

	struct pe_image *img = pe_open(argv[1]);
	if (!img) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}

	status = 0;
	print_header(argv[1], img);
	print_sections(img);
	print_exports(img);
	print_imports(img, 0);
	if (pe_dir(img, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT))
		print_imports(img, 1);
	print_tls(img);
	print_resources(img);
	pe_free(img);
	return status;
}
