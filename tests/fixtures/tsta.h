/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TSTA, the fixtures' lossless audio codec: PCM in packets of up to
 * TSTA_FRAMES sample frames, each byte stored as its difference from the
 * same byte of the sample frame before (the first frame of a packet from
 * zero), so every packet decodes on its own and back to the exact PCM.
 *
 *   packet  'T' 'A' bits channels, DWORD frames, frames * block bytes
 *
 * Its format is a WAVEFORMATEX with the tag WAVE_FORMAT_TSTA, the PCM's
 * channels, rate and bits per sample, a block of a whole packet, and two
 * bytes after it: the frames in a whole packet (as IMA ADPCM gives its
 * samples per block).
 */
#ifndef TSTA_H
#define TSTA_H

#define WAVE_FORMAT_TSTA 0x7354
#define TSTA_FRAMES 1024
#define TSTA_HEADER 8

/* 64-by-32-bit division, which needs no libgcc helper. */
static inline DWORDLONG tsta_udiv64(DWORDLONG n, DWORD d)
{
	DWORD hi = (DWORD)(n >> 32), lo = (DWORD)n, qh = hi / d, r = hi % d, ql;

	__asm__("divl %4" : "=a"(ql), "=d"(r) : "a"(lo), "d"(r), "rm"(d));
	return (DWORDLONG)qh << 32 | ql;
}

/* The time of frames sample frames at rate, in 100 ns units. */
static inline DWORDLONG tsta_ticks(DWORDLONG frames, DWORD rate)
{
	return tsta_udiv64(frames * 10000000u, rate);
}

/* The bytes of one PCM sample frame, or 0 if bits isn't 8, 16, 24 or 32. */
static inline DWORD tsta_pcm_block(DWORD bits, DWORD channels)
{
	if ((bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
	    !channels || channels > 8)
		return 0;
	return bits / 8 * channels;
}

/* The format of TSTA made from PCM of these bits, channels and rate. */
static inline void tsta_format(WAVEFORMATEX *wf, WORD bits, WORD channels,
			       DWORD rate, DWORD bitrate)
{
	DWORD block = tsta_pcm_block(bits, channels);

	wf->wFormatTag = WAVE_FORMAT_TSTA;
	wf->nChannels = channels;
	wf->nSamplesPerSec = rate;
	wf->wBitsPerSample = bits;
	wf->nBlockAlign = (WORD)(TSTA_HEADER + TSTA_FRAMES * block);
	/* The data rate, or the rate asked for (as real encoders give it). */
	wf->nAvgBytesPerSec = bitrate ? bitrate / 8 :
			      rate * block + rate / (TSTA_FRAMES / TSTA_HEADER);
	wf->cbSize = 2;
	((BYTE *)(wf + 1))[0] = (BYTE)TSTA_FRAMES;
	((BYTE *)(wf + 1))[1] = (BYTE)(TSTA_FRAMES >> 8);
}

/* Encodes frames sample frames of PCM. Returns the packet's size. */
static inline DWORD tsta_encode(const BYTE *pcm, DWORD frames, DWORD bits,
				DWORD channels, BYTE *out)
{
	DWORD block = tsta_pcm_block(bits, channels), n = frames * block;

	out[0] = 'T';
	out[1] = 'A';
	out[2] = (BYTE)bits;
	out[3] = (BYTE)channels;
	out[4] = (BYTE)frames;
	out[5] = (BYTE)(frames >> 8);
	out[6] = (BYTE)(frames >> 16);
	out[7] = (BYTE)(frames >> 24);
	for (DWORD i = 0; i < n; i++)
		out[TSTA_HEADER + i] = (BYTE)(pcm[i] - (i >= block ?
							pcm[i - block] : 0));
	return TSTA_HEADER + n;
}

/*
 * Decodes a packet of len bytes into at most max sample frames of PCM.
 * Returns the frames, or -1 if it isn't a packet of this format.
 */
static inline LONG tsta_decode(const BYTE *in, DWORD len, DWORD bits,
			       DWORD channels, BYTE *pcm, DWORD max)
{
	DWORD block = tsta_pcm_block(bits, channels);

	if (!block || len < TSTA_HEADER || in[0] != 'T' || in[1] != 'A' ||
	    in[2] != bits || in[3] != channels)
		return -1;
	DWORD frames = in[4] | (DWORD)in[5] << 8 | (DWORD)in[6] << 16 |
		       (DWORD)in[7] << 24;
	if (frames > max || frames > TSTA_FRAMES ||
	    len != TSTA_HEADER + frames * block)
		return -1;
	for (DWORD i = 0; i < frames * block; i++)
		pcm[i] = (BYTE)(in[TSTA_HEADER + i] + (i >= block ?
						       pcm[i - block] : 0));
	return (LONG)frames;
}

#endif /* TSTA_H */
