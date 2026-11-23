# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

seance loads 32-bit (and, via `seance64`, 64-bit) Windows codec DLLs — VfW/ICM, ACM, DirectShow filters, DMOs, RealMedia codecs — natively on Linux, without Wine. It maps PE images itself, supplies its own Win32 runtime (built-in kernel32, ntdll, msvcrt, user32, gdi32, ole32, …), and drives codecs through reimplemented host APIs (msvfw32 ICM, msacm32, quartz). It is codec-generic: nothing may depend on a particular codec.

## Build and test

`seance` must be an **i386** program; `seance64` (x86-64) is built beside it by default. Requires a compiler that links 32-bit programs (`gcc-multilib` on Debian/Ubuntu), CMake ≥ 3.20 (3.25 for presets), Ninja or make. mingw-w64 (fixture DLLs, ABI check) and FFmpeg (media cross-checks) are optional; tests needing them are skipped without them.

```sh
cmake --preset default            # configure build/
cmake --build --preset default    # build/seance and build/seance64
ctest --preset default            # all tests
ctest --preset default -R test_avi        # one test (names: ctest --test-dir build -N)
ctest --preset default -L unit            # by label: unit, abi, cli, media, samples
ctest --preset default -LE media          # skip the FFmpeg checks
cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # ASan+UBSan in build-asan/
```

- `-DVFW_BUILD_X86_64=OFF` builds only the i386 `seance` (faster).
- The compiler choice for the i386 build is cached; delete the build directory to re-pick (`cmake/i386.cmake`).
- The `ci` preset turns on `VFW_REQUIRE_MINGW`, `VFW_REQUIRE_MINGW64`, `VFW_REQUIRE_FFMPEG` (fail instead of skip).
- Code is C11 with `-Wall -Wextra -Wno-unused-parameter`. Files carry `SPDX-License-Identifier: GPL-2.0-only`.

## Architecture

The pipeline for running a codec is: PE loader → runtime → built-in DLLs → host API layer → command.

- `src/loader/` parses, maps and relocates PE images and resolves imports (including delay-loads) against the module table. Unresolvable imports bind to traps (`src/runtime/`) that report the missing function only if the codec actually calls it.
- `src/runtime/` sets up the TEB/`%fs`, TLS, SEH, the built-in DLL registry, the "DLL memory below 2 GB" allocator (`lowmem.c`), crash/hang reports (`crash.c`), and 3DNow! emulation (`amd3dnow.c`).
- `src/win32/` holds the built-in DLL implementations, one area per DLL (`k32_*.c`, `crt_*.c`, `u32_*.c`, `gdi_*.c`, `oleaut_*.c`, …). Their struct layouts live in `include/win32/` and must match 32-bit Windows; `tests/abi/` (`check_abi`) compiles every size/offset/constant against mingw-w64 headers.
- Host API layers: `src/vfw/` (installable drivers, codec registry `vfw.ini`, ICM, sequence compression), `src/acm/`, `src/dshow/`, `src/dmo/`, `src/real/`.
- Media I/O is codec-free: `src/avi/` (AVI 1.0 + OpenDML reader/writer), `src/in/` (Y4M/BMP/raw/AVI frame sources for `encode`), `src/out/` (DIB ↔ BGR24/I420, Y4M/BMP/raw writers), `src/media/`.
- CLI: `src/main.c` dispatches to `cmd_*.c`; shared helpers in `cmd_util.c`. A codec of the other architecture is handed off to the other binary (`handoff.c`).
- Tests: `tests/unit/test_*.c` are standalone programs; `tests/fixtures/` builds small CRT-free test DLLs (e.g. the lossless `TST0` VfW codec) with mingw as an external CMake project into `build/fixtures/`; `tests/smoke.sh`, `tests/validate_media.sh` (FFmpeg cross-check), `tests/samples.sh`.

Further design notes: `docs/IMPLEMENTATION_PLAN.md`, `docs/X64_PLAN.md`, `docs/DIRECT_SHOW_PLAN.md`, `docs/ACM_PLAN.md`, `docs/DMO_AUDIO_PLAN.md`; known gaps per codec in `MISSING_FEATURES.md`.

## Useful debugging switches

`VFW_TRACE=1` logs calls into built-in DLLs and every driver message; `VFW_TIMEOUT=<s>` stops hung calls; `VFW_CORE=1` dumps core on crash/trap; `VFW_ROOT` (default `~/.seance`) holds the fake `drive_c`, `vfw.ini` and `registry.reg`. The full list is in the README.

## Driving a codec as a black box

seance is also used as an oracle for clean-room work (see the sibling `libblox` repo): exercising a DLL only through `probe`, `encode` and `decode`, never disassembling it. Things that matter when scripting it:

- `seance probe codec.dll` lists accepted input/output formats, flags and defaults.
- Raw frame input to `encode` needs a video extension (`.yuv`), plus `--input-format YUY2 --size WxH`; a `.raw` input is taken as audio PCM.
- `decode … -o out.raw --format raw --output-format YUY2` gives the codec's native frames; BGR24 output frames are bottom-up DIBs.
- `encode … --keyint N --quality Q`; many frames can be batched into one AVI per run, which is far faster than one seance invocation per frame.

## Scope rule for this checkout

When working on codec reverse-engineering tasks from this workspace, change seance only for genuine bug fixes or codec-generic instrumentation, and develop on the session's assigned branch.
