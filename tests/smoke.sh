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
	rm -f "$reg"
	unset VFW_REGISTRY
	fails $emu "$harness" decode "$fixtures/testcodec.dll" "$junk" -o /dev/null \
		--format raw

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
