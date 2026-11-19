# Type library support

seance's oleaut32 has no type libraries: `LoadTypeLib`, `LoadTypeLibEx`
and `LoadRegTypeLib` always fail, and `RegisterTypeLib` has nothing to
register. This plan adds them.

## Why

The codec zip's sweep shows what that costs:

- `abcAVI.dll` and `abcAVIIT.dll` (Delphi): `LoadTypeLib` on the DLL's own
  path fails, Delphi raises its 0x0EEDFADE exception in DllMain, and its
  finalization faults afterwards.
- The LEAD encoders (`LEncAAC.dll` and 14 more, i386 and x64): ATL's
  registration loads the DLL's own type library, fails ("Failed to
  register type library"), and the encoder object never opens
  (0x800401f0).

233 files in the zip carry a type library. All but four are in the
current format (`MSFT`); `ILog`, `OLEAUT32`, `OLEPRO32` and `ir41_32.ax`
are in the older `SLTG` one. About 60 DirectShow filters import
`LoadTypeLib` through the base classes, but only use it for `IDispatch`
on their property interfaces. What needs real type libraries:

- ATL's and Delphi's registration: load the module's own library and
  `RegisterTypeLib(ForUser)` it (LEAD, `ltmm175`, `DSKernel2`,
  `eescreen`);
- Delphi's COM objects, which look up their class's type information at
  start-up and answer `IDispatch` from it (abcAVI).

## Phase A: reading type libraries (done)

Status: `src/win32/oleaut_typelib.c`, `seance info --typelib` and
`test_typelib`. Of the zip's 225 files with a type library, 221 load with
every reference resolved, on both architectures and under ASan. The
others are the three `SLTG` libraries (`ILog`, `OLEPRO32`, `ir41_32.ax`)
and `QTOControl.dll`, which imports `QTOLibrary.tlb`, not in the zip.
Changes from the plan below:

- The built-in stdole2 has `GUID`, `IUnknown`, `IDispatch`,
  `IEnumVARIANT`, `OLE_COLOR`, `OLE_HANDLE`, and `Font` and `Picture` with
  their aliases `IFontDisp` and `IPictureDisp`: what the zip's libraries
  refer to. References by index into stdole2 are known for `GUID` (0),
  `IFontDisp` (32, ATL's) and `IPictureDisp` (36, LEAD's).
- `test_typelib` writes its library itself (an `MSFT` writer in C, for
  Win32 and Win64 layouts) rather than through `gen_tlb.py`, and runs the
  fuzz pass in the same program: every truncation and 1,500 mutated
  copies must fail cleanly or load into something every call can walk.
  The PE fixture with a `TYPELIB` resource comes with Phase C, which
  needs a DLL anyway.

New `src/win32/oleaut_typelib.c`, with declarations in `oleaut.h`.

1. Headers (`include/win32/oleauto.h`): `ITypeLib`, `ITypeLib2`,
   `ITypeInfo`, `ITypeInfo2`, `ITypeComp`; `TYPEATTR`, `FUNCDESC`,
   `VARDESC`, `ELEMDESC`, `TYPEDESC`, `ARRAYDESC`, `IDLDESC`,
   `PARAMDESC(EX)`, `TLIBATTR`, `BINDPTR` and their enums and flags. All
   checked against mingw-w64 in `check_abi`, on both architectures.
2. An `MSFT` parser, written from the format's documented layout (not
   copied from Wine): header, type info blocks, the GUID, name and string
   tables, function and variable records with their parameters, custom
   data, the array descriptor table and the import tables. Every offset
   and count is bounds-checked; a bad file fails with `TYPE_E_*`. It fills
   one internal model, which everything else reads.
3. Sources: a `.tlb` file; a PE file's `TYPELIB` resource 1, or N given
   as `path\N`; a module already loaded, found by path. Paths go through
   the drive mapping. `LoadTypeLibEx`'s `REGKIND_REGISTER` registers the
   library (Phase B); `REGKIND_NONE` does not.
4. `ITypeLib`/`ITypeLib2` and `ITypeInfo`/`ITypeInfo2` over the model:
   everything reading needs (attributes, descriptions, names,
   implemented interfaces and their flags, `GetRefTypeInfo`,
   `GetIDsOfNames`, documentation, custom data). What is not implemented
   returns `E_NOTIMPL` and traces (`CreateInstance`, `GetTypeComp` to
   start with). Libraries are cached by path and reference-counted.
