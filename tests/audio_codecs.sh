#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Runs an audio decoder you supply (a DirectShow filter or DMO server, such
# as LAV Audio's LAVAudio.ax) over streams FFmpeg makes in AVI files, and
# compares what it gives with FFmpeg's own decoding of the same streams:
# stereo at 44100 Hz and 5.1 at 48000 Hz (GSM: mono at 8000 Hz), as float
# and as 16-bit. An ACM driver (such as Wine's l3codeca.acm) works too.
#
# For each it reports the signal-to-distortion ratio against FFmpeg (the
# lowest of the channels; "inf" when identical) and the lengths. A codec
# the decoder doesn't take, or FFmpeg can't make, is reported and skipped.
# It fails if the harness crashes or hangs, or a stream it decodes comes
# out below the SDR given (40 dB by default: decoders built from different
# FFmpeg versions can differ by a few steps of 16 bits). Settings a decoder keeps in
# the registry come from $VFW_REGISTRY, as for every command. Under the
# asan preset, set ASAN_OPTIONS=detect_leaks=0: a decoder's own
# allocations, which it frees only when unloaded, count as leaks.
#
# usage: tests/audio_codecs.sh <seance> <decoder> <work dir> [<min SDR>]
set -u

usage="usage: $0 <seance> <decoder> <work dir> [<min SDR>]"
harness=${1:?$usage}
decoder=${2:?$usage}
work=${3:?$usage}
min=${4:-40}
emu=${VFW_EMULATOR:-}
failures=0

for tool in ffmpeg ffprobe; do
	if ! command -v $tool >/dev/null 2>&1; then
		echo "audio_codecs: $tool not found" >&2
		exit 77
	fi
done
rm -rf "$work"
mkdir -p "$work"

# sdr <a> <b> <format> <rate> <channels>: the lowest SDR of the channels
sdr() {
	ffmpeg -hide_banner -f "$3" -ar "$4" -ac "$5" -i "$1" -f "$3" -ar "$4" \
		-ac "$5" -i "$2" -lavfi "[0][1]asdr" -f null - 2>&1 |
		sed -n 's/.*SDR ch[0-9]*: \([^ ]*\) dB.*/\1/p' |
		awk '/nan/ { next }	# a channel silent in both
		     low == "" || ($1 != "inf" && (low == "inf" || $1 + 0 < low + 0)) \
		     { low = $1 } END { print low == "" ? "inf" : low }'
}

# make <name> <channels> <rate> <encoder> [<options>...]: name.avi, and
# name.ref.f32/.s16, FFmpeg's decoding. Opus goes through Matroska (whose
# end trimming AVI can't carry) and into AVI with its tag.
make() {
	m_name=$1 m_ch=$2 m_rate=$3 m_enc=$4
	shift 4
	# A different tone in each channel, so a swapped pair shows.
	m_graph="[0][1]amerge=inputs=2"
	if [ "$m_ch" = 1 ]; then
		m_graph="[0]anull"
	elif [ "$m_ch" = 6 ]; then
		m_graph="[0][1][2][3][4][5]amerge=inputs=6,aformat=channel_layouts=5.1"
	fi
	m_tone=":duration=2:sample_rate=$m_rate"
	set -- -f lavfi -i "sine=frequency=440$m_tone" \
		-f lavfi -i "sine=frequency=550$m_tone" \
		-f lavfi -i "sine=frequency=660$m_tone" \
		-f lavfi -i "sine=frequency=110$m_tone" \
		-f lavfi -i "sine=frequency=880$m_tone" \
		-f lavfi -i "sine=frequency=990$m_tone" \
		-filter_complex "$m_graph" -c:a $m_enc "$@"
	m_from=$work/$m_name.avi
	if [ "$m_enc" = libopus ]; then
		m_from=$work/$m_name.mka
		ffmpeg -v error -y "$@" "$m_from" 2>/dev/null &&
		ffmpeg -v error -y -i "$m_from" -c copy -tag:a 0x704F \
			"$work/$m_name.avi" 2>/dev/null
	else
		ffmpeg -v error -y "$@" "$m_from" 2>/dev/null
	fi || return 1
	ffmpeg -v error -y -i "$m_from" -f f32le "$work/$m_name.ref.f32" &&
	ffmpeg -v error -y -i "$m_from" -f s16le "$work/$m_name.ref.s16"
}

