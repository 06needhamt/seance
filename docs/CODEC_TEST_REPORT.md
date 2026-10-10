<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Codec test report: `ffmpeg-unimplemented-codecs-2026-10-03`

Every file in the archive was run through `seance` at commit `cee2fc1`
(32-bit build, October 2026), over every encode and decode path each
codec offers. This report records what works and what doesn't. For each
failure it says what would have to be added or changed to make it work,
and whether the fault is in seance or in the codec.

## How it was tested

- **Survey:** `tests/samples.sh` (`info` and `probe`) over all 58 files,
  then `probe` on each one.
- **VfW encoders:** each input format `probe` accepted was encoded
  (`encode --codec-input <fmt>`) from a 10-frame 320x240 `testsrc2` clip.
  Each encoded file was then decoded to each output format the codec
  accepted for it (`decode --output-format <fmt>`), and the result was
  compared with the source by PSNR. Lossless codecs come out at 48–50 dB
  rather than infinite, because the test source is 4:2:0 and goes through
  a YUV↔RGB conversion.
- **Decoders:** real files from samples.ffmpeg.org (ADV1, ASLC, LCW2,
  MWV1, PSIV, SCLS, SMSV, SN40, SP54, SP61, VDOM, VSSV, VTLP, WINX, WNV1,
  LSVX, LSVM, MVLZ, MFZ0, FRWD, FRWT, TY0N), decoded to each accepted
  output format. The pictures were then checked by eye.
- **ACM drivers:** test tones at 8–44.1 kHz, mono and stereo, encoded
  with each format tag and decoded back. Where samples existed (0x0070,
  0x0072, 0x0120, 0x1fc4, 0x181c), those were decoded as well.
- **DirectShow:** encode and decode through the built-in graph, with
  every output format.
- **Confirming root causes:** the suspected cause of each failure was
  checked with a scratch build carrying the proposed change. "Verified"
  below means the codec worked once the change was made. None of those
  changes are part of this commit.

## Summary

