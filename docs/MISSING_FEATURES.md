<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Missing features found with real-world filters

This file records compatibility gaps found by running `tests/samples.sh`
against third-party codec and DirectShow binaries. It is an observation log,
not a promise to support every component in a tested package: seance's
DirectShow and DMO scope is transform decoders and encoders, of video and
(since the DMO/audio plan) of audio.

## LAVFilters 0.83 (64-bit)

Tested on 2026-11-07 with the official `LAVFilters-0.83-x64.zip` release (the
`.ax` filters are MSVC builds with a statically linked C++ runtime; the FFmpeg
libraries are GCC builds), in `seance64` and through the hand-off from
`seance`:

```sh
curl -L -o /tmp/lavfilters64.zip \
  https://github.com/Nevcairiel/LAVFilters/releases/download/0.83/LAVFilters-0.83-x64.zip
unzip /tmp/lavfilters64.zip -d /tmp/lavfilters64
sh tests/samples.sh build/seance64 /tmp/lavfilters64 60
```

All 11 DLLs load with no unimplemented imports; `LAVVideo.ax` and
`LAVAudio.ax` are found as a video and an audio decoder. Decoding 2-second
clips made with ffmpeg (160x120 at 10 fps, and 44.1/48 kHz mono):

| Stream | Through | Compared with ffmpeg's decode |
|---|---|---|
| H.264 (`H264`) | `LAVVideo.ax` | pixel-identical (PSNR inf on all 20 frames) |
| MPEG-4 part 2 (`FMP4`) | `LAVVideo.ax` | pixel-identical |
| AC-3 | `LAVAudio.ax` | sample-identical |
| MPEG audio layer 2 | `LAVAudio.ax` | within rounding (mean difference 0.24 at a peak of about 4100) |
| H.265 / HEVC (`HEVC`, Annex B, 160x120, 20 frames) | `LAVVideo.ax` | pixel-identical (all 20 frames byte-equal) |
| AAC-LC (tag `0x00ff`, stereo 48 kHz, 128 kb/s) | `LAVAudio.ax` | sample-identical (97280 samples per channel) |
| VP9 (`VP90`, 160x120, 20 frames) | `LAVVideo.ax` | pixel-identical (all 20 frames byte-equal) |
| Opus (tag `0x704f`, `OpusHead` as extradata, stereo 48 kHz, 96 kb/s) | `LAVAudio.ax` | sample-identical over the 96000 frames ffmpeg gives; LAV also outputs the encoder's 648 frames of end padding, which ffmpeg trims from the Ogg end position (an AVI has none) |

`seance decode LAVVideo.ax clip.avi` from the i386 binary hands off and gives
the same bytes. Getting here needed msvcrt additions
(`_time64`, `_stat64`, `_aligned_malloc`, ...) and the processor topology
(`GetLogicalProcessorInformationEx`, which MSVC's Concurrency Runtime needs
to start its scheduler), with the unwinder fixes found on the way. Not tried: `LAVSplitter.ax` (a source filter), hardware
decoding, and anything but these eight codecs. (ffmpeg's AVI muxer won't take Opus, so
the Opus AVI was written by hand from the Ogg packets.)

A test clip whose AVI header disagrees with its frame times (say, 20 fps in
the header for frames 0.1 s apart, as `ffmpeg -c copy` from an MP4 can leave
it) comes out with repeated frames: the output writer places frames by
timestamp. Make the clip with a consistent rate (`-framerate`/`-r`).

## LAVFilters 0.83 (64-bit): every format

Tested on 2026-11-07 with `tests/lav_matrix/run_video.py` and `run_audio.py`
(the 0.83 x64 release in `seance64`; ffmpeg 6 makes the clips and gives the
reference decodes). Each case is a short clip (10 frames of `testsrc2`, 176x144
unless a codec needs another size; 1.5 s of stereo tones and noise) in an AVI.
Video is compared by PSNR after both sides are converted to 8-bit 4:2:0, and
RGB codecs also as BGR24 bytes; audio by its 16-bit samples. To repeat it:

```sh
SEANCE=build/seance64 LAV_DIR=/tmp/lavfilters64 WORK=/tmp/lav_matrix \
  python3 tests/lav_matrix/run_video.py    # or run_audio.py, or a label name
```

The filters are MSVC builds whose exception handling is `__CxxFrameHandler3`
tables in a statically linked handler (FuncInfo magic `0x19930522`), so they
exercise the SEH core and not msvcrt's C++ runtime.

