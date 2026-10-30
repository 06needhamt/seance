/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The DMO host (src/dmo) driving testdmo.dll's TST0 decoder and encoder:
 * type negotiation, decoding and encoding round trips, times and preroll
 * carried through, flushing, encoder controls and state, and no references
 * left behind (DllCanUnloadNow). Each of the fixture's modes (latency,
 * bursts, refused input, a type switch, codec data, strict pointers) runs
 * in a child process of its own, since the fixture reads them from the
 * environment as it first sees it.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dmo/dmo.h"
#include "dshow/dshow.h"
#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "test.h"

static const CLSID clsid_decoder = { 0x5e6c9b41, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
static const CLSID clsid_encoder = { 0x5e6c9b42, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };

/* 4x2 I420 pictures: 8 bytes of Y, 2 of U, 2 of V. */
#define W 4
#define H 2
#define RAW 12
#define FRAMES 6
static BYTE pictures[FRAMES][RAW];

static const char *server;

/* A TST0 frame by hand: header, then literal runs only. */
static size_t tst0(const BYTE *raw, const BYTE *prev, BYTE *out)
{
	size_t o = 0;

	out[o++] = prev ? 'D' : 'K';
	out[o++] = 12;
	out[o++] = 0;
	out[o++] = 0;
	out[o++] = RAW - 1;
	for (int i = 0; i < RAW; i++)
		out[o++] = prev ? (BYTE)(raw[i] ^ prev[i]) : raw[i];
	return o;
}

static void make_types(AM_MEDIA_TYPE *in, AM_MEDIA_TYPE *out, FOURCC from,
		       FOURCC to, WORD out_bits)
{
	BITMAPINFOHEADER b = { .biSize = sizeof(b), .biWidth = W, .biHeight = H,
		.biPlanes = 1, .biBitCount = 12, .biCompression = from,
		.biSizeImage = RAW };
	CHECK_EQ(dshow_bih_to_mt(&b, sizeof(b), 25, 1, 0, in), S_OK);
	b.biCompression = to;
	b.biBitCount = out_bits;
	CHECK_EQ(dshow_bih_to_mt(&b, sizeof(b), 25, 1, 0, out), S_OK);
}

/* What came out. */
struct got {
	int n;
	BYTE data[16][64];
	DWORD size[16], flags[16];
	REFERENCE_TIME time[16];
	int preroll[16], changed_at;
	GUID changed_to;
};

static HRESULT collect(void *opaque, const struct dmo_output *o,
		       const DMO_MEDIA_TYPE *new_type)
{
	struct got *g = opaque;

	if (g->n >= 16 || o->size > 64)
		return E_FAIL;
	memcpy(g->data[g->n], o->data, o->size);
	g->size[g->n] = o->size;
	g->flags[g->n] = o->flags;
	g->time[g->n] = o->time;
	g->preroll[g->n] = o->preroll;
	if (new_type) {
		g->changed_at = g->n;
		g->changed_to = new_type->subtype;
	}
	g->n++;
	return S_OK;
}

static REFERENCE_TIME at(int i)
{
	return (REFERENCE_TIME)i * 400000;
}

static IMediaObject *open_dmo(const CLSID *clsid, struct dmo_object **obj)
{
	IMediaObject *mo = NULL;

	CHECK_EQ(dmo_object_open(server, clsid, obj, &mo), S_OK);
	return mo;
}

static int unloadable(void)
{
	struct module *m = mod_find("testdmo.dll");
	HRESULT (WINAPI *can)(void) = m ? (HRESULT (WINAPI *)(void))
		mod_get_proc(m, "DllCanUnloadNow") : NULL;
	return !can || can() == S_OK;
}

/* Decodes every picture: K D D K D D, the first marked preroll. */
static void decode_all(struct got *g, int check_order)
{
	struct dmo_object *obj = NULL;
	IMediaObject *mo = open_dmo(&clsid_decoder, &obj);
	AM_MEDIA_TYPE in = { 0 }, out = { 0 };
	struct dmo_host *host = NULL;
	BYTE frame[64];

	memset(g, 0, sizeof(*g));
	g->changed_at = -1;
	make_types(&in, &out, mmioFOURCC('T', 'S', 'T', '0'),
		   mmioFOURCC('I', '4', '2', '0'), 12);
	if (!mo)
		return;
	CHECK_EQ(dmo_host_create(mo, &in, &out, collect, g, &host), S_OK);
	for (int i = 0; host && i < FRAMES; i++) {
		int key = i % 3 == 0;
		size_t n = tst0(pictures[i], key ? NULL : pictures[i - 1], frame);
		DWORD flags = (key ? AM_SAMPLE_SPLICEPOINT : 0) |
			      (i == 0 ? AM_SAMPLE_PREROLL : 0);
		CHECK_EQ(dmo_host_push(host, frame, n, at(i), at(i + 1), flags),
			 S_OK);
	}
	if (host)
		CHECK_EQ(dmo_host_end(host), S_OK);
	CHECK_EQ(g->n, FRAMES);
	for (int i = 0; check_order && i < g->n && i < FRAMES; i++) {
		CHECK_EQ(g->size[i], RAW);
		CHECK(!memcmp(g->data[i], pictures[i], RAW));
		CHECK_EQ(g->time[i], at(i));
		CHECK(g->flags[i] & DMO_OUTPUT_DATA_BUFFERF_TIME);
		CHECK_EQ(g->preroll[i], i == 0);
	}
	dmo_host_destroy(host);
	dmo_object_close(obj, mo);
	dshow_free_media_type(&in);
	dshow_free_media_type(&out);
	CHECK(unloadable());
}

static void test_decode(void)
{
	struct got g;

	decode_all(&g, 1);
}

static void test_types(void)
{
	struct dmo_object *obj = NULL;
	IMediaObject *mo = open_dmo(&clsid_decoder, &obj);
	AM_MEDIA_TYPE in = { 0 }, out = { 0 };
	struct dmo_host *host = NULL;
	struct got g = { 0 };

	if (!mo)
		return;
	/* The stream is 12-bit, so YUY2 out won't do; YV12 will. */
	make_types(&in, &out, mmioFOURCC('T', 'S', 'T', '0'),
		   mmioFOURCC('Y', 'U', 'Y', '2'), 16);
	CHECK_EQ(dmo_host_create(mo, &in, &out, collect, &g, &host),
		 DMO_E_TYPE_NOT_ACCEPTED);
	CHECK(host == NULL);
	dshow_free_media_type(&out);
	dshow_free_media_type(&in);
	make_types(&in, &out, mmioFOURCC('X', 'V', 'I', 'D'),
		   mmioFOURCC('Y', 'V', '1', '2'), 12);
	CHECK_EQ(dmo_host_create(mo, &in, &out, collect, &g, &host),
		 DMO_E_TYPE_NOT_ACCEPTED);
	dshow_free_media_type(&out);
	dshow_free_media_type(&in);
	make_types(&in, &out, mmioFOURCC('T', 'S', 'T', '0'),
		   mmioFOURCC('Y', 'V', '1', '2'), 12);
	CHECK_EQ(dmo_host_create(mo, &in, &out, collect, &g, &host), S_OK);
	CHECK(dmo_host_output_type(host)->cbFormat == out.cbFormat);

	/* A difference after a flush has no key to apply to. */
	BYTE frame[64];
	size_t n = tst0(pictures[0], NULL, frame);
	CHECK_EQ(dmo_host_push(host, frame, n, at(0), at(1),
			       AM_SAMPLE_SPLICEPOINT), S_OK);
	CHECK_EQ(dmo_host_flush(host), S_OK);
	n = tst0(pictures[1], pictures[0], frame);
	CHECK_EQ(dmo_host_push(host, frame, n, at(1), at(2), 0), S_OK);
	n = tst0(pictures[2], NULL, frame);
	CHECK_EQ(dmo_host_push(host, frame, n, at(2), at(3),
			       AM_SAMPLE_SPLICEPOINT), S_OK);
	CHECK_EQ(g.n, 2);
	CHECK(!memcmp(g.data[1], pictures[2], RAW));
	/* A broken frame is an error, and the stream goes on. */
	frame[0] = 'X';
	CHECK(FAILED(dmo_host_push(host, frame, n, at(3), at(4), 0)));
	n = tst0(pictures[3], pictures[2], frame);
	CHECK_EQ(dmo_host_push(host, frame, n, at(3), at(4), 0), S_OK);
	CHECK_EQ(g.n, 3);
	CHECK_EQ(g.time[2], at(3));
	dmo_host_destroy(host);
	dmo_object_close(obj, mo);
	dshow_free_media_type(&in);
	dshow_free_media_type(&out);
	CHECK(unloadable());
}

/* A partial request keeps the offered format unless the DMO rejects it. */
static void test_partial_output(int encoder, int filled)
{
	struct dmo_object *obj = NULL;
	IMediaObject *mo = open_dmo(encoder ? &clsid_encoder : &clsid_decoder, &obj);
	AM_MEDIA_TYPE in = { 0 }, out = { 0 }, offered = { 0 };
	struct dmo_host *host = NULL;
	struct got g = { 0 };
	FOURCC raw = mmioFOURCC('I', '4', '2', '0');
	FOURCC compressed = mmioFOURCC('T', 'S', 'T', '0');

	if (!mo)
		return;
	make_types(&in, &out, encoder ? raw : compressed,
		   encoder ? compressed : raw, 12);
	CHECK_EQ(mo->lpVtbl->SetInputType(mo, 0, &in, 0), S_OK);
	CHECK_EQ(mo->lpVtbl->GetOutputType(mo, 0, 0, &offered), S_OK);
	AM_MEDIA_TYPE partial = {
		.majortype = out.majortype, .subtype = out.subtype,
	};
	CHECK_EQ(dmo_host_create(mo, &in, &partial, collect, &g, &host), S_OK);
	if (host) {
		const DMO_MEDIA_TYPE *actual = dmo_host_output_type(host);
		CHECK(!memcmp(&actual->majortype, &offered.majortype, sizeof(GUID)));
		CHECK(!memcmp(&actual->subtype, &offered.subtype, sizeof(GUID)));
		CHECK(!memcmp(&actual->formattype, &offered.formattype, sizeof(GUID)));
		CHECK_EQ(actual->cbFormat, offered.cbFormat);
		if (filled && actual->cbFormat >= sizeof(VIDEOINFOHEADER)) {
			const VIDEOINFOHEADER *v = (const VIDEOINFOHEADER *)actual->pbFormat;
			CHECK(v->dwBitRate > 0);
			CHECK_EQ(v->rcSource.right - v->rcSource.left, W);
			CHECK_EQ(v->rcSource.bottom - v->rcSource.top, H);
			CHECK_EQ(v->rcTarget.right - v->rcTarget.left, W);
			CHECK_EQ(v->rcTarget.bottom - v->rcTarget.top, H);
		} else if (!filled && actual->cbFormat == offered.cbFormat) {
			CHECK(!memcmp(actual->pbFormat, offered.pbFormat, offered.cbFormat));
		}
	}
	dmo_host_destroy(host);
	dmo_object_close(obj, mo);
	dshow_free_media_type(&in);
	dshow_free_media_type(&out);
	dshow_free_media_type(&offered);
	CHECK(unloadable());
}

/* Encodes every picture; returns the outputs. */
static void encode_all(struct got *g, LONG key_interval, AM_MEDIA_TYPE *final)
{
	struct dmo_object *obj = NULL;
	IMediaObject *mo = open_dmo(&clsid_encoder, &obj);
	AM_MEDIA_TYPE in = { 0 }, out = { 0 };
	struct dmo_host *host = NULL;

	memset(g, 0, sizeof(*g));
	if (!mo)
		return;
	if (key_interval >= 0) {
		struct dshow_encoder_options o = { key_interval, -1.0, -1 };
		CHECK_EQ(dshow_encoder_configure(mo, &o), S_OK);
	}
	make_types(&in, &out, mmioFOURCC('I', '4', '2', '0'),
		   mmioFOURCC('T', 'S', 'T', '0'), 12);
	CHECK_EQ(dmo_host_create(mo, &in, &out, collect, g, &host), S_OK);
	for (int i = 0; host && i < FRAMES; i++)
		CHECK_EQ(dmo_host_push(host, pictures[i], RAW, at(i), at(i + 1),
				       i ? 0 : AM_SAMPLE_SPLICEPOINT), S_OK);
	if (host) {
		CHECK_EQ(dmo_host_end(host), S_OK);
		if (final)
			CHECK_EQ(dshow_copy_media_type(final,
					dmo_host_output_type(host)), S_OK);
	}
	CHECK_EQ(g->n, FRAMES);
	dmo_host_destroy(host);
	dmo_object_close(obj, mo);
	dshow_free_media_type(&in);
	dshow_free_media_type(&out);
	CHECK(unloadable());
}

/* Decodes encoded outputs and checks they give the pictures back. */
static void check_round_trip(const struct got *e)
{
	struct dmo_object *obj = NULL;
	IMediaObject *mo = open_dmo(&clsid_decoder, &obj);
	AM_MEDIA_TYPE in = { 0 }, out = { 0 };
	struct dmo_host *host = NULL;
	struct got d = { 0 };

	if (!mo)
		return;
	make_types(&in, &out, mmioFOURCC('T', 'S', 'T', '0'),
		   mmioFOURCC('I', '4', '2', '0'), 12);
	CHECK_EQ(dmo_host_create(mo, &in, &out, collect, &d, &host), S_OK);
	for (int i = 0; host && i < e->n; i++)
		CHECK_EQ(dmo_host_push(host, e->data[i], e->size[i], at(i),
			at(i + 1), (e->flags[i] & DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT) ?
			AM_SAMPLE_SPLICEPOINT : 0), S_OK);
	if (host)
		CHECK_EQ(dmo_host_end(host), S_OK);
	CHECK_EQ(d.n, FRAMES);
	for (int i = 0; i < d.n && i < FRAMES; i++)
		CHECK(!memcmp(d.data[i], pictures[i], RAW));
	dmo_host_destroy(host);
	dmo_object_close(obj, mo);
	dshow_free_media_type(&in);
	dshow_free_media_type(&out);
}

static void test_encode(void)
{
	struct got g;

	/* Only the first is a key frame by default... */
	encode_all(&g, -1, NULL);
	for (int i = 0; i < g.n; i++)
		CHECK_EQ(!!(g.flags[i] & DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT),
			 i == 0);
	check_round_trip(&g);
	/* ...and every other one through ICodecAPI's GOP size. */
	encode_all(&g, 2, NULL);
	for (int i = 0; i < g.n; i++)
		CHECK_EQ(!!(g.flags[i] & DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT),
			 i % 2 == 0);
	check_round_trip(&g);
}

/* IPersistStream and the interfaces probe reports. */
static void test_state(void)
{
	struct dmo_object *obj = NULL;
	IMediaObject *mo = open_dmo(&clsid_encoder, &obj);
	void *saved = NULL;
	size_t size = 0;
	char names[160];

	if (!mo)
		return;
	CHECK_EQ(dshow_filter_interfaces(mo, names, sizeof(names)), 2);
	CHECK_STR(names, "ICodecAPI, IPersistStream");
	struct dshow_encoder_options o = { 5, -1.0, -1 };
	CHECK_EQ(dshow_encoder_configure(mo, &o), S_OK);
	CHECK_EQ(dshow_filter_save_state(mo, &saved, &size), S_OK);
	CHECK_EQ(size, 8);
	dmo_object_close(obj, mo);

	mo = open_dmo(&clsid_encoder, &obj);
	if (mo) {
		CHECK_EQ(dshow_filter_load_state(mo, saved, size), S_OK);
		void *again = NULL;
		size_t n = 0;
		CHECK_EQ(dshow_filter_save_state(mo, &again, &n), S_OK);
		CHECK(n == size && !memcmp(again, saved, n));
		free(again);
		dmo_object_close(obj, mo);
	}
	free(saved);
	mo = open_dmo(&clsid_decoder, &obj);
	if (mo) {
		CHECK_EQ(dshow_filter_interfaces(mo, names, sizeof(names)), 0);
		dmo_object_close(obj, mo);
	}
	IMediaObject *none = NULL;
	CHECK_EQ(dmo_object_open(server, &IID_IUnknown, &obj, &none),
		 CLASS_E_CLASSNOTAVAILABLE);
	CHECK(unloadable());
}

/* The modes, each in a process of its own. */

static void mode_delay(void)
{
	struct got g;

	/* Late and unstamped, but given the right times by the host. */
	decode_all(&g, 1);
}

static void mode_burst(void)
{
	struct got g;

	decode_all(&g, 1);
}

static void mode_notaccepting(void)
{
	struct got g;

	decode_all(&g, 1);
	encode_all(&g, -1, NULL);
	check_round_trip(&g);
}

static void mode_switch(void)
{
	struct got g;

	decode_all(&g, 0);
	CHECK_EQ(g.changed_at, 1);
	CHECK(!memcmp(&g.changed_to, &MEDIASUBTYPE_YV12, sizeof(GUID)));
	CHECK(!memcmp(g.data[0], pictures[0], RAW));
	/* YV12: the same Y, then V before U. */
	CHECK(!memcmp(g.data[1], pictures[1], 8));
	CHECK(!memcmp(g.data[1] + 8, pictures[1] + 10, 2));
	CHECK(!memcmp(g.data[1] + 10, pictures[1] + 8, 2));
}

static void mode_private(void)
{
	struct got g;
	AM_MEDIA_TYPE final = { 0 };

	encode_all(&g, -1, &final);
	CHECK_EQ(final.cbFormat, sizeof(VIDEOINFOHEADER) + 4);
	if (final.cbFormat == sizeof(VIDEOINFOHEADER) + 4) {
		VIDEOINFOHEADER *vih = (VIDEOINFOHEADER *)final.pbFormat;
		CHECK_EQ(vih->bmiHeader.biSize, sizeof(BITMAPINFOHEADER) + 4);
		CHECK(!memcmp(final.pbFormat + sizeof(*vih), "TSTP", 4));
	}
	dshow_free_media_type(&final);
	check_round_trip(&g);
}

static void mode_strict(void)
{
	struct got g;

	/* Sanitizer builds don't keep memory below 2 GB (lowmem.c). */
	if (!lowmem_active()) {
		printf("test_dmo: no low-memory reservation, skipping strict\n");
		return;
	}
	decode_all(&g, 1);
	encode_all(&g, 3, NULL);
	check_round_trip(&g);
}

static void mode_bitrate(void)
{
	test_partial_output(1, 1);
}

static const struct {
	const char *name, *variable;
	void (*run)(void);
} modes[] = {
	{ "delay", "TESTDMO_DELAY", mode_delay },
	{ "burst", "TESTDMO_BURST", mode_burst },
	{ "notaccepting", "TESTDMO_NOTACCEPTING", mode_notaccepting },
	{ "switch", "TESTDMO_SWITCH", mode_switch },
	{ "private", "TESTDMO_PRIVATE", mode_private },
	{ "strict", "TESTDMO_STRICT", mode_strict },
	{ "bitrate", "TESTDMO_BITRATE", mode_bitrate },
};
#define NMODES (sizeof(modes) / sizeof(*modes))

int main(int argc, char **argv)
{
	static char path[4096];
	const char *dir = getenv("VFW_FIXTURES");

	snprintf(path, sizeof(path), "%s/testdmo.dll", dir ? dir : ".");
	if (!dir || access(path, R_OK)) {
		printf("test_dmo: testdmo.dll not built, skipping\n");
		return 77;
	}
	server = path;
	for (int i = 0; i < FRAMES; i++)
		for (int j = 0; j < RAW; j++)
			pictures[i][j] = (BYTE)(i * 31 + j * 7 + (j > 5 ? i : 0));
	CHECK_EQ(lowmem_reserve(), 0);
	CHECK_EQ(runtime_enter(), 0);

	if (argc == 2) {
		for (size_t i = 0; i < NMODES; i++)
			if (!strcmp(argv[1], modes[i].name))
				modes[i].run();
		return test_summary(argv[1]);
	}

	test_types();
	test_partial_output(0, 0);
	test_partial_output(1, 0);
	test_decode();
	test_encode();
	test_state();
	for (size_t i = 0; i < NMODES; i++) {
		char cmd[4400];
		snprintf(cmd, sizeof(cmd), "VFW_ENV_%s=1 %s'%s' %s",
			 modes[i].variable, test_rerun_prefix(), argv[0],
			 modes[i].name);
		int status = system(cmd);
		CHECK_EQ(status, 0);
		if (status)
			fprintf(stderr, "test_dmo: mode %s failed\n",
				modes[i].name);
	}
	return test_summary("test_dmo");
}
