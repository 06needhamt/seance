/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio formats: tags, checks, names, PCM formats and preferences, media
 * types, sample conversion and time arithmetic.
 */
#include <math.h>
#include <stdint.h>

#include "common/error.h"
#include "media/audio.h"
#include "win32/ole.h"
#include "test.h"

static WAVEFORMATEXTENSIBLE pcm(enum audio_sample s, WORD ch, DWORD rate,
				enum audio_form form, size_t *size)
{
	WAVEFORMATEXTENSIBLE x;
	*size = audio_pcm_format(s, ch, rate, 0, form, &x);
	return x;
}

static void test_samples(void)
{
	enum audio_sample s = AUDIO_NONE;

	CHECK_STR(audio_sample_name(AUDIO_S24), "s24");
	CHECK_STR(audio_sample_name(AUDIO_NONE), "none");
	CHECK_STR(audio_sample_name((enum audio_sample)99), "none");
	CHECK_EQ(audio_sample_from_name("f32", &s), 0);
	CHECK_EQ(s, AUDIO_F32);
	CHECK_EQ(audio_sample_from_name("s20", &s), -1);
	CHECK(strstr(vfw_error(), "s20") != NULL);
	CHECK_EQ(audio_sample_from_name("none", &s), -1);
	CHECK_EQ(audio_sample_bytes(AUDIO_U8), 1);
	CHECK_EQ(audio_sample_bytes(AUDIO_S24), 3);
	CHECK_EQ(audio_sample_bytes(AUDIO_F64), 8);
	CHECK_EQ(audio_sample_bytes(AUDIO_NONE), 0);
}

