#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# seance and seance64 hand a codec of the other architecture to each other:
# by path, by a FOURCC in vfw.ini, for register and in sweep, and list
# names each codec's architecture. The round trip through the 64-bit test
# codec from the i386 binary gives what the i386 codec gives.
#
# usage: tests/handoff.sh <seance> <seance64> <fixtures> <fixtures64>
set -eu

usage="usage: $0 <seance> <seance64> <fixtures> <fixtures64>"
s32=${1:?$usage}
s64=${2:?$usage}
f32=${3:?$usage}
f64=${4:?$usage}
emu=${VFW_EMULATOR:-}
# The x86-64 binary run directly needs its own emulator on a host that can't run it.

fail() {
	echo "handoff: $*" >&2
	exit 1
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export VFW_INI="$work/vfw.ini" VFW_REGISTRY="$work/reg" VFW_REMEMBER=0
: >"$VFW_INI"
# The binaries find each other beside themselves.
export VFW_SEANCE64="$s64" VFW_SEANCE32="$s32"

# By path, both ways.
$emu "$s32" probe "$f64/testcodec.dll" | grep -q "handler 'TST0'" ||
	fail "seance didn't probe the x86-64 testcodec"
# An option whose value is "auto" doesn't hide the DLL from the hand-off.
if $emu "$s32" probe --fourcc auto "$f64/testcodec.dll" 2>&1 | grep -q "cannot run"; then
	fail "probe --fourcc auto wasn't handed off"
fi
${VFW_EMULATOR64:-} "$s64" probe "$f32/testcodec.dll" | grep -q "handler 'TST0'" ||
	fail "seance64 didn't probe the i386 testcodec"

# A round trip through each testcodec, from the i386 binary: the same.
{
	printf 'YUV4MPEG2 W32 H32 F25:1 Ip A1:1 C420jpeg\n'
	for i in 1 2 3; do
		printf 'FRAME\n'
		head -c 1536 /dev/urandom
	done
} >"$work/in.y4m"
for a in 32 64; do
	eval f=\$f$a
	$emu "$s32" encode "$f/testcodec.dll" "$work/in.y4m" \
		-o "$work/$a.avi" 2>/dev/null || fail "encode with the $a-bit codec"
	$emu "$s32" decode "$f/testcodec.dll" "$work/$a.avi" \
		-o "$work/$a.y4m" 2>/dev/null || fail "decode with the $a-bit codec"
done
cmp "$work/32.y4m" "$work/64.y4m" || fail "the 64-bit codec decodes otherwise"
cmp "$work/in.y4m" "$work/64.y4m" || fail "the round trip isn't lossless"

# By a FOURCC that vfw.ini gives a 64-bit DLL.
printf '[drivers32]\nvidc.TST0=%s\n' "$f64/testcodec.dll" >"$VFW_INI"
$emu "$s32" probe TST0 | grep -q "testcodec.dll" ||
	fail "seance didn't hand off a FOURCC registered to a 64-bit DLL"

# list names the architecture, and doesn't open what it can't run.
printf 'msacm.tst=%s\n' "$f64/testacm.acm" >>"$VFW_INI"
out=$($emu "$s32" list)
printf '%s\n' "$out" | grep -q "testcodec.dll (x86-64)" ||
	fail "list didn't name the codec's architecture"
printf '%s\n' "$out" | grep -q "not opened: seance64 runs it" ||
	fail "list didn't say seance64 runs it"
: >"$VFW_INI"

# register runs the server in seance64; list then names it x86-64.
$emu "$s32" register "$f64/testfilter.ax" >/dev/null ||
	fail "register of the 64-bit testfilter"
$emu "$s32" list | grep -q "x86-64, run by seance64" ||
	fail "list didn't name the 64-bit filter server"
$emu "$s32" unregister "$f64/testfilter.ax" >/dev/null ||
	fail "unregister of the 64-bit testfilter"

# sweep over both kinds.
out=$($emu "$s32" sweep "$f32/testcodec.dll" "$f64/testcodec.dll" 2>&1)
[ "$(printf '%s\n' "$out" | grep -c '^round-trip testcodec.dll')" = 2 ] ||
	fail "sweep didn't run both testcodecs: $out"

# No sibling: one message saying so, and the loader's refusal.
if VFW_SEANCE64=/nonexistent $emu "$s32" probe "$f64/testcodec.dll" \
	>/dev/null 2>"$work/err"; then
	fail "probe succeeded without seance64"
fi
[ "$(grep -c "isn't there" "$work/err")" = 1 ] ||
	fail "missing seance64 not reported once: $(cat "$work/err")"

echo "handoff: ok"
