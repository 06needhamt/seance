/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Finding DMOs (src/dmo/discover.c and dshow_server_discover): what
 * testdmo.dll's DllRegisterServer registers, through DMORegister and by
 * writing the keys itself, seen in a sandbox and then for real; the DMOs
 * read back from the registry (including a .reg file exported on Windows);
 * other servers' DMOs left out; and the roles given to DMOs.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dmo/discover.h"
#include "dshow/discover.h"
#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "win32/reg.h"
#include "test.h"

#define OBJECTS "HKEY_LOCAL_MACHINE\\Software\\Classes\\DirectShow\\MediaObjects\\"
#define CLASSES "HKEY_LOCAL_MACHINE\\Software\\Classes\\"
#define DECODER "{5E6C9B41-3A7D-4F0E-9C11-0D2B7A6E4F80}"
#define ENCODER "{5E6C9B42-3A7D-4F0E-9C11-0D2B7A6E4F80}"
#define OTHER "{22222222-3333-4444-5555-666666666666}"

static const GUID clsid_decoder = { 0x5e6c9b41, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
static const GUID clsid_encoder = { 0x5e6c9b42, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
static const GUID clsid_audio_decoder = { 0x5e6c9b43, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
static const GUID clsid_audio_encoder = { 0x5e6c9b44, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
static const GUID st_tsta = WAVE_FORMAT_SUBTYPE_INIT(0x7354);
static const GUID clsid_other = { 0x22222222, 0x3333, 0x4444,
	{ 0x55, 0x55, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66 } };
static const GUID st_tst0 = FOURCC_SUBTYPE_INIT(mmioFOURCC('T', 'S', 'T', '0'));
static const GUID st_mp3 = WAVE_FORMAT_SUBTYPE_INIT(WAVE_FORMAT_MPEGLAYER3);
static const GUID guid_null;

static char regfile[300];

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

static int key_exists(const char *path)
{
	reg_lock();
	int r = reg_open(path) != NULL;
	reg_unlock();
	return r;
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

/* testdmo.dll's four DMOs (in any order). */
static void check_dmos(const struct dmo_reg *d, size_t n)
{
	int seen = 0;

	CHECK_EQ(n, 4);
	for (size_t i = 0; i < n; i++) {
		char *name = d[i].name ? utf16_to_utf8_dup(d[i].name) : NULL;
		if (guid_eq(&d[i].clsid, &clsid_decoder)) {
			seen |= 1;
			CHECK_STR(name, "seance test DMO decoder");
			CHECK(guid_eq(&d[i].category, &DMOCATEGORY_VIDEO_DECODER));
			CHECK_EQ(dmo_role(&d[i]), DMO_ROLE_VIDEO_DECODER);
			CHECK(d[i].nin == 1 && d[i].nout == 6 &&
			      guid_eq(&d[i].in[0].subtype, &st_tst0) &&
			      guid_eq(&d[i].out[4].subtype,
				      &MEDIASUBTYPE_RGB24));
		} else if (guid_eq(&d[i].clsid, &clsid_encoder)) {
			/* Written by the DLL itself, not by DMORegister. */
			seen |= 2;
			CHECK_STR(name, "seance test DMO encoder");
			CHECK(guid_eq(&d[i].category, &DMOCATEGORY_VIDEO_ENCODER));
			CHECK_EQ(dmo_role(&d[i]), DMO_ROLE_VIDEO_ENCODER);
			CHECK(d[i].nin == 6 && d[i].nout == 1 &&
			      guid_eq(&d[i].out[0].subtype, &st_tst0));
		} else if (guid_eq(&d[i].clsid, &clsid_audio_decoder)) {
			seen |= 4;
			CHECK_STR(name, "seance test DMO audio decoder");
			CHECK(guid_eq(&d[i].category, &DMOCATEGORY_AUDIO_DECODER));
			CHECK_EQ(dmo_role(&d[i]), DMO_ROLE_AUDIO_DECODER);
			CHECK(d[i].nin == 1 && d[i].nout == 1 &&
			      guid_eq(&d[i].in[0].subtype, &st_tsta) &&
			      guid_eq(&d[i].out[0].subtype, &MEDIASUBTYPE_PCM));
		} else {
			seen |= 8;
			CHECK(guid_eq(&d[i].clsid, &clsid_audio_encoder));
			CHECK_STR(name, "seance test DMO audio encoder");
			CHECK(guid_eq(&d[i].category, &DMOCATEGORY_AUDIO_ENCODER));
			CHECK_EQ(dmo_role(&d[i]), DMO_ROLE_AUDIO_ENCODER);
		}
		CHECK_EQ(d[i].flags, 0);
		free(name);
	}
	CHECK_EQ(seen, 15);
}

/* Another server's DMO, and one with no server, already registered. */
static void add_foreign(void)
{
	static const char text[] =
		"Windows Registry Editor Version 5.00\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\DirectShow\\MediaObjects\\" OTHER "]\n"
		"@=\"Imported MP3 decoder\"\n"
		"\"InputTypes\"=hex:61,75,64,73,00,00,10,00,80,00,00,aa,00,38,9b,"
		"71,55,00,00,00,00,00,10,00,80,00,00,aa,00,38,9b,71\n"
		"\"OutputTypes\"=hex:61,75,64,73,00,00,10,00,80,00,00,aa,00,38,9b,"
		"71,01,00,00,00,00,00,10,00,80,00,00,aa,00,38,9b,71\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\DirectShow\\MediaObjects\\" OTHER "\\Keyed]\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\DirectShow\\MediaObjects\\Categories\\"
		"{57F2DB8B-E6BB-4513-9D43-DCD2A6593125}\\" OTHER "]\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\CLSID\\" OTHER "\\InprocServer32]\n"
		"@=\"C:\\\\Windows\\\\System32\\\\imported.dll\"\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\DirectShow\\MediaObjects\\"
		"{33333333-3333-4444-5555-666666666666}]\n"
		"@=\"No server\"\n";

	reg_lock();
	CHECK_EQ(reg_import(text, sizeof(text) - 1), 0);
	reg_unlock();
}

/* The imported DMO, read back as Windows wrote it. */
static void test_reg_file(void)
{
	struct dmo_registered *r = NULL;
	size_t n = dmo_registered(&r), seen = 0;

	CHECK_EQ(n, 2);
	for (size_t i = 0; i < n; i++) {
		if (!guid_eq(&r[i].d.clsid, &clsid_other))
			continue;
		seen++;
		CHECK_STR(r[i].path, "C:\\Windows\\System32\\imported.dll");
		CHECK_EQ(r[i].d.flags, DMO_REGISTERF_IS_KEYED);
		CHECK(r[i].d.nin == 1 &&
		      guid_eq(&r[i].d.in[0].type, &MEDIATYPE_Audio) &&
		      guid_eq(&r[i].d.in[0].subtype, &st_mp3));
		CHECK(r[i].d.nout == 1 &&
		      guid_eq(&r[i].d.out[0].subtype, &MEDIASUBTYPE_PCM));
		CHECK_EQ(dmo_role(&r[i].d), DMO_ROLE_AUDIO_DECODER);
	}
	CHECK_EQ(seen, 1);
	dmo_registered_free(r, n);
}

/* In a sandbox: its DMOs, nobody else's, and nothing left behind. */
static void test_dry_run(const char *server)
{
	struct dshow_server_found found;
	HRESULT hr = E_FAIL;

	CHECK_EQ(dshow_server_discover(server, &found, &hr), 0);
	CHECK_EQ(hr, S_OK);
	CHECK_EQ(found.nfilters, 0);
	check_dmos(found.dmos, found.ndmos);
	dshow_server_found_free(&found);

	CHECK(!key_exists(OBJECTS DECODER));
	CHECK(!key_exists(OBJECTS ENCODER));
	CHECK(!key_exists(CLASSES "CLSID\\" DECODER));
	CHECK(mod_find("testdmo.dll") == NULL);

	/* A filter server registers no DMOs. */
	CHECK_EQ(dshow_server_discover(fixture("testfilter.ax"), &found, &hr), 0);
	CHECK_EQ(found.nfilters, 4);
	CHECK_EQ(found.ndmos, 0);
	dshow_server_found_free(&found);
}

static void test_register(const char *server)
{
	struct dshow_server_found found;
	HRESULT hr = E_FAIL;

	CHECK_EQ(dshow_server_register(server, 0, &found, &hr), 0);
	CHECK_EQ(hr, S_OK);
	check_dmos(found.dmos, found.ndmos);
	dshow_server_found_free(&found);
	CHECK(key_exists(OBJECTS DECODER));
	CHECK(key_exists(OBJECTS "Categories\\"
			 "{33D9A760-90C8-11D0-BD43-00A0C911CE86}\\" ENCODER));
	CHECK(file_exists(regfile));

	/* Read back, with the DLL that serves each. */
	struct dmo_registered *r = NULL;
	size_t n = dmo_registered(&r), ours = 0;
	CHECK_EQ(n, 6);
	for (size_t i = 0; i < n; i++) {
		if (guid_eq(&r[i].d.clsid, &clsid_other) ||
		    !r[i].path)
			continue;
		ours++;
		CHECK(strstr(r[i].path, "testdmo.dll") != NULL);
	}
	CHECK_EQ(ours, 4);
	dmo_registered_free(r, n);

	/* Registering again finds the same four, once each. */
	CHECK_EQ(dshow_server_register(server, 0, &found, &hr), 0);
	CHECK_EQ(found.ndmos, 4);
	dshow_server_found_free(&found);

	/* Unregistering takes it all away again, and only that. */
	CHECK_EQ(dshow_server_register(server, 1, &found, &hr), 0);
	CHECK_EQ(hr, S_OK);
	CHECK_EQ(found.ndmos, 0);
	dshow_server_found_free(&found);
	CHECK(!key_exists(OBJECTS DECODER));
	CHECK(!key_exists(OBJECTS ENCODER));
	CHECK(!key_exists(CLASSES "CLSID\\" ENCODER));
	CHECK(key_exists(OBJECTS OTHER));
	n = dmo_registered(&r);
	CHECK_EQ(n, 2);
	dmo_registered_free(r, n);
}

static void test_roles(void)
{
	DMO_PARTIAL_MEDIATYPE video = { MEDIATYPE_Video, st_tst0 };
	DMO_PARTIAL_MEDIATYPE yuy2 = { MEDIATYPE_Video, MEDIASUBTYPE_YUY2 };
	DMO_PARTIAL_MEDIATYPE mp3 = { MEDIATYPE_Audio, st_mp3 };
	DMO_PARTIAL_MEDIATYPE pcm = { MEDIATYPE_Audio, MEDIASUBTYPE_PCM };
	DMO_PARTIAL_MEDIATYPE any = { MEDIATYPE_Video, { 0 } };
	struct dmo_reg d = { .nin = 1, .nout = 1, .in = &video, .out = &yuy2 };

	/* From the types, with no category. */
	CHECK_EQ(dmo_role(&d), DMO_ROLE_VIDEO_DECODER);
	d.out = &any;
	CHECK_EQ(dmo_role(&d), DMO_ROLE_VIDEO_DECODER);
	d.in = &yuy2;
	d.out = &video;
	CHECK_EQ(dmo_role(&d), DMO_ROLE_VIDEO_ENCODER);
	d.in = &mp3;
	d.out = &pcm;
	CHECK_EQ(dmo_role(&d), DMO_ROLE_AUDIO_DECODER);
	d.in = &pcm;
	d.out = &mp3;
	CHECK_EQ(dmo_role(&d), DMO_ROLE_AUDIO_ENCODER);
	d.out = &pcm;
	CHECK_EQ(dmo_role(&d), DMO_ROLE_OTHER);
	d.nin = d.nout = 0;
	CHECK_EQ(dmo_role(&d), DMO_ROLE_OTHER);

	/* The category decides when there is one. */
	d.category = DMOCATEGORY_AUDIO_EFFECT;
	CHECK_EQ(dmo_role(&d), DMO_ROLE_EFFECT);
	CHECK(!dmo_role_is_codec(DMO_ROLE_EFFECT));
	d.category = DMOCATEGORY_AUDIO_ENCODER;
	CHECK_EQ(dmo_role(&d), DMO_ROLE_AUDIO_ENCODER);
	CHECK(dmo_role_is_codec(DMO_ROLE_AUDIO_ENCODER));
	CHECK_STR(dmo_role_name(DMO_ROLE_VIDEO_DECODER), "video decoder");

	char buf[DSHOW_GUID_NAME_SIZE];
	CHECK_STR(dmo_category_name(&DMOCATEGORY_VIDEO_ENCODER, buf),
		  "DMOCATEGORY_VIDEO_ENCODER");
	CHECK_STR(dmo_category_name(&guid_null, buf), "none");
	CHECK_STR(dshow_guid_name(&st_mp3, buf), "wave format 0x0055");
	CHECK_STR(dshow_guid_name(&MEDIASUBTYPE_PCM, buf), "MEDIASUBTYPE_PCM");
	CHECK_STR(dshow_guid_name(&MEDIATYPE_Audio, buf), "MEDIATYPE_Audio");
}

int main(void)
{
	const char *server = fixture("testdmo.dll");
	char dir[] = "/tmp/vfw-dmo-discover-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(regfile, sizeof(regfile), "%s/registry.reg", dir);
	setenv("VFW_ROOT", dir, 1);
	setenv("VFW_REGISTRY", regfile, 1);
	CHECK_EQ(lowmem_reserve(), 0);
	CHECK_EQ(runtime_enter(), 0);
	reg_reload();

	test_roles();
	add_foreign();
	test_reg_file();
	test_dry_run(server);
	test_register(server);

	char cmd[128];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_dmo_discover");
}