| File | Kind | Result |
|---|---|---|
| frwd.dll, frwt.dll (FRWD/FRWT 5.0.1.1) | VfW | **all paths work**: 4 inputs × 4 outputs, 43–49 dB; samples decode |
| icmw_32.dll (MWV1 1.65) | VfW | **all paths work**: 7 inputs × 2 outputs, 36–41 dB; sample decodes |
| MLZCodec.dll (MVLZ) | VfW | **all paths work**: 3 × 1, lossless; sample decodes |
| MyFlashZip0.ax (MFZ0) | VfW (despite the name) | **all paths work**: 2 × 1, lossless; sample decodes |
| SCLS.DLL (SCLS) | VfW | **all paths work**: 3 inputs, 5 paths, lossless; sample decodes |
| wtvc.DLL (WTVC 1.12) | VfW | **all paths work**: 3 × 1, lossless (no public sample) |
| nsvideo.dll (NSVI) | VfW | **all paths work**: 1 × 1, 45 dB |
| pvwv220.dll (PVW2 2.10) | VfW | **works**: 8 inputs × 8 outputs, 35.8–39.5 dB; PAL8 input untested (**H9**) |
| Sif1_vfw.dll (SIF1) | VfW | **all paths work**: 4 × 5, 36.5–37.1 dB |
| wavelet.dll (ADV1 1.20) | VfW | **all paths work**: 1 × 1, 40.8 dB; ADV1 sample decodes |
| Amv2Codec.dll (AMV2) | VfW | **works**: 3 inputs × BGR32; 30–32 dB because this build stamps an "AMV2 VideoCodec" logo |
| lsvxdec.dll, psiv.dll, vdowave.drv | VfW decoders | **work** on their samples, every output format |
| SN4Codec.dll (SN40) | VfW | decoding **works** (I420/YV12/BGR24); encoding broken, see below |
| WavLor.dll (Verint ADV1) | VfW decoder | **works** on the ADV1 sample; can't read ADV601's own output (a different ADV1 variant) |
| TRICDC32.DRV (Trident ty0n) | VfW decoder | **works** on the ty0n sample (BGR24, RGB555); `probe` without a file can't open it (**H6**) |
| wnvwinx.dll (WINX 2.9) | VfW | **works only with `--fourcc WINX`** (**H6**); 3 × 7 paths |
| ADV601.dll (ADV1 1.11) | VfW | encoding **crashes**; fixed by **H1** (verified, 42.6 dB) |
| wv32vfw.dll (SMSV 2.00b6) | VfW | **black output** whenever an output format is named; fixed by **H3** (verified) |
| vsslight.dll (VSSV 1.3.1) | VfW | encoding **refused**; fixed by **H4** (verified, 35–41 dB). UYVY is broken in the codec |
| sp6x_32.dll (SP61) | VfW decoder | **heap corruption** after decoding; fixed by **H2** (verified) |
| aslcodec_vfw.dll (ASLC 1.6) | VfW | 5 of 7 inputs work (trial logo in a corner); UYVY swapped by the codec; PAL8 untested (**H9**) |
| aslcodec_dshow.dll | DirectShow | encoder **not found** by seance (**H7**) |
| LCODCCMW2E.dll (LEAD MCMW 2.0 eval) | DirectShow | encode and BGR24 decode work; PAL8 decode wrong (**H8**) |
| Sif1Dec.ax | DirectShow | YV12/YUY2/UYVY/BGR32 work; BGR24 misaligned (**H8**) |
| WavCWAIP.dll (wavc 1.20) | VfW | BGR24 works with `VFW_DATE` before 2008 (17.8 dB, ignores quality); I420 crashes in the codec (**H11**) |
| xfcodec.dll (XFR1) | VfW | only BGR32 input is right; the other inputs and YV12 output are broken in the codec |
| vgpix32d.dll (VGPX 3.10) | VfW decoder | decode-only; no VGPX sample exists (the VTLP sample isn't VGPX); `probe` misreports it as an encoder (**H5**) |
| lhacm.acm, lhacm2.acm (L&H 0x1101–4, 0x70–73) | ACM | **work**: encode and decode; the 0x70/0x72 samples decode to clear speech; names missing (**H12**) |
| smcelp32.acm (Philips CELP 0x0120) | ACM | **works** at 8 and 16 kHz; sample decodes; name missing (**H12**) |
| alf2cd.acm (0x1fc4) | ACM | **works** at 44.1 kHz stereo (round trip correlation 1.00); name missing (**H12**) |
| nsrt2432.acm (Voxware RT24 0x181c) | ACM | **needs `rt32cmp.dll`**, which isn't in the archive |
| vdowave.acm (VDOWave 0xfffc/0xffff) | ACM | **needs `RATECFG.DLL`**, which isn't in the archive |
| MultiCodec.dll, NuB2.dll, tm2x.dll, decvdo.dll, vdo32_30.drv, vsswlt.dll | VfW | **don't load**: companion DLLs missing (see below) |
| camfc.dll (XJPG), scrvid.dll (ZDSV), nuvision.ax (NTN1), aoxdxipl.ax, sp4x_32.dll (SP41–47) | VfW decoders | open and answer queries; **no sample exists** to decode |
| DECVW_32.DLL, LCMW2.dll, MultiDecoder.dll, VDODEC32.dll, rt32dcmp.dll, zlib1.dll, mvoiced.vwp | libraries | not codecs: helper DLLs that other codecs load |
| 3ivx Delta 3.5.qtx, ACTLComponent.qtx, BeHereiVideo.qtx | QuickTime | out of scope (QuickTime components) |
| vid_3ivX.xa | XAnim | out of scope (a Linux ELF plug-in for XAnim) |

## What seance needs (by priority)

### H1. Give codecs a status callback before compressing (verified)

**ADV601.dll** crashes with `SIGSEGV at (nil)` on the first
`ICM_COMPRESS`. At `+0x2004` it calls `*(inst+0x28)(inst+0x2c, ICSTATUS_STATUS, 0)`.
That field is the `Status` callback from `ICM_SET_STATUS_PROC`, which it
calls without checking for NULL, and seance never sends that message.

**Change:** in `ic_seq_start_ex` (`src/vfw/seq.c`), before
`ICM_COMPRESS_BEGIN`, send `ICM_SET_STATUS_PROC` with an `ICSETSTATUSPROC`
whose `Status` is a `CALLBACK` that returns 0. Do the same on the
`ICImageCompress` path. With that change ADV601 encodes and round-trips
at 42.6 dB.

### H2. Pad the compressed input buffers (verified)

**sp6x_32.dll** decodes all 148 frames of its sample, but then glibc
aborts with `free(): invalid next size`: the codec writes past the end of
its compressed input buffer. `cmd_decode.c` allocates that buffer at
exactly the chunk size. **vgpix32d.dll**'s bit reader also reads a word
past the end of its input.

**Change:** allocate decode input buffers with zeroed padding after the
data, as FFmpeg does (`AV_INPUT_BUFFER_PADDING_SIZE`, 64 bytes), in
`cmd_decode.c`'s chunk buffer. Do the same for `ICImageDecompress`,
DrawDib, and the DirectShow and DMO sample buffers. With 64 bytes, sp6x
runs clean and its output is unchanged.

### H3. Send `ICM_DECOMPRESS_GET_FORMAT` even when the output is forced (verified)

**wv32vfw.dll** returns `ICERR_OK` from `ICM_DECOMPRESS` but writes
nothing (black frames, 0.002 ms/frame) unless `ICM_DECOMPRESS_GET_FORMAT`
came before `ICM_DECOMPRESS_BEGIN`. `decode` only sends
`ICM_DECOMPRESS_GET_FORMAT` when it is choosing the output itself, so
every `--output-format` run, the encode matrix included, was black.

**Change:** in `cmd_decode.c`, always send `ICM_DECOMPRESS_GET_FORMAT`
(into a scratch buffer) before `ICM_DECOMPRESS_BEGIN`. Applications such
as VirtualDub do the same. With that change both encode paths
round-trip (26 dB, the codec's default ratio) and the SMSV sample
decodes.

### H4. Don't require `ICM_COMPRESS_QUERY(in, out)` to accept the codec's own format (verified)

**vsslight.dll** accepts `ICM_COMPRESS_QUERY(in, NULL)`, then rejects
`ICM_COMPRESS_QUERY(in, out)` with `ICERR_BADFORMAT` when `out` is the
format its own `ICM_COMPRESS_GET_FORMAT` just returned. `ic_seq_start_ex`
treats that as fatal. If it is ignored, `ICM_COMPRESS_BEGIN` and every
frame succeed: all 7 inputs encode. Apart from UYVY input (12 dB, a codec
bug), the results decode at 35–41 dB to every output except UYVY, which
crashes in the codec (H10).

**Change:** when the full query fails on a format the codec suggested
itself, warn and carry on to `ICM_COMPRESS_BEGIN`, and fail only if
`ICM_COMPRESS_BEGIN` fails.

### H5. Make `probe`'s compression section match what `encode` does

`probe` opens in `ICMODE_QUERY` and doesn't send the `(in, out)` query,
so it reports encoders that `encode` can't use:

- **vgpix32d.dll** refuses `DRV_OPEN` for `ICMODE_COMPRESS` and
  `ICMODE_FASTCOMPRESS` (its open handler accepts only modes 2–4 and 8),
  yet `probe` lists "BGR24 input: accepted".
- **vsslight.dll** fails at H4.
- **SN4Codec.dll** compresses to an output format of `BI_RGB` 24 bits,
  which isn't compression at all.

**Change:** open the codec in `ICMODE_COMPRESS` for the compression
section, and say so if it refuses. Also run the `(in, out)` query and
report the result, and flag an output format that isn't compressed.

### H6. Find a codec's FOURCC when it is given only as a DLL

- **wnvwinx.dll** accepts `DRV_OPEN` with any handler. It answers
  `ICM_GETINFO` only when opened as `WINX`. Opened any other way it
  returns `ICERR_OK` to every message and does nothing: `encode` wrote
  6 MB of junk, and decoding gave 0x0 formats. With `--fourcc WINX`,
  every path works. Outputs are 32–36 dB, and the picture is flipped
  when converting between packed YUV and RGB, which is the codec's doing.
- **TRICDC32.DRV** opens only for the lowercase handlers in a table
  inside its data section (`ty0n`, `ty2n`, `ty2c`, `rtc3`, `rtc5`,
  `rtc6`, `r0r3` …). It works on a file, which supplies the handler, but
  `probe` and `samples.sh` report "refused DRV_OPEN".

**Change:** when `ICM_GETINFO` gets no answer, or `DRV_OPEN` is refused,
for a DLL opened without a handler, try the handlers `vfw.ini` maps to
that path. Failing that, try 4-byte immediates the DLL compares
`ICOPEN.fccHandler` against (both cases). At the least, `probe` should
say "the codec answers nothing without its FOURCC" and suggest
`--fourcc`.

### H7. Accept DirectShow encoders that register no output subtype

**aslcodec_dshow.dll** registers its output pin as `MEDIATYPE_Video` with
subtype `GUID_NULL`. `xform_find_output` takes the FOURCC from the
registered subtype, finds none, and skips the filter, so `encode` says
"no unique DirectShow or DMO video encoder", with `--clsid` as well.
The same CLSID is also registered in the legacy category, where its role
is "other".

**Change:** if the registration names no FOURCC, connect the filter and
take the FOURCC from the output pin's connected media type
(`biCompression` or the subtype). Treat one CLSID in several categories
as one filter even when the roles worked out for each entry differ.

### H8. Use the media type the DirectShow decoder actually connected with

- **LCODCCMW2E.dll**, PAL8 output: the frames come out as black-and-white
  dither (7.7 dB). For filters and DMOs, `decode` converts output with
  the grey palette of the standard PAL8 format it offered
  (`cmd_decode.c`, the `memcpy(out, &fmt, …)` after `xform_connect`). It
  ignores the palette in the media type the filter connected with or put
  on its samples.
- **Sif1Dec.ax**, BGR24 output: each sample is 307,200 bytes (the size of
  a 32-bit frame) and the picture is one row off, so edges are doubled
  (27 dB against 37 dB for its own BGR32 output). The VfW build of the
  same codec gives identical BGR24 and BGR32. Not root-caused: check the
  connected media type, the allocator's buffer size after the output
  type changes, and the sample's `lActualDataLength` and media type.

**Change:** after connecting, read the output pin's connection media type
(and any media type a sample carries). Build the converter from that:
palette, `biHeight` sign and stride. Size the allocator from it as well.

### H9. Accept PAL8 as encode input

pvwv220, wnvwinx, xfcodec and aslcodec_vfw all advertise PAL8 input, but
`--codec-input PAL8` gives "unknown codec input format", so that path
couldn't be tested on any of them.

**Change:** add PAL8 to the encoder's input formats, with a fixed
palette (the standard grey ramp, or a 6×6×6 cube) and a quantiser from
the input picture.

### H10. Catch output buffer overruns before they corrupt the heap

- **SN4Codec.dll**'s compressor reports at most 230,400 bytes, then
  writes 313,852. seance notices after the fact, but by then glibc's
  heap is corrupt (`double free or corruption`).
- **vsslight.dll**, UYVY output: its first write lands 4 rows past the
  end of the 153,600-byte output buffer (`SIGSEGV` at `+0x4a27b`).

**Change:** put compress and decompress output buffers in their own
mapping with an inaccessible guard page after them, sized up to whole
pages. An overrun then becomes a clean error naming the codec and the
message.

### H11. Finish the file when a codec crashes in `*_END`

**WavCWAIP.dll**, I420 input: all 10 frames compress, then
`ICM_COMPRESS_END` raises `SIGFPE` at `+0x9a3f`. The codec divides by
its count of compressed bytes to print a "Compressed ratio" line, and
its I420 path never adds to that count. That is a codec bug. seance
could still finish the AVI if `ICM_COMPRESS_END` or
`ICM_DECOMPRESS_END` crashes after every frame was processed, and print
a warning.

### H12. `GetDriverModuleHandle` returns NULL during `DRV_LOAD` and `DRV_OPEN` (cosmetic, root-caused)

alf2cd, lhacm, lhacm2, nsrt2432 and smcelp32 call
`GetDriverModuleHandle(hdrvr)` while loading and later `LoadStringA` from
that handle. `drv_module()` uses `slot_of(h, 0)`, which skips instances
that are still opening, so the drivers get NULL. Their names and
format-tag names then come out empty, which `samples.sh` shows as `'?'`.

**Change:** use `slot_of(h, 1)` in `drv_module()`. Separately, `ICM_GETINFO`
strings that a codec writes as ANSI into the WCHAR fields (WavLor.dll,
TRICDC32.DRV) are shown as CJK mojibake. `probe` could spot that and
decode them as ANSI.

## Problems in the codecs themselves

seance can't fix these, but they explain results that would otherwise
look like harness bugs. In each case, other codecs pass the same harness
paths.

- **aslcodec_vfw.dll:** the UYVY round trip swaps U and V (byte-exact
  check: `dd4aa64a…` in, `a64add4a…` out). This trial build also draws a
  logo in the bottom-right corner, which is why the lossless paths score
  28–29 dB.
- **xfcodec.dll:** I420 and YV12 input fail `ICM_COMPRESS_BEGIN`
  (`ICERR_UNSUPPORTED`) even though its query accepts them. BGR24 input
  comes out striped, RGB565 input loses colour (grey turns navy), and
  YUY2/UYVY input is garbled and upside down. YV12 output always has U
  and V swapped. Only BGR32 input → YUY2/UYVY/BGR32/BGR24 output is
  right (47–49 dB).
- **vsslight.dll:** UYVY input comes back at 12 dB, and UYVY output
  overruns its buffer (H10).
- **SN4Codec.dll:** the encode side doesn't work as a VfW compressor. Its
  output format is `BI_RGB` with handler `0000`, and it overruns its
  buffer (H10). Decoding works.
- **WavCWAIP.dll:** an evaluation build that stops in 2008, so set
  `VFW_DATE=2007-06-01`. It ignores `--quality` (≈68 kbit/s, 17.8 dB),
  and its I420 path crashes in `ICM_COMPRESS_END` (H11).
- **ADV601.dll vs WavLor.dll vs wavelet.dll:** all three use the ADV1
  FOURCC but aren't compatible with each other. wavelet.dll crashes on
  ADV601's output, and ADV601 decodes wavelet.dll's output at 11.8 dB.
  WavLor only decodes the reference sample, and only at its own cropped
  size (348x235 for 360x243).
- **wnvwinx.dll:** accepts any FOURCC in `ICM_DECOMPRESS_QUERY`, the WNV1
  sample included, but decodes only WINX. Converting between packed YUV
  and RGB flips the picture.
- **vgpix32d.dll:** checks for `biCompression == 'VGPX'`. The only public
  sample (`toocool.vgm.avi`) is VTLP, and relabelling it as VGPX gives
  garbage, so VGPX decoding is still unverified.
- **Amv2Codec.dll, LCODCCMW2E.dll:** unregistered or evaluation builds
  that stamp a logo or banner on every frame. LEAD's BGR24 output scores
  34.9 dB if the 16-row banner is left out.

## Missing companion files (not seance's to provide)

| Codec | Needs | Notes |
|---|---|---|
| MultiCodec.dll (Insignis 6.2.4) | SImageLib.dll; MultiDecoder.dll (in the archive), which needs DeltaHF.dll, DeltaV4.dll, kdu_v60R.dll, kdu_a60R.dll | Kakadu and Insignis libraries |
| NuB2.dll | MFC42.DLL | retail VC6 MFC runtime |
| tm2x.dll (TrueMotion 2X 2.5.2.8) | MFC42.DLL, MSVCIRT.dll | VC6 runtimes |
| decvdo.dll, vdo32_30.drv (VDOWave 512k) | DecFrame.dll; vdo32_30 also YUV2RGB.dll, YUVNF2QC.dll | VDOnet |
| vsswlt.dll (VSS Wavelet 1.3) | vsswltconf.dll | VSS |
| nsrt2432.acm | rt32cmp.dll (ToolVox compressor) | its `DRV_OPEN` loads it; without it the stream open fails `MMSYSERR_NOTENABLED`, and no decoder is offered either, so the RT24 sample can't be decoded |
| vdowave.acm | RATECFG.DLL | loaded at open; without it every `ACMDM_FORMAT_SUGGEST` is `ACMERR_NOTPOSSIBLE` |

Each should be supplied from the same product installer (see the
README's "Codec dependency DLLs").

## Out of scope

- **QuickTime components** (`*.qtx`: 3ivx Delta 3.5, ACTL, BeHere
  iVideo): they export `…ComponentDispatch` entry points for QuickTime's
  Component Manager. Supporting them would mean a QuickTime host
  (Component Manager, ImageCodec selectors, the QTML data types), which
  is a separate project.
- **vid_3ivX.xa:** an i386 ELF shared object written to the XAnim plug-in
  API, not a PE DLL. It could only be run by a host that implements
  XAnim's interface.

## Not testable for lack of samples

camfc.dll (XJPG), scrvid.dll (ZDSV), nuvision.ax (NTN1, a VfW driver
despite the `.ax`), aoxdxipl.ax (VfW, reports `YVYU`) and sp4x_32.dll
(SP41–SP47) all load and answer the decompression queries. No sample of
their formats is on samples.ffmpeg.org and none of them encodes, so their
decode paths remain unverified. The same goes for VGPX (above) and for
real-world AMV2, NSVI, WTVC and XFR1 files, which were tested only
through their own encoders.

## Reproducing

The scripts used were small wrappers around these commands:

```sh
# survey
sh tests/samples.sh build/seance <codecs> 30
# an encoder over each input and output format
build/seance encode <codec> src.y4m -o out.avi --codec-input <in>
build/seance decode <codec> out.avi -o out.y4m --output-format <out>
ffmpeg -i out.y4m -i src.y4m -lavfi psnr -f null -
# ACM
build/seance encode <driver.acm> tone.wav -o enc.wav --format-tag <tag>
build/seance decode <driver.acm> enc.wav -o dec.wav --sample-format s16
```

Needed settings: `VFW_DATE=2007-06-01` for WavCWAIP.dll, `--fourcc WINX`
for wnvwinx.dll, and `--clsid` for servers with several classes.
