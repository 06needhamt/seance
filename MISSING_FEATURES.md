<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Missing features found with real-world filters

This file records compatibility gaps found by running `tests/samples.sh`
against third-party codec and DirectShow binaries. It is an observation log,
not a promise to support every component in a tested package: seance's
DirectShow scope remains video transform decoders and encoders.

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

### Still blocked

| Component | First blocker |
|---|---|
| `LAVAudio.ax`, `LAVSplitter.ax` | `ncrypt.dll` (needed through `avformat-lav-63.dll`), then `ws2_32.dll`; and unimplemented CRT/advapi32 calls: `_findclose`, `_wfindfirst`/`_wfindnext`, `_wstat`/`_stat`, `_wfopen`, `_wfullpath`, `_wgetenv`, `_getcwd`, `_open_osfhandle`, `_getmaxstdio`/`_setmaxstdio`, `raise`, `wcstombs`, `CryptCreateHash` and relatives. Audio filters and splitters are outside the DirectShow scope, so these are noted rather than planned. |
| `IntelQuickSyncDecoder.dll` | `dxva2.dll`, `d3d11.dll`, `dxgi.dll`: hardware decoding, which the harness can't offer |
| `avformat-lav-63.dll`, `libbluray.dll` and the other FFmpeg libraries as standalone DLLs | They aren't filters or codecs; `libbluray.dll` and the others now load without unimplemented imports, `avformat` and `swresample` still have the calls listed above |

Not yet exercised: other LAV Video inputs (HEVC, VP9, AV1, WMV), its
hardware-acceleration settings, and anything that depends on thread-pool
timers.

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
