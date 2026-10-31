#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# A quick run of every CLI command: the usage paths, failures on files that
# aren't what a command wants, and, when the fixture DLLs were built, info
# and probe on them.
#
# With lowmem=1 it also checks, through testcodec's strict mode, that the
# codec never sees a pointer at or above 2 GB.
#
# usage: tests/smoke.sh <seance> <not a DLL or AVI> [<fixture dir> [lowmem]]
set -eu

usage="usage: $0 <seance> <file> [<fixture dir> [lowmem]]"
harness=${1:?$usage}
junk=${2:?$usage}
fixtures=${3:-}
lowmem=${4:-0}
# On a cross build, what runs i386 programs (qemu-i386), from CMake.
emu=${VFW_EMULATOR:-}

fails() {
	if "$@" >/dev/null 2>&1; then
		echo "smoke: '$*' should have failed" >&2
		exit 1
	fi
}

$emu "$harness" --version
$emu "$harness" --help >/dev/null
fails $emu "$harness" no-such-command
fails $emu "$harness" info "$junk"
fails $emu "$harness" probe "$junk"
VFW_INI=/nonexistent $emu "$harness" list >/dev/null
fails $emu "$harness" avi "$junk"
fails $emu "$harness" convert "$junk" -o /dev/null --format raw
fails $emu "$harness" decode
fails $emu "$harness" encode
fails $emu "$harness" decode TST0 "$junk" -o /dev/null --format raw
fails $emu "$harness" encode TST0 "$junk" -o /dev/null
fails $emu "$harness" register
fails $emu "$harness" register --dry-run "$junk"
fails $emu "$harness" unregister

# Audio without a codec: raw PCM to WAV, to AVI and back, exactly.
work=$(mktemp -d)
head -c 9600 /dev/urandom >"$work/in.pcm"
$emu "$harness" convert --raw s16 --rate 8000 --channels 2 "$work/in.pcm" \
	-o "$work/a.wav" 2>/dev/null
$emu "$harness" convert "$work/a.wav" -o "$work/a.avi" 2>/dev/null
$emu "$harness" avi "$work/a.avi" | grep -q "PCM s16, 2 channels, 8000 Hz"
$emu "$harness" avi "$work/a.avi" | grep -q "duration: 0.300 s (2400 blocks)"
$emu "$harness" convert "$work/a.avi" -o "$work/b.wav" 2>/dev/null
cmp "$work/a.wav" "$work/b.wav"
$emu "$harness" convert "$work/b.wav" -o "$work/b.pcm" 2>/dev/null
cmp "$work/in.pcm" "$work/b.pcm"
# Through pipes, and to the sample: 0.1 s from sample 100.
$emu "$harness" convert - -o - --format pcm <"$work/a.wav" 2>/dev/null |
	cmp - "$work/in.pcm"
$emu "$harness" convert "$work/a.wav" -o - 2>/dev/null |
	$emu "$harness" convert - -o "$work/c.pcm" 2>/dev/null
cmp "$work/in.pcm" "$work/c.pcm"
$emu "$harness" convert "$work/a.wav" -o "$work/d.pcm" --start 100 \
	--duration 0.1s 2>/dev/null
tail -c +401 "$work/in.pcm" | head -c 3200 | cmp - "$work/d.pcm"
# s16 to f32 and back loses nothing.
$emu "$harness" convert "$work/a.wav" -o "$work/f.wav" --sample-format f32 \
	2>/dev/null
$emu "$harness" convert "$work/f.wav" -o "$work/g.pcm" --sample-format s16 \
	2>/dev/null
cmp "$work/in.pcm" "$work/g.pcm"
fails $emu "$harness" convert --raw s16 "$work/in.pcm" -o "$work/x.wav"
fails $emu "$harness" convert --raw s17 --rate 1 --channels 1 "$work/in.pcm" \
	-o "$work/x.wav"
fails $emu "$harness" convert "$work/a.wav" -o "$work/x.wav" --frames 3
fails $emu "$harness" convert "$work/a.wav" -o "$work/x.wav" --start 1.5x
fails $emu "$harness" convert "$work/a.wav" -o "$work/x.mp3"
# The null device needs no extension.
$emu "$harness" convert "$work/a.wav" -o /dev/null 2>&1 | grep -q -- "-> raw"
fails $emu "$harness" convert "$work/a.avi" -o "$work/x.y4m"
rm -rf "$work"