static void test_pcm_formats(void)
{
	size_t size;
	WAVEFORMATEXTENSIBLE x;

	/* The usual form: plain for 16-bit stereo. */
	x = pcm(AUDIO_S16, 2, 44100, AUDIO_FORM_USUAL, &size);
	CHECK_EQ(size, 18);
	CHECK_EQ(x.Format.wFormatTag, WAVE_FORMAT_PCM);
	CHECK_EQ(x.Format.nBlockAlign, 4);
	CHECK_EQ(x.Format.nAvgBytesPerSec, 176400);
	CHECK_EQ(x.Format.wBitsPerSample, 16);
	CHECK_EQ(x.Format.cbSize, 0);
	CHECK_EQ(audio_format_check(&x.Format, size), 0);
	CHECK_EQ(audio_format_sample(&x.Format, size), AUDIO_S16);
	CHECK_EQ(audio_format_tag(&x.Format, size), WAVE_FORMAT_PCM);

	/* Extensible for more than 16 bits, and for more than 2 channels. */
	x = pcm(AUDIO_S24, 2, 48000, AUDIO_FORM_USUAL, &size);
	CHECK_EQ(size, 40);
	CHECK_EQ(x.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
	CHECK_EQ(x.Format.cbSize, 22);
	CHECK_EQ(x.Samples.wValidBitsPerSample, 24);
	CHECK_EQ(x.dwChannelMask, 3);
	CHECK(!memcmp(&x.SubFormat, &KSDATAFORMAT_SUBTYPE_PCM, sizeof(GUID)));
	CHECK_EQ(audio_format_tag(&x.Format, size), WAVE_FORMAT_PCM);
	CHECK_EQ(audio_format_sample(&x.Format, size), AUDIO_S24);
	CHECK_EQ(audio_format_check(&x.Format, size), 0);

	x = pcm(AUDIO_F32, 6, 48000, AUDIO_FORM_USUAL, &size);
	CHECK_EQ(size, 40);
	CHECK_EQ(x.dwChannelMask, 0x3f);
	CHECK_EQ(audio_format_tag(&x.Format, size), WAVE_FORMAT_IEEE_FLOAT);
	CHECK_EQ(audio_format_sample(&x.Format, size), AUDIO_F32);
	CHECK_EQ(x.Format.nBlockAlign, 24);

	x = pcm(AUDIO_F64, 1, 8000, AUDIO_FORM_USUAL, &size);
	CHECK_EQ(size, 18);
	CHECK_EQ(x.Format.wFormatTag, WAVE_FORMAT_IEEE_FLOAT);
	CHECK_EQ(audio_format_sample(&x.Format, size), AUDIO_F64);

	/* Forced forms, and a given mask. */
	x = pcm(AUDIO_S32, 6, 48000, AUDIO_FORM_PLAIN, &size);
	CHECK_EQ(size, 18);
	CHECK_EQ(audio_format_sample(&x.Format, size), AUDIO_S32);
	CHECK_EQ(audio_pcm_format(AUDIO_U8, 2, 8000, 0x600,
				  AUDIO_FORM_EXTENSIBLE, &x), 40);
	CHECK_EQ(x.dwChannelMask, 0x600);
	CHECK_EQ(audio_format_sample(&x.Format, 40), AUDIO_U8);
	/* Read as 18 bytes, an extensible format is only its tag. */
	CHECK_EQ(audio_format_sample(&x.Format, 18), AUDIO_NONE);
	CHECK_EQ(audio_format_tag(&x.Format, 18), WAVE_FORMAT_EXTENSIBLE);
	CHECK_EQ(audio_pcm_format(AUDIO_NONE, 2, 8000, 0, AUDIO_FORM_USUAL,
				  &x), 0);

	CHECK_EQ(audio_channel_mask(1), SPEAKER_FRONT_CENTER);
	CHECK_EQ(audio_channel_mask(8), 0x63f);
	CHECK_EQ(audio_channel_mask(9), 0);
}

static void test_tags_and_checks(void)
{
	WAVEFORMATEXTENSIBLE x;
	size_t size;
	BYTE mp3[30] = { 0 };
	WAVEFORMATEX *wf = (WAVEFORMATEX *)mp3;

	/* A subformat that isn't a tag GUID stays "extensible". */
	x = pcm(AUDIO_S16, 4, 44100, AUDIO_FORM_USUAL, &size);
	x.SubFormat.Data2 = 0x1234;
	CHECK_EQ(audio_format_tag(&x.Format, size), WAVE_FORMAT_EXTENSIBLE);
	CHECK_EQ(audio_format_sample(&x.Format, size), AUDIO_NONE);
	/* A subformat holding a tag is that tag. */
	x.SubFormat = (GUID)WAVE_FORMAT_SUBTYPE_INIT(WAVE_FORMAT_MPEGLAYER3);
	CHECK_EQ(audio_format_tag(&x.Format, size), WAVE_FORMAT_MPEGLAYER3);

	wf->wFormatTag = WAVE_FORMAT_MPEGLAYER3;
	wf->nChannels = 2;
	wf->nSamplesPerSec = 44100;
	wf->nAvgBytesPerSec = 16000;
	wf->nBlockAlign = 1;
	wf->cbSize = 12;
	CHECK_EQ(audio_format_check(wf, sizeof(mp3)), 0);
	CHECK_EQ(audio_format_sample(wf, sizeof(mp3)), AUDIO_NONE);
	/* cbSize must fit. */
	CHECK_EQ(audio_format_check(wf, 20), -1);
	CHECK(strstr(vfw_error(), "12 bytes") != NULL);
	/* A 16-byte WAVEFORMAT is fine. */
	CHECK_EQ(audio_format_check(wf, 16), 0);
	CHECK_EQ(audio_format_check(wf, 15), -1);
	CHECK_EQ(audio_format_check(NULL, 18), -1);

	x = pcm(AUDIO_S16, 2, 44100, AUDIO_FORM_PLAIN, &size);
	x.Format.nAvgBytesPerSec++;
	CHECK_EQ(audio_format_check(&x.Format, size), -1);
	CHECK(strstr(vfw_error(), "byte rate") != NULL);
	x = pcm(AUDIO_S16, 2, 44100, AUDIO_FORM_PLAIN, &size);
	x.Format.wBitsPerSample = 12;
	CHECK_EQ(audio_format_check(&x.Format, size), -1);
	x = pcm(AUDIO_F32, 2, 44100, AUDIO_FORM_PLAIN, &size);
	x.Format.wBitsPerSample = 16;
	x.Format.nBlockAlign = 4;
	x.Format.nAvgBytesPerSec = 4 * 44100;
	CHECK_EQ(audio_format_check(&x.Format, size), -1);
	x = pcm(AUDIO_S16, 2, 44100, AUDIO_FORM_PLAIN, &size);
	x.Format.nChannels = 0;
	CHECK_EQ(audio_format_check(&x.Format, size), -1);
	CHECK(strstr(vfw_error(), "channels") != NULL);
	x = pcm(AUDIO_S16, 2, 44100, AUDIO_FORM_EXTENSIBLE, &size);
	x.Samples.wValidBitsPerSample = 20;
	CHECK_EQ(audio_format_check(&x.Format, size), -1);
	x.Samples.wValidBitsPerSample = 12;
	CHECK_EQ(audio_format_check(&x.Format, size), 0);
	x.Format.cbSize = 10;
	CHECK_EQ(audio_format_check(&x.Format, 28), -1);

	CHECK_STR(audio_tag_name(WAVE_FORMAT_MPEGLAYER3), "MP3");
	CHECK_STR(audio_tag_name(0x2000), "AC-3");
	CHECK_STR(audio_tag_name(0x6750), "Vorbis");
	CHECK(audio_tag_name(0x1234) == NULL);
}

static void test_desc(void)
{
	char buf[AUDIO_DESC_SIZE];
	WAVEFORMATEXTENSIBLE x;
	size_t size;
	WAVEFORMATEX mp3 = { WAVE_FORMAT_MPEGLAYER3, 2, 44100, 16000, 1, 0, 0 };
	WAVEFORMATEX odd = { 0x1234, 1, 8000, 1000, 10, 0, 0 };

	x = pcm(AUDIO_S16, 2, 44100, AUDIO_FORM_USUAL, &size);
	CHECK_STR(audio_format_desc(&x.Format, size, buf),
		  "PCM s16, 2 channels, 44100 Hz");
	x = pcm(AUDIO_S32, 1, 96000, AUDIO_FORM_USUAL, &size);
	CHECK_STR(audio_format_desc(&x.Format, size, buf),
		  "PCM s32 extensible, 1 channel, 96000 Hz");
	x.Samples.wValidBitsPerSample = 24;
	CHECK_STR(audio_format_desc(&x.Format, size, buf),
		  "PCM s32 (24 valid bits), 1 channel, 96000 Hz");
	CHECK_STR(audio_format_desc(&mp3, sizeof(mp3), buf),
		  "MP3 (0x0055), 2 channels, 44100 Hz, 128 kb/s");
	CHECK_STR(audio_format_desc(&odd, sizeof(odd), buf),
		  "wave format 0x1234, 1 channel, 8000 Hz, 8 kb/s");
	CHECK_STR(audio_format_desc(&odd, 10, buf), "10-byte audio format");
}

static void test_preferences(void)
{
	WAVEFORMATEXTENSIBLE out[16], in;
	WAVEFORMATEX mp3 = { WAVE_FORMAT_MPEGLAYER3, 2, 44100, 16000, 1, 0, 0 };
	size_t n;

	n = audio_output_preferences(&mp3, sizeof(mp3), AUDIO_NONE, out, 16);
	CHECK_EQ(n, 10);
	CHECK_EQ(out[0].Format.wFormatTag, WAVE_FORMAT_PCM);
	CHECK_EQ(out[0].Format.wBitsPerSample, 16);
	CHECK_EQ(out[0].Format.nChannels, 2);
	CHECK_EQ(out[0].Format.nSamplesPerSec, 44100);
	CHECK_EQ(out[1].Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
	CHECK_EQ(out[1].Format.wBitsPerSample, 16);
	CHECK_EQ(out[2].Format.wBitsPerSample, 24);
	CHECK_EQ(out[6].Format.wFormatTag, WAVE_FORMAT_IEEE_FLOAT);
	CHECK_EQ(out[7].Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
	CHECK_EQ(out[8].Format.wBitsPerSample, 8);

	/* Above two channels, extensible first, with the input's speakers. */
	audio_pcm_format(AUDIO_F32, 6, 48000, 0x60f, AUDIO_FORM_EXTENSIBLE,
			 &in);
	n = audio_output_preferences(&in.Format, 40, AUDIO_S24, out, 16);
	CHECK_EQ(n, 2);
	CHECK_EQ(out[0].Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
	CHECK_EQ(out[0].dwChannelMask, 0x60f);
	CHECK_EQ(out[0].Format.wBitsPerSample, 24);
	CHECK_EQ(out[1].Format.wFormatTag, WAVE_FORMAT_PCM);
	CHECK_EQ(out[1].Format.nChannels, 6);

	/* max is respected; a broken input gives nothing. */
	CHECK_EQ(audio_output_preferences(&mp3, sizeof(mp3), AUDIO_NONE, out, 3), 3);
	mp3.nChannels = 0;
	CHECK_EQ(audio_output_preferences(&mp3, sizeof(mp3), AUDIO_NONE, out, 16), 0);
}

static void test_media_types(void)
{
	WAVEFORMATEXTENSIBLE x;
	AM_MEDIA_TYPE mt;
	WAVEFORMATEX *back;
	size_t size, bsize;
	GUID mp3_sub = WAVE_FORMAT_SUBTYPE_INIT(WAVE_FORMAT_MPEGLAYER3);
	BYTE mp3[30] = { 0 };
	WAVEFORMATEX *wf = (WAVEFORMATEX *)mp3;

	x = pcm(AUDIO_S16, 2, 44100, AUDIO_FORM_PLAIN, &size);
	CHECK_EQ(audio_wfx_to_mt(&x.Format, size, &mt), S_OK);
	CHECK(!memcmp(&mt.majortype, &MEDIATYPE_Audio, sizeof(GUID)));
	CHECK(!memcmp(&mt.subtype, &MEDIASUBTYPE_PCM, sizeof(GUID)));
	CHECK(!memcmp(&mt.formattype, &FORMAT_WaveFormatEx, sizeof(GUID)));
	CHECK_EQ(mt.cbFormat, 18);
	CHECK_EQ(mt.bFixedSizeSamples, TRUE);
	CHECK_EQ(mt.lSampleSize, 4);
	CHECK_EQ(audio_mt_to_wfx(&mt, &back, &bsize), S_OK);
	CHECK_EQ(bsize, 18);
	CHECK(!memcmp(back, &x.Format, 18));
	free(back);
	ole_taskmem_free(mt.pbFormat);

	/* Extensible float: the subtype is the SubFormat. */
	x = pcm(AUDIO_F32, 6, 48000, AUDIO_FORM_USUAL, &size);
	CHECK_EQ(audio_wfx_to_mt(&x.Format, size, &mt), S_OK);
	CHECK(!memcmp(&mt.subtype, &MEDIASUBTYPE_IEEE_FLOAT, sizeof(GUID)));
	CHECK_EQ(mt.cbFormat, 40);
	CHECK_EQ(audio_mt_to_wfx(&mt, &back, &bsize), S_OK);
	CHECK_EQ(bsize, 40);
	CHECK(!memcmp(back, &x, 40));
	free(back);
	ole_taskmem_free(mt.pbFormat);

	/* Compressed, with extra bytes; and a 16-byte form. */
	wf->wFormatTag = WAVE_FORMAT_MPEGLAYER3;
	wf->nChannels = 2;
	wf->nSamplesPerSec = 44100;
	wf->nAvgBytesPerSec = 16000;
	wf->nBlockAlign = 1;
	wf->cbSize = 12;
	mp3[18] = 1;
	CHECK_EQ(audio_wfx_to_mt(wf, sizeof(mp3), &mt), S_OK);
	CHECK(!memcmp(&mt.subtype, &mp3_sub, sizeof(GUID)));
	CHECK_EQ(mt.cbFormat, 30);
	CHECK_EQ(mt.bFixedSizeSamples, FALSE);
	CHECK_EQ(mt.lSampleSize, 0);
	CHECK_EQ(mt.pbFormat[18], 1);
	ole_taskmem_free(mt.pbFormat);
	CHECK_EQ(audio_wfx_to_mt(wf, 16, &mt), S_OK);
	CHECK_EQ(mt.cbFormat, 18);
	CHECK_EQ(((WAVEFORMATEX *)mt.pbFormat)->cbSize, 0);
	/* A media type whose cbSize says more than cbFormat holds. */
	((WAVEFORMATEX *)mt.pbFormat)->cbSize = 50;
	CHECK_EQ(audio_mt_to_wfx(&mt, &back, &bsize), S_OK);
	CHECK_EQ(bsize, 18);
	CHECK_EQ(back->cbSize, 0);
	free(back);
	/* A 16-byte media type format. */
	mt.cbFormat = 16;
	CHECK_EQ(audio_mt_to_wfx(&mt, &back, &bsize), S_OK);
	CHECK_EQ(bsize, 18);
	CHECK_EQ(back->nSamplesPerSec, 44100);
	free(back);
	/* Not audio. */
	mt.majortype = MEDIATYPE_Video;
	CHECK_EQ(audio_mt_to_wfx(&mt, &back, &bsize), E_INVALIDARG);
	CHECK(back == NULL);
	ole_taskmem_free(mt.pbFormat);
	CHECK_EQ(audio_wfx_to_mt(wf, 10, &mt), E_INVALIDARG);
}

static void test_convert(void)
{
	int16_t s16[] = { 0, 1, -1, 32767, -32768, 12345, -12345, 256 };
	enum { N = sizeof(s16) / sizeof(s16[0]) };
	BYTE a[N * 8], b[N * 8];
	int16_t back[N];
	static const enum audio_sample all[] = {
		AUDIO_S24, AUDIO_S32, AUDIO_F32, AUDIO_F64,
	};

	/* Widening and back is exact, through every wider format. */
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		audio_convert(AUDIO_S16, s16, all[i], a, N);
		audio_convert(all[i], a, AUDIO_S16, back, N);
		CHECK(!memcmp(back, s16, sizeof(s16)));
	}

	/* Known values. */
	float f[N];
	audio_convert(AUDIO_S16, s16, AUDIO_F32, f, N);
	CHECK(f[3] == 32767 / 32768.0f);
	CHECK(f[4] == -1.0f);
	BYTE u8[N];
	audio_convert(AUDIO_S16, s16, AUDIO_U8, u8, N);
	CHECK_EQ(u8[0], 128);
	CHECK_EQ(u8[3], 255);
	CHECK_EQ(u8[4], 0);
	CHECK_EQ(u8[7], 129);
	audio_convert(AUDIO_U8, u8, AUDIO_S16, back, N);
	CHECK_EQ(back[7], 256);
	CHECK_EQ(back[4], -32768);
	audio_convert(AUDIO_S16, s16, AUDIO_S24, a, N);
	CHECK_EQ(a[3 * 7], 0);
	CHECK_EQ(a[3 * 7 + 1], 0);
	CHECK_EQ(a[3 * 7 + 2], 1);

	/* Floats clip and round. */
	float fl[] = { 1.5f, -2.0f, 1.0f, 0.5f / 32768, 1.5f / 32768, NAN };
	int16_t got[6];
	audio_convert(AUDIO_F32, fl, AUDIO_S16, got, 6);
	CHECK_EQ(got[0], 32767);
	CHECK_EQ(got[1], -32768);
	CHECK_EQ(got[2], 32767);
	CHECK_EQ(got[3], 0);	/* ties to even */
	CHECK_EQ(got[4], 2);
	CHECK_EQ(got[5], 0);
	int32_t i32[2];
	audio_convert(AUDIO_F32, fl, AUDIO_S32, i32, 2);
	CHECK_EQ(i32[0], INT32_MAX);
	CHECK_EQ(i32[1], INT32_MIN);
	double d[2];
	audio_convert(AUDIO_F32, fl, AUDIO_F64, d, 2);
	CHECK(d[0] == 1.5 && d[1] == -2.0);

	/* In place, narrowing; same format is a copy. */
	memcpy(b, s16, sizeof(s16));
	audio_convert(AUDIO_S16, b, AUDIO_U8, b, N);
	CHECK(!memcmp(b, u8, N));
	audio_convert(AUDIO_S16, s16, AUDIO_S16, a, N);
	CHECK(!memcmp(a, s16, sizeof(s16)));

	CHECK_EQ(audio_converted_size(AUDIO_S16, AUDIO_S24, 100), 150);
	CHECK_EQ(audio_converted_size(AUDIO_F64, AUDIO_U8, 80), 10);
	CHECK_EQ(audio_converted_size(AUDIO_NONE, AUDIO_U8, 80), 0);
}

static void test_ticks(void)
{
	CHECK_EQ(audio_ticks(44100, 1, 44100), 10000000);
	CHECK_EQ(audio_ticks(1, 1, 3), 3333333);
	CHECK_EQ(audio_ticks(5, 2, 4), 25000000);
	CHECK_EQ(audio_ticks(10, 1, 0), 0);
	/* Large values don't overflow along the way. */
	CHECK_EQ(audio_ticks(0xffffffffull, 0xffffffffu, 0xffffffffu),
		 0xffffffffll * 10000000);
	CHECK_EQ(audio_ticks(48000ull * 3600 * 24, 1, 48000),
		 3600ll * 24 * 10000000);
	CHECK_EQ(audio_ticks(1000, 1152, 44100),
		 1000ll * 1152 * 10000000 / 44100);
}

int main(void)
{
	test_samples();
	test_pcm_formats();
	test_tags_and_checks();
	test_desc();
	test_preferences();
	test_media_types();
	test_convert();
	test_ticks();
	return test_summary("test_audio");
}