# check <name> <channels> <rate>
check() {
	c_name=$1 ch=$2 rate=$3
	for fmt in f32 s16; do
		out=$work/$c_name.$fmt
		VFW_TIMEOUT=30 $emu "$harness" decode "$decoder" "$work/$c_name.avi" \
			-o "$out" --format pcm --sample-format $fmt \
			>"$work/$c_name.$fmt.log" 2>&1
		rc=$?
		# A sanitizer's report (under the asan preset) is a crash too.
		if [ $rc -ge 124 ] ||
		   grep -q 'Sanitizer\|runtime error:' "$work/$c_name.$fmt.log"; then
			echo "FAIL $c_name ($fmt): the harness crashed or hung (exit $rc)"
			failures=$((failures + 1))
			continue
		fi
		if [ $rc -ne 0 ]; then
			echo "skip $c_name ($fmt): $(grep 'seance:' "$work/$c_name.$fmt.log" |
				head -n 1 | sed 's/^seance: //')"
			return
		fi
		ref=$work/$c_name.ref.$fmt
		if [ ! -s "$out" ]; then
			echo "FAIL $c_name ($fmt): nothing decoded"
			failures=$((failures + 1))
			continue
		fi
		f=${fmt}le
		[ $fmt = f32 ] || f=s16le
		value=$(sdr "$out" "$ref" $f $rate $ch)
		got=$(wc -c <"$out" | tr -d ' ')
		want=$(wc -c <"$ref" | tr -d ' ')
		size=$(($([ $fmt = f32 ] && echo 4 || echo 2) * ch))
		note=""
		[ "$got" = "$want" ] ||
			note=", $((got / size)) samples for FFmpeg's $((want / size))"
		changes=$(sed -n 's/.*changed its output format \([0-9]*\) time.*/, \1 format change(s)/p' \
			"$work/$c_name.$fmt.log")
		# IMA ADPCM decoders round its nibbles differently, more so
		# as a block goes on: libavcodec changed how, between versions.
		floor=$min
		case $c_name in adpcm_ima-*) floor=30 ;; esac
		if [ "$value" != inf ] && [ "$value" != "?" ] &&
		   awk "BEGIN { exit !($value < $floor) }"; then
			echo "FAIL $c_name ($fmt): SDR $value dB$note$changes"
			failures=$((failures + 1))
		else
			echo "ok   $c_name ($fmt): SDR $value dB$note$changes"
		fi
	done
}

for codec in \
	"mp3 libmp3lame -b:a 192k" \
	"mp2 mp2 -b:a 256k" \
	"ac3 ac3 -b:a 384k" \
	"eac3 eac3 -b:a 384k" \
	"aac aac -b:a 192k" \
	"flac flac" \
	"vorbis libvorbis -q:a 5" \
	"opus libopus -b:a 192k" \
	"wma wmav2 -b:a 192k" \
	"adpcm_ms adpcm_ms" \
	"adpcm_ima adpcm_ima_wav" \
	"alaw pcm_alaw" \
	"mulaw pcm_mulaw" \
	"gsm libgsm_ms"; do
	shapes="2:44100 6:48000"
	case $codec in gsm*) shapes="1:8000" ;; esac
	for shape in $shapes; do
		ch=${shape%:*} rate=${shape#*:}
		set -- $codec
		label=$1-${ch}ch
		shift
		if ! make $label $ch $rate "$@"; then
			echo "skip $label: FFmpeg can't make it in AVI"
			continue
		fi
		check $label $ch $rate
	done
done

echo "audio_codecs: $failures failure(s)"
[ $failures -eq 0 ]
