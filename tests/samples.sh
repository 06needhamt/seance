#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Runs `info` and `probe` over every DLL in a directory of codecs you supply
# (*.dll, *.drv, *.ax, *.acm, any case), and reports for each whether it
# parses, which of its imports aren't implemented, and whether it opens and
# answers ICM_GETINFO. A DirectShow or DMO server is reported with its
# filters or DMOs and what they are (video or audio decoders and encoders),
# an ACM driver with its name and what it does with each format tag.
# Nothing specific to any codec is checked. It fails only if the harness
# itself crashes or hangs, or a file can't be parsed.
#
# usage: tests/samples.sh <seance> <dir> [<timeout in seconds>]
set -u

harness=${1:?usage: $0 <seance> <dir> [<timeout>]}
dir=${2:?usage: $0 <seance> <dir> [<timeout>]}
timeout=${3:-30}
failures=0
count=0
# On a cross build, what runs i386 programs (qemu-i386), from CMake.
emu=${VFW_EMULATOR:-}

if [ ! -d "$dir" ]; then
	echo "samples: $dir is not a directory" >&2
	exit 1
fi

# A crash or VFW_TIMEOUT hang, from the exit status.
bad_status() {
	[ "$1" -ge 124 ]
}

out=$(mktemp)
trap 'rm -f "$out"' EXIT
for f in "$dir"/*; do
	case $(printf '%s' "$f" | tr 'A-Z' 'a-z') in
	*.dll | *.drv | *.ax | *.acm) ;;
	*) continue ;;
	esac
	count=$((count + 1))
	name=$(basename "$f")

	VFW_TIMEOUT=$timeout $emu "$harness" info "$f" >"$out" 2>&1
	rc=$?
	if [ $rc -ne 0 ]; then
		echo "FAIL $name: info exited $rc"
		sed 's/^/     /' "$out" | tail -n 5
		failures=$((failures + 1))
		continue
	fi
	traps=$(grep -c '\[not implemented\]' "$out")
	native=$(grep -c '(loaded from disk)' "$out")

	VFW_TIMEOUT=$timeout $emu "$harness" probe "$f" >"$out" 2>&1
	rc=$?
	if bad_status $rc; then
		echo "FAIL $name: probe crashed or hung (exit $rc)"
		sed 's/^/     /' "$out" | grep 'seance:' | tail -n 5
		failures=$((failures + 1))
		continue
	fi
	handler=$(sed -n "s/.*handler '\\(.*\\)'.*/\\1/p" "$out" | head -n 1)
	if grep -q '^DirectShow server:\|^DMO server:' "$out"; then
		kind=$(sed -n 's/^\(.*\) server:.*/\1/p' "$out" | head -n 1)
		dmos=$(grep -c '^      DMO, ' "$out")
		filters=$(($(grep -c '^  {' "$out") - dmos))
		# The roles, counted: "1 video decoder, 2 audio encoders".
		roles=$(grep -o '^      \(DMO, \)\?\(video\|audio\) \(decoder\|encoder\)' \
			"$out" | sed 's/^ *\(DMO, \)\?//' | sort | uniq -c |
			awk '{ printf "%s%d %s %s%s", (NR > 1 ? ", " : ""), $1,
			       $2, $3, ($1 > 1 ? "s" : "") }')
		state="$kind server, $filters filter(s), $dmos DMO(s)${roles:+: $roles}"
	elif grep -q '^ACM driver:' "$out"; then
		# "'MPEG Layer-3 Codec': 0x0055 decodes to PCM, 0x0050 ..."
		acm=$(sed -n 's/^  name *//p' "$out" | head -n 1)
		tags=$(sed -n 's/^    \(0x[0-9a-f]*\) [^:]*: [^;]*; \([^(]*\).*/\1 \2/p' \
			"$out" | sed 's/ *$//' |
			awk '{ printf "%s%s", (NR > 1 ? ", " : ""), $0 }')
		state="ACM driver '${acm:-?}'${tags:+: $tags}"
	elif [ $rc -eq 0 ]; then
		state="opens as '${handler:-?}'"
	else
		state="doesn't open: $(grep 'seance:' "$out" | head -n 1 |
			sed 's/^seance: //')"
	fi
	echo "ok   $name: $traps unimplemented import(s), $native DLL(s)" \
		"needed from disk, $state"
done

echo "samples: $count DLL(s), $failures failure(s)"
[ $count -gt 0 ] || echo "samples: nothing to look at in $dir"
[ $failures -eq 0 ]
