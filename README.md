# vfw-harness

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
| 2 | Win32 runtime (TEB, SEH, threads, stub DLLs) | not started |
| 3 | VfW layer and `probe` | not started |
| 4 | AVI / Y4M I/O, `decode` and `encode` | not started |
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
```

The loader tests use small DLLs from `tests/fixtures/`, built with
mingw-w64 into `build/fixtures/`. They are built without a C runtime, so
they import only from each other until the Win32 runtime exists (Phase 2).

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
| `src/common/` | Error reporting, file and UTF-16 helpers |
| `src/` | CLI entry point and commands (`main.c`, `cmd_*.c`) |
| `tests/unit/` | Unit tests (`test_*.c`, each a standalone program) |
| `tests/fixtures/` | Sources of the test DLLs |
| `tests/abi/` | ABI cross-check against the mingw-w64 headers |
| `docs/` | Implementation plan |

## License

GPL v2; see [LICENSE](LICENSE).