5. A built-in stdole2: nearly every library imports it, and a dual
   interface derives from its `IDispatch`. seance defines its library and
   types (`IUnknown`, `IDispatch`, `IEnumVARIANT`, `IFontDisp`,
   `IPictureDisp`, `GUID`, `OLE_COLOR`, ...) in C, rather than carrying
   Microsoft's `stdole2.tlb`. References into another library resolve to
   that, or to a registered library through `LoadRegTypeLib`.
6. `seance info --typelib FILE` lists a library's types, with their
   GUIDs, kinds, functions, parameters and flags.

## Phase B: registration

1. `RegisterTypeLib(ForUser)` writes what Windows does:
   `HKCR\TypeLib\{lib}\{maj.min}` (name), `\0\win32` or `\win64`
   (path), `FLAGS`, `HELPDIR`; and for each dual or `oleautomation`
   interface `HKCR\Interface\{iid}` with its name, `ProxyStubClsid32`
   (`{00020424-0000-0000-C000-000000000046}`) and `TypeLib`.
2. `UnRegisterTypeLib(ForUser)` removes them; an interface key only if
   it still names this library.
3. `LoadRegTypeLib` and `QueryPathOfRegTypeLib` find a library by GUID
   and version (the highest minor version), and load it from its path.

## Phase C: calling through a type library

1. `DispCallFunc`: a vtable slot or a function, called with a
   `VARIANTARG` list. stdcall and cdecl on i386, ms_abi (four register
   arguments, floats in XMM registers) on x86-64, through seance's guest
   call helpers. `HRESULT`, integer, floating-point and `VARIANT` results.
2. `ITypeInfo::Invoke`: `DISPPARAMS` to a call, with named arguments,
   property get, put and putref, `[out]` and `[retval]` parameters,
   optional parameters and defaults, conversions by `VariantChangeType`,
   and `DISP_E_*` with the argument's position.
3. `DispGetIDsOfNames`, `DispInvoke`, `CreateStdDispatch` and
   `CreateDispTypeInfo` over those.

## Phase D: tests

- There is no `midl` or `widl` here, so `tests/fixtures/gen_tlb.py`
  writes an `MSFT` library: a dual interface with methods, properties,
  `[out, retval]` and optional parameters; a plain interface; an enum, a
  record and a coclass; and an import of stdole2. `windres` puts it in
  `pe_typelib.dll` as `TYPELIB` 1. The DLL implements the coclass both
  with `CreateStdDispatch` and by calling `ITypeInfo::Invoke` itself, as
  Delphi's `TAutoObject` does.
- `test_typelib` (i386 and x64): loading by path, `path\1` and loaded
  module; every `TYPEATTR`, `FUNCDESC` and `VARDESC` field, names and
  documentation; a dual interface's base resolving to stdole2's
  `IDispatch`; `GetIDsOfNames`, and `Invoke` with each kind of parameter
  and its errors; registering, `LoadRegTypeLib`, `QueryPathOfRegTypeLib`,
  unregistering, and the registry in between.
- A fuzz target for the parser: a malformed library fails with `TYPE_E_*`
  and never crashes.
- During development, not committed: every type in the zip's 229 `MSFT`
  libraries loads, and every name and reference resolves. That checks the
  parser against libraries `gen_tlb.py` did not write.

## Phase E: the codecs

Re-run `abcAVI.dll`, `abcAVIIT.dll`, the 15 LEAD encoders (both
architectures), `ltmm175`, `DSKernel2`, `eescreen` and the LEAD decoders
that register their library, under `VFW_TRACE`. Fix what follows, with a
test each. Where LEAD stops at its unlock check, document it and stop.
Re-sweep the affected DLLs with `--compare`, then update
`MISSING_FEATURES.md` and the catalogue.

## Not included

- `SLTG` libraries: four files in the zip, and the one codec among them
  (`ir41_32.ax`) uses its library only for `IDispatch`.
- Creating type libraries (`CreateTypeLib2`, `ICreateTypeLib2`): no codec
  builds one.
- `ITypeComp` binding and `ITypeInfo::CreateInstance`, until a codec
  needs them.
- Marshalling: seance runs everything in one process, so the proxy entry
  is written but never used.

## Order

A, then B and C, then D's codec checks and E; two or three PRs. A is the
largest (about 1,500 lines and the headers). The main risk is a parser
and a generator that agree with each other and not with Windows; the
zip's own libraries are the check on that.
