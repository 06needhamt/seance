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
		for mode in "" NOTACCEPTING PRIVATE; do
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
