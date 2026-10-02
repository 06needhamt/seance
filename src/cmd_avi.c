/*
 * seance avi <file.avi> [--chunks]: describe an AVI file's headers,
 * streams and index.
 */
#include <stdio.h>
#include <string.h>

#include "avi/avi.h"
#include "commands.h"
#include "common/error.h"
#include "common/fourcc.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)

struct flag_name {
	DWORD bit;
	const char *name;
};

static const struct flag_name avi_flags[] = {
	{ AVIF_HASINDEX, "HASINDEX" },
	{ AVIF_MUSTUSEINDEX, "MUSTUSEINDEX" },
	{ AVIF_ISINTERLEAVED, "ISINTERLEAVED" },
	{ AVIF_TRUSTCKTYPE, "TRUSTCKTYPE" },
	{ AVIF_WASCAPTUREFILE, "WASCAPTUREFILE" },
	{ AVIF_COPYRIGHTED, "COPYRIGHTED" },
	{ 0, NULL }
};

static const struct flag_name stream_flags[] = {
	{ AVISF_DISABLED, "DISABLED" },
	{ AVISF_VIDEO_PALCHANGES, "VIDEO_PALCHANGES" },
	{ 0, NULL }
};

static void print_flags(DWORD value, const struct flag_name *names)
{
	int any = 0;

	printf("0x%08lx", (unsigned long)value);
	for (const struct flag_name *f = names; f->name; f++)
		if (value & f->bit) {
			printf(" %s", f->name);
			any = 1;
		}
	if (!any && value)
		printf(" (unknown)");
	printf("\n");
}

static WORD rd16(const BYTE *p)
{
	WORD v;
	memcpy(&v, p, 2);
	return v;
}

static DWORD rd32(const BYTE *p)
{
	DWORD v;
	memcpy(&v, p, 4);
	return v;
}

static void print_video_format(const struct avi_stream *s)
{
	const BITMAPINFOHEADER *bih = avi_stream_bih(s);
	char fcc[FOURCC_STR_SIZE];

	if (!bih) {
		printf("    format: not a BITMAPINFOHEADER (%lu bytes)\n",
		       (unsigned long)s->format_size);
		return;
	}
	printf("    format: %ldx%ld, %u bits, ", (long)bih->biWidth,
	       (long)bih->biHeight, bih->biBitCount);
	switch (bih->biCompression) {
	case BI_RGB:       printf("BI_RGB"); break;
	case BI_RLE8:      printf("BI_RLE8"); break;
	case BI_RLE4:      printf("BI_RLE4"); break;
	case BI_BITFIELDS: printf("BI_BITFIELDS"); break;
	default:
		printf("'%s'", fourcc_str(bih->biCompression, fcc));
	}
	printf(", image size %lu", (unsigned long)bih->biSizeImage);
	if (bih->biClrUsed)
		printf(", %lu colours", (unsigned long)bih->biClrUsed);
	if (s->format_size > bih->biSize)
		printf(", %lu bytes after the header",
		       (unsigned long)(s->format_size - bih->biSize));
	printf("\n");
}

static void print_audio_format(const struct avi_stream *s)
{
	const BYTE *w = s->format;

	if (s->format_size < 16) {
		printf("    format: %lu bytes\n", (unsigned long)s->format_size);
		return;
	}
	printf("    format: tag 0x%04x, %u channels, %lu Hz, %u bits, "
	       "block %u, %lu bytes/s\n", rd16(w), rd16(w + 2),
	       (unsigned long)rd32(w + 4), rd16(w + 14), rd16(w + 12),
	       (unsigned long)rd32(w + 8));
}

