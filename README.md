<!-- SPDX-License-Identifier: GPL-2.0-only -->
# seance

[![CI](https://github.com/06needhamt/seance/actions/workflows/ci.yml/badge.svg)](https://github.com/06needhamt/seance/actions/workflows/ci.yml)

seance is a harness for loading and running 32-bit Video for Windows
(VfW / ICM) codec DLLs natively on Linux, without Wine. It maps the PE image
itself, provides a small Win32 runtime, and drives the codec's `DriverProc`
through a minimal reimplementation of the msvfw32 ICM API. The goal is to test
codecs and to decode or encode real AVI files with them. Its command-line
tool is `seance`.

It is generic: nothing in it depends on a particular codec.

## Status

Work is split into phases; see [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).

| Phase | Scope | State |
|---|---|---|
| 0 | Build skeleton, Win32 headers, ABI cross-check, CLI skeleton | done |
| 1 | PE loader and `info` | done |
| 2 | Win32 runtime (TEB, SEH, threads, stub DLLs) | done: TEB, built-in DLLs, kernel32 (including SRW locks, condition variables, `InitOnce`, interlocked lists and thread-pool timers, waits and work), ntdll, SEH dispatch, delay-load binding, the msvcrt-family CRT (math, string, stdio, low-level I/O, `_findfirst`, `_stat`, startup, 64-bit helpers, `__try`/`__except`, setjmp/longjmp, C++ exceptions and RTTI, printf dialect), headless user32, gdi32 and advapi32 (registry, security), version, ole32, oleaut32, shell32, comctl32, comdlg32, and offline ws2_32, bcrypt, ncrypt, crypt32, d3d9, dxva2, d3d11 and dxgi |
| 3 | VfW layer and `probe` | done: driver lifecycle, ICM API (including ICSeqCompressFrame*, ICImageCompress/ICImageDecompress and DrawDib*), winmm/msvfw32 built-ins, `probe`, `list` |
| 4 | AVI / Y4M I/O, `decode` and `encode` | done: AVI reader and writer, Y4M/BMP/raw input and output, `avi`, `convert`, `decode`, `encode` |
| 5 | Hardening and CI | done: DLL memory below 2 GB, crash and hang reports, SSE/FPU fixture, fuzzing, samples run, CI |

DirectShow video decoders and encoders (`.ax`) have phases of their own; see
[docs/DIRECT_SHOW_PLAN.md](docs/DIRECT_SHOW_PLAN.md). They are done: the headers
and their ABI check, a built-in quartz.dll (filter mappers, device enumerator,
`AMGetErrorText`), finding and registering filters (`register`, `list`), the
minimal source/sink graph, and `probe`, `decode` and `encode` for filters,
including dynamic format changes, `ICodecAPI` and `IAMVideoCompression`
encoder controls, persisted filter state and MPEG-1/MPEG-2 media types.

The real-world check is the LAV Filters 0.83 package (32-bit): LAV Video
loads, registers, connects and decodes 18 codecs identically to FFmpeg, and
the whole package loads with no unimplemented imports. The results, and what
is not supported or not yet exercised, are in
[MISSING_FEATURES.md](MISSING_FEATURES.md). Out of scope: audio, splitter and
renderer filters, DMOs, capture devices, a real Filter Graph Manager, network
access and hardware decoding.

DMOs are found, registered and run for video decoding and encoding as well;
audio decoders and encoders (filters and DMOs) are planned in
[docs/DMO_AUDIO_PLAN.md](docs/DMO_AUDIO_PLAN.md).

## Prerequisites

The codecs are i386 code, so the harness is built as a 32-bit Linux program.
On a 64-bit x86 host the kernel must support running 32-bit programs, which
most distributions enable by default.

- A C compiler that builds i386 programs, one of:
  - GCC with 32-bit support (`gcc-multilib` on Debian and Ubuntu);
  - an i686 Linux cross compiler (`gcc-i686-linux-gnu`, with
    `libc6-dev-i386-cross`). On Debian and Ubuntu it conflicts with
    `gcc-multilib`, so install one or the other;
  - clang with an i386 sysroot.

  On a host that isn't x86 (aarch64, say) only the last two work, and the
  tests need qemu-user (`qemu-i386`) to run.
- CMake 3.20 or later (3.25 for the presets) and Ninja or make
- Optional: the mingw-w64 i686 cross compiler (`mingw-w64`). It builds the
  test fixture DLLs and runs the ABI cross-check; without it those tests are
  skipped.
- Optional: FFmpeg (`ffmpeg`, `ffprobe`) for the media cross-checks.

```sh
sudo apt install build-essential gcc-multilib cmake ninja-build mingw-w64 ffmpeg
```

## Building and testing

```sh
cmake --preset default        # configure build/ (or: cmake -B build)
cmake --build --preset default
ctest --preset default        # unit tests, ABI cross-check, CLI smoke test,
                              # FFmpeg media checks

cmake --preset asan && cmake --build --preset asan && ctest --preset asan
                              # the same under AddressSanitizer and UBSan,
                              # in build-asan/
```

The harness is always built as an i386 program. Before anything else,
configuring picks a compiler for that ([`cmake/i386.cmake`](cmake/i386.cmake)),
trying each until one links a 32-bit program with the headers the harness
needs:

1. With a toolchain file, the toolchain file decides.
2. A compiler you give with `-DCMAKE_C_COMPILER=...` or `$CC` is used with
   `-m32`.
3. On an x86 host (x86_64 or i386), the default compiler with `-m32`.
4. An i686 cross compiler: `i686-linux-gnu-gcc` or another of its usual
   names.
5. `clang --target=i686-linux-gnu`.

Configuring fails early, saying what to install, if none of them works. The
choice is kept in the cache (delete the build directory to choose again),
and the configure output names it.

On a host that isn't x86 this is a cross build. The tests then run through
`qemu-i386` when it is installed (or binfmt_misc, if it runs i386 programs
directly). If neither is available, `BUILD_TESTING` defaults to off, since
nothing built could run. Under qemu-user all the tests pass, except that
`test_lowmem` skips: the guest's memory is all below 2 GB anyway.

Useful CMake options:

| Option | Effect |
| --- | --- |
| `VFW_REQUIRE_MINGW=ON` | fail instead of skipping what needs mingw-w64 |
| `VFW_REQUIRE_FFMPEG=ON` | fail instead of skipping the FFmpeg checks |
| `VFW_SANITIZE=ON` | build with ASan and UBSan |
| `VFW_MINGW_PREFIX=...` | the cross tools' prefix (`i686-w64-mingw32-`) |
| `VFW_SAMPLES_DIR=<dir>` | add a `samples` test: `info` and `probe` over the codec DLLs in `<dir>` |
| `VFW_HOST_ARCH=...` | the host architecture as `uname -m` spells it, if detecting it goes wrong |

The `ci` preset turns on both `REQUIRE` options. Tests carry the CTest
labels `unit`, `abi`, `cli`, `media` and `samples`, so `ctest --preset default -L unit`
runs only the unit tests and `-LE media` leaves out the FFmpeg checks.

The media checks (`validate_media`) need `ffmpeg` and `ffprobe`. FFmpeg
reads the AVI, Y4M and BMP files the harness writes, and
`seance convert` has to reproduce FFmpeg's own decoding of AVIs FFmpeg
wrote in each uncompressed format. Setting `VFW_KEEP_TEST_FILES=<dir>` when
running `test_avi` or `test_image` keeps the files they write.

The loader and codec tests use small DLLs from `tests/fixtures/`, which
has its own CMake project: the top-level build cross-compiles it with
`cmake/mingw-i686.cmake` as an external project, into `build/fixtures/`.
The DLLs are built without a C runtime, so each imports only what it names.

CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs on every
push and pull request: the `ci` preset with nothing skipped, the `asan`
preset, and a build with only `gcc-multilib` installed to check that the
fixture tests skip cleanly.

The `check_abi` test compiles every struct size, field offset and constant
in `include/win32/` against the mingw-w64 Windows headers and fails if any
of them differ.

## Usage

```sh
build/seance --help
build/seance info some-codec.dll
```

`info` parses a DLL without loading or running it. It prints the headers,
sections, exports (including ordinal-only exports and forwarders), imports
and delay imports, TLS callbacks, resources and version information.
When a DLL is loaded, its delay-load imports are bound straight away, like
its other imports; one whose DLL or function can't be found is bound to a
trap that says why if the codec ever calls it.

```sh
build/seance avi clip.avi [--chunks]
build/seance convert clip.avi -o clip.y4m
build/seance convert clip.avi -o 'frame%05d.bmp' --start 100 --frames 10
build/seance convert clip.avi -o - --format y4m | ffplay -
```

`avi` shows an AVI file's main header, its streams and formats, and which
index was used, with `--chunks` listing every chunk. The reader handles AVI
1.0 files and OpenDML (AVI 2.0, over 1 GB) files. It builds the index from
the OpenDML `indx`/`ix##` indexes, then `idx1`, then a scan of the `movi`
lists. It checks every offset and size, so damaged or truncated files give
whatever frames can be recovered.

`convert` writes a stream of uncompressed video without a codec. The output
is chosen by extension or `--format`:

| Output | Contents |
|---|---|
| `.y4m` | YUV4MPEG2, 4:2:0 |
| `.bmp` | one 24-bit BMP per frame; the name needs a frame number: `%d`, `%u` or `%0Nd` (`%%` for a `%`) |
| `.yuv` | raw I420 frames |
| `.bgr` | raw top-down BGR24 frames |
| `.raw` | the frames exactly as stored |

It accepts these inputs:
- BI_RGB at 1, 4 and 8 bits with a palette (including AVI palette changes), and at 16 (RGB555), 24 and 32 bits.
- BI_BITFIELDS at 16 and 32 bits.
- BI_RLE8 and BI_RLE4.
- YUY2, UYVY, YVYU, I420/IYUV, YV12, NV12, NV21 and Y800.

YUV is BT.601 limited range, and empty (dropped) frames repeat the previous one.
Compressed streams need a codec: see `decode`.

### Codecs: `probe` and `list`

```sh
build/seance probe codec.dll         # what the codec is and accepts
build/seance probe codec.dll clip.avi # can it decode this file, and to what
build/seance probe XVID              # a codec registered in vfw.ini
build/seance list
```

`probe` opens a codec the way msvfw32's `ICOpen` does. It loads the DLL,
sends `DRV_LOAD`, `DRV_ENABLE` and `DRV_OPEN` with an `ICOPEN` to its
`DriverProc`, then asks it about itself:
- `ICM_GETINFO`: the name, description, version and flags.
- The size of its state, its default quality and keyframe rate.
- Whether it has configure and about dialogs.

It then checks decompression. With an AVI, it uses the file's video format;
without one, it uses the codec's own FOURCC. It prints the codec's suggested
output, which of the standard formats it accepts (I420, YV12, YUY2, UYVY,
BGR32, BGR24, RGB565, RGB555, PAL8), and the output the harness would pick.

For compression, it tries each standard format as input, at `--size`
(default 320x240). For each accepted input it shows the compressed format,
the largest frame size, and whether the codec decodes that format again.

Some codecs only open under their own FOURCC. `probe` retries with the
handler the codec reports, and `--fourcc XXXX` sets it explicitly.
`VFW_TRACE=1` logs every driver message and its result.

Codecs are registered for opening by FOURCC (and for codecs that open other
codecs through msvfw32) in `vfw.ini`, in a `[drivers32]` section like
Windows' `system.ini`:

```ini
[drivers32]
vidc.XVID=/opt/codecs/xvidvfw.dll
vidc.CVID=C:\windows\system32\iccvid.dll
```

The file is `$VFW_INI`, else `$VFW_ROOT/vfw.ini` (default
`~/.seance/vfw.ini`). Relative paths are taken from the file's
directory, and Windows paths go through the drive mapping (`C:` is
`$VFW_ROOT/drive_c`, `Z:` is `/`). `list` shows the entries and opens each
one to show its name.

Codecs usually import more than kernel32, winmm and msvfw32. The msvcrt
family (msvcrt.dll, msvcr70-140, msvcp*, ucrtbase, api-ms-win-crt-*) is a
real built-in: math, string/memory, stdio, CRT startup and the 64-bit
arithmetic helpers a codec calls whatever CRT it links; a native DLL of
the same name on the search path takes precedence over it.

user32, gdi32, advapi32, version, ole32, oleaut32, shell32, comctl32 and
comdlg32 are built in too, headless:

- **user32:** window classes and windows exist and get the messages
  Windows would send them, each thread has a message queue with timers,
  and LoadString, wsprintf and the `Char*` functions work; nothing is drawn
  and no input arrives. A message box prints to stderr and answers with
  its default button, as if someone pressed Enter. Dialog boxes can't be
  shown: `DialogBox*` and `CreateDialog*` fail and say so. msvfw32's
  compressor dialog, `ICCompressorChoose`, answers as if OK were pressed
  on its initial selection (the compressor chosen last time, else Full
  Frames), or as `$VFW_COMPRESSOR` says: a FOURCC, `uncompressed` or
  `cancel`. DrawDib draws DIBs, compressed or not, onto a memory DC as on
  Windows; drawing to a window's DC is discarded.
- **gdi32:** memory DCs, device-dependent bitmaps and DIB sections, with
  conversion between every DIB format and blits between them (`BitBlt`,
  `StretchBlt`, `StretchDIBits`, `GetDIBits`, ...), palettes, brushes,
  pens and fonts. The display is a 1024x768 32-bit device that discards
  what is drawn on it; text is measured but not drawn.
- **advapi32:** the registry, kept in a `.reg` file (see below), with
  change notification; `Drivers32` there shows the codecs `vfw.ini`
  registers. Security answers as for an administrator on XP: tokens,
  privileges, SIDs and account names, ACLs and security descriptors, with
  nothing enforced. `CryptGenRandom` gives random numbers.
- **version:** `GetFileVersionInfo*` and `VerQueryValue` read a DLL's
  version resource. The built-in DLLs answer with a version block of their
  own, versioned like Windows XP SP3's (5.1.2600.5512), for codecs that
  check a system DLL's version.
- **ole32:** `CoInitialize` and the rest of the apartment calls (counted,
  with no other effect), task memory, GUID strings and ProgIDs, and
  in-process servers: `CoCreateInstance` finds a class registered with
  `CoRegisterClassObject`, or loads the DLL named by the registry's
  `HKEY_CLASSES_ROOT\CLSID\{...}\InprocServer32` and calls its
  `DllGetClassObject`. There are no out-of-process servers or marshalling.
- **oleaut32:** BSTRs, VARIANTs with `VariantChangeType` and the whole
  `Var*From*` family (Windows' rounding and overflow rules, strings read
  and written as in US English), SAFEARRAYs, date conversions and error
  information, with Windows' ordinals, since code built with MSVC imports
  oleaut32 by ordinal. A property frame (`OleCreatePropertyFrame`) is
  closed as if with Cancel and says so on stderr; there are no type
  libraries, OLE pictures or fonts.
- **shell32:** special folders (`SHGetFolderPath` and friends) laid out
  as on Windows XP under drive C:, with the profile named after `$USER`
  (`C:\Documents and Settings\<user>\Application Data`, ...) and each
  created when first asked for; item ID lists for them;
  `CommandLineToArgvW`; `SHFileOperation` (copy, move, rename and delete,
  with wildcards and whole trees, answering every confirmation "Yes to
  all"), `SHCreateDirectoryEx` and `SHGetFileInfo`. With no desktop,
  `ShellExecute` has nothing to open anything with, `SHBrowseForFolder` is
  cancelled, and there are no icons or dropped files; each says so on
  stderr.

- **comctl32:** `InitCommonControls(Ex)` registers the control classes.
  Nothing is drawn, so a control is its state: trackbars, progress bars,
  up-down controls (with their buddy's text) and status bars keep and
  answer theirs, and the other classes are plain windows. Window
  subclassing (`SetWindowSubclass`), the DPA and DSA arrays, `Alloc` and
  `Str_SetPtr`, and image lists (kept at 32 bits, drawn through gdi32
  with their masks) work, with Windows' ordinals, since much of comctl32
  is imported by ordinal. `PropertySheet` destroys its pages (their
  `PSPCB_RELEASE` callbacks run) and answers as if Cancel were pressed;
  `TaskDialog` answers with its default button, as `MessageBox` does.
- **comdlg32:** `GetOpenFileName` and `GetSaveFileName` answer with the
  file `$VFW_OPEN_FILE` or `$VFW_SAVE_FILE` names (a Windows path, or an
  absolute Unix one; relative ones are in the dialog's initial folder),
  checked as the dialog would and with the default extension added;
  unset or `cancel`, they are cancelled. `ChooseColor` and `ChooseFont`
  answer OK with what they start with, there are no printers
  (`PrintDlg` fails with `PDERR_NODEFAULTPRN`), and the find and replace
  dialogs fail, as `CreateDialog` does. Each says so on stderr.
- **winspool.drv:** there are no printers. `OpenPrinter` fails with
  `ERROR_INVALID_PRINTER_NAME`; `DocumentProperties` and `ClosePrinter` fail
  with `ERROR_INVALID_HANDLE`. This lets codecs that import the spooler for a
  configuration page load and continue to use their non-printing paths.
- **setupapi:** device-information sets are empty: codecs can enumerate for
  capture hardware, find none, and continue with file-based operation.
- **oleacc:** Microsoft Active Accessibility has no visible controls to expose;
  creating or marshalling an `IAccessible` proxy reports `E_NOTIMPL`.
- **avifil32:** the legacy COM-based AVIFile library is present so codecs with
  optional AVI utilities can load. Those utilities report
  `AVIERR_UNSUPPORTED`; seance commands use the native AVI reader instead.

- **d3d9:** there is no adapter. `Direct3DCreate9` returns NULL and
  `Direct3DCreate9Ex` fails with `D3DERR_NOTAVAILABLE`, which video filters
  take as "decode in software".
- **bcrypt:** just the `RNG` provider and `BCryptGenRandom`; other algorithms
  are not found.
- **crypt32:** certificate stores that are always empty.
- **ws2_32, wsock32:** Winsock for a machine with no network. It starts,
  converts byte order and address text, and resolves numeric addresses, but
  `socket` fails as if the network were down and names don't resolve.
- **ncrypt:** bcrypt's random numbers; there is no key storage provider.
- **dxva2, d3d11, dxgi:** no adapter, like d3d9, so decoders fall back to
  software.
- **kernel32, newer calls:** slim reader/writer locks, condition variables,
  `InitOnce` and interlocked lists work (built on one process-wide lock, so
  they are correct but not fast). Thread-pool timers, waits and work items
  run on a thread each.
- **Memory:** `malloc`, `operator new` and `HeapAlloc` return zeroed memory by
  default, as a fresh Windows heap usually does, since code that was only ever
  run there can depend on it. (This also makes a codec's output repeatable.)
  `VFW_ZERO_MEMORY=0` turns it off, to find reads of uninitialised memory
  (`HEAP_ZERO_MEMORY` is still honoured).
- **CryptoAPI:** the random numbers work; hashes and keys can't be created.

A function a built-in DLL doesn't implement still lets a codec load:
`info` shows which of its imports those are, and calling one stops the
harness with a message naming it.

#### Codec dependency DLLs

Not every DLL imported by a codec is part of Windows. Codec distributions
often contain private companion DLLs, and seance does not reimplement those.
For example, an error such as

```
MultiCodec.dll: SImageLib.dll: not found (searched the importer's directory, $VFW_PATH and the current directory)
```

means that `MultiCodec.dll` has a normal PE import for `SImageLib.dll`, but no
file by that name was found. Obtain the **32-bit** companion DLL from the same
codec installation or distribution, then either place it beside
`MultiCodec.dll` or add its directory to the colon-separated `VFW_PATH`:

```sh
VFW_PATH=/path/to/codec-dependencies build/seance probe /path/to/MultiCodec.dll
```

Use the companion from the exact product and version that supplied the codec.
Do not substitute a same-named file from a generic DLL-download site: private
DLL names do not identify a stable public ABI, so a different build may have
incompatible exports or behaviour. If the original installer is unavailable,
record the codec's version information and checksum before looking for an
archived copy.

Name matching is case-insensitive, as it is on Windows. The codec's own
directory is searched first, followed by each directory in `VFW_PATH`, then
the current directory. A missing private library is therefore a packaging or
search-path issue, not a Win32 API stub that should be added to seance. By
contrast, if the missing name is a Windows system DLL, it may indicate that
seance needs another built-in DLL; `info` can be used to inspect the import
without loading the codec.

Some dependencies fall between those two cases: they are vendor runtimes,
not Windows API DLLs and not private parts of the codec. `MFC42.DLL`, for
example, is the 32-bit Microsoft Foundation Classes runtime used by programs
built with older Visual C++ toolsets. It should not be reimplemented as a
seance built-in. Supply the matching retail runtime DLL from a licensed
Visual C++ redistributable, the codec's original installer, or a 32-bit
Windows installation, and put it beside the codec or on `VFW_PATH`. Do not
use `mfc42d.dll`, which is the non-redistributable debug build. A current
Visual C++ redistributable does not replace this versioned legacy runtime.

### The registry

Codecs that keep their settings in the registry get one that persists
between runs, in `$VFW_REGISTRY`, else `$VFW_ROOT/registry.reg` (default
`~/.seance/registry.reg`). It is saved after every change, in the
format regedit exports, so it can be read, edited or seeded by hand, and a
`.reg` file exported from Windows (UTF-16 or REGEDIT4) can be dropped in:

```
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\SomeCodec]
"Quality"=dword:0000004b
"Mode"="fast"
```

`HKEY_CLASSES_ROOT` is `HKEY_LOCAL_MACHINE\Software\Classes`. Setting
`VFW_REGISTRY=` (empty) keeps the registry in memory only, starting empty
each run.

### DirectShow filters and DMOs: `register` and `unregister`

```sh
build/seance register --dry-run filter.ax  # what it would register
build/seance probe filter.ax               # filters, roles, pins and types
build/seance probe filter.ax clip.avi      # dynamically accepted outputs
build/seance register filter.ax
build/seance list                          # ... then DirectShow filters
build/seance unregister filter.ax
```

A DirectShow filter server (usually an `.ax` file, which `info` calls a COM
server) registers its filters itself, from its `DllRegisterServer`, with
quartz's filter mappers. `register` runs it, as `regsvr32` would, into the
registry above; `unregister` runs `DllUnregisterServer`. Both print the
filters registered: CLSID, name, whether it looks like a video decoder or
encoder (from its pins' media types), merit, category and pins. With
`--dry-run` it runs in a copy of the registry that is thrown away
afterwards, so nothing changes.

`probe` also discovers filters in a server without registering it and prints
the same CLSID, role, merit, pin and media-type metadata. Servers containing
several classes can be narrowed with `--clsid {GUID}`. With an AVI argument,
`probe` also builds the minimal graph and reports the standard output formats
to which each decoder actually connects.

`list` shows the video decoders and encoders registered, from the
filter categories and the older `HKCR\Filter` keys, with the DLL that
serves each; `--all` shows every filter.

DirectX Media Objects (DMOs) are found the same way. A DMO's DLL registers
it from its `DllRegisterServer`, through the built-in msdmo.dll's
`DMORegister` or by writing `HKCR\DirectShow\MediaObjects` itself, and
`register`, `unregister` and `probe` show each DMO's CLSID, name, role,
category and registered input and output types, and `probe` the interfaces
a DMO decoder or encoder has and, given an AVI, the output formats a decoder
takes for it. `list` adds the DMO decoders and encoders registered
(`--all`: every DMO). `decode` and `encode` drive DMOs as they drive filters
(below); audio is still to come, see
[docs/DMO_AUDIO_PLAN.md](docs/DMO_AUDIO_PLAN.md). A `.reg` file exported from
Windows with filters in it can be dropped in as the registry; the
filters' DLLs are then found through the drive mapping.

### Decoding and encoding: `decode` and `encode`

```sh
build/seance decode codec.dll clip.avi -o clip.y4m
build/seance decode filter.ax clip.avi -o clip.y4m --clsid '{GUID}'
build/seance decode dmo.dll clip.avi -o clip.y4m
build/seance decode auto clip.avi -o 'frame%05d.bmp' --start 250 --frames 10
build/seance decode XVID clip.avi -o clip.yuv --output-format YV12

build/seance encode codec.dll in.y4m -o out.avi --keyint 30 --quality 8000
build/seance encode filter.ax in.y4m -o out.avi --clsid '{GUID}'
build/seance encode TST0 'shot%04d.bmp' -o out.avi --rate 30000/1001
build/seance encode codec.dll in.yuv -o out.avi --input-format I420 --size 720x576
ffmpeg -i any.mkv -f yuv4mpegpipe - | build/seance encode codec.dll - -o out.avi
```

`decode` runs an AVI's video stream through a codec's decompressor
(`ICM_DECOMPRESS_BEGIN`, then `ICDecompress` per frame) and writes the
frames with the same writers as `convert`. The codec is a DLL, a FOURCC
from `vfw.ini`, or `auto` for whichever registered codec accepts the
stream (`ICLocate`). A DLL is opened as the stream's handler unless
`--fourcc` says otherwise. The output format is `--output-format` if
given; otherwise I420 for Y4M and YUV output and BGR24 for BMP and BGR, if
the codec offers them, then what the codec suggests, then the first
standard format it accepts. `--start` begins decoding at the keyframe
before that frame, feeding the frames in between with
`ICDECOMPRESS_PREROLL`; frames that aren't keyframes get
`ICDECOMPRESS_NOTKEYFRAME`. A frame the codec rejects stops the decode,
or with `--keep-going` repeats the previous picture. `.raw` output keeps
the codec's output exactly, in whatever format it is. A DirectShow `.ax`
server is loaded without registration, its decoder is connected between the
built-in source and sink pins, and compressed AVI chunks are delivered as
timestamped media samples. `--clsid` selects a class when necessary.
A stream's format is offered to the filter as `VideoInfo`, then
`VideoInfo2`, then, for MPEG-1 and MPEG-2 video, as the `MPEG1Video` or
`MPEG2_VIDEO` subtype that decoders register for; the first the filter accepts
is used. A sample that carries a new media type is followed when only the pixel
format changes. For `--start`, the DirectShow graph is flushed and given a new
segment before frames are preroll-fed from the preceding keyframe; frames
before the start (by time stamp) or past `--frames` are dropped even if the
filter doesn't flag them as preroll. Output and EOS may arrive
from a filter worker thread; EOS waits are bounded by `VFW_TIMEOUT` (five
seconds by default).

`encode` compresses video into an AVI through the msvfw32 sequence API
(`ICSeqCompressFrameStart`, `ICSeqCompressFrame`, `ICSeqCompressFrameEnd`),
which the harness also exports to codecs. It reads:
- Y4M, 4:2:0 (any chroma siting) or mono, from a file or `-` for stdin.
- An uncompressed AVI stream; RLE and palette-changing streams are turned
  into BGR24.
- A BMP file, or numbered files (`shot%04d.bmp`, from 0 or 1).
- Raw frames with `--input-format` (I420, YV12, YUY2, UYVY, BGR32, BGR24,
  RGB565, RGB555) and `--size`; raw RGB is top-down with unpadded rows,
  as FFmpeg's `rawvideo` and our `.bgr` output are.

Frames go to the codec as they are if it takes them
(`ICM_COMPRESS_QUERY`); otherwise they are converted to the first standard
format it takes, or to `--codec-input`. `--keyint` (default: the codec's
`ICM_GETDEFAULTKEYFRAMERATE`), `--quality` (0 to 10000) and `--bitrate`
(kbit/s, for codecs that can aim for one) set the sequence, and
`ICM_COMPRESS_FRAMES_INFO` tells the codec about them and the frame rate.
The keyframe flags in the AVI index are the codec's. `--state` sets a
codec state saved earlier with `--save-state` (`ICM_SETSTATE` /
`ICM_GETSTATE`), and `--odml` writes OpenDML indexes even for small
files; files over 1 GB always get them.

For a DirectShow `.ax` server, `encode` selects its video encoder (or the
class named by `--clsid`), negotiates the source or a converted standard raw
format on the input pin, and writes each compressed output sample to AVI. Its
sync-point flag becomes the AVI keyframe flag. Quality, keyframe interval and
bitrate use `IAMVideoCompression` and `ICodecAPI`, whichever the filter has
(an option neither takes is an error); `--state` and `--save-state` use
`IPersistStream`.

A DMO is driven the same way, without a graph. The DLL's video decoder (or
encoder) DMO is created from its `DllGetClassObject`, its input type set
(each form tried, as for a filter) and then its output type, each first with
`DMO_SET_TYPEF_TEST_ONLY`. Each AVI chunk goes in with `ProcessInput` as a
buffer marked as a sync point and stamped with its time. Everything the DMO
has comes out with `ProcessOutput`, asked again while it says
`DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE`. A DMO that answers
`DMO_E_NOTACCEPTING` is drained first, and at the end `Discontinuity` lets
out what it still holds. DMOs know no preroll and needn't stamp their output,
so the harness gives each output the preroll flag and, if unstamped, the times
of the input it came from. A change in `GetOutputCurrentType` is handled like
a filter's new media type. For `--start`, the DMO is flushed. An encoder with
`IWMCodecPrivateData` has its codec data appended to the stream header.
Encoder options and state go through `ICodecAPI`/`IAMVideoCompression` and
`IPersistStream`, as for filters. `decode auto` also tries the DMOs
registered as video decoders, after the filters.

Both commands print the frame count, the formats on each side and the time
spent in the codec per frame. With `testcodec.dll`, encoding a clip and
decoding it again gives the input back byte for byte, which the unit tests
and `validate_media` check for Y4M, BMP, raw and AVI input.

### Real codecs: the samples run

```sh
tests/samples.sh build/seance ~/codecs   # or -DVFW_SAMPLES_DIR=~/codecs
```

`tests/samples.sh` runs `info` and `probe` over every DLL in a directory
you supply. For each it reports how many of its imports the harness
doesn't implement yet, how many DLLs it needs from disk, and whether it
opens and under which FOURCC. It asserts nothing about any particular
codec; it fails only when the harness can't parse a file, or crashes or
hangs on one.

### Robustness

- **Memory below 2 GB.** Windows gives programs that aren't large-address
  aware the low 2 GB, and codec code may treat pointers as signed. At
  startup the harness reserves the free address space from 2 GB up, and
  runs each command on a thread with a low stack, so every heap block,
  frame buffer, stack, TEB and DLL image a codec sees is below 2 GB.
  `VFW_LOW_MEMORY=0` turns this off; sanitizer builds leave it off.
- **Crashes.** A fault in DLL code is reported with the signal, the
  faulting `module+offset`, the address touched, and the call in progress,
  and the process then dies of the signal:

  ```
  seance: crash: SIGSEGV at testcodec.dll+0x178e, accessing (nil)
  seance:   in DriverProc(DRV_USER+0x64) of testcodec.dll
  ```
- **Hangs.** With `VFW_TIMEOUT=<seconds>`, a single call into a DLL that
  takes longer is reported the same way, and the harness exits with 124.

### Environment variables

| Variable | Effect |
|---|---|
| `VFW_PATH` | `:`-separated directories to search for DLLs, after the importing DLL's own directory |
| `VFW_FORCE_RELOC=1` | Load every image away from its preferred base, to exercise relocations |
| `VFW_ROOT` | The harness's Windows world: `drive_c`, `vfw.ini` and `registry.reg` (default `~/.seance`) |
| `VFW_INI` | The codec registry file (default `$VFW_ROOT/vfw.ini`) |
| `VFW_REGISTRY` | The Windows registry file (default `$VFW_ROOT/registry.reg`); empty keeps it in memory only |
| `VFW_TRACE=1` | Log calls into the built-in DLLs and every driver message to stderr |
| `VFW_TIMEOUT=<s>` | Report and stop (exit 124) when one call into a DLL takes longer |
| `VFW_COMPRESSOR` | What `ICCompressorChoose` answers: a FOURCC (`TST0`), `uncompressed` or `cancel` |
| `VFW_OPEN_FILE`, `VFW_SAVE_FILE` | What `GetOpenFileName` and `GetSaveFileName` answer: a file, or `cancel` (the default) |
| `VFW_ZERO_MEMORY=0` | Don't zero memory from `malloc`, `operator new` and `HeapAlloc` (zeroed by default) |
| `VFW_LOW_MEMORY=0` | Don't keep DLL-visible memory below 2 GB |
| `VFW_ENV_<NAME>=<value>` | Put `NAME=value` in the Windows environment codecs see |

## Layout

| Path | Contents |
|---|---|
| `include/win32/` | Win32 types, PE image structures, GDI, window-manager and registry definitions, driver and ICM definitions, laid out to match 32-bit Windows |
| `src/loader/` | PE parsing, mapping and relocation (`pe.c`); the module table and dependency loading (`modules.c`) |
| `src/runtime/` | TEB and `%fs`, implicit TLS, the built-in DLL registry, trap thunks for unimplemented imports, the 2 GB limit (`lowmem.c`), crash and hang reports (`crash.c`) |
| `src/win32/` | Built-in kernel32 (`k32_*.c`), ntdll, the msvcrt family (`crt_*.c`), user32 (`user32.c`, `u32_*.c`), gdi32 (`gdi32.c`, `gdi_bitmap.c`), advapi32 (`advapi32.c`, `adv_security.c`) and its registry (`reg.c`), version, ole32, oleaut32 (`oleaut32.c`, `oleaut_conv.c`, `oleaut_array.c`, `errinfo.c`), shell32 (`shell32.c`, `shell_file.c`), comctl32 (`comctl32.c`, `comctl_dpa.c`, `comctl_imagelist.c`, `comctl_sheet.c`), comdlg32, winmm, msvfw32 and quartz (`quartz.c`) |
| `src/dshow/` | DirectShow: finding and registering filters (`discover.c`) |
| `src/vfw/` | Installable drivers (`driver.c`), the codec registry (`registry.c`), the ICM API (`icm.c`), sequence compression (`seq.c`) |
| `src/avi/` | AVI reader (AVI 1.0 and OpenDML) and writer |
| `src/in/` | Frame sources for `encode`: Y4M, uncompressed AVI, BMP files, raw frames |
| `src/out/` | DIB to and from BGR24 / I420; Y4M, BMP and raw writers |
| `src/common/` | Error reporting, file, UTF-16 and FOURCC helpers |
| `src/` | CLI entry point and commands (`main.c`, `cmd_*.c`, shared helpers in `cmd_util.c`) |
| `tests/unit/` | Unit tests (`test_*.c`, each a standalone program) |
| `tests/fixtures/` | Sources of the test DLLs, including `testcodec.c`, a lossless VfW codec (`TST0`), and `testfilter.c`, a DirectShow filter server |
| `tests/abi/` | ABI cross-check against the mingw-w64 headers |
| `tests/validate_media.sh` | Cross-check of the AVI and image code against FFmpeg |
| `tests/smoke.sh` | A quick run of every CLI command |
| `tests/samples.sh` | `info` and `probe` over a directory of codec DLLs |
| `cmake/` | The mingw-w64 toolchain file for the fixtures, and build helpers |
| `docs/` | Implementation plan |

## License

GPL v2; see [LICENSE](LICENSE).