if [ -n "$fixtures" ] && [ -f "$fixtures/testcodec.dll" ]; then
	$emu "$harness" info "$fixtures/pe_dep.dll" >/dev/null
	$emu "$harness" probe "$fixtures/testcodec.dll" | grep -q "handler 'TST0'"
	fails $emu "$harness" register --dry-run "$fixtures/testcodec.dll"

	# DirectShow filters, registered in a registry of their own.
	reg=$(mktemp)
	rm -f "$reg"
	export VFW_REGISTRY="$reg"
	$emu "$harness" register --dry-run "$fixtures/testfilter.ax" |
		grep -q "seance test decoder"
	# Probe performs the same sandboxed discovery through the normal CLI,
	# reports both filter roles and can select one class from a multi-class
	# server without registering it.
	probe=$($emu "$harness" probe "$fixtures/testfilter.ax")
	printf '%s\n' "$probe" | grep -q "DirectShow server:"
	printf '%s\n' "$probe" | grep -q "seance test decoder"
	printf '%s\n' "$probe" | grep -q "video decoder"
	printf '%s\n' "$probe" | grep -q "seance test encoder"
	printf '%s\n' "$probe" | grep -q "video encoder"
	selected=$($emu "$harness" probe "$fixtures/testfilter.ax" --clsid \
		'{5E6C9B32-3A7D-4F0E-9C11-0D2B7A6E4F80}')
	printf '%s\n' "$selected" | grep -q "seance test encoder"
	if printf '%s\n' "$selected" | grep -q "seance test decoder"; then
		echo "smoke: --clsid did not select one DirectShow filter" >&2
		exit 1
	fi
	fails $emu "$harness" probe "$fixtures/testfilter.ax" --clsid bad
	fails $emu "$harness" probe "$fixtures/testfilter.ax" --fourcc TST0
	[ ! -e "$reg" ] || { echo "smoke: probe wrote $reg" >&2; exit 1; }
	[ ! -e "$reg" ] || { echo "smoke: --dry-run wrote $reg" >&2; exit 1; }
	$emu "$harness" register "$fixtures/testfilter.ax" >/dev/null
	VFW_INI=/nonexistent $emu "$harness" list |
		grep -q "seance test encoder"
	$emu "$harness" unregister "$fixtures/testfilter.ax" >/dev/null
	if VFW_INI=/nonexistent $emu "$harness" list | grep -q "seance test"; then
		echo "smoke: unregister left filters behind" >&2
		exit 1
	fi

	# DMOs: one registered through DMORegister, one whose keys the DLL
	# writes itself; found the same ways.
	rm -f "$reg"
	probe=$($emu "$harness" probe "$fixtures/testdmo.dll")
	printf '%s\n' "$probe" | grep -q "DMO server:"
	printf '%s\n' "$probe" | grep -q "seance test DMO decoder"
	printf '%s\n' "$probe" | grep -q "DMO, video encoder"
	selected=$($emu "$harness" probe "$fixtures/testdmo.dll" --clsid \
		'{5E6C9B41-3A7D-4F0E-9C11-0D2B7A6E4F80}')
	if printf '%s\n' "$selected" | grep -q "seance test DMO encoder"; then
		echo "smoke: --clsid did not select one DMO" >&2
		exit 1
	fi
	$emu "$harness" register --dry-run "$fixtures/testdmo.dll" |
		grep -q "seance test DMO encoder"
	[ ! -e "$reg" ] || { echo "smoke: DMO --dry-run wrote $reg" >&2; exit 1; }
	$emu "$harness" register "$fixtures/testdmo.dll" >/dev/null
	VFW_INI=/nonexistent $emu "$harness" list |
		grep -q "seance test DMO decoder"
	$emu "$harness" unregister "$fixtures/testdmo.dll" >/dev/null
	if VFW_INI=/nonexistent $emu "$harness" list | grep -q "seance test DMO"; then
		echo "smoke: unregister left DMOs behind" >&2
		exit 1
	fi
	rm -f "$reg"
	unset VFW_REGISTRY
	fails $emu "$harness" decode "$fixtures/testcodec.dll" "$junk" -o /dev/null \
		--format raw

	# Audio through testfilter.ax's TSTA encoder and decoder: PCM back
	# exactly, in every mode of the decoder, through AVI and WAV.
	work=$(mktemp -d)
	f=$fixtures/testfilter.ax
	head -c 40000 /dev/urandom >"$work/in.pcm"
	$emu "$harness" convert --raw s16 --rate 22050 --channels 2 \
		"$work/in.pcm" -o "$work/in.wav" 2>/dev/null
	$emu "$harness" encode "$f" "$work/in.wav" -o "$work/t.avi" 2>/dev/null
	$emu "$harness" avi "$work/t.avi" | grep -q "wave format 0x7354"
	for mode in "" SPLIT HOLD FLOAT; do
		env ${mode:+VFW_ENV_TESTFILTER_AUDIO_$mode=1} $emu "$harness" \
			decode "$f" "$work/t.avi" -o "$work/out.pcm" 2>/dev/null
		cmp -s "$work/in.pcm" "$work/out.pcm" || {
			echo "smoke: TSTA decode${mode:+ ($mode)} differs" >&2
			exit 1
		}
	done
	fails env VFW_ENV_TESTFILTER_AUDIO_RATE=1 $emu "$harness" decode "$f" \
		"$work/t.avi" -o "$work/x.wav"
	$emu "$harness" decode "$f" "$work/t.avi" -o /dev/null 2>&1 |
		grep -q "22050 Hz -> raw"
	$emu "$harness" encode "$f" "$work/in.wav" -o "$work/t.wav" \
		--bitrate 128 2>/dev/null
	$emu "$harness" decode "$f" "$work/t.wav" -o - --format pcm 2>/dev/null |
		cmp - "$work/in.pcm"
	$emu "$harness" encode "$f" --audio - -o "$work/p.avi" \
		<"$work/in.wav" 2>/dev/null
	cmp -s "$work/t.avi" "$work/p.avi"
	# To the sample, and as float and back.
	$emu "$harness" decode "$f" "$work/t.avi" -o "$work/part.pcm" \
		--start 1000 --duration 0.1s 2>/dev/null
	tail -c +4001 "$work/in.pcm" | head -c 8820 | cmp - "$work/part.pcm"
	$emu "$harness" decode "$f" "$work/t.avi" -o "$work/f.wav" \
		--sample-format f32 2>/dev/null
	$emu "$harness" convert "$work/f.wav" -o "$work/f.pcm" \
		--sample-format s16 2>/dev/null
	cmp "$work/in.pcm" "$work/f.pcm"
	$emu "$harness" encode "$f" "$work/in.wav" -o "$work/part.avi" \
		--start 0.5s --duration 1000 2>/dev/null
	$emu "$harness" decode "$f" "$work/part.avi" -o "$work/part2.pcm" \
		2>/dev/null
	tail -c +44101 "$work/in.pcm" | head -c 4000 | cmp - "$work/part2.pcm"
	$emu "$harness" probe "$f" "$work/t.avi" |
		grep -q "outputs accepted for .*t.avi: s16 s16 (extensible)"
	$emu "$harness" probe "$f" "$work/in.wav" | grep -q "makes of .*0x7354"
	fails $emu "$harness" decode "$f" "$work/in.wav" -o "$work/x.wav"
	fails $emu "$harness" encode "$f" "$work/t.avi" -o "$work/x.avi"
	fails $emu "$harness" decode "$f" "$work/t.avi" -o "$work/x.wav" \
		--frames 3
	reg="$work/registry"
	export VFW_REGISTRY="$reg"
	$emu "$harness" register "$f" >/dev/null
	VFW_INI=/nonexistent $emu "$harness" list | grep -q "audio decoder"
	$emu "$harness" decode auto "$work/t.avi" -o "$work/auto.pcm" 2>/dev/null
	cmp "$work/in.pcm" "$work/auto.pcm"
	$emu "$harness" unregister "$f" >/dev/null

	# The same through testdmo.dll's TSTA DMOs: the same AVI as the
	# filter's, each decoding the other's, in every mode of the DMOs.
	d=$fixtures/testdmo.dll
	strict=
	[ "$lowmem" = 1 ] && strict=STRICT
	for mode in "" NOTACCEPTING $strict; do
		env ${mode:+VFW_ENV_TESTDMO_$mode=1} $emu "$harness" encode "$d" \
			"$work/in.wav" -o "$work/dmo.avi" 2>/dev/null
		cmp -s "$work/t.avi" "$work/dmo.avi" || {
			echo "smoke: TSTA DMO encode${mode:+ ($mode)} differs" >&2
			exit 1
		}
	done
	for mode in "" SPLIT HOLD FLOAT NOTACCEPTING $strict; do
		var=VFW_ENV_TESTDMO_AUDIO_$mode
		case $mode in NOTACCEPTING|STRICT) var=VFW_ENV_TESTDMO_$mode ;; esac
		env ${mode:+$var=1} $emu "$harness" decode "$d" "$work/t.avi" \
			-o "$work/dmo.pcm" 2>/dev/null
		cmp -s "$work/in.pcm" "$work/dmo.pcm" || {
			echo "smoke: TSTA DMO decode${mode:+ ($mode)} differs" >&2
			exit 1
		}
	done
	fails env VFW_ENV_TESTDMO_AUDIO_RATE=1 $emu "$harness" decode "$d" \
		"$work/t.avi" -o "$work/x.wav"
	$emu "$harness" decode "$d" "$work/t.wav" -o - --format pcm 2>/dev/null |
		cmp - "$work/in.pcm"
	$emu "$harness" encode "$d" "$work/in.wav" -o "$work/dmo-b.avi" \
		--bitrate 128 2>/dev/null
	$emu "$harness" avi "$work/dmo-b.avi" | grep -q "128 kb/s"
	$emu "$harness" probe "$d" "$work/t.avi" |
		grep -q "outputs accepted for .*t.avi: s16 s16 (extensible)"
	$emu "$harness" probe "$d" "$work/in.wav" | grep -q "makes of .*0x7354"
	$emu "$harness" register "$d" >/dev/null
	VFW_INI=/nonexistent $emu "$harness" decode auto "$work/t.avi" \
		-o "$work/dmo-auto.pcm" 2>/dev/null
	cmp "$work/in.pcm" "$work/dmo-auto.pcm"
	$emu "$harness" unregister "$d" >/dev/null

	# And through testacm.acm, an ACM driver: its AVI (blocks of a constant
	# size) decoded by itself and by the filter, the filter's by it, in each
	# of its modes; then found through vfw.ini by list and decode auto.
	a=$fixtures/testacm.acm
	for mode in "" SMALL PREPARE NOTREALTIME $strict; do
		env ${mode:+VFW_ENV_TESTACM_$mode=1} $emu "$harness" encode "$a" \
			"$work/in.wav" -o "$work/acm.avi" 2>/dev/null
		for avi in acm.avi t.avi; do
			for dec in "$a" "$f"; do
				env ${mode:+VFW_ENV_TESTACM_$mode=1} $emu "$harness" \
					decode "$dec" "$work/$avi" -o "$work/acm.pcm" \
					2>/dev/null
				cmp -s "$work/in.pcm" "$work/acm.pcm" || {
					echo "smoke: ACM${mode:+ ($mode)}: $avi through" \
						"${dec##*/} differs" >&2
					exit 1
				}
			done
		done
	done
	$emu "$harness" avi "$work/acm.avi" | grep -q "sample size 4104"
	$emu "$harness" encode "$a" "$work/in.wav" -o "$work/acm.wav" \
		--format-tag 0x7354 --bitrate 1000 2>/dev/null
	$emu "$harness" decode "$a" "$work/acm.wav" -o - --format pcm \
		2>/dev/null | cmp - "$work/in.pcm"
	$emu "$harness" probe "$a" | grep -q "0x7354 TSTA: 45 standard formats"
	$emu "$harness" probe "$a" "$work/t.avi" |
		grep -q "outputs accepted for .*t.avi: s16"
	$emu "$harness" probe "$a" "$work/in.wav" | grep -q "makes of .*0x7354"
	fails $emu "$harness" encode "$a" "$work/in.wav" -o "$work/x.avi" \
		--quality 5000
	fails $emu "$harness" encode "$a" "$work/in.wav" -o "$work/x.avi" \
		--format-tag 0x55
	fails $emu "$harness" decode "$a" "$work/acm.avi" -o "$work/x.wav" \
		--state "$work/in.pcm"
	# Given video, it isn't opened as a video codec: one that crashes when
	# it is (as vdowave.acm does) is told it is an audio driver instead.
	{
		printf 'YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420jpeg\nFRAME\n'
		head -c 384 /dev/zero
	} >"$work/v.y4m"
	# VFW_DATE has to be a date.
	VFW_DATE=2007-02-29 $emu "$harness" list 2>&1 |
		grep -q "VFW_DATE wants a date"
	VFW_DATE=2007-06-01T12:00 $emu "$harness" list >/dev/null
	# A codec that only decompresses is said to, not to refuse the formats.
	VFW_ENV_TESTCODEC_NOCOMPRESS=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/v.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "doesn't compress: .*ICERR_UNSUPPORTED"
	VFW_ENV_TESTCODEC_NOCOMPRESS=1 $emu "$harness" probe \
		"$fixtures/testcodec.dll" | grep -q "it only decompresses"
	# One that answers only under its FOURCC, named only as a DLL: the
	# FOURCC is found in it.
	VFW_INI=/dev/null VFW_ENV_TESTCODEC_NAMED_ONLY=1 $emu "$harness" probe \
		"$fixtures/testcodec.dll" 2>&1 | grep -q "opened as 'TST0', found"
	# Its FOURCC kept as an upper and lower case pair in data.
	VFW_INI=/dev/null VFW_ENV_TESTCODEC_NAMED_ONLY=2 $emu "$harness" probe \
		"$fixtures/testcodec.dll" 2>&1 | grep -q "opened as 'TSTP', found"
	# One that opens without its FOURCC but then gives none, nor a name.
	VFW_INI=/dev/null VFW_ENV_TESTCODEC_NAMED_ONLY=3 $emu "$harness" probe \
		"$fixtures/testcodec.dll" 2>&1 | grep -q "opened as 'TST0', found"
	# One that takes YUV when asked but begins only with RGB is given RGB.
	VFW_ENV_TESTCODEC_BEGIN_RGB=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/v.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "takes I420 but won't begin with it; giving it BGR32"
	# One that begins only at a multiple of 16 is said to, with --pad.
	{
		printf 'YUV4MPEG2 W16 H8 F25:1 Ip A1:1 C420jpeg\nFRAME\n'
		head -c 192 /dev/zero
	} >"$work/h8.y4m"
	VFW_ENV_TESTCODEC_ALIGN16=2 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/h8.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "begins with no input at this size, but takes 16x16: add --pad 16"
	# A decoder that opens only under its FOURCC is said to be one.
	VFW_INI=/dev/null VFW_ENV_TESTCODEC_DECODE_ONLY=2 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/v.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "opens only as 'TST0' (found in it), and only to decompress"
	# A companion DLL it looks for and can't find is named.
	VFW_ENV_TESTCODEC_NEEDS_DLL=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/v.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "refused DRV_LOAD; it looked for zz_companion.dll, which isn't here"
	# For a DLL that is there but whose import isn't, the import is named.
	VFW_ENV_TESTCODEC_NEEDS_DLL=2 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/v.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "refused DRV_LOAD; it looked for zz_nosuch.dll, which isn't here"
	# And one it gives a full path to.
	VFW_ENV_TESTCODEC_NEEDS_DLL=3 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/v.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "refused DRV_LOAD; it looked for zz_path.dll, which isn't here"
	# YUV a codec writes upside down: RGB instead, or turned over.
	{
		printf 'YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420jpeg\nFRAME\n'
		head -c 384 /dev/urandom
	} >"$work/up.y4m"
	$emu "$harness" encode "$fixtures/testcodec.dll" "$work/up.y4m" \
		-o "$work/up.avi" 2>/dev/null
	$emu "$harness" decode "$fixtures/testcodec.dll" "$work/up.avi" \
		-o "$work/up.y4m" 2>/dev/null
	VFW_ENV_TESTCODEC_YUV_UPSIDE=1 $emu "$harness" decode \
		"$fixtures/testcodec.dll" "$work/up.avi" -o "$work/up2.y4m" 2>&1 |
		grep -q "writes I420 upside down; decoding to BGR24 instead"
	VFW_ENV_TESTCODEC_YUV_UPSIDE=1 $emu "$harness" decode \
		"$fixtures/testcodec.dll" "$work/up.avi" -o "$work/up2.y4m" \
		--output-format I420 2>&1 | grep -q "writes I420 upside down; turned over"
	cmp "$work/up.y4m" "$work/up2.y4m"
	# Raw output is the codec's own, untouched.
	VFW_ENV_TESTCODEC_YUV_UPSIDE=1 $emu "$harness" decode \
		"$fixtures/testcodec.dll" "$work/up.avi" -o "$work/up.raw" \
		--format raw 2>&1 | grep -q "upside down" && {
		echo "smoke: raw output was turned over" >&2
		exit 1
	}
	# One that opens and answers under any handler but takes formats only
	# under its own: found by what it takes, and kept in vfw.ini so the
	# next run opens it so at once.
	rm -f "$work/remember.ini" "$work/none.ini"
	VFW_INI="$work/remember.ini" VFW_ENV_TESTCODEC_NAMED_ONLY=4 $emu \
		"$harness" encode "$fixtures/testcodec.dll" "$work/v.y4m" \
		-o "$work/n4.avi" 2>&1 | grep -q "but does as 'TST0', found in it"
	grep -q "^vidc.TST0=.*testcodec.dll" "$work/remember.ini"
	if VFW_INI="$work/remember.ini" VFW_ENV_TESTCODEC_NAMED_ONLY=4 $emu \
		"$harness" encode "$fixtures/testcodec.dll" "$work/v.y4m" \
		-o "$work/n4.avi" 2>&1 | grep -q "found in it"; then
		echo "smoke: a remembered FOURCC was looked for again" >&2
		exit 1
	fi
	# Decoding too; VFW_REMEMBER=0 keeps nothing.
	VFW_INI="$work/none.ini" VFW_REMEMBER=0 VFW_ENV_TESTCODEC_NAMED_ONLY=4 \
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/n4.avi" \
		-o "$work/n4.y4m" 2>&1 | grep -q "but does as 'TST0', found in it"
	[ ! -e "$work/none.ini" ]
	# What a DLL is under each FOURCC found in it.
	VFW_INI=/dev/null VFW_ENV_TESTCODEC_NAMED_ONLY=4 $emu "$harness" probe \
		"$fixtures/testcodec.dll" --fourccs >"$work/fourccs.txt"
	grep -q "^  'TST0' (compared with in its code):" "$work/fourccs.txt"
	grep -q "compresses at 320x240: nothing" "$work/fourccs.txt"
	grep -q "compresses at 320x240: I420" "$work/fourccs.txt"
	# A DLL that doesn't load isn't loaded again for each FOURCC or mode
	# to try (Mvicod32.dll showed a message box each time): once to see
	# whether it is an ACM driver, once as a codec.
	[ "$(VFW_TRACE=1 $emu "$harness" probe "$fixtures/pe_fail.dll" 2>&1 |
		grep -c "loaded .*pe_fail.dll at")" = 2 ]
	# A DMO server's decoder isn't taken for a missing encoder.
	$emu "$harness" encode "$fixtures/testdmo.dll" "$work/v.y4m" \
		-o "$work/x.avi" --clsid '{5E6C9B41-3A7D-4F0E-9C11-0D2B7A6E4F80}' \
		2>&1 | grep -q "has only a video decoder, not an encoder"
	# A Windows 9x DLL that thunks to a 16-bit one (Mvicod32.dll) is said
	# to be one, and its DllMain isn't run.
	out=$($emu "$harness" encode "$fixtures/pe_thunk.dll" "$work/v.y4m" \
		-o "$work/x.avi" 2>&1 || true)
	echo "$out" | grep -q "a Windows 9x DLL that runs through a 16-bit one (it imports ThunkConnect32"
	if echo "$out" | grep -q "not implemented"; then
		echo "pe_thunk.dll's DllMain ran" >&2
		exit 1
	fi
	$emu "$harness" info "$fixtures/pe_thunk.dll" |
		grep -q "ThunkConnect32: a Windows 9x DLL that thunks"
	# RealPlayer's codecs (rv203260.dll) are said to be what they are.
	$emu "$harness" encode "$fixtures/pe_realcodec.dll" "$work/v.y4m" \
		-o "$work/x.avi" 2>&1 | grep -q "a RealPlayer codec (it exports RV20"
	# One that won't open to compress is probed as encode would use it.
	VFW_ENV_TESTCODEC_DECODE_ONLY=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/v.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "only decompresses: it opens to decompress, not to compress"
	VFW_ENV_TESTCODEC_DECODE_ONLY=1 $emu "$harness" probe \
		"$fixtures/testcodec.dll" | grep -q "won't open to compress"
	VFW_ENV_TESTCODEC_PICKY=1 $emu "$harness" probe \
		"$fixtures/testcodec.dll" | grep -q "refuses its own format"
	# One that only knows how big a frame gets once it has begun is asked
	# again then, and given room for it.
	{
		printf 'YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420jpeg\nFRAME\n'
		head -c 384 /dev/urandom
	} >"$work/noise.y4m"
	VFW_ENV_TESTCODEC_LATESIZE=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/noise.y4m" -o "$work/late.avi" \
		2>/dev/null
	$emu "$harness" decode "$fixtures/testcodec.dll" "$work/late.avi" \
		-o "$work/late.y4m" 2>/dev/null
	cmp "$work/noise.y4m" "$work/late.y4m"
	# What a codec writes to its standard output goes to our stderr, not
	# into ours, where the output can be.
	VFW_ENV_TESTCODEC_CHATTY=1 $emu "$harness" decode \
		"$fixtures/testcodec.dll" "$work/late.avi" -o - --format y4m \
		2>"$work/chat.err" | cmp - "$work/late.y4m"
	grep -q "testcodec: hello" "$work/chat.err"
	# Codecs as fussy as some real ones: one calls a status callback it
	# never checked it was given and refuses its own format with the input
	# (encoding), one reads past its input and decodes nothing unless asked
	# its format first (decoding, the output named).
	VFW_ENV_TESTCODEC_STATUS=1 VFW_ENV_TESTCODEC_PICKY=1 \
		$emu "$harness" encode "$fixtures/testcodec.dll" "$work/noise.y4m" \
		-o "$work/fussy.avi" 2>/dev/null
	VFW_ENV_TESTCODEC_PADDING=1 VFW_ENV_TESTCODEC_ASK_FORMAT=1 \
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/fussy.avi" \
		-o "$work/fussy.y4m" --output-format I420 2>/dev/null
	cmp "$work/noise.y4m" "$work/fussy.y4m"
	# One that wants heights in 16s says so, and --pad or --crop give it
	# them (repeating the last row, or dropping rows), from YUV or RGB.
	{
		printf 'YUV4MPEG2 W16 H24 F25:1 Ip A1:1 C420jpeg\nFRAME\n'
		head -c 576 /dev/urandom
	} >"$work/h24.y4m"
	head -c 2304 /dev/urandom >"$work/h24.bgr"
	VFW_ENV_TESTCODEC_ALIGN16=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/h24.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "at 16x24, but takes 16x32 and 16x16: add --pad 16 or --crop 16"
	VFW_ENV_TESTCODEC_ALIGN16=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/h24.y4m" -o "$work/pad.avi" \
		--pad 16 2>/dev/null
	$emu "$harness" decode "$fixtures/testcodec.dll" "$work/pad.avi" \
		-o "$work/pad.y4m" 2>/dev/null
	head -1 "$work/pad.y4m" | grep -q "W16 H32 "
	VFW_ENV_TESTCODEC_ALIGN16=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/h24.bgr" --input-format BGR24 \
		--size 16x24 -o "$work/crop.avi" --crop 16 2>/dev/null
	$emu "$harness" avi "$work/crop.avi" | grep -q "16x16, 24 bits, 'TST0'"
	fails $emu "$harness" encode "$fixtures/testcodec.dll" "$work/h24.y4m" \
		-o "$work/x.avi" --pad 3
	# One that writes no output format is refused, not written as 0x0;
	# one that flags no keyframes still gets its first frame as one.
	VFW_ENV_TESTCODEC_EMPTY_FORMAT=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/noise.y4m" -o "$work/x.avi" 2>&1 |
		grep -q "gives no output format (0x0)"
	VFW_ENV_TESTCODEC_NO_KEYS=1 $emu "$harness" encode \
		"$fixtures/testcodec.dll" "$work/noise.y4m" \
		-o "$work/nokeys.avi" 2>/dev/null
	$emu "$harness" avi --chunks "$work/nokeys.avi" |
		grep -q "^ *0  00dc .* key$"
	# A codec that crashes in ICM_*_END after the last frame still gives
	# whole files (not under a sanitizer, which takes faults itself).
	if [ "$lowmem" = 1 ]; then
		VFW_ENV_TESTCODEC_END_CRASH=1 $emu "$harness" encode \
			"$fixtures/testcodec.dll" "$work/noise.y4m" \
			-o "$work/end.avi" 2>&1 |
			grep -q "crashed in ICM_COMPRESS_END, after the last frame"
		VFW_ENV_TESTCODEC_END_CRASH=1 $emu "$harness" decode \
			"$fixtures/testcodec.dll" "$work/end.avi" \
			-o "$work/end.y4m" 2>/dev/null
		cmp "$work/noise.y4m" "$work/end.y4m"
		# One that crashes as it is unloaded has done its work by then.
		VFW_ENV_TESTCODEC_DETACH_CRASH=1 $emu "$harness" probe \
			"$fixtures/testcodec.dll" 2>&1 >/dev/null |
			grep -q "crashed while being unloaded; carrying on"
		VFW_ENV_TESTCODEC_DETACH_CRASH=1 $emu "$harness" probe \
			"$fixtures/testcodec.dll" >/dev/null 2>&1
		# One that writes past its output buffer faults right there.
		VFW_ENV_TESTCODEC_OVERRUN=1 $emu "$harness" encode \
			"$fixtures/testcodec.dll" "$work/noise.y4m" \
			-o "$work/over.avi" 2>&1 |
			grep -q "bytes past the end of the [0-9]*-byte output buffer"
	fi
	VFW_ENV_TESTACM_OPENCRASH=1 $emu "$harness" encode "$a" "$work/v.y4m" \
		-o "$work/x.avi" 2>&1 | grep -q "is an ACM audio driver"
	VFW_ENV_TESTACM_OPENCRASH=1 $emu "$harness" probe "$a" --fourcc TSTA \
		2>&1 | grep -q "is an ACM audio driver"
	printf '[drivers32]\nmsacm.tsta=%s\n' "$a" >"$work/vfw.ini"
	VFW_INI="$work/vfw.ini" $emu "$harness" list |
		grep -q "TSTA (seance TSTA test codec), codec"
	VFW_INI="$work/vfw.ini" $emu "$harness" decode auto "$work/acm.avi" \
		-o "$work/acm-auto.pcm" 2>/dev/null
	cmp "$work/in.pcm" "$work/acm-auto.pcm"
	unset VFW_REGISTRY
	rm -rf "$work"

	if [ "$lowmem" = 1 ]; then
		work=$(mktemp -d)
		trap 'rm -rf "$work"' EXIT
		{
			printf 'YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420jpeg\n'
			for i in 1 2 3 4; do
				printf 'FRAME\n'
				head -c 384 /dev/urandom
			done
		} >"$work/in.y4m"
		export VFW_ENV_TESTCODEC_STRICT=1
		$emu "$harness" encode "$fixtures/testcodec.dll" "$work/in.y4m" \
			-o "$work/t.avi" 2>/dev/null
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/t.avi" \
			-o "$work/out.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/out.y4m" || {
			echo "smoke: strict round trip differs" >&2
			exit 1
		}
		# The DirectShow decoder must negotiate from the AVI's media type and
		# produce exactly the same frames as the VfW decoder.
		$emu "$harness" probe "$fixtures/testfilter.ax" "$work/t.avi" |
			grep -q "outputs accepted.*I420"
		$emu "$harness" decode "$fixtures/testfilter.ax" "$work/t.avi" \
			-o "$work/dshow.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dshow.y4m" || {
			echo "smoke: DirectShow decode differs" >&2
			exit 1
		}
		# A filter that supplies its own allocator (three or more buffers, with a
		# prefix and alignment) has the host fill its samples, both ways.
		# An encoder registered in two categories is still one encoder,
		# one that connects only with the output type it proposes gets
		# it (both as LEAD's encoders), and one that proposes none gets
		# the type made from the input.
		VFW_ENV_TESTFILTER_TWO_CATEGORIES=1 \
			$emu "$harness" encode "$fixtures/testfilter.ax" \
			"$work/in.y4m" -o "$work/two.avi" 2>/dev/null
		VFW_ENV_TESTFILTER_OWN_OUTPUT=1 \
			$emu "$harness" encode "$fixtures/testfilter.ax" \
			"$work/in.y4m" -o "$work/own.avi" 2>&1 |
			grep -q "4 bytes of extra data"
		# An intra-only encoder that marks no keyframes of its own passes
		# on its inputs' sync points: every uncompressed frame is one.
		VFW_ENV_TESTFILTER_INTRA=1 \
			$emu "$harness" encode "$fixtures/testfilter.ax" \
			"$work/in.y4m" -o "$work/intra.avi" 2>&1 |
			grep -q "4 frames (4 key)"
		VFW_ENV_TESTFILTER_NO_PROPOSAL=1 \
			$emu "$harness" encode "$fixtures/testfilter.ax" \
			"$work/in.y4m" -o "$work/made.avi" 2>&1 |
			grep -v "extra data" | grep -q "'TST0'"
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/made.avi" \
			-o "$work/made.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/made.y4m" || {
			echo "smoke: made output type round trip differs" >&2
			exit 1
		}
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/own.avi" \
			-o "$work/own.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/own.y4m" || {
			echo "smoke: own output type round trip differs" >&2
			exit 1
		}
		# An encoder registered as making any video still encodes: the
		# FOURCC comes from the type it connects with.
		VFW_ENV_TESTFILTER_NO_SUBTYPE=1 \
			$emu "$harness" encode "$fixtures/testfilter.ax" \
			"$work/in.y4m" -o "$work/any.avi" 2>&1 |
			grep -q "'TST0'"
		VFW_ENV_TESTFILTER_OWN_ALLOCATOR=1 \
			$emu "$harness" decode "$fixtures/testfilter.ax" "$work/t.avi" \
			-o "$work/dshow-own.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dshow-own.y4m" || {
			echo "smoke: own-allocator DirectShow decode differs" >&2
			exit 1
		}
		VFW_ENV_TESTFILTER_OWN_ALLOCATOR=1 \
			$emu "$harness" encode "$fixtures/testfilter.ax" "$work/in.y4m" \
			-o "$work/dshow-own.avi" 2>/dev/null
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/dshow-own.avi" \
			-o "$work/dshow-own-vfw.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dshow-own-vfw.y4m" || {
			echo "smoke: own-allocator DirectShow encode differs" >&2
			exit 1
		}
		# Decoding to the null device needs no extension.
		$emu "$harness" decode "$fixtures/testcodec.dll" \
			"$work/dshow-own.avi" -o /dev/null 2>&1 |
			grep -q -- "-> raw frames"
		# Every pointer the host gives a DirectShow filter is below 2 GB too.
		VFW_ENV_TESTFILTER_STRICT=1 \
			$emu "$harness" decode "$fixtures/testfilter.ax" "$work/t.avi" \
			-o "$work/dshow-strict.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dshow-strict.y4m" || {
			echo "smoke: strict DirectShow decode differs" >&2
			exit 1
		}
		VFW_ENV_TESTFILTER_STRICT=1 \
			$emu "$harness" encode "$fixtures/testfilter.ax" "$work/in.y4m" \
			-o "$work/dshow-strict.avi" 2>/dev/null
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/dshow-strict.avi" \
			-o "$work/dshow-strict-vfw.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dshow-strict-vfw.y4m" || {
			echo "smoke: strict DirectShow encode differs" >&2
			exit 1
		}
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/t.avi" \
			-o "$work/start.ref.y4m" --start 1 2>/dev/null
		VFW_ENV_TESTFILTER_ASYNC=1 VFW_ENV_TESTFILTER_REQUIRE_SEGMENT=1 \
			$emu "$harness" decode "$fixtures/testfilter.ax" "$work/t.avi" \
			-o "$work/start.y4m" --start 1 2>/dev/null
		cmp -s "$work/start.ref.y4m" "$work/start.y4m" || {
			echo "smoke: asynchronous seek decode differs" >&2
			exit 1
		}
		# A decoder that writes the layout of the type it proposes (top-down
		# RGB here), whatever it was given, is read in that layout.
		$emu "$harness" encode "$fixtures/testcodec.dll" "$work/in.y4m" \
			-o "$work/rgb.avi" --codec-input BGR24 2>/dev/null
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/rgb.avi" \
			-o "$work/rgb.ref.y4m" 2>/dev/null
		VFW_ENV_TESTFILTER_OWN_LAYOUT=1 \
			$emu "$harness" decode "$fixtures/testfilter.ax" "$work/rgb.avi" \
			-o "$work/rgb.y4m" --output-format BGR24 2>/dev/null
		cmp -s "$work/rgb.ref.y4m" "$work/rgb.y4m" || {
			echo "smoke: decoder's own output layout differs" >&2
			exit 1
		}
		# A filter that announces a new output format mid-stream (here YV12,
		# with its chroma swapped) must be followed, not rejected; and one that
		# doesn't flag its preroll frames must not leak them into the output.
		VFW_ENV_TESTFILTER_DYNAMIC=1 \
			$emu "$harness" decode "$fixtures/testfilter.ax" "$work/t.avi" \
			-o "$work/dynamic.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dynamic.y4m" || {
			echo "smoke: dynamic format change decode differs" >&2
			exit 1
		}
		VFW_ENV_TESTFILTER_NO_PREROLL=1 \
			$emu "$harness" decode "$fixtures/testfilter.ax" "$work/t.avi" \
			-o "$work/noprerollstart.y4m" --start 1 2>/dev/null
		cmp -s "$work/start.ref.y4m" "$work/noprerollstart.y4m" || {
			echo "smoke: decode without preroll flags differs" >&2
			exit 1
		}
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/t.avi" \
			-o "$work/frames.ref.y4m" --start 1 --frames 1 2>/dev/null
		VFW_ENV_TESTFILTER_NO_PREROLL=1 \
			$emu "$harness" decode "$fixtures/testfilter.ax" "$work/t.avi" \
			-o "$work/frames.y4m" --start 1 --frames 1 2>/dev/null
		cmp -s "$work/frames.ref.y4m" "$work/frames.y4m" || {
			echo "smoke: decode --frames count differs" >&2
			exit 1
		}
		if env VFW_ENV_TESTFILTER_NO_EOS=1 VFW_TIMEOUT=1 $emu \
			"$harness" decode "$fixtures/testfilter.ax" "$work/t.avi" \
			-o "$work/timeout.y4m" 2>"$work/timeout.err"; then
			echo "smoke: DirectShow missing-EOS mode should time out" >&2
			exit 1
		fi
		grep -q VFW_E_TIMEOUT "$work/timeout.err"
		# And the reverse direction: DirectShow encode must produce a TST0
		# stream which the VfW decoder reads byte-for-byte. Exercise the
		# encoder controls and persisted state while doing so.
		$emu "$harness" encode "$fixtures/testfilter.ax" "$work/in.y4m" \
			-o "$work/dshow.avi" --keyint 2 --quality 7000 \
			--bitrate 100 --save-state "$work/dshow.state" 2>/dev/null
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/dshow.avi" \
			-o "$work/dshow-vfw.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dshow-vfw.y4m" || {
			echo "smoke: DirectShow encode differs" >&2
			exit 1
		}
		VFW_ENV_TESTFILTER_DYNAMIC=1 \
			$emu "$harness" encode "$fixtures/testfilter.ax" "$work/in.y4m" \
			-o "$work/dshow-dyn.avi" 2>/dev/null
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/dshow-dyn.avi" \
			-o "$work/dshow-dyn.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dshow-dyn.y4m" || {
			echo "smoke: DirectShow encode with a type change differs" >&2
			exit 1
		}
		# An encoder that has ICodecAPI instead of IAMVideoCompression gets the
		# same options: every second of the four frames is a keyframe.
		VFW_ENV_TESTFILTER_CODECAPI=1 $emu "$harness" probe \
			"$fixtures/testfilter.ax" | grep -q "interfaces: ICodecAPI"
		VFW_ENV_TESTFILTER_CODECAPI=1 $emu "$harness" encode \
			"$fixtures/testfilter.ax" "$work/in.y4m" -o "$work/dshow-api.avi" \
			--keyint 2 2>&1 | grep -q "(2 key)" || {
			echo "smoke: ICodecAPI key interval was not applied" >&2
			exit 1
		}
		# A type that adds format data the file's stream header lacks can't be
		# followed, and says so rather than writing a misleading file.
		if env VFW_ENV_TESTFILTER_DYNAMIC_EXTRA=1 $emu "$harness" encode \
			"$fixtures/testfilter.ax" "$work/in.y4m" \
			-o "$work/dshow-extra.avi" 2>"$work/extra.err"; then
			echo "smoke: encode should refuse a type that adds format data" >&2
			exit 1
		fi
		grep -q VFW_E_INVALIDMEDIATYPE "$work/extra.err"
		$emu "$harness" encode "$fixtures/testfilter.ax" "$work/in.y4m" \
			-o "$work/dshow-state.avi" --state "$work/dshow.state" --clsid \
			'{5E6C9B32-3A7D-4F0E-9C11-0D2B7A6E4F80}' \
			2>/dev/null
		reg="$work/registry"
		export VFW_REGISTRY="$reg"
		$emu "$harness" register "$fixtures/testfilter.ax" >/dev/null
		VFW_INI=/nonexistent $emu "$harness" decode auto "$work/t.avi" \
			-o "$work/dshow-auto.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dshow-auto.y4m" || {
			echo "smoke: registered DirectShow auto decode differs" >&2
			exit 1
		}
		$emu "$harness" unregister "$fixtures/testfilter.ax" >/dev/null
		unset VFW_REGISTRY

		# DMOs: the same stream decoded through testdmo.dll, in each of its
		# modes, matches testcodec; its encoder's output decodes back
		# exactly through testcodec and testfilter.
		for mode in "" DELAY BURST NOTACCEPTING SWITCH STRICT; do
			env ${mode:+VFW_ENV_TESTDMO_$mode=1} $emu "$harness" decode \
				"$fixtures/testdmo.dll" "$work/t.avi" \
				-o "$work/dmo.y4m" 2>/dev/null
			cmp -s "$work/in.y4m" "$work/dmo.y4m" || {
				echo "smoke: DMO decode${mode:+ ($mode)} differs" >&2
				exit 1
			}
		done
		env VFW_ENV_TESTDMO_DELAY=1 $emu "$harness" decode \
			"$fixtures/testdmo.dll" "$work/t.avi" -o "$work/dmo-start.y4m" \
			--start 1 --frames 2 2>/dev/null
		$emu "$harness" decode "$fixtures/testcodec.dll" "$work/t.avi" \
			-o "$work/dmo-start.ref.y4m" --start 1 --frames 2 2>/dev/null
		cmp -s "$work/dmo-start.ref.y4m" "$work/dmo-start.y4m" || {
			echo "smoke: DMO decode from --start differs" >&2
			exit 1
		}
		for mode in "" NOTACCEPTING PRIVATE BITRATE; do
			env ${mode:+VFW_ENV_TESTDMO_$mode=1} $emu "$harness" encode \
				"$fixtures/testdmo.dll" "$work/in.y4m" \
				-o "$work/dmo.avi" --keyint 2 2>/dev/null
			$emu "$harness" decode "$fixtures/testcodec.dll" "$work/dmo.avi" \
				-o "$work/dmo-vfw.y4m" 2>/dev/null
			$emu "$harness" decode "$fixtures/testfilter.ax" "$work/dmo.avi" \
				-o "$work/dmo-dshow.y4m" 2>/dev/null
			cmp -s "$work/in.y4m" "$work/dmo-vfw.y4m" &&
			cmp -s "$work/in.y4m" "$work/dmo-dshow.y4m" || {
				echo "smoke: DMO encode${mode:+ ($mode)} differs" >&2
				exit 1
			}
		done
		VFW_ENV_TESTDMO_PRIVATE=1 $emu "$harness" encode \
			"$fixtures/testdmo.dll" "$work/in.y4m" -o "$work/dmo.avi" 2>&1 |
			grep -q "4 bytes of extra data" || {
			echo "smoke: the DMO's codec data is not in the stream" >&2
			exit 1
		}
		$emu "$harness" encode "$fixtures/testdmo.dll" "$work/in.y4m" \
			-o "$work/dmo-keys.avi" --keyint 3 --save-state \
			"$work/dmo.state" 2>/dev/null
		$emu "$harness" encode "$fixtures/testdmo.dll" "$work/in.y4m" \
			-o "$work/dmo-state.avi" --state "$work/dmo.state" 2>/dev/null
		cmp -s "$work/dmo-keys.avi" "$work/dmo-state.avi" || {
			echo "smoke: DMO state did not carry the key interval" >&2
			exit 1
		}
		$emu "$harness" probe "$fixtures/testdmo.dll" "$work/t.avi" |
			grep -q "interfaces: ICodecAPI, IPersistStream"
		reg="$work/registry"
		export VFW_REGISTRY="$reg"
		$emu "$harness" register "$fixtures/testdmo.dll" >/dev/null
		VFW_INI=/nonexistent $emu "$harness" decode auto "$work/t.avi" \
			-o "$work/dmo-auto.y4m" 2>/dev/null
		cmp -s "$work/in.y4m" "$work/dmo-auto.y4m" || {
			echo "smoke: registered DMO auto decode differs" >&2
			exit 1
		}
		$emu "$harness" unregister "$fixtures/testdmo.dll" >/dev/null
		unset VFW_REGISTRY
		# The check itself works: without the reservation it trips.
		# Not under an emulator, where the guest's memory is all below
		# 2 GB anyway (qemu-user maps it low).
		if [ -z "$emu" ]; then
			fails env VFW_LOW_MEMORY=0 $emu "$harness" encode \
				"$fixtures/testcodec.dll" "$work/in.y4m" \
				-o "$work/t2.avi"
			fails env VFW_LOW_MEMORY=0 VFW_ENV_TESTFILTER_STRICT=1 \
				$emu "$harness" decode "$fixtures/testfilter.ax" \
				"$work/t.avi" -o "$work/t2.y4m"
		fi
		echo "smoke: the codec saw no pointers above 2 GB"
	fi
fi
echo "smoke: CLI ok"
