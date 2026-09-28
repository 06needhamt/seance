#!/bin/sh
# A quick run of every CLI command: the usage paths, failures on files that
# aren't what a command wants, and, when the fixture DLLs were built, info
# and probe on them.
#
# With lowmem=1 it also checks, through testcodec's strict mode, that the
# codec never sees a pointer at or above 2 GB.
#
# usage: tests/smoke.sh <vfw-harness> <not a DLL or AVI> [<fixture dir> [lowmem]]
set -eu

usage="usage: $0 <vfw-harness> <file> [<fixture dir> [lowmem]]"
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

if [ -n "$fixtures" ] && [ -f "$fixtures/testcodec.dll" ]; then
	$emu "$harness" info "$fixtures/pe_dep.dll" >/dev/null
	$emu "$harness" probe "$fixtures/testcodec.dll" | grep -q "handler 'TST0'"
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
		# The check itself works: without the reservation it trips.
		# Not under an emulator, where the guest's memory is all below
		# 2 GB anyway (qemu-user maps it low).
		if [ -z "$emu" ]; then
			fails env VFW_LOW_MEMORY=0 $emu "$harness" encode \
				"$fixtures/testcodec.dll" "$work/in.y4m" \
				-o "$work/t2.avi"
		fi
		echo "smoke: the codec saw no pointers above 2 GB"
	fi
fi
echo "smoke: CLI ok"
