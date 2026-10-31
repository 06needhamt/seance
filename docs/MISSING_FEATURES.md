<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Missing features found with real-world filters

This file records compatibility gaps found by running `tests/samples.sh`
against third-party codec and DirectShow binaries. It is an observation log,
not a promise to support every component in a tested package: seance's
DirectShow and DMO scope is transform decoders and encoders, of video and
(since the DMO/audio plan) of audio.

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

Not yet exercised: VC-1 / WMV3 (no encoder to make a stream with here),
interlaced and field-coded streams, 10-bit and 4:4:4 sources (the standard
output list is 8-bit), more than one video stream, and LAV Video's other
settings (format selection, deinterlacing, dithering).

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
| `avimszh.dll` | should no longer overrun (not yet re-run) | the slack Windows' heap leaves after a block (its 32-byte header), now kept after guarded output buffers |
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

**Can't work in seance:**

- **Win9x thunk codec:** `Mvicod32.dll` (Motion Pixels) calls a 16-bit DLL,
  `MVICOD16.DLL`, through `ThunkConnect32`. Such DLLs are now refused, with
  that reason, before their `DllMain` runs.
- **16-bit and 64-bit DLLs:** 16-bit NE DLLs (`vp5r*`, `ativcr1`,
  `mvicod16`, ...) and 64-bit `cinevf2w.dll`.
- **RealPlayer codecs:** `*3260.dll`.
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

**Still open, waiting for a re-run on the current build:**

- **Refuse `DRV_LOAD`:** `rricm` and `cvpcdvc`; the message may now name
  a companion DLL they look for.
- **Fail in `ICM_COMPRESS`:**
  - `BMDCodecLib`, opened as `HDYC`, returns `ICERR_ERROR`.
  - `AvidAVICodec` returns `ICERR_INTERNAL`, probably because DNxHD
    supports only fixed frame sizes.
- **Unload:** `packed.dll`'s detach jumps to a bad address. It is now
  survived, but the cause is not known.

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
