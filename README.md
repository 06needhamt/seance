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
| 3 | VfW layer and `probe` | not started |
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

`list`, `probe`, `decode` and `encode` are listed but not implemented yet.

### Loader environment variables

| Variable | Effect |
|---|---|
| `VFW_PATH` | `:`-separated directories to search for DLLs, after the importing DLL's own directory |
| `VFW_FORCE_RELOC=1` | Load every image away from its preferred base, to exercise relocations |

## Layout

| Path | Contents |
|---|---|
| `include/win32/` | Win32 types, PE image structures, GDI bitmap structs, driver and ICM definitions, laid out to match 32-bit Windows |
| `src/loader/` | PE parsing, mapping and relocation (`pe.c`); the module table and dependency loading (`modules.c`) |
| `src/runtime/` | TEB and `%fs`, the built-in DLL registry, trap thunks for unimplemented imports |
| `src/win32/` | Built-in kernel32 and ntdll |
| `src/avi/` | AVI reader (AVI 1.0 and OpenDML) and writer |
| `src/out/` | DIB to BGR24 / I420 conversion; Y4M, BMP and raw writers |
| `src/common/` | Error reporting, file, UTF-16 and FOURCC helpers |
| `src/` | CLI entry point and commands (`main.c`, `cmd_*.c`) |
| `tests/unit/` | Unit tests (`test_*.c`, each a standalone program) |
| `tests/fixtures/` | Sources of the test DLLs |
| `tests/abi/` | ABI cross-check against the mingw-w64 headers |
| `tests/validate_media.sh` | Cross-check of the AVI and image code against FFmpeg |
| `docs/` | Implementation plan |

## License

GPL v2; see [LICENSE](LICENSE).