static void print_stream(const struct avi_file *avi, unsigned i, int chunks)
{
	const struct avi_stream *s = &avi->streams[i];
	const AVISTREAMHEADER *h = &s->header;
	char a[FOURCC_STR_SIZE], b[FOURCC_STR_SIZE];

	printf("\nStream %u: '%s', handler '%s'", i, fourcc_str(h->fccType, a),
	       fourcc_str(h->fccHandler, b));
	if (s->name[0])
		printf(", \"%s\"", s->name);
	printf("\n");
	printf("    flags: ");
	print_flags(h->dwFlags, stream_flags);
	printf("    rate: %lu / %lu", (unsigned long)h->dwRate,
	       (unsigned long)h->dwScale);
	if (h->dwScale)
		printf(" = %.4f per second", (double)h->dwRate / h->dwScale);
	printf("\n");
	printf("    start %lu, length %lu, suggested buffer %lu, sample size %lu\n",
	       (unsigned long)h->dwStart, (unsigned long)h->dwLength,
	       (unsigned long)h->dwSuggestedBufferSize,
	       (unsigned long)h->dwSampleSize);

	if (h->fccType == FCC('v', 'i', 'd', 's'))
		print_video_format(s);
	else if (h->fccType == FCC('a', 'u', 'd', 's'))
		print_audio_format(s);
	else
		printf("    format: %lu bytes\n", (unsigned long)s->format_size);
	if (s->codec_data_size)
		printf("    codec data (strd): %lu bytes\n",
		       (unsigned long)s->codec_data_size);

	size_t keys = 0, zero = 0;
	uint64_t bytes = 0;
	DWORD max = 0;
	for (size_t c = 0; c < s->nchunks; c++) {
		const struct avi_chunk *k = &s->chunks[c];
		if (!avi_chunk_is_frame(k))
			continue;
		keys += (k->flags & AVIIF_KEYFRAME) != 0;
		zero += k->size == 0;
		bytes += k->size;
		if (k->size > max)
			max = k->size;
	}
	printf("    chunks: %zu", s->nchunks);
	if (h->fccType == FCC('v', 'i', 'd', 's'))
		printf(" (%zu frames, %zu keyframes, %zu empty, %zu palette "
		       "changes)", s->frames, keys, zero, s->nchunks - s->frames);
	printf(", %llu bytes, largest %lu\n", (unsigned long long)bytes,
	       (unsigned long)max);

	if (!chunks)
		return;
	for (size_t c = 0; c < s->nchunks; c++) {
		const struct avi_chunk *k = &s->chunks[c];
		printf("      %6zu  %s  offset %12llu  size %9lu%s\n", c,
		       fourcc_str(k->id, a), (unsigned long long)k->offset,
		       (unsigned long)k->size,
		       k->flags & AVIIF_KEYFRAME ? "  key" : "");
	}
}

static int usage(void)
{
	fprintf(stderr, "usage: seance avi <file.avi> [--chunks]\n");
	return 2;
}

int cmd_avi(int argc, char **argv)
{
	const char *path = NULL;
	int chunks = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--chunks"))
			chunks = 1;
		else if (argv[i][0] == '-' || path)
			return usage();
		else
			path = argv[i];
	}
	if (!path)
		return usage();

	struct avi_file *avi = avi_open(path);
	if (!avi) {
		fprintf(stderr, "seance: %s\n", vfw_error());
		return 1;
	}

	const AVIMAINHEADER *m = &avi->header;
	printf("File: %s (%llu bytes)\n", path, (unsigned long long)avi->size);
	printf("Format: %s, %u RIFF chunk%s\n",
	       avi->riff_count > 1 ? "OpenDML (AVI 2.0)" : "AVI",
	       avi->riff_count, avi->riff_count == 1 ? "" : "s");
	printf("Index: %s\n", avi_index_source_name(avi->index_source));
	printf("Main header:\n");
	printf("    %lux%lu, %lu us per frame, %lu frames",
	       (unsigned long)m->dwWidth, (unsigned long)m->dwHeight,
	       (unsigned long)m->dwMicroSecPerFrame,
	       (unsigned long)m->dwTotalFrames);
	if (avi->total_frames != m->dwTotalFrames)
		printf(" (%lu in all)", (unsigned long)avi->total_frames);
	printf(", %lu streams\n", (unsigned long)m->dwStreams);
	printf("    flags: ");
	print_flags(m->dwFlags, avi_flags);
	printf("    max bytes per second %lu, suggested buffer %lu\n",
	       (unsigned long)m->dwMaxBytesPerSec,
	       (unsigned long)m->dwSuggestedBufferSize);

	for (unsigned i = 0; i < avi->nstreams; i++)
		print_stream(avi, i, chunks);

	if (avi->truncated)
		printf("\nWarning: the file is truncated or damaged; some chunks "
		       "were left out\n");
	avi_close(avi);
	return 0;
}
