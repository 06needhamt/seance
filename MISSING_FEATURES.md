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
be loaded far enough to discover or connect their classes.

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

In approximate priority order:

1. Implement `InitializeSListHead` and audit the related interlocked singly
   linked-list (`SLIST`) APIs. An unsupported call must not abort a samples run
   merely while probing a dependency.
2. Add the subset of `bcrypt.dll` required by the bundled FFmpeg libraries.
3. Add the subset of `CRYPT32.dll` needed by `avformat-lav-63.dll`.
4. Provide enough `d3d9.dll` surface for software-only decoder startup, or a
   safe headless fallback that lets filters reject unavailable hardware paths.
5. Fill in common imports visible directly in `LAVVideo.ax`, including SRW
   locks (`AcquireSRWLockExclusive`/`ReleaseSRWLockExclusive`),
   `PathIsFileSpecW`, `PathRemoveFileSpecW`, `FindFirstFileExW`, and the
   extended semaphore/version/thread-affinity helpers reported by `seance
   info`.
6. Rerun discovery, dynamic `probe`, decode, and timeout tests after the loader
   reaches `DllRegisterServer` and `IClassFactory::CreateInstance`; the current
   run did not reach media-type negotiation, so it says nothing yet about LAV
   Video's graph compatibility.

The import counts above are per top-level image as printed by
`tests/samples.sh`; they are useful for triage but should not be interpreted as
the minimum number of functions needed for a successful software decode.

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
