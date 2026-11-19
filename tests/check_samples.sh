#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Makes the samples tests/gen_samples.sh makes, and checks that each is
# listed with its reference and that seance's AVI reader takes every video
# one (a RealMedia one only needs to be one). Skipped (77) without ffmpeg.
#
# usage: tests/check_samples.sh <seance> <dir>
set -u

harness=${1:?usage: $0 <seance> <dir>}
dir=${2:?usage: $0 <seance> <dir>}
emu=${VFW_EMULATOR:-}
here=$(dirname "$0")

rm -rf "${dir:?}"
sh "$here/gen_samples.sh" "$dir" >/dev/null
rc=$?
[ $rc -eq 77 ] && exit 77
[ $rc -eq 0 ] || exit 1
failures=0
video=0
audio=0
while IFS='	' read -r name kind tag file ref; do
	if [ ! -s "$dir/$file" ] || [ ! -s "$dir/$ref" ]; then
		echo "check_samples: $name: $file or $ref is missing" >&2
		failures=$((failures + 1))
		continue
	fi
	case $kind in
	video)
		video=$((video + 1))
		case $file in
		*.rm)
			if [ "$(head -c 4 "$dir/$file")" != .RMF ]; then
				echo "check_samples: $name: $file isn't RealMedia" >&2
				failures=$((failures + 1))
			fi
			continue ;;
		esac
		if ! $emu "$harness" avi "$dir/$file" >/dev/null 2>&1; then
			echo "check_samples: $name: seance can't read $file" >&2
			failures=$((failures + 1))
		fi ;;
	audio) audio=$((audio + 1)) ;;
	*)
		echo "check_samples: $name: kind '$kind'" >&2
		failures=$((failures + 1)) ;;
	esac
done <"$dir/index.tsv"
echo "check_samples: $video video and $audio audio samples"
[ $video -gt 0 ] && [ $audio -gt 0 ] && [ $failures -eq 0 ]
