#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Makes short test streams with the system's ffmpeg, for `seance sweep
# --samples DIR` to give the decoders that nothing in a sweep encodes for:
# AVI clips under the FOURCCs decoders register (MJPG, XVID, DIV3, WMV2,
# H264, dvsd, CVID, ...) and WAV files under the format tags (mu-law,
# MS ADPCM, GSM 6.10, MP3, AC3, WMA, ...). Each comes with ffmpeg's own
# decode of it, which is what a decoder's output is measured against:
# NAME.y4m (I420) for video, NAME.ref.wav (16-bit PCM) for audio.
#
# DIR/index.tsv lists them, a line each:
#   name  video|audio  FOURCC or 0xTAG  file  reference
#
# An encoder this ffmpeg lacks is left out. Exits 77 (skipped) without
# ffmpeg. Nothing it makes is committed: generate it when it's wanted.
#
# usage: tests/gen_samples.sh <dir>
set -u

dir=${1:?usage: $0 <dir>}
ff=${FFMPEG:-ffmpeg}
frames=10

if ! command -v "$ff" >/dev/null 2>&1; then
	echo "gen_samples: no ffmpeg; skipped" >&2
	exit 77
fi
mkdir -p "$dir" || exit 1
index="$dir/index.tsv"
: >"$index" || exit 1
made=0
skipped=0

run_ff() {
	"$ff" -hide_banner -loglevel error -nostdin -y "$@"
}

# video NAME FOURCC SIZE PIX_FMT ENCODER [encoder options...]
video() {
	name=$1 tag=$2 size=$3 pix=$4 enc=$5
	shift 5
	file="$dir/$name.avi"
	if ! run_ff -f lavfi -i "testsrc2=size=$size:rate=25" -frames:v $frames \
		-pix_fmt "$pix" -c:v "$enc" "$@" -vtag "$tag" "$file" \
		2>"$dir/$name.log"; then
		echo "gen_samples: $name: $(tail -n 1 "$dir/$name.log")" >&2
		rm -f "$file"
		skipped=$((skipped + 1))
		return
	fi
	if ! run_ff -i "$file" -pix_fmt yuv420p -f yuv4mpegpipe \
		"$dir/$name.y4m" 2>"$dir/$name.log"; then
		echo "gen_samples: $name: ffmpeg can't decode it back" >&2
		rm -f "$file" "$dir/$name.y4m"
		skipped=$((skipped + 1))
		return
	fi
	rm -f "$dir/$name.log"
	printf '%s\tvideo\t%s\t%s\t%s\n' "$name" "$tag" "$name.avi" \
		"$name.y4m" >>"$index"
	made=$((made + 1))
}

# audio NAME 0xTAG RATE CHANNELS ENCODER [encoder options...]
audio() {
	name=$1 tag=$2 rate=$3 ch=$4 enc=$5
	shift 5
	file="$dir/$name.wav"
	if ! run_ff -f lavfi -i \
		"aevalsrc=0.4*sin(2*PI*440*t)+0.2*sin(2*PI*(200+600*t)*t):s=$rate:d=2" \
		-ac "$ch" -c:a "$enc" "$@" "$file" 2>"$dir/$name.log"; then
		echo "gen_samples: $name: $(tail -n 1 "$dir/$name.log")" >&2
		rm -f "$file"
		skipped=$((skipped + 1))
		return
	fi
	if ! run_ff -i "$file" -c:a pcm_s16le "$dir/$name.ref.wav" \
		2>"$dir/$name.log"; then
		echo "gen_samples: $name: ffmpeg can't decode it back" >&2
		rm -f "$file" "$dir/$name.ref.wav"
		skipped=$((skipped + 1))
		return
	fi
	rm -f "$dir/$name.log"
	printf '%s\taudio\t%s\t%s\t%s\n' "$name" "$tag" "$name.wav" \
		"$name.ref.wav" >>"$index"
	made=$((made + 1))
}