**Video: 174 clips (covering 152 of the 262 FOURCCs LAV declares; the rest are formats
ffmpeg can't encode).** Results where the decoder runs:

| Result | Formats |
|---|---|
| Bit-identical to ffmpeg | H.264 (also with B-frames; `H264 h264 X264 x264 avc1 AVC1 CCV1`), HEVC (`HEVC HVC1 HM10 H265`), MPEG-1, MPEG-2 (`MPG2 mpg2 MP2V`), MPEG-4 ASP (24 FOURCCs: `FMP4 XVID DIVX DX50 MP4V M4S2 3IVX BLZ0 SEDG ...`), MS MPEG-4 v2 and v3 (19 FOURCCs, `DIV2`..`DIV6`, `MPG3`, `DVX3`), WMV1, WMV2, FLV1, H.263 and H.263+, H.261, JPEG 2000 (5 FOURCCs), DV PAL (12 FOURCCs), Theora, VP8, VP9, AV1 (libaom and SVT), FFVHuff, FFV1, Snow, Ut Video 4:2:0, ASV1, ASV2, WebP (as YV12), MJPEG (raw YUV planes) |
| Bit-identical as BGR24 | PNG, TIFF, Targa, BMP, GIF, zlib, ZMBV, QuickTime RLE, Cinepak, MS Video 1 (`CRAM MSVC WHAM`), Apple RPZA, FFV1 RGB, Ut Video RGB and RGBA, Hap1, Hap5, HapY, lossless JPEG (`LJPG`) |
| 42 to 62 dB (10-bit, 4:2:2 or 4:4:4 sources compared after conversion to 8-bit 4:2:0; with `--native`, bit-identical, see below) | H.264, HEVC and VP9 at 10 bits, ProRes (all five profiles, `apch apcn apcs apco ap4h ap4x`), DNxHD at 1080p, CineForm (`CFHD`), MagicYUV, HuffYUV, Ut Video 4:2:2 and 4:4:4, JPEG-LS, DV 4:1:1 and DVCPRO50, SVQ1 |
| Decodes; differs from ffmpeg | `smc` (Apple Graphics): all black, on the 32-bit LAV too (see below). VC-2 (`drac`/`vcrd`) from a 10-bit source: 30 dB from ffmpeg's decode, the same on the 32-bit LAV (see below). `GEOV`: GeoVision's MPEG-4 variant, not the bitstream made |
| Refused by LAV's input pin (on the 32-bit LAV too) | `v210`, uncompressed YUY2, UYVY, YV12, NV12, BGR24, `CLJR` |
| Not testable | RealVideo 1.0 and 2.0 (`RV10`, `RV20`): ffmpeg can't decode the AVI it writes, and LAV rejects it. MS MPEG-4 v1 (`MPG4 MP41 DIV1`): ffmpeg has no encoder. `MJPB`, `VP70`, `VP8X`: these FOURCCs name other codecs, and the clips are not their bitstreams |

Not generated (no ffmpeg encoder): VC-1 and WMV3 (`WVC1 WMV3 WMVA WVP2 WMVP`),
VP6 and FLV4, RealVideo 3/4, SVQ3, Indeo, TechSmith screen codecs, Lagarith,
Bink, Smacker, MVC and VVC, Ut Video and MagicYUV variants beyond those above,
DVCPRO HD, and the rest of the long tail of game and screen codecs LAV
declares (`8BPS LOCO VCR1 AASC VMnc G2M4 DUCK TM20 ...`).

**Audio: 28 clips.**

| Result | Formats |
|---|---|
| Sample-identical | AC-3 (stereo and 5.1), E-AC-3, MP3 (stereo; mono at 22 kHz), AAC-LC (stereo and 5.1), DTS, FLAC (16 and 24 bit), Vorbis, Opus, MS ADPCM, A-law, mu-law |
| Within one LSB | MP2, Speex |
| Decodes; differs from ffmpeg | IMA ADPCM (`0x0011`): max difference 42, mean 8 at a peak of 3000; identical on the 32-bit LAV |
| Sample-identical once enabled | WMA 1 and 2: LAV Audio has them off by default (its changelog: "Disabled the WMA codecs by default"), hence `VFW_E_UNSUPPORTED_AUDIO`; `HKCU\Software\LAV\Audio\Formats` `wma`=1 (what `tests/lav_matrix/run_audio.py` sets) turns them on, and every sample then matches ffmpeg's (LAV gives 2048 frames fewer at the end) |
| Refused by LAV (32-bit too) | G.726, G.722, Yamaha ADPCM (none is in its input list) |
| Not run | PCM (`seance decode` won't take PCM), and formats ffmpeg can't encode: AMR, Cook and the other Real audio codecs but 14.4, ... |
| Sample-identical, by `--subtype` | ALAC, TrueHD and MLP: LAV knows them by subtypes no 16-bit wave format tag stands for, and ffmpeg won't write them into an AVI under any tag. `tests/lav_matrix/mkavi_audio.py` puts ffmpeg's Matroska packets in an AVI by hand (`run_audio.py` does this for them), and `decode --subtype alac`, `'MLP '` or `{EB27CEC4-163E-4CA3-8B74-8E25F91B517E}` (TrueHD) says what they are; the PCM is the source's, bit for bit, on the 64- and 32-bit LAV |
| Identical to ffmpeg's decode, by `--subtype` | RealAudio 1.0 (14.4, `--subtype 14_4`) and Nellymoser (`NELL`): lossy, so compared with ffmpeg's own decode; `mkavi_audio.py` makes their AVIs, Nellymoser's from FLV (Matroska won't take it). Nellymoser on the 32-bit LAV is within 2 LSB, a float codec's rounding |
| Sample-identical, hand-tagged | WavPack: `ffmpeg -c:a wavpack -tag:a 0x5756 -strict unofficial` makes an AVI its own demuxer doesn't know but LAV takes; the PCM is the source's, bit for bit |
| Sample-identical once enabled, with its header | TTA: LAV has it off by default (`HKCU\Software\LAV\Audio\Formats` `tta`=1 turns it on), and FFmpeg's TTA decoder won't open without the 22-byte TTA1 header as its extra data, which `ffmpeg -c:a tta -tag:a 0x77a1` leaves out of an AVI (hence `VFW_E_UNSUPPORTED_AUDIO` from LAV's `avcodec_open2`). `mkavi_audio.py <in.mka> <out.avi> 0x77a1` carries the header Matroska's demuxer rebuilds; the PCM is then the source's, bit for bit, on the 64- and 32-bit LAV |

Longer clips and seeking (LAV x64): a 12 s H.264 AVI with B-frames and a
keyframe every second decodes 300 frames sample-for-sample like ffmpeg, and
`--start 5s --frames 50` gives frames 125 to 174 of that decode exactly
(the seek starts at the keyframe before it). A 40 s AC-3 5.1 stream decodes
byte-identical to ffmpeg (1.92 M samples). The IMA ADPCM difference above
is the libavcodec version one: ffmpeg 6.1 here, LAV's is 8.x.

Neither video difference is seance's. LAV Video
(`decoder/LAVVideo/decoders/avcodec.cpp`) builds a palette from the
format block only for MSVIDEO1, MSRLE, Cinepak, 8BPS, QPEG, QTRLE and
TSCC, and only when LAV Splitter isn't upstream. seance passes the
AVI's 256-colour table in the `VIDEOINFOHEADER`, but SMC isn't on that
list, so libavcodec decodes it against an all-zero palette. VC-2 from an
8-bit source (176x144 and 352x288, 4:2:0) decodes bit-identically to
ffmpeg. The matrix's 10-bit 4:2:2 stream at 176x144 is already 20 dB
from the source in both decoders (ffmpeg 6.1's encoder at that size).
At 352x288 the two decodes are 50 dB apart, which is LAV dithering 10
bits down to YV12.

Two things this turned up besides the table: `GetLogicalProcessorInformationEx`
was a stub (MSVC's Concurrency Runtime, behind `parallel_for`, threw
`scheduler_resource_allocation_error` as soon as a filter converted colours
in parallel), and every `--output-format` request for RGB from a YUV decoder
crashed on it. Both fixed.

## LAVFilters 0.83 (32-bit)

Tested on 2026-10-04 with the official `LAVFilters-0.83-x86.zip` release:

```sh
curl -L -o /tmp/lavfilters.zip \
  https://github.com/Nevcairiel/LAVFilters/releases/download/0.83/LAVFilters-0.83-x86.zip
unzip /tmp/lavfilters.zip -d /tmp/lavfilters
VFW_EMULATOR=qemu-i386 \
  sh tests/samples.sh build/seance /tmp/lavfilters 30
```

The run examined 11 `.ax`/`.dll` files. The PE parser handled every file, but
the run reported one harness failure and none of the three LAV filters could
be loaded far enough to discover or connect their classes. (That was the first
run; what followed is below.)

### Immediate runtime blockers

| Component | Result | First blocker |
|---|---|---|
| `LAVVideo.ax` | 38 unimplemented imports; 5 disk DLLs | Missing `d3d9.dll` (`Direct3DCreate9`) |
| `LAVAudio.ax` | 6 unimplemented imports; 4 disk DLLs | Missing `CRYPT32.dll`, imported through `avformat-lav-63.dll` |
| `LAVSplitter.ax` | 13 unimplemented imports; 4 disk DLLs | Missing `CRYPT32.dll`, imported through `avformat-lav-63.dll` |
| `IntelQuickSyncDecoder.dll` | 36 unimplemented imports; 4 disk DLLs | Missing `d3d9.dll` |
| `avutil-lav-61.dll` | 38 unimplemented imports | Missing `bcrypt.dll` |
| `avcodec-lav-63.dll` | 22 unimplemented imports | Its `avutil-lav-61.dll` dependency needs `bcrypt.dll` |
| `avformat-lav-63.dll` | 69 unimplemented imports | Missing `CRYPT32.dll` |
| `libbluray.dll` | Probe aborted (exit 134) | Called unimplemented `KERNEL32!InitializeSListHead` |

The audio filter and splitter are outside the DirectShow transform scope, but
their failures identify runtime DLL gaps shared by modern filters. `LAVVideo`
is in scope and is currently blocked before its class can be instantiated.

### Work indicated by the run

In approximate priority order, with what became of each:

1. **Done.** `InitializeSListHead` and the rest of the interlocked list
   functions (`k32_modern.c`), so a dependency probe no longer aborts.
2. **Done.** The subset of `bcrypt.dll` the FFmpeg libraries use: the `RNG`
   provider (`BCryptOpenAlgorithmProvider`, `BCryptGenRandom`,
   `BCryptCloseAlgorithmProvider`). Other algorithms report `STATUS_NOT_FOUND`.
3. **Done.** The subset of `CRYPT32.dll` that `avformat-lav-63.dll` imports:
   certificate stores that are always empty.
4. **Done.** `d3d9.dll` reduced to `Direct3DCreate9` returning NULL and
   `Direct3DCreate9Ex` returning `D3DERR_NOTAVAILABLE`, which LAV Video takes as
   "no hardware decoding" and decodes in software.
5. **Done.** The imports seen in `LAVVideo.ax`: SRW locks, condition variables
   and `InitOnce*`; `PathIsFileSpecW`, `PathRemoveFileSpecW`;
   `FindFirstFileExW`; `CreateEventExW`/`CreateSemaphoreExW`;
   `VerifyVersionInfoW`/`VerSetConditionMask`; thread-group affinity, NUMA and
   processor-number queries; `GetThreadTimes`/`GetProcessTimes`;
   `Encode`/`DecodePointer`; `HeapQueryInformation`; stubs for
   `TrackPopupMenu` and `GetShellWindow`. Thread-pool timers and waits exist as
   stubs whose `Create*` fail (`ERROR_CALL_NOT_IMPLEMENTED`); nothing in the
   video path has needed them.
6. **Done for the video path.** See the rerun below.

### Rerun after those changes

Same package, same day. `tests/samples.sh build/seance /tmp/lavfilters 60`
reports 0 failures, and `LAVVideo.ax` has no unimplemented imports. Using
FFmpeg-made 64x48 AVIs:

| Stream | Result |
|---|---|
| H.264 (x264) | `probe` accepts YV12, YUY2, UYVY, BGR32 and BGR24; `decode` output is byte-identical to `ffmpeg -pix_fmt yuv420p`, including `--start`/`--frames` |
| MPEG-4 (`XVID`) | identical to FFmpeg's decode |
| Motion JPEG | identical to FFmpeg's `yuvj420p` decode |

`register` followed by `list` and `decode auto` also pick LAV Video up from the
registry. The run found three faults in the harness's graph rather than in
LAV, all fixed with tests: the source and sink pins had no owning filter (LAV
asks the filter on the other side of a pin for its class and pins);
`IMediaSample2::SetProperties` freed the media type it was about to copy; and
`decode` rejected a sample carrying a new media type, and let through frames
before `--start` or after `--frames` when a filter doesn't flag preroll.

### Second round: the rest of the package

The same package, the same day, working through what the first round left:

| Item | Result |
|---|---|
| `avformat-lav-63.dll`, `LAVAudio.ax`, `LAVSplitter.ax` | 0 unimplemented imports and every DLL found. Added: `ws2_32.dll` (and its `wsock32.dll` name) as Winsock with no network, `ncrypt.dll` (bcrypt's RNG plus key-store calls that report no provider), the CRT calls `_findfirst` family, `_stat`/`_wstat`, `_wfopen`, `_wfullpath`, `_wgetenv`, `_getcwd`, `_open_osfhandle`, `_getmaxstdio`/`_setmaxstdio`, `signal`/`raise`, `wcstombs`/`mbstowcs`; `CryptCreateHash` and the other CryptoAPI hash/key calls (which fail: no algorithm), `InsertMenuItemW`/`CreatePopupMenu`, `UrlGetPartW`, `CompareStringEx`, `LCMapStringEx`, `EnumSystemLocalesW`. `register --dry-run` finds LAV Audio's filter and LAV Splitter's two. |
| `IntelQuickSyncDecoder.dll` | 0 unimplemented imports and it loads: `dxva2.dll`, `d3d11.dll` and `dxgi.dll` exist and report no adapter (`DXGI_ERROR_UNSUPPORTED`, `D3DERR_NOTAVAILABLE`). |
| Thread-pool timers and waits | Real now (`k32_pool.c`): timers, waits, work items and `TrySubmitThreadpoolCallback`, with the `...WhenCallbackReturns` requests. |
| LAV Video inputs | See below. |
| LAV Video's hardware settings | `HWAccel` 1 to 5 (CUDA, Quick Sync, DXVA2 copy-back and native, D3D11) all fall back to software and decode identically. |

LAV Video, FFmpeg-made 64x48 AVIs, each compared with `ffmpeg -fps_mode
passthrough -pix_fmt yuv420p`:

| Input | Result |
|---|---|
| H.264, HEVC, VP9, AV1 | byte-identical |
| WMV1, WMV2, MS-MPEG4 v3 (`MP43`), H.263, FLV1, Theora, FFV1, MPEG-4 (`XVID`) | byte-identical |
| MPEG-1, MPEG-2 | byte-identical, once offered as `MPEG1Video`/`MPEG2_VIDEO` with an `MPEG1VIDEOINFO`/`MPEG2VIDEOINFO` (LAV refuses the FOURCC subtype); `decode` and `probe` now try that form after `VideoInfo` and `VideoInfo2` |
| HuffYUV (4:2:2) | identical in `YUY2`; in `YV12` it differs by LAV's own chroma conversion |
| Cinepak, MS Video 1 | identical in `BGR24` (the DIB comes out bottom-up, as for a VfW codec) |
| Motion JPEG | identical to FFmpeg's `yuvj420p` decode |

What the second round found and fixed in the harness, each with a test:
`SleepConditionVariable` let a thread that began waiting after a wake-up take it
from the thread it was meant for, which deadlocks FFmpeg's thread pool when it
wakes a worker and then waits for the answer on the same variable (it is now a
first-in first-out wait queue); and LAV's Quick Sync mode destroyed a member its
constructor never set when set-up failed, which only works if fresh memory is
zero, so memory from `malloc`, `operator new` and `HeapAlloc` is now zeroed by
default (as a fresh Windows heap usually gives; `VFW_ZERO_MEMORY=0` turns it
off). `struct _stat` also had the
wrong layout in `_fstat` and is now MSVCRT's.

### Still not supported

| Component | Limit |
|---|---|
| `LAVSplitter.ax` | It loads and registers, but splitters are outside the scope (transform decoders and encoders), so nothing drives it. `LAVAudio.ax` now decodes: see below. |
| Network | Winsock starts and resolves numeric addresses, but no socket can be created: `socket` fails as if the network were down, so FFmpeg's protocols find nothing reachable. |
| CryptoAPI hashes, keys; NCrypt | There is no hashing algorithm to create and no key storage provider to open. |
| Hardware decoding | Every route (DXVA2, D3D11, Quick Sync, CUDA) reports no hardware and LAV decodes in software. |

Interlaced streams (`run_video.py`'s `h264-mbaff`, `mpeg2-interlaced`, at
352x288) are bit-identical to ffmpeg on the 64-bit LAV, and the Y4M takes
the field order LAV gives each frame (`It` for the MPEG-2, `Ib` for x264's
MBAFF, as ffmpeg's own frames say). On the 32-bit LAV they are 98 dB: its
older FFmpeg decodes interlaced MPEG-2 a few pixels differently, though
progressive MPEG-2 is identical. LAV's YADIF (`SWDeintMode`=1 in
`HKCU\Software\LAV\Video`) is bit-identical to ffmpeg's `yadif` in both
output modes: a frame for two fields, and a frame for each field, at twice
the rate, which `decode` follows (before that, it wrote half the frames at
the stream's rate). After `--start`, the first frame's fields differ, as
YADIF has no earlier frame there. An AVI of two video streams decodes
either, identically, with `--stream`.

With `decode --native` (seance64, LAV Video 64-bit), the decoder's own
format is written as it is: 10-bit and 12-bit, 4:2:2 and 4:4:4 come out
as `C420p10`, `C422p10`, `C444p10`, `C444p16`, `C422` and `C444`, and are
bit-identical to ffmpeg's decode at the same pixel format for H.264
(10-bit; 4:2:2 and 4:4:4, 8 and 10 bits), HEVC (10-bit 4:2:0 and 4:2:2,
12-bit 4:4:4, which LAV gives as Y416), VP9 10-bit, ProRes (all five
profiles; 4444 as Y416), CineForm, DNxHD, DVCPRO50, HuffYUV, MagicYUV and
Ut Video 4:2:2 and 4:4:4. DV NTSC (4:1:1) and SVQ1 (4:1:0) aren't layouts
LAV gives, so it converts them, to 4:2:2 and 4:2:0 (45 and 49 dB). The
32-bit LAV gives the same for the H.264 streams. LAV connects as NV12
before it has seen the stream, then calls the sink's `ReceiveConnection`
with its own format (P010, P210, YV24, Y410, Y416, YUY2) and sends the
frames in it without marking them: `--native` takes that reconnection, as
a renderer does. Forced with `--output-format`, LAV's P010, P016, P210,
P216, YV24, Y410, Y416 and v410 are read exactly (identical where the
format keeps the stream's sampling and depth); it refuses YV16 and AYUV.
Its v210 is delivered only at widths that are a multiple of 48 (192x144
identical): at 176 its own type is 208 pixels wide in 512-byte rows, and
it sends nothing, which decode now reports as a failure.

Not yet exercised: VC-1 / WMV3 (no encoder to make a stream with here),
field-coded streams (ffmpeg makes no PAFF or MPEG-2 field pictures), and
LAV Video's other settings (dithering, the other deinterlacers).

### LAV Audio (DMO/audio plan, Phase 6)

Tested on 2026-10-08 with the same package, `tests/audio_codecs.sh`
comparing LAV Audio with FFmpeg 6.1.1's own decoding of streams FFmpeg made
in AVI files: stereo at 44100 Hz and 5.1 at 48000 Hz (a different tone in
each channel), decoded to float and to 16-bit:

```sh
printf 'Windows Registry Editor Version 5.00\n\n[HKEY_CURRENT_USER\\Software\\LAV\\Audio\\Formats]\n"wma"=dword:00000001\n' >lav.reg
VFW_REGISTRY=$PWD/lav.reg \
  sh tests/audio_codecs.sh build/seance /tmp/lavfilters/LAVAudio.ax /tmp/ac
```

| Codec | Against FFmpeg (float; 16-bit) | Notes |
|---|---|---|
| MP3 | identical; identical | |
| FLAC | identical; identical | LAV gives 16-bit, its own format, whatever is asked; converted back exactly |
| MS ADPCM, A-law | identical; identical | |
| AC-3, E-AC-3 | 142-143 dB; 107-114 dB | float rounding; 16-bit by rounding LAV's float |
| AAC | 141 dB; 106-108 dB | |
| Vorbis | 141 dB; 106-108 dB | once offered as `Vorbis2` with a `VORBISFORMAT2` (see below) |
| Opus | 146-148 dB; 109-111 dB | 648 more samples: the end padding Matroska tells FFmpeg to trim, which AVI can't carry |
| WMA (v2) | 141 dB; 107 dB | needs LAV's `wma` format setting (off by default: `VFW_E_UNSUPPORTED_AUDIO`); 2048 fewer samples, the frame FFmpeg's decoder gives when flushed at the end |
| MP2 | 72-77 dB; 70-75 dB | LAV's MPEG audio decoder rounds differently from FFmpeg 6.1's, by about a 16-bit step |
| IMA ADPCM | 46 dB (stereo), 36 dB (5.1) | a few 16-bit steps, growing through each block and reset at the next: the two libavcodec versions expand IMA nibbles differently; block and sample counts agree |

Every 5.1 stream comes out with its channels in order. What the run found,
all fixed with tests:

- **Vorbis** in AVI (FFmpeg's tag `0x566f`, its three headers Xiph-laced
  after the `WAVEFORMATEX`) wasn't taken: LAV registers Vorbis as
  `Vorbis2` with `FORMAT_VorbisFormat2`. Decoders are now offered a
  stream's format in each form decoders know it by
  (`audio_input_variant`): as it is, then AC-3 as `MEDIASUBTYPE_DOLBY_AC3`,
  DTS as `MEDIASUBTYPE_DTS`, MPEG audio as `MPEG1Audio`/`MPEG2_AUDIO`,
  AAC's other tags as `RAW_AAC1`, Vorbis as `VORBISFORMAT2`. `decode auto`
  matches registered decoders on any of these subtypes.
- **More than two channels** of a compressed format: FFmpeg writes them as
  `WAVE_FORMAT_EXTENSIBLE`, the codec's extra data after the extensible
  part. LAV took 5.1 AAC that way and decoded nothing, and refused 5.1 FLAC,
  Vorbis and Opus. Such formats are now offered unwrapped first (a
  `WAVEFORMATEX` of the real tag with the codec's own extra data), as
  splitters give them, then as they are.
- **Buffers for other format types**: the host sized audio input buffers
  only for `FORMAT_WaveFormatEx`, so a `VORBISFORMAT2` connection got a
  one-byte buffer (`VFW_E_BUFFER_OVERFLOW`). Any audio input now gets at
  least a megabyte.
- **Format changes counted**: LAV attaches its output type to sample after
  sample, unchanged; `decode` counted each as a change. Only a type that
  differs counts now (FLAC's switch to 16-bit, say).

Not working: FLAC in a WAV file (as FFmpeg writes it, with a block of 4
bytes, so no frame boundaries) decodes only its first frames, since LAV
Audio needs whole frames and only a splitter would find them; FLAC in AVI
decodes exactly. `tests/samples.sh` over the package reports LAV Audio as
one audio decoder and LAV Video as one video decoder.

No DMOs were tried: Windows' own (the WMA, WMV and MP3 DMOs) aren't
redistributable, and none were at hand. `testdmo.dll` covers the DMO
audio paths, and any DMO DLL can be run through `tests/audio_codecs.sh`.

## Wine 9.0's ACM drivers (32-bit)

Tested on 2026-10-08 with the i386 PE builds of Wine 9.0's five ACM
drivers, from the `wine-9.0-amd64` package of Kron4ek's Wine-Builds
(`lib/wine/i386-windows/*.acm`): `imaadp32.acm`, `msadp32.acm`,
`msg711.acm`, `msgsm32.acm` and `l3codeca.acm`. Windows' own drivers
aren't redistributable and none were at hand; no open-source ones (LAME
ACM, AC3ACM) were tried either.

### What they needed

At first `l3codeca.acm` didn't load (no `kernelbase.dll`), and all five
imported functions the harness didn't have. All fixed, with tests:

- **`kernelbase.dll`**: built in, as kernel32's functions and shlwapi's;
  shlwapi gained `PathCombineW` and `PathIsUNCA`/`W`.
- **ucrtbase's stdio**: `__acrt_iob_func`, and the `__stdio_common_*`
  functions its headers build `printf`, `sprintf`, `snprintf`, `sscanf`
  and their `_s` forms from, with their options (C99's `inf`, `nan` and
  two-digit exponents unless an option keeps msvcrt's). `sscanf` now
  reads msvcrt's sizes (`I64`, `I32`, `I`, `L` as double) instead of
  handing the format to glibc.
- **ntdll's `_vsnprintf`** (and `_snprintf`, `_snwprintf`, `_vsnwprintf`).
- **The process heap in the PEB**: Wine's `GetProcessHeap` reads
  `NtCurrentTeb()->Peb->ProcessHeap` inline, which was NULL until
  something called `GetProcessHeap`; every stream open then failed with
  `MMSYSERR_NOMEM`. The heap is now made before any DLL code runs.

The ACM backend changed too:

- `l3codeca.acm` lists no standard formats for MP3 or MPEG audio, so
  nothing showed it decoding them. A tag with none to try is now taken
  as decodable; the stream, opened from the real format, decides.
- `msadp32.acm` suggests an MS ADPCM format for PCM, but won't open a
  stream to make it ("We don't support encoding yet"). Whether a driver
  decodes or encodes is now checked with a query stream open as well.
- An IMA ADPCM encoder makes only whole blocks of 2041 samples, so up to
  2040 at the end were lost. The last part block is now padded with
  silence, as Windows applications do.
- G.711's blocks are one sample frame each, and each became an AVI
  chunk: 88200 for two seconds of stereo. Blocks under 1 KB now go out
  together.

### Results

`tests/audio_codecs.sh` with each driver, against FFmpeg 6.1.1's decoding
of streams FFmpeg made (float; 16-bit):

| Driver | Codec | Decoding | Encoding |
|---|---|---|---|
| `l3codeca.acm` | MP3 | 73.7 dB; 72.7 dB (44100 Hz), 69.0 dB; 68.1 dB (48000 Hz) | not offered (decoder only) |
| `l3codeca.acm` | MP2 | 72.3 dB (44100 Hz), 67.5 dB (48000 Hz) | not offered |
| `imaadp32.acm` | IMA ADPCM | 45.8 dB (stereo), 35.7 dB (5.1), as for LAV Audio | FFmpeg decodes it at 42 dB from the source; the driver at 45 dB |
| `msadp32.acm` | MS ADPCM | identical (stereo and 5.1) | not supported by the driver |
| `msg711.acm` | A-law, mu-law | identical (stereo); 5.1 refused by the driver | FFmpeg reads it; 38-39 dB from the source, as FFmpeg's own encoders (some codes rounded the other way) |
| `msgsm32.acm` | GSM 6.10 (mono, 8000 Hz) | identical | not supported by the driver |

MP3 and MP2 differ from FFmpeg by its and mpg123's rounding (`l3codeca`
is mpg123), about a 16-bit step. FFmpeg can't put 5.1 MP3 or MP2 in AVI,
so their "5.1" runs are stereo at 48000 Hz. `tests/samples.sh` over the
five reports no unimplemented imports, and each driver's tags with what
it does with them.

## Other codecs (32-bit), October 2026

Single codecs tried one at a time, with what each needed:

| Codec | Kind | Result | What it needed |
|---|---|---|---|
| Sunplus `sp4x_32.dll`, `sp6x_32.dll` (SP41-SP47, SP60-SP62) | VfW | decoders only: every compress message is a stub; 12-bit input at 160x120 to 640x480 only | `encode` and `probe` now say a codec only decompresses |
| Pegasus `pvwv220.dll` (PVW2) | VfW | encodes and decodes, 38 dB | runs DLLs it carries from heap memory with its own PE loader: data pages are made executable on the fault, as Windows without DEP and Wine do |
| `wtvc.dll` (WTVC) | VfW | encodes and decodes, 47 dB | answers `ICM_COMPRESS_GET_SIZE` only after `ICM_COMPRESS_BEGIN`: asked again then |
| `WavCWAIP.dll` (wavc) | VfW | encodes and decodes with `VFW_DATE` before 2008 | an evaluation build that stops in 2008; writes `\Windows\WavCWAIP.ini`, now on `C:`; its I420 path crashes in `ICM_COMPRESS_END`, which is now survived |
| LEAD MCMW 2.0 (`LCODCCMW2E.dll` and `LCMW2.dll`, LCW2) | DirectShow | encodes and decodes, 37.5-45 dB outside a 16-row band | `SHDeleteKeyA`; one filter in two categories; an output type of its own; samples full until told otherwise; `IMediaSample2` properties in part |

The LEAD encoder is an evaluation build ("LEAD MCMW 2.0 Eval Encoder") that
draws that banner into the bottom 16 rows of every frame with GDI text, as
it appears on Windows (the round trip's 37.5-45 dB leaves those rows out). It sets no sync points of its own and keeps
its inputs', so every frame is a keyframe, as on Windows (LCW2 frames stand
alone); it has no `IAMVideoCompression`, so `--keyint` is refused.

### The whole `ffmpeg-unimplemented-codecs-2026-10-03` archive

All 58 files in that archive were later tested together, over every
encode and decode path. The results are in
[docs/CODEC_TEST_REPORT.md](docs/CODEC_TEST_REPORT.md), with the harness
changes each failing codec needed (H1-H12, all since made: a status
callback, input padding, `ICM_DECOMPRESS_GET_FORMAT` before every
decompression, a lenient compress query, `probe` in compress mode, FOURCC
discovery, encoders that register no subtype, decoders' own output types,
PAL8 input, guarded output buffers, surviving a crash in `*_END`, and
drivers' module handles), the faults that are the codecs' own, and the
companion DLLs that are missing.

### A batch of about 150 DLLs, late October 2026

`seance encode` was run over about 150 DLLs with 640x360 input. Most of
the failures it showed are fixed. Below are the codecs that now work, and
what keeps the others from working.

**Now working:**

| Codec | Result | What it needed |
|---|---|---|
| `SN4Codec.dll`, `Sif1_vfw.dll`, `vsslight.dll`, `frwd.dll`, `frwt.dll` | encode with `--pad 16` | they take only heights that are a multiple of 16; `encode` names the nearest size a codec takes, and `--pad N` / `--crop N` reach it |
| `ADV601.dll` | encodes at 640x480 only | `encode` names that size |
| `icmw_32.dll`, `WavCWAIP.dll` | frame 0 is now a keyframe | they flag no keyframes; frame 0 is always written as one |
| `avimszh.dll` | round-trips losslessly, clean under ASan; FFmpeg decodes its BGR24 stream to the source too | the slack Windows' heap leaves after a block (its 32-byte header), now kept after guarded output buffers; and top-down RGB input turned over to bottom-up, since it copies a top-down input's negative height into its MSZH format |
| `msvidc32.dll` (Microsoft Video 1), `mcmjpg32.dll` and other NT 4 codecs | load | built-in `crtdll.dll` |
| `cinevfw3.dll` | loads | built-in `psapi.dll` |
| Morgan `M3JP2K322.dll` | round-trips with `VFW_DATE=2001-06-01` | an expired trial build |
| On2 `vp7vfw.dll` (VP70) | round-trips 640x360, 37.6 dB | built-in `oledlg.dll` and `dbghelp.dll` (it is built with MFC); it reports no FOURCC until opened as `VP70`, which is now found in the DLL |
| Softimage `siaviref.dll` (SIRF) | loads; decodes only | built-in `netapi32.dll` (`NetShareEnum`) and `_wsplitpath`; a reference codec, whose frames point into other media |
| On2 `vp31vfw.dll` (VP31) | round-trips with `--pad 16`, 45.7 dB | it takes 640x360 when asked but begins only at a multiple of 16; `encode` now says so and names `--pad 16` / `--crop 16` |
| On2 `vp4vfw.dll` (VP40) | round-trips with `--pad 16`, 47.2 dB | as `vp31vfw.dll`; and it writes I420 upside down (and a row out), so `decode` now checks the first picture against BGR24 and decodes to BGR24 instead |
| innoheim `iscc.dll` (ISCC) | round-trips, 26.9 dB (an evaluation build: it draws a watermark, and decodes to RGB555 only) | it takes I420 and YV12 when asked but won't begin with them, and takes YUY2 as 16-bit RGB: `encode` now tries another input, RGB first, when a codec won't begin |
| ASUS `asusasvd.dll` (ASV1), `asusasv2.dll` (ASV2) | decode only; FFmpeg's ASV1 and ASV2 files decode at 46.4 and 51.6 dB | their RGB is a poor conversion (28-29 dB): `decode` now prefers packed YUV (YUY2, UYVY) to a codec's RGB for Y4M |

**Partly working, because of faults in the codecs:**

- `m3jp2k32.dll` encodes but fails to decode frame 1.
- `m3jpeg32.dll`'s own decoder gets chroma and RGB wrong. FFmpeg decodes
  its files correctly.
- Both Morgan codecs are trial builds whose trial has run out ("The trial
  period has expired !"): at today's date they put up that dialog and
  refuse `ICM_COMPRESS_BEGIN` and `ICM_DECOMPRESS_BEGIN` with
  `ICERR_BADFORMAT`, even for FFmpeg's MJPEG. The two faults above were
  seen before then and can't be looked into again without getting around
  the trial, which seance doesn't do.

**Can't work in seance:**

- **Win9x thunk codec:** `Mvicod32.dll` (Motion Pixels) calls a 16-bit DLL,
  `MVICOD16.DLL`, through `ThunkConnect32`. Such DLLs are now refused, with
  that reason, before their `DllMain` runs.
- **16-bit DLLs:** 16-bit NE DLLs (`vp5r*`, `ativcr1`, `mvicod16`, ...).
- **64-bit DLLs in the i386 `seance`:** they are handed to `seance64`
  (`VFW_ARCH=x86_64`), which dispatches `__try` SEH and C++ exceptions of
  `__CxxFrameHandler3` and `__CxxFrameHandler4` (VS 2019 and later; only
  tested with hand-encoded tables, which cover a try block inside a catch
  block, under either, and `_set_se_translator`) code; g++'s exceptions work; see [X64_PLAN.md](X64_PLAN.md). The one
  64-bit codec tried so far, CineForm's `cinevf2w.dll` (v5.8.1, 2009),
  needed `rpcrt4.dll` (`UuidCreate`) and heap addresses below 4 GB (its
  `DriverProc` keeps the instance it returns from `DRV_OPEN` in 32 bits,
  as Win64 heaps of that time allowed): both are now provided, and it
  probes fully from either binary. Its encoder is licence-locked ("This
  installation is not licensed for 64-bit CineForm HD encoding"; not worked
  around), so the only CFHD clips to hand came from FFmpeg's `cfhd` encoder.
  The decoder handles those at 320x240 and 640x360 (YUY2 output; PSNR
  against FFmpeg's own decode ~35 dB for yuv422p10, ~25 dB for gbrp12 since
  that is RGB-to-YUV). Of 28 sizes from 64x64 to 640x360, it decodes 13
  (272x208, 288x208, 288x216, 304x208, 304x224, 224x288, 160x400,
  400x160, 304x224, 320x200, 320x240, 640x360 ...) and fails the rest,
  by crashing in its own Huffman decode loop (`+0x37dbb`: reading past the
  end of the frame, or with 4 MB of padding after it writing through a
  wild index) or by returning `ICERR_ERROR`. It isn't a size threshold
  (272x208 works, 240x240 and 256x192 don't, 304x216 errors) and the loop is
  plain integer code run natively, so it looks like a property of
  FFmpeg's (experimental) encoder output, which only its own decoder is known to take; `VFW_LOWHEAP=0`, `VFW_ZERO_MEMORY=0`
  and `VFW_FORCE_RELOC=1` change nothing. No genuine CineForm stream is
  available to confirm.
- **RealPlayer codecs:** `*3260.dll`. These are now run through their
  own entry points; see "RealPlayer codecs" below.
- **Locked, or needs setup:**
  - `wmvdmoe2.dll` is a keyed DMO and needs an unlock key.
  - `tsccvid.dll` and `tsc2_codec32.dll` (Camtasia) need a licence.
  - `vp5vfw.dll` needs a WIBU dongle.
  - `raylightCodec.dll` needs an output folder set up.
- **Locked to its vendor's software:** `fjht264.dll` (HTC H.264) begins
  compressing only after a private message (`ICM_USER+0x6B`) carrying a
  key that its own checks; it isn't worked around.
- **Decoders only, under every FOURCC:** Canopus `CLLCcodc.dll` (CLLC) and
  `cdvccodc.dll` (CDVC), and `xanlib.dll`, which opens only as `XXAN` or
  `Xxan`; `encode` now says so (and, for `xanlib.dll`, as what). There are
  no files here to decode with them.
- **Missing companion DLLs:** the user supplies these through `VFW_PATH`.
  They are `MFC42.DLL`, SImageLib, `pthreadVC2.dll`, VTACCESS, DecFrame,
  ENC263, `vssh264enc.dll`, `vsswltconf.dll` and DSOUND, and Canopus's
  `cseuvec.dll` and `csehqa.dll` for `CUVCcodc.dll`. A driver that refuses
  to load or open, or a DLL whose `DllMain` fails, after looking for a DLL
  that isn't there, now names it.

**Re-run on the current build (November 2026):**

| Codec | Result | What it needed |
|---|---|---|
| `avimszh.dll` (MSZH) | round-trips, 75 dB from RGB (lossless but for rounding) | the slack after guarded buffers; FFmpeg reads its BGR24 files (48.9 dB from YUV), but not its BGR32 ones, which its LCL decoder has no mode for |
| `AvidAVICodec.dll` (AVRn) | round-trips at 36.7-41.6 dB (FFmpeg's AVRn decoder: 37.2) once set up | Avid's software M-JPEG codec, not DNxHD. It picks a resolution from `HKCU\Software\Avid Technology\Software M-JPEG Codec\Resolution`, which needs an Avid resolution code (0x4B-0x52, 0x97 for 720x576 or 720x486; 0x4E-0x50 for 352x288); its own default, 1, is none, so `ICM_COMPRESS` fails in its `__except` with `ICERR_INTERNAL`. Seed the value in the registry file, and give it the size with `--scale` |
| `wnvwinx.dll` (WINX) | round-trips, 31.6 dB (was 13.6) | it reads YUY2 input bottom-up, as if it were RGB: `encode` now checks the first frame and turns the input over |
| `SN4Codec.dll` (SN40) | encodes and decodes, but at 6 dB at every size | it says a 320x240 frame takes 230400 bytes and writes 300 KB ones: the output buffer now has room after it, as the Windows heap would, and a warning. Its frames are the same size whatever the settings, and decode to noise: a fault of its own |
| `rricm.dll` (Matrox Rainbow Runner) | refuses `DRV_LOAD` | the board's driver: it looks for `HKLM\Software\Matrox\RRPath` and `...\RainbowRunner\ICM`, which refusals now name |
| `cvpcdvc.dll` | refuses `DRV_LOAD` | `PAVAPI.DLL`, which the message names (supply it through `VFW_PATH`) |
| `BMDCodecLib.dll` | doesn't load | `libguide40.dll`, Intel's OpenMP runtime (supply it through `VFW_PATH`) |
| `packed.dll` | isn't a codec | it is `alcoholx.dll` from Alcohol 120%, packed with UPX; its `DllMain` fails without Alcohol's virtual drive. The crash on unloading it was its unpacker running on after `GetOverlappedResult`, now built in, wasn't found |

**Found by `seance sweep`** (62 DLLs at 320x240, in 7 seconds), besides
the above:

- `lhacm.acm`, `lhacm2.acm` (L&H CELP and SBC) and `alf2cd.acm` (NCT
  ALF2 CD, 44.1 kHz stereo) round-trip; they need `--format-tag` and the
  PCM layout the sweep finds.
- `aslcodec_dshow.dll` encodes, but has no decoder for its output
  (`aslcodec_vfw.dll` decodes it).
- `nsrt2432.acm` (Voxware RT24) takes none of the PCM layouts offered.

### The codec zip, November 2026: per-codec investigations

The sweep over the zip (504 codecs, catalogued separately) left codecs
whose failures needed a closer look. Each was traced (`VFW_TRACE=1`, gdb)
to a cause; what was seance's is fixed, and the rest is recorded here.

- **LEAD DirectShow filters** (`LEncAAC`, `LEncAC3`, `LEncAMR`, `LEncVP8`,
  `LEncmpga2`, `LENCSCR2`, `LMVMtnDet2` and their `x` variants):
  - They look up a class of `DSKernel2.dll` while being created, with
    `CoGetClassObject`. seance ran registration under `CoInitialize` but
    created and ran filters and DMOs with COM down, so this failed with
    `CO_E_NOTINITIALIZED` (0x800401f0). A host application has COM
    initialised, so now seance does too, for as long as a filter or DMO
    is open.
  - The class is `DSKernel2.dll`'s (`{E2B7DDA9-38C5-11D5-91F6-00104BDB8FF9}`),
    which LEAD's installer registers. Without it the open fails with
    `REGDB_E_CLASSNOTREG`; a class that isn't registered is now named in
    the trace. `seance register DSKernel2.dll` registers it; its
    `DllRegisterServer` then fails at the type library
    (`TYPE_E_CANTLOADLIBRARY`), after writing the classes.
  - With that done they reach LEAD's licence check ("The evaluation
    period has expired"), and fail with 0x80050010. That isn't worked
    around.
- **Ut Video** (`utv_vcm.dll`): round-trips now (ULRA, lossless).
  - Its `DRV_OPEN` takes any FOURCC and makes its codec only for one of
    its own, which are kept in `utv_core.dll`, the DLL it calls. seance
    opened it as 'July', a month name of the C runtime's locale table, and
    it then crashed on `ICM_DECOMPRESS_QUERY` with no codec behind it.
  - The search of a DLL for its FOURCCs now leaves out the C runtime's
    month names and debug signatures ('RSDS', 'NB10'), and goes on to the
    DLLs it imports that sit next to it.
  - A driver that says nothing of itself without a FOURCC is now taken
    under one only if it then gives a name or description as well.
- **3ivx** (`3ivxVfWCodec.dll` and its `DivXVfWCodec.dll` and
  `SamsungVfWCodec.dll` brands, the `3ivxDS*` filters): `difftime`, which
  they import, is now built in.
  - The VfW codecs and the video encoder filter then fail in `3ivx.dll`,
    their core: it is a trial build ("3ivx Trial", "Demo Expired") that
    reads `trial.pbm` from the system directory. That file comes with its
    installer, not the zip, and without it the core makes no encoder (the
    VfW codecs crash on its NULL in `ICM_COMPRESS_GET_FORMAT`).
  - The audio encoder filter, also a trial build ("This period has now
    expired"), fails to be created with `E_NOINTERFACE`. Why wasn't
    looked into past its trial code.
  - None of this is worked around.
- **BergWave** (`vfwbw.dll`, BW00): faults of its own, so it doesn't work.
  - It reads its quantisers from `HKCU\Software\GNU\BergWave` (`i_quant`,
    `p_quant`, ...), which its configure dialog writes. With none, they
    are 0 and `ICM_COMPRESS` divides by zero. Seed them in the registry
    file (16 works) to get further.
  - Its allocator rounds a pointer up with `& ~0x11` where `& ~0xf` was
    meant, so for half of the blocks it is given its buffer starts 2
    bytes before the block. It then frees one byte before its pointer,
    not the block. On Windows those frees fail, which msvcrt's `free`
    ignores, and the headers it overwrote are never looked at again.
    libc's heap checks its neighbours' headers, and the run aborts
    ("corrupted size vs. prev_size"). Given its own mapping per block, it
    encodes 320x240 at 830 kbit/s.
  - seance now ignores a `free` of a pointer that can't start a block (not
    8-aligned), with a warning, as Windows does, instead of aborting.
- `__dllonexit` now adds to the DLL's own exit table, as MinGW's DLL
  startup expects (it runs the table when it detaches). It used to add
  to the process's, which ran the functions at exit, after the DLL was gone.
- **MPEG4V** (`MPEG4V.DLL`, 3RVC): locked to a DVR product, so it doesn't
  work here. Its first `ICM_COMPRESS_QUERY` reads `Codec` under `[PRODUCT]`
  from `C:\Dvr\Ini\Config.ini` and only works when the value is 333.
  Without the file it goes on to use a NULL `FILE` and faults. This is
  the vendor's lock, and it isn't worked around.
- **dgcolorxvfw** (`dgcolorxvfw.dll`): not a codec. It is a colour-conversion
  library that declares no FOURCC, and its driver entry opens and refuses
  every format. Under the sweep it used to crash, until seance's
  `TerminateThread`, which used to do nothing, was implemented.
  `TerminateThread` now ends the thread:
  - at once in guest code;
  - when it wakes, if it is waiting or sleeping in seance;
  - before it starts, if it hasn't run.
- **CapAvi32** (`capavi32.dll`, CAPA): round-trips now (lossless).
  - It is a capture codec. Its frames go to `C:\capavi.000`, and the AVI
    holds only short references into that file, so the AVI is no use
    without it.
  - It reads that file with overlapped `ReadFile` on a handle it has
    associated with a completion port, and then waits on the port. seance's
    file I/O never stays pending, and associating a file with a port did
    nothing, so no packet came and the decode hung.
  - Now a handle associated with a port records it, and overlapped
    `ReadFile` and `WriteFile` on a handle opened with
    `FILE_FLAG_OVERLAPPED` queue a packet as they return. As on Windows,
    no packet is queued when the event handle's low bit is set. The
    OVERLAPPED's event is set, and `Internal` holds the status.
- **VTZ32** (`VTZ32.DLL`, VDTZ): round-trips now (49 dB).
  - On `DRV_LOAD` it starts a thread that makes a window and runs its
    message loop. On `DRV_FREE` it sends that window `WM_CLOSE` with
    `SendMessage` and waits for the thread to end.
  - seance ran a message sent to another thread's window on the sender's
    thread. So `WM_DESTROY`'s `PostQuitMessage` quit the wrong queue, the
    window's thread never ended, and `DRV_FREE` hung.
  - Now a message sent to a window of another, live thread runs on that
    thread when it next looks at its queue: in `GetMessage`,
    `PeekMessage`, `WaitMessage`, `MsgWaitForMultipleObjects`, or while
    it sends a message itself. The sender waits and runs what is sent to
    it meanwhile.
  - `SendMessageTimeout` gives up after its timeout (`ERROR_TIMEOUT`), and
    `SendNotifyMessage` doesn't wait. `InSendMessage` and `ReplyMessage`
    work.
  - A window whose thread has ended still gets the message on the caller's
    thread, as before.
- **DiAVC** (`diavc.ax`): protected by a code virtualizer, and doesn't work
  here. As it is created, it runs code that raises faults on purpose for an
  `__except` filter, itself virtualized, to act on. Three of the things it
  relies on were missing in seance, and are now there:
  - `PEB->Ldr` was NULL. It now holds Windows' loader lists (load,
    memory and initialisation order), seeded as Windows XP has them (the
    main image, ntdll, kernel32) and kept with the modules loaded.
  - A general-protection fault, such as `int 1` (CD 01) from user mode, is
    reported as Windows reports it: an access violation reading -1. A
    privileged instruction (`hlt`, `cli`, ...) is reported as
    `STATUS_PRIVILEGED_INSTRUCTION`.
  - `icebp` (F1) is reported as a single step, past the instruction, not
    as a breakpoint.
  - With these, its filter still sends it into a loop of faults (writing
    to its own code at +0x801a8). Why wasn't worked out: that would mean
    reversing the virtualized filter. `VFW_TRACE` now names each handler
    that continues execution, and where.
- **racodec / racode64** (`racodec.ax`, `racode64.ax`, RASC; the
  RemotelyAnywhere screen codec): registers and opens now, but nothing
  round-trips.
  - Its `DllRegisterServer` registers the encoder under
    VideoCompressorCategory, then calls `BindToStorage(IID_IPropertyBag)` on
    the moniker that `IFilterMapper2::RegisterFilter` returns, to write
    `FccHandler`. seance returned no moniker, so the call went through
    NULL.
  - `RegisterFilter` now returns a device moniker for the registration. Its
    property bag reads and writes the instance key's values (REG_SZ as
    `VT_BSTR`, REG_DWORD as `VT_I4`, REG_BINARY as `VT_ARRAY | VT_UI1`), as
    Windows' `CDeviceMoniker` does.
  - The encoder's samples are its input frames, uncompressed, whatever the
    input format (I420 or RGB32).
  - The decoder offers only RGB32 output, and makes the picture square on
    purpose: max(width, height) both ways, for sizes up to 640. So the
    sweep sees "a different picture". Let through, it decodes those
    streams as black frames. The pair probably expects a RemotelyAnywhere
    session to drive it; this wasn't pursued.
  - Cropping a decoder's larger picture to the input's size is Phase 5's
    padded-output item.
- **Canopus DV** (`csccdvc.dll` 2.03 and `cscdvsd.dll` 2.07, DV
  encoder and decoder): these don't match the zip's `csedv.dll` (3.11), so
  they don't work here. `csccdvcx.dll` (3.02) is not an encoder; it doesn't
  take dvsd either.
  - Both get their DV engine's entry points from `csedv`'s only export,
    `_SoftEngine@4`. It returns each address minus a constant, and the
    caller adds the constants it was built with.
  - csccdvc's constants still match. But it passes the engine a function
    pointer (an entry of its own per-format table) where csedv 3.11 takes
    a pointer to the table. The engine calls through the function's first
    four bytes and faults (`SIGSEGV at 0x53ec8b55`, in `ICM_COMPRESS`).
  - cscdvsd's constants don't match, so the address it calls is wild
    (`0xd9d00d04`, in `ICM_DECOMPRESS`).
  - They need the `csedv.dll` they were released with, which isn't in the
    zip.
  - A crash at an address in no module now also names up to three stack
    words that point into modules (`[sp+0x1c] csccdvc.dll+0x15bf4`). These
    usually include the caller.
- **BHA XVD video encoder** (`BHA_XVDVEnc.dll`, a DMO with VGMV output):
  locked by its vendor, so it doesn't work here.
  - Its normal path opens its encoder engine through a stub that always
    fails, so `GetOutputType` offers nothing (`DMO_E_NO_MORE_ITEMS`).
  - Given an output type anyway, it takes it. Its first `ProcessInput`
    then reads through its unsized frame queue's NULL front (`accessing
    0x14`).
  - A working encoder path sits behind a registry switch whose name the
    DLL keeps encrypted. That is the vendor's gate, and seance doesn't set
    it.
- **Matrox software codecs** (`swcdvvfw.dll`, `swcjpegvfw.dll`,
  `swcmpegvfw.dll`) and **DigiVCap** (`digivcap.dll`): DigiVCap round-trips
  now, as dvsd, dv25, dv50 and mjpg (DV at 720x480: 33.8 dB, and 36.0 dB
  decoding ffmpeg's DV).
  - The swc* DLLs aren't DirectShow filters, hence "no filters". Each
    registers a plain COM class, its engine for DigiVCap (the VfW driver),
    with `InprocServer32` a bare `swcdvvfw.dll`.
  - seance looked for a bare server name only on `VFW_PATH` and in the
    current directory, so DigiVCap's `DRV_OPEN` failed. `CoGetClassObject`
    and `CoCreateInstance` now look next to the calling DLL first, as
    `LoadLibrary` does. (On Windows these DLLs are in system32.)
  - `seance register` the three swc* DLLs first.
  - DigiVCap's other FOURCCs (MPEG-2, HD, dvsl) want
    `HKLM\Software\Movie2`, which Matrox's software installs.
- **OggDS** (`oggds.dll`: Ogg splitter and muxer, Vorbis encoder and
  decoder): its Vorbis encoder now connects, but seance can't store what it
  makes.
  - It registers its output as a subtype that is no format tag's. seance
    asked such an encoder for the subtype of format tag 0, which it never
    offers. An encoder registered with no tag now gets its first audio
    output.
  - That output's format is OggDS's own (`{6BDDFA7E-9F22-46A9-AB5E-884EFF294D9F}`),
    meant for its Ogg muxer, not a WAVEFORMATEX, so it can't go into an AVI
    or WAV. seance now says so, where it used to say the encoder took no PCM.
  - Its decoder takes the same type, which only its Ogg splitter gives.
    seance reads no Ogg.
  - Fixed on the way: the PCM retry compared formats past the end of a
    16-byte `fmt ` chunk.
- **Canopus DirectShow codecs** (`csDShowcodc.dll`: DV, DVCPRO50/HD, HQ,
  Lossless and MPEG2-Intra decoders, HQ encoder): they need Canopus's
  engine, `PAVEDIUS.DLL`, which isn't in the zip. Each filter loads it when
  created and fails (`E_FAIL`) without it.
  - Opening a DirectShow filter or DMO now names the DLLs and registry keys
    it looked for and didn't find, as opening a VfW driver does: "it looked
    for PAVEDIUS.DLL, which isn't here (add its directory to VFW_PATH)".
- **Intel H.261 and H.263** (`h261_32.ax`, `h263_32.ax`, NetMeeting-era
  VfW codecs): they encode in real time only.
  - Their rate control times frames with `timeGetTime` and refuses a delta
    frame that arrives sooner than the frame interval (`ICERR_INTERNAL`).
    seance compresses as fast as it can, so the first delta frame fails.
  - Fed one frame per 110 ms with `--rate 10`, H.261 encodes at CIF (4
    frames, 1 key). H.263 then fails inside the codec, through a NULL local.
  - `--keyint 1` (every frame a keyframe) encodes with H.263.
  - seance doesn't pace an encode to real time.
- **Broadway** (`bw10.dll`, BW10): the encoder drives Data Translation's
  Broadway capture card through `VTACCESS.dll` (`_VT_GetHardwareBuffer`).
  It needs that hardware.
- **TechSmith screen codecs** (`tsccvid.dll`, TSCC; `tsc2_codec32.dll`,
  TSC2): their compressors are locked to TechSmith's products ("TSCC allows
  compression only if you have registered ... Camtasia Studio", "TSC2 only
  allows compression from TechSmith products"). seance doesn't get round
  that.
- **Duck TrueMotion 2X** (`tm2x.dll`, TM2X): it needs its installer.
  - `ICM_COMPRESS_BEGIN` loads a compression profile, a `.cfg` file in the
    directory that `HKLM\Software\Duck\VFW Encoder/Decoder\TM2X`
    `DuckDir` names. There is no key and no `.cfg` in the zip, so the
    encoder engine is never made.
  - `ICM_COMPRESS` then faults on it, catches the fault and returns
    `0x8000ffff`. Its installer also has serial-number and trial modes.
  - A codec can open without the registry keys or DLLs it looks for and fail
    only later. An `ICM_COMPRESS_BEGIN` or `ICM_COMPRESS` failure now names
    them too: "it looked for the registry key
    HKLM\Software\Duck\VFW Encoder/Decoder\TM2X, which isn't set".
- **March Networks H.263** (`TxsH263.dll`, T263): it round-trips now, 27.9
  dB at CIF.
  - Its `ICM_COMPRESS_GET_FORMAT` refuses an output buffer whose `biSize`
    is under 40. seance passed a zeroed one and now sets `biSize` first, as
    a caller that copies its input format in does.
  - It decompresses only to 8-bit paletted output while
    `HKCU\Software\MarchNetworks\H263` `"8bit Decoding"` is 1, which it
    writes by default. Set it to 0 in the registry file for 16 or 32 bits
    (`--output-format BGR32`); with MMX it doesn't give 24.
  - It also takes its vendor's own MYUV and TXSY formats.
- **UPX-packed DLLs** (`DivX520.dll`, `scmpack.dll`): they load now, and
  DivX 5.2 round-trips (39.3 dB).
  - UPX's stub binds the packed DLL's import table itself, through
    `GetProcAddress`, and fails `DllMain` at the first NULL (gdi32's
    `StartDocA`, kernel32's `GlobalAddAtomW`). The loader binds an
    unimplemented import to a trap that reports it if it is called; these
    are the same imports bound late.
  - `GetProcAddress` called from such a stub, before its DLL's `DllMain`
    has returned, now gives a trap for an unimplemented built-in function,
    as the loader does. The stub is code in a writable and executable
    section that holds the entry point. A probe from anywhere else still
    gets NULL.
  - Added for what scmpack calls from its `DllMain`: `GlobalAddAtomW` and
    the other W atom functions, `EnumResourceLanguagesW`,
    `GetVolumeInformationW`, `PathAppendW` and `PathStripToRootA/W`.
  - scmpack (SMV Media Pack) is a set of DirectShow decoders for SMV, its
    vendor's own container. The zip has no SMV files to try them on.
- **CoreAVC** (`CoreAVCDecoder.ax`): wrapped in Licenturion's licence
  protector, which checks for a kernel debugger (`SICE`, `NTICE`) and its
  licence key under `Software\Licenturion GmbH`. Without the key `DllMain`
  fails. seance doesn't get round that.
- **Insignis MultiCodec** (`MultiCodec.dll`, INMC): it opens, but
  `ICM_COMPRESS` gives `ICERR_UNSUPPORTED`. It needs LEADTOOLS 9
  (`LTKRN90N.dll`, `LTDIS90N.dll`, `LTFIL90N.DLL`), `d3dx9_30.dll` and its
  own software's `HKLM\Software\Insignis Technologies\REMOTEDG` keys,
  none of which are in the zip. The error now names them.
- **DivX Pro 5.1.1** (`DivX511.dll`, the adware-supported edition): its
  first `ICM_COMPRESS` looks for the Gator (GAIN) adware's COM class and
  `HKLM\SOFTWARE\Gator.com\CMEII`. Without them it opens a dialog and
  refuses (`ICERR_ERROR`). That is the vendor's licence condition; seance
  doesn't get round it. It decodes.

### The codec zip, November 2026: what the sweep reports

The sweep's own heuristics, rechecked against the 75 DLLs whose result
was "needs an option", "decodes at another size", "no output format",
"no frames to compare" or a round trip below 10 dB:

- **Several encoders in one DLL** (`utv_dmo.dll`, `LEncH2644.dll` and its
  `x` variant, `3ivxDSVideoEncoder.ax`): each encoder the probe lists is
  tried with `--clsid` until one encodes, and the TSV's "fix" names it.
  `decode` on a DLL of several decoders now takes the one that registers
  the stream's FOURCC. That was the only cause of its "cannot open a
  unique decoder" error.
  - `utv_dmo.dll`'s encoders run. Its decoders refuse what they make
    (`DMO_E_INVALIDTYPE`): the DMO encoder offers its output type
    without the codec data its decoder wants, before streaming and after.
  - None of `3ivxDSVideoEncoder.ax`'s seven encoders takes any standard
    input, at any size.
- **"Try --fourcc"** (`LCodcScr2`, `Mvicod32`, `digivcap`, `ff_vfw`):
  `encode`, `decode` and `probe` already try every FOURCC found in the
  DLL. These refuse `DRV_OPEN` under all of them:
  - `digivcap` and `Mvicod32` want their vendors' drivers ("Device Driver
    mviddnt is not started") or registry keys;
  - `LCodcScr2` is LEAD's;
  - `ff_vfw` wants ffdshow installed.
- **Other sizes:** a DirectShow or DMO encoder that takes no standard
  input at 320x240 is tried at 352x288, 640x480, 720x480, 720x576,
  1280x720 and 1920x1080 (`--scale`). `encode` already asks a VfW codec
  those sizes and names one it takes. None of the zip's took one.
- **Decoders that pad** (`TxsH263.dll`, `LEncMpg23.dll`, 352x288 or 352x240
  for 320x240) are measured on the top-left of their output.
  - `qdv.dll` decodes DV at 720x480 whatever it was given, so its number
    (15.6 dB) compares a corner of a rescaled picture.
- **Round trips below 10 dB** are tried again decoding to BGR24, and
  are "encodes" if that doesn't help. `decode auto` below 10 dB counts as
  no decoder here.
  - `mhuffyuv.dll` freed a buffer twice in `ICM_DECOMPRESS_END`, and
    libc aborted. Windows' heap fails a second free (msvcrt ignores
    that), and now seance's does too, for the 64 blocks freed last. It
    decodes nothing as YUY2 (ICERR_OK, all zeros), and to BGR24 at
    22 dB.
  - The decoders of `SN4Codec.dll` (12 dB), `davcvfw.dll` (15 dB) and
    `mmtvmj.dll` (12 dB) give noise, black or zeros in their own output
    format, and do better as BGR24. `mmtvmj` is Morgan's M-JPEG, whose
    trial puts its logo in the picture and decodes half the lines.
  - `H264Vfw.dll` writes 230,400 zero bytes per frame, a raw frame's size
    with nothing in it. It is "encodes".
- **No output format** (`AJPG.DLL`, `KHCS.DLL`): they answer
  `ICM_COMPRESS_GET_FORMAT` with nothing, and decompress their own FOURCC,
  so they are decoders.

### The codec zip, November 2026: decoders given samples

`sweep --samples` gave the 160 decode-only DLLs the 64 streams
`tests/gen_samples.sh` makes with ffmpeg, matched by the FOURCC or tag
each registers (and, for VfW, FOURCCs its DLL holds). 19 decode a
sample to within 25 dB of ffmpeg's own decode (most 45-70 dB), and are
now "decodes":

- video: `AxMJPGDec`, `GXJPG` (MJPG); `AxMP4Dec` (DX50); `khvc.ax`
  (XVID); `divxdec.ax`, `windivx.ax` (DIV3); `mp43decd`, `mpg4ds32.ax`
  (MP43); `mpg4decd` (MP42); `wmvdmod`, `wmv10dmod` (WMV1); `ilvc20.ax`
  (H263); `asusasv1`, `asusasvd`, `asusasv2` (ASV1, ASV2); `CFDecode2.ax`
  (CFHD); `msyuv.dll` (UYVY, exactly); `CtWbJpg` (YUY2);
- audio: `wmadmod.dll` (WMA 1, 104 dB once 2048 samples of start-up
  delay are allowed for).

What the samples found in seance, fixed:

- **`CLSID_MemoryAllocator`.** A filter built on Microsoft's base classes
  makes its input pin's allocator with `CoCreateInstance` of quartz's
  memory allocator, which seance's quartz didn't have. Its input pin
  then had none (QNAP's `G711.ax` and `G726.ax`). Now seance's own
  allocator is that class.
- **Input buffers the size of a packet.** Audio input samples were a
  megabyte (or two seconds), whatever the packet. `G711.ax` works out how
  much it will decode from the input buffer's `GetSize`, not the data in
  it, and overran its 8 KB output buffer. A parser's buffers are packet-
  sized, so seance's are now too: a WAVEFORMATEX format's block, or 4 KB
  for formats with small blocks, grown when a larger packet comes. The
  zeroed padding after the data is now kept past `GetSize` in seance's
  allocator.
- **I420 is IYUV.** Asked for I420, a DirectShow decoder that offers the
  same layout only as IYUV (`xvid.ax`, `khvc.ax`) was handed a bare I420
  type it accepted and then wrote nothing into (all-zero frames, 5.2 dB).
  The host now takes the offered IYUV type.
- **`msvcrt!isleadbyte`** (`wmvsdecd.dll`, the WMV screen decoder): no
  lead bytes, as `_ismbblead` says.

What they found that isn't seance's:

- `xvid.ax` decodes XVID with its luma shifted throughout (21.9 dB: luma
  20 dB, chroma 65 dB). The picture is right; its brightness isn't
  ffmpeg's.
- `mp4sdecd.dll` (MP4S, M4S2) and `m3jpegdec.ax` (MJPG) report success
  and give all-zero frames for ffmpeg's streams.
- LEAD's decoders (`LDecMpg42`, `LDecH2643`, ...) decode at about 20 dB,
  the level of their trial's overlay.
- `3ivxDSDecoder.ax` writes through an object it never made (3ivx's
  trial), `G726.ax` through codec state that `Receive` never set up,
  `dsrmp4.dll` and `mlcom.ax` divide by zero.
- `l3codecx.ax` (MP3) and `iviaudio.ax` (MP2) take every packet and give
  no PCM. `G711.ax` gives half the samples. `ac3filter.ax` decodes AC-3
  at 6 dB SNR against ffmpeg, with its own gain and mixing. Same before
  these changes.
- Many VfW decoders that register no FOURCC of ffmpeg's were given YUY2,
  UYVY or MSVC because their DLLs hold those bytes, and refused them.
  In all, 63 were given samples that didn't check out. The other 77 had
  no sample at all: their formats (Indeo, VP6, Fraps,
  AMR, Vorbis, Theora, DV variants, vendor FOURCCs) are ones ffmpeg can't
  encode, or the DLL names none.
- `kdvyuv8.dll` is the 160th: it now "fails", as it doesn't answer
  `ICM_GETINFO`, so it never got as far as a sample. It does the same on
  main.

The 137 DLLs that round-tripped or encoded, swept again with these
changes against the catalogue's results:

- `divxg400.ax` had stopped at `msvcrt!_fsopen` since seance's msvcrt
  became a KnownDLL. `_fsopen` and `_wfsopen` are there now, and it
  round-trips as it did.
- `direccional.dll` crashes in `ICM_COMPRESS`, where it was lossless. It
  takes I420 but reads it as RGB24, 230,400 bytes from a 115,200-byte
  frame. Its "lossless" round trip was the first half of each frame
  copied through, and the rest read from whatever memory followed.
- The rest that changed are Phase 5's: round trips below 10 dB are now
  "encodes", and `mhuffyuv`, `SN4Codec`, `davcvfw`, `mmtvmj` and `qdv`
  measure better.

### The codec zip, November 2026: RealPlayer codecs

seance now hosts RealPlayer's codecs (`src/real/`) and reads RealMedia
files (`src/in/rm.c`). The zip's 34 RealPlayer DLLs, swept with the
RealMedia samples `tests/gen_samples.sh` makes (RV10, RV20, AC-3 as
`dnet`, 14.4 as `lpcJ`: all ffmpeg can write to RealMedia):

- **Verified:**
  - `drv2.dll` and `drv23260.dll` decode RV20 at 53 dB PSNR against
    ffmpeg's decode.
  - `14_43260.dll` decodes 14.4 identically to ffmpeg.
- **`dnet3260.dll` decodes AC-3 correctly, but at a different level.**
  Its output is 1.5 times ffmpeg's (with or without ffmpeg's dynamic
  range compression). Scaled by 2/3 it matches to 84 dB SNR, but the
  sweep compares without scaling, so it reports 6 dB. `ac3filter.ax`
  shows the same.
- **No sample to give them:**
  - video: `drv33260.dll` (RV30), `drv43260.dll` (RV40) and `drvc.dll`
    (RV30 and RV40);
  - audio: `28_83260.dll`, `cook.dll`, `cook3260.dll`, `atrc.dll`,
    `atrc3260.dll`, `sipr.dll`, `sipr3260.dll` and `raac.dll`.

  ffmpeg encodes none of these. They open and are listed as decoders.
  The interleaving their streams use (`genr`, `Int4`, `sipr`) is undone
  as RealNetworks' format describes. The unit tests check that with
  made-up streams, but no real file has been tried.
- **RV10:** RealPlayer decodes it with `rv10.dll`, through the older
  `PNCodec_` interface. The RV20 entry points refuse it (`0x80040005`).
- **Not hosted:**
  - `PNCodec_` plug-ins: `rv10.dll`, `rv103260.dll`, `rv20.dll`,
    `rv203260.dll`, `rv30.dll`, `rv303260.dll`, `rv40.dll`,
    `rv403260.dll` and `ra32clv1.dll`. The `rv*` ones front the `drv*`
    decoders above, which are run directly.
  - Helix decoders (`RACreateDecoderInstance`): `amrn.dll`, `amrw.dll`,
    `qclp.dll` and `ralf.dll`.
- **Not codecs:** RealPlayer's renderer plug-ins (`RMACreateInstance`):
  `h263render.dll`, `mp4arender.dll`, `rare3260.dll`, `rarender.dll`,
  `rvre3260.dll` and `rvrender.dll`.
- **`RealMediaSplitter.ax`** has both sets of entry points as well, but
  is a DirectShow server and is run as one.

### The codec zip, November 2026: 3DNow!

No CPU made in the last fifteen years or so has AMD's 3DNow!, so its
instructions fault with SIGILL. seance now emulates them
(`src/runtime/amd3dnow.c`):
- the 3DNow! and Extended 3DNow! operations (`0F 0F` and a suffix);
- FEMMS;
- PREFETCH and PREFETCHW, should the CPU lack those too.

They work on the MMX registers in the signal context, on i386 and
x86-64. The unit test `test_3dnow` checks each operation, each
addressing form and the x87 stack top.

- **Limits:**
  - PFRCP and PFRSQRT give full-precision results, not the hardware's
    14- and 15-bit estimates. The steps that refine those estimates
    (PFRCPIT1, PFRSQIT1, PFRCPIT2) pass the estimate through, so AMD's
    documented sequences still end at the right answer.
  - An operand behind an `fs:` or `gs:` override isn't emulated, nor is
    one at an address that can't be read. Either is reported as the
    illegal instruction it is.
  - Each instruction costs a signal. `_arvic.dll` takes about 2.5 s to
    encode or decode ten 320x240 frames.
- **`IsProcessorFeaturePresent(PF_3DNOW_INSTRUCTIONS_AVAILABLE)` still
  reports the CPU's own answer** (FALSE here). Codecs that check use
  their MMX or SSE code instead.
- **Result:**
  - `_arvic.dll` (ARVI) crashed with SIGILL in ICM_COMPRESS (at
    `+0x76e9`) and ICM_DECOMPRESS (at `+0x7894`). Both are `pavgusb`,
    which it uses without a CPU check.
  - It now round-trips at 30 dB PSNR (11.4 Mbit/s). `VFW_TRACE=1`
    reports the first emulated instruction.
  - 33 other DLLs in the zip contain 3DNow! code, among them
    libavcodec, xvidcore, ffdshow, the IMM5/IMM6 decoders, `kdm4.dll`
    and `blox.dll`.
    - 20 of them are codecs. They check the CPU first, and they get
      the same sweep results with and without the emulator.
    - The rest are libraries, not codecs.

## In-repository baseline

For comparison, the same command over `build/fixtures` examined 23 fixture
binaries with zero harness failures. `testfilter.ax` reported zero
unimplemented imports and no DLLs required from disk, while `testcodec.dll`
opened as `TST0`:

```sh
VFW_EMULATOR=qemu-i386 \
  sh tests/samples.sh build/seance build/fixtures 30
```

## Updating this file

For each future real-world run, record the exact product version and
architecture, keep the timeout finite, distinguish an ordinary “doesn't open”
result from a harness crash/hang, and list the first dependency or trap that
prevents progress. Once a blocker is fixed, retain it as completed history or
replace it with the next observed blocker rather than claiming full filter
compatibility prematurely.
