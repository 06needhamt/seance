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
| `LAVAudio.ax`, `LAVSplitter.ax` | They load and register, but audio decoders and splitters are outside the DirectShow scope (video transform decoders and encoders), so nothing drives them. |
| Network | Winsock starts and resolves numeric addresses, but no socket can be created: `socket` fails as if the network were down, so FFmpeg's protocols find nothing reachable. |
| CryptoAPI hashes, keys; NCrypt | There is no hashing algorithm to create and no key storage provider to open. |
| Hardware decoding | Every route (DXVA2, D3D11, Quick Sync, CUDA) reports no hardware and LAV decodes in software. |

Not yet exercised: VC-1 / WMV3 (no encoder to make a stream with here),
interlaced and field-coded streams, 10-bit and 4:4:4 sources (the standard
output list is 8-bit), more than one video stream, and LAV Video's other
settings (format selection, deinterlacing, dithering).

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