q=320x240 cif=352x288

# Lossless and intra
video mjpg MJPG $q yuvj420p mjpeg -q:v 3
video hfyu HFYU $q yuv422p huffyuv
video ffvh FFVH $q yuv420p ffvhuff
video ffv1 FFV1 $q yuv420p ffv1
video uly0 ULY0 $q yuv420p utvideo
video uly2 ULY2 $q yuv422p utvideo
video magy MAGY $q yuv420p magicyuv
video mjls MJLS $q yuv420p jpegls
video ljpg LJPG $q yuvj420p ljpeg
video zlib ZLIB $q bgr24 zlib
video zmbv ZMBV $q bgr24 zmbv
video mpng MPNG $q rgb24 png
video asv1 ASV1 $q yuv420p asv1
video asv2 ASV2 $q yuv420p asv2
video cfhd CFHD $q yuv422p10le cfhd
video v210 v210 $q yuv422p10le v210
video y41p Y41P $q yuv411p y41p
video yuy2 YUY2 $q yuyv422 rawvideo
video uyvy UYVY $q uyvy422 rawvideo
video yvyu YVYU $q yvyu422 rawvideo
# Old intra/temporal codecs
video cvid cvid $q rgb24 cinepak
video cram CRAM $q rgb555le msvideo1
video msvc MSVC $q rgb555le msvideo1
video mrle MRLE $q pal8 msrle
video dvsd dvsd 720x480 yuv411p dvvideo
video snow SNOW $q yuv420p snow
# MPEG-4 part 2 under its many FOURCCs
for t in XVID DIVX DX50 FMP4 MP4S M4S2 RMP4 3IV2; do
	video "mpeg4_$t" $t $q yuv420p mpeg4 -bf 0 -q:v 3
done
video div3 DIV3 $q yuv420p msmpeg4 -q:v 3
video mp43 MP43 $q yuv420p msmpeg4 -q:v 3
video mp42 MP42 $q yuv420p msmpeg4v2 -q:v 3
video wmv1 WMV1 $q yuv420p wmv1 -q:v 3
video wmv2 WMV2 $q yuv420p wmv2 -q:v 3
video h261 H261 $cif yuv420p h261
video h263 H263 $cif yuv420p h263 -q:v 3
video flv1 FLV1 $q yuv420p flv -q:v 3
video mpg1 mpg1 $q yuv420p mpeg1video -bf 0 -q:v 3
video mpg2 MPG2 $q yuv420p mpeg2video -bf 0 -q:v 3
for t in H264 X264 AVC1; do
	video "h264_$t" $t $q yuv420p libx264 -bf 0 -crf 18 -preset fast
done
video vp80 VP80 $q yuv420p libvpx -crf 10 -b:v 1M

# Audio, under the format tags of the WAVs ffmpeg writes
audio ulaw 0x0007 8000 1 pcm_mulaw
audio alaw 0x0006 8000 1 pcm_alaw
audio msadpcm 0x0002 22050 1 adpcm_ms
audio imaadpcm 0x0011 22050 1 adpcm_ima_wav
audio yamaha 0x0020 22050 1 adpcm_yamaha
audio gsm 0x0031 8000 1 libgsm_ms
audio g726 0x0045 8000 1 g726 -b:a 32k
audio g723 0x0042 8000 1 g723_1 -b:a 6300
audio g722 0x028f 16000 1 g722
audio mp2 0x0050 44100 2 mp2 -b:a 192k
audio mp3 0x0055 44100 2 libmp3lame -b:a 128k
audio ac3 0x2000 44100 2 ac3 -b:a 192k
audio wma1 0x0160 44100 2 wmav1 -b:a 128k
audio wma2 0x0161 44100 2 wmav2 -b:a 128k
audio flac 0xf1ac 44100 2 flac
audio float 0x0003 44100 2 pcm_f32le

echo "gen_samples: $made samples in $dir ($skipped left out)"
[ $made -gt 0 ]
