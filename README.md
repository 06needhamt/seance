# vfw-harness

[![CI](https://github.com/06needhamt/vfw-harness/actions/workflows/ci.yml/badge.svg)](https://github.com/06needhamt/vfw-harness/actions/workflows/ci.yml)

A harness for loading and running 32-bit Video for Windows (VfW / ICM) codec
DLLs natively on Linux, without Wine. It maps the PE image itself, provides a
small Win32 runtime, and drives the codec's `DriverProc` through a minimal
reimplementation of the msvfw32 ICM API. The goal is to test codecs and to
decode or encode real AVI files with them.

It is generic: nothing in it depends on a particular codec.

## Status

Work is split into phases; see [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).

| Phase | Scope | State |
|---|---|---|
| 0 | Build skeleton, Win32 headers, ABI cross-check, CLI skeleton | done |
| 1 | PE loader and `info` | done |
| 2 | Win32 runtime (TEB, SEH, threads, stub DLLs) | in progress: TEB, built-in DLLs, kernel32 and ntdll done |
| 3 | VfW layer and `probe` | done: driver lifecycle, ICM API, winmm/msvfw32 built-ins, `probe`, `list` |
| 4 | AVI / Y4M I/O, `decode` and `encode` | in progress: AVI reader and writer, `avi`, `convert` and the output writers done |
| 5 | Hardening and CI | not started |

## Prerequisites

The codecs are i386 code, so the harness is built as a 32-bit Linux program.
On a 64-bit host the kernel must support running 32-bit programs, which most
distributions enable by default.

- GCC with 32-bit support (`gcc-multilib` on Debian and Ubuntu)
- GNU make
- Optional: the mingw-w64 i686 cross compiler (`mingw-w64`). It builds the
  test fixture DLLs and runs the ABI cross-check; without it those tests are
  skipped.

```sh
sudo apt install build-essential gcc-multilib mingw-w64
```

## Building and testing

```sh
make          # builds build/vfw-harness
make test     # unit tests, ABI cross-check, CLI smoke test
make asan     # the unit tests again under AddressSanitizer and UBSan
make validate-media   # AVI and image code checked against FFmpeg
```

`make validate-media` needs `ffmpeg` and `ffprobe`. FFmpeg reads the AVI,
Y4M and BMP files the harness writes, and `vfw-harness convert` has to
reproduce FFmpeg's own decoding of AVIs FFmpeg wrote in each uncompressed
format. Setting `VFW_KEEP_TEST_FILES=<dir>` when running `test_avi` or
`test_image` keeps the files they write.

The loader tests use small DLLs from `tests/fixtures/`, built with
mingw-w64 into `build/fixtures/`. They are built without a C runtime, so
they import only from each other until the Win32 runtime exists (Phase 2).

CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs on every
push and pull request: the full `make test REQUIRE_MINGW=1`, `make asan`,
`make validate-media`, and a build with only `gcc-multilib` installed to
check that the fixture tests skip cleanly.

`make check-abi` compiles every struct size, field offset and constant in
`include/win32/` against the mingw-w64 Windows headers and fails if any of
them differ. Without mingw-w64 it is skipped; `make test REQUIRE_MINGW=1`
makes that an error.

## Usage

```sh
build/vfw-harness --help
build/vfw-harness info some-codec.dll
```

`info` parses a DLL without loading or running it. It prints the headers,
sections, exports (including ordinal-only exports and forwarders), imports
and delay imports, TLS callbacks, resources and version information.

```sh
build/vfw-harness avi clip.avi [--chunks]
build/vfw-harness convert clip.avi -o clip.y4m
build/vfw-harness convert clip.avi -o 'frame%05d.bmp' --start 100 --frames 10
build/vfw-harness convert clip.avi -o - --format y4m | ffplay -
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
Compressed streams need a codec, which will be the job of `decode`.

### Codecs: `probe` and `list`

```sh
build/vfw-harness probe codec.dll              # what the codec is and accepts
build/vfw-harness probe codec.dll clip.avi     # can it decode this file, and to what
build/vfw-harness probe XVID                   # a codec registered in vfw.ini
build/vfw-harness list
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
`~/.vfw-harness/vfw.ini`). Relative paths are taken from the file's
directory, and Windows paths go through the drive mapping (`C:` is
`$VFW_ROOT/drive_c`, `Z:` is `/`). `list` shows the entries and opens each
one to show its name.

Codecs usually import more than kernel32, winmm and msvfw32. user32, gdi32,
advapi32, version, ole32, oleaut32, shell32, comctl32, comdlg32 and the
msvcrt family exist as placeholder built-ins with no functions yet, and a
native DLL of the same name on the search path takes precedence over them.
A codec importing them still loads, `info` shows which of its imports are
not implemented, and calling one stops the harness with a message naming it.

`decode` and `encode` are listed but not implemented yet.

### Loader environment variables

| Variable | Effect |
|---|---|
| `VFW_PATH` | `:`-separated directories to search for DLLs, after the importing DLL's own directory |
| `VFW_FORCE_RELOC=1` | Load every image away from its preferred base, to exercise relocations |
| `VFW_ROOT` | The harness's Windows world: `drive_c` and `vfw.ini` (default `~/.vfw-harness`) |
| `VFW_INI` | The codec registry file (default `$VFW_ROOT/vfw.ini`) |
| `VFW_TRACE=1` | Log calls into the built-in DLLs and every driver message to stderr |

## Layout

| Path | Contents |
|---|---|
| `include/win32/` | Win32 types, PE image structures, GDI bitmap structs, driver and ICM definitions, laid out to match 32-bit Windows |
| `src/loader/` | PE parsing, mapping and relocation (`pe.c`); the module table and dependency loading (`modules.c`) |
| `src/runtime/` | TEB and `%fs`, the built-in DLL registry, trap thunks for unimplemented imports |
| `src/win32/` | Built-in kernel32, ntdll, winmm and msvfw32; placeholders for the other system DLLs |
| `src/vfw/` | Installable drivers (`driver.c`), the codec registry (`registry.c`), the ICM API (`icm.c`) |
| `src/avi/` | AVI reader (AVI 1.0 and OpenDML) and writer |
| `src/out/` | DIB to BGR24 / I420 conversion; Y4M, BMP and raw writers |
| `src/common/` | Error reporting, file, UTF-16 and FOURCC helpers |
| `src/` | CLI entry point and commands (`main.c`, `cmd_*.c`) |
| `tests/unit/` | Unit tests (`test_*.c`, each a standalone program) |
| `tests/fixtures/` | Sources of the test DLLs, including `testcodec.c`, a lossless VfW codec (`TST0`) |
| `tests/abi/` | ABI cross-check against the mingw-w64 headers |
| `tests/validate_media.sh` | Cross-check of the AVI and image code against FFmpeg |
| `docs/` | Implementation plan |

## License

GPL v2; see [LICENSE](LICENSE).
