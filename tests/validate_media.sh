#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Cross-checks the AVI reader, the AVI writer and the output writers against
# FFmpeg:
#   - FFmpeg reads the AVIs our writer produced (AVI 1.0 and OpenDML) and
#     the Y4M and BMP files the output writers produced;
#   - `seance convert` on AVIs FFmpeg wrote in each uncompressed format
#     gives the same pictures FFmpeg's own decoding does;
#   - FFmpeg reads the WAVs and audio streams our writers produced, and
#     `seance convert` gives the same samples as FFmpeg from WAVs and AVIs
#     FFmpeg wrote, in every PCM format, with MP3 audio copied as it is;
#   - with the fixture DLLs built, `encode` with testcodec.dll takes what
#     FFmpeg writes (Y4M, BMP files, AVI) and writes AVIs FFmpeg reads, and
#     `decode` gives back FFmpeg's pictures exactly.
#
# usage: tests/validate_media.sh <build dir> <work dir>
set -eu

build=${1:?usage: $0 <build dir> <work dir>}
work=${2:?usage: $0 <build dir> <work dir>}
harness=$build/seance
# On a cross build, what runs i386 programs (qemu-i386), from CMake.
emu=${VFW_EMULATOR:-}
failures=0

for tool in ffmpeg ffprobe; do
	if ! command -v $tool >/dev/null 2>&1; then
		echo "validate_media: $tool not found" >&2
		exit 77
	fi
done

rm -rf "$work"
mkdir -p "$work/unit" "$work/ff"

pass() { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# expect <what> <actual> <wanted>
expect() {
	if [ "$2" = "$3" ]; then pass "$1: $2"; else fail "$1: got '$2', wanted '$3'"; fi
}

# packets <file>: "<packets> <keyframes> <bytes>" of the first video stream
packets() {
	ffprobe -v error -select_streams v:0 -show_entries packet=size,flags \
		-of csv=p=0 "$1" |
		awk -F, '{ n++; s += $1; if ($2 ~ /K/) k++ } END { print n+0, k+0, s+0 }'
}

# frames <file>: decoded video frames
frames() {
	ffprobe -v error -count_frames -select_streams v:0 \
		-show_entries stream=nb_read_frames -of csv=p=0 "$1"
}

# size <file>: WxH
size() {
	ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
		-of csv=s=x:p=0 "$1"
}

# psnr <a> <b>: the average PSNR between two videos (inf when identical)
psnr() {
	ffmpeg -hide_banner -i "$1" -i "$2" -lavfi psnr -f null - 2>&1 |
		sed -n 's/.*PSNR.* average:\([^ ]*\).*/\1/p' | tail -n 1
}

# at_least <what> <psnr> <minimum>
at_least() {
	if [ "$2" = inf ] || awk "BEGIN { exit !($2 >= $3) }"; then
		pass "$1: PSNR $2"
	else
		fail "$1: PSNR $2 is below $3"
	fi
}

echo "== Files written by the unit tests"
VFW_KEEP_TEST_FILES=$work/unit $emu "$build/tests/test_avi" >/dev/null
VFW_KEEP_TEST_FILES=$work/unit $emu "$build/tests/test_image" >/dev/null
VFW_KEEP_TEST_FILES=$work/unit $emu "$build/tests/test_wav" >/dev/null
u=$work/unit
# test_avi: frames, keyframes and bytes as written.
expect "plain.avi" "$(packets $u/plain.avi)" "20 20 30720"
expect "plain.avi decodes" "$(frames $u/plain.avi)" 20
expect "idx1abs.avi" "$(packets $u/idx1abs.avi)" "7 7 1344"
expect "noindex.avi" "$(packets $u/noindex.avi)" "9 9 1728"
expect "odml1.avi (OpenDML, one RIFF)" "$(packets $u/odml1.avi)" "12 12 2304"
expect "odml.avi (OpenDML, AVIX)" "$(packets $u/odml.avi)" "150 15 450000"
# test_image: 3 frames and a repeat, 33x17.
expect "out.y4m frames" "$(frames $u/out.y4m)" 4
expect "out.y4m size" "$(size $u/out.y4m)" 33x17
expect "out002.bmp size" "$(size $u/out002.bmp)" 33x17
ffmpeg -v error -y -i $u/out002.bmp -f rawvideo -pix_fmt bgr24 $u/bmp.bgr
ffmpeg -v error -y -f rawvideo -pix_fmt bgr24 -s 33x17 -i $u/out.bgr \
	-vf "select=eq(n\,2)" -f rawvideo -pix_fmt bgr24 $u/third.bgr
if cmp -s $u/bmp.bgr $u/third.bgr; then
	pass "out002.bmp matches the third BGR frame"
else
	fail "out002.bmp differs from the third BGR frame"
fi

echo "== Converting AVIs written by FFmpeg"
f=$work/ff
src="testsrc2=size=64x48:rate=10:duration=1"

# make <name> <ffmpeg output options...>
make_avi() {
	name=$1
	shift
	ffmpeg -v error -y -f lavfi -i "$src" "$@" "$f/$name.avi"
	if ! $emu "$harness" avi "$f/$name.avi" >"$f/$name.txt"; then
		fail "$name: seance avi"
	fi
}

# Uncompressed YUV: our Y4M must be FFmpeg's, exactly.
for fmt in yuyv422 uyvy422 yuv420p nv12 nv21; do
	make_avi $fmt -c:v rawvideo -pix_fmt $fmt
	$emu "$harness" convert $f/$fmt.avi -o $f/$fmt.y4m 2>/dev/null
	ffmpeg -v error -y -i $f/$fmt.avi -pix_fmt yuv420p -strict -1 $f/$fmt.ref.y4m
	at_least "$fmt -> Y4M" "$(psnr $f/$fmt.y4m $f/$fmt.ref.y4m)" inf
done
make_avi yv12 -c:v rawvideo -pix_fmt yuv420p -vtag YV12
$emu "$harness" convert $f/yv12.avi -o $f/yv12.y4m 2>/dev/null
ffmpeg -v error -y -i $f/yv12.avi -pix_fmt yuv420p -strict -1 $f/yv12.ref.y4m
at_least "YV12 -> Y4M" "$(psnr $f/yv12.y4m $f/yv12.ref.y4m)" inf
# FFmpeg treats Y800 as full range and resamples YVYU chroma differently.
make_avi gray -c:v rawvideo -pix_fmt gray
$emu "$harness" convert $f/gray.avi -o $f/gray.y4m 2>/dev/null
ffmpeg -v error -y -i $f/gray.avi -vf scale=in_range=limited:out_range=limited \
	-pix_fmt yuv420p -strict -1 $f/gray.ref.y4m
at_least "Y800 -> Y4M" "$(psnr $f/gray.y4m $f/gray.ref.y4m)" inf
make_avi yvyu422 -c:v rawvideo -pix_fmt yvyu422
$emu "$harness" convert $f/yvyu422.avi -o $f/yvyu422.y4m 2>/dev/null
ffmpeg -v error -y -i $f/yvyu422.avi -pix_fmt yuv420p -strict -1 $f/yvyu422.ref.y4m
at_least "YVYU -> Y4M" "$(psnr $f/yvyu422.y4m $f/yvyu422.ref.y4m)" 40

# RGB, palettes and RLE: our BGR24 must be FFmpeg's, exactly; Y4M close
# (FFmpeg's RGB to YUV rounds differently).
for fmt in bgr24 pal8 rgb555le msrle bgr24_odd; do
	case $fmt in
	msrle)     make_avi $fmt -c:v msrle -pix_fmt pal8 ;;
	bgr24_odd) ffmpeg -v error -y -f lavfi -i testsrc2=size=63x47:rate=10:duration=1 \
			-c:v rawvideo -pix_fmt bgr24 $f/$fmt.avi ;;
	*)         make_avi $fmt -c:v rawvideo -pix_fmt $fmt ;;
	esac
	$emu "$harness" convert $f/$fmt.avi -o $f/$fmt.bgr 2>/dev/null
	ffmpeg -v error -y -i $f/$fmt.avi -f rawvideo -pix_fmt bgr24 $f/$fmt.ref.bgr
	if cmp -s $f/$fmt.bgr $f/$fmt.ref.bgr; then
		pass "$fmt -> BGR24 identical"
	else
		fail "$fmt -> BGR24 differs"
	fi
	$emu "$harness" convert $f/$fmt.avi -o $f/$fmt.y4m 2>/dev/null
	ffmpeg -v error -y -i $f/$fmt.avi -pix_fmt yuv420p -strict -1 $f/$fmt.ref.y4m
	at_least "$fmt -> Y4M" "$(psnr $f/$fmt.y4m $f/$fmt.ref.y4m)" 30
done

echo "== convert options"
$emu "$harness" convert $f/bgr24.avi -o $f/part.y4m --start 3 --frames 4 2>/dev/null
expect "--start 3 --frames 4" "$(frames $f/part.y4m)" 4
ffmpeg -v error -y -i $f/bgr24.avi -vf "select=between(n\,3\,6)" -fps_mode passthrough \
	-f rawvideo -pix_fmt bgr24 $f/part.ref.bgr
$emu "$harness" convert $f/bgr24.avi -o $f/part.bgr --start 3 --frames 4 2>/dev/null
if cmp -s $f/part.bgr $f/part.ref.bgr; then
	pass "--start/--frames pick the right frames"
else
	fail "--start/--frames picked the wrong frames"
fi
$emu "$harness" convert $f/msrle.avi -o "$f/rle%02d.bmp" --start 5 --frames 2 2>/dev/null
ffmpeg -v error -y -i $f/rle01.bmp -f rawvideo -pix_fmt bgr24 $f/rle01.bgr
ffmpeg -v error -y -i $f/msrle.avi -vf "select=eq(n\,6)" -fps_mode passthrough \
	-f rawvideo -pix_fmt bgr24 $f/rle6.ref.bgr
if cmp -s $f/rle01.bgr $f/rle6.ref.bgr; then
	pass "RLE from frame 5 to BMP (delta frames replayed)"
else
	fail "RLE from frame 5 to BMP differs"
fi
$emu "$harness" convert $f/bgr24.avi -o - --format y4m 2>/dev/null >$f/stdout.y4m
expect "Y4M on stdout" "$(frames $f/stdout.y4m)" 10

echo "== Our AVI writer's files through seance"
$emu "$harness" avi $u/odml.avi >$f/odml.txt
if grep -q "Index: OpenDML" $f/odml.txt && grep -q "150 frames" $f/odml.txt; then
	pass "seance avi odml.avi"
else
	fail "seance avi odml.avi"
fi

# audio <file> <entries>: the first audio stream's fields, comma-separated
audio() {
	ffprobe -v error -select_streams a:0 -show_entries stream=$2 \
		-of csv=p=0 "$1"
}

# bytes <file> <format>: the size of the first audio stream decoded
bytes() {
	ffmpeg -v error -i "$1" -map 0:a:0 -f "$2" - | wc -c | tr -d ' '
}

# same <what> <a> <b>
same() {
	if cmp -s "$2" "$3"; then pass "$1"; else fail "$1"; fi
}

echo "== Audio our writers made, read by FFmpeg"
expect "av.avi audio" "$(audio $u/av.avi codec_name,sample_rate,channels)" \
	"pcm_s16le,44100,2"
expect "av.avi samples" "$(bytes $u/av.avi s16le)" $((17640 * 4))
expect "av.avi video still" "$(packets $u/av.avi | cut -d' ' -f1)" 10
expect "av_odml.avi (OpenDML) samples" "$(bytes $u/av_odml.avi s16le)" \
	$((105840 * 4))
expect "av_odml.avi video" "$(packets $u/av_odml.avi | cut -d' ' -f1)" 60
expect "audio.avi (ADPCM blocks)" "$(audio $u/audio.avi codec_name,channels)" \
	"adpcm_ima_wav,1"
expect "s16.wav" "$(audio $u/s16.wav codec_name,sample_rate,channels)" \
	"pcm_s16le,44100,2"
expect "s16.wav samples" "$(bytes $u/s16.wav s16le)" 40000
expect "rf64.wav (RF64)" "$(audio $u/rf64.wav codec_name,channels)" \
	"pcm_f32le,6"
expect "rf64.wav samples" "$(bytes $u/rf64.wav f32le)" 12000

echo "== Converting audio FFmpeg wrote"
a=$work/audio
mkdir -p $a
tone="sine=frequency=440:duration=1:sample_rate=48000"
for fmt in u8 s16le s24le s32le f32le f64le; do
	ffmpeg -v error -y -f lavfi -i "$tone" -ac 2 -c:a pcm_$fmt $a/$fmt.wav
	ffmpeg -v error -y -i $a/$fmt.wav -f $fmt $a/$fmt.ref
	$emu "$harness" convert $a/$fmt.wav -o $a/$fmt.pcm 2>/dev/null
	same "$fmt WAV -> raw PCM identical" $a/$fmt.pcm $a/$fmt.ref
done
ffmpeg -v error -y -f lavfi -i "$tone" -ac 6 -c:a pcm_s16le $a/six.wav
ffmpeg -v error -y -i $a/six.wav -f s16le $a/six.ref
$emu "$harness" convert $a/six.wav -o $a/six.pcm 2>/dev/null
same "6-channel (extensible) WAV -> raw PCM identical" $a/six.pcm $a/six.ref
ffmpeg -v error -y -f lavfi -i "$tone" -c:a pcm_s16le -rf64 always $a/rf64.wav
$emu "$harness" convert $a/rf64.wav -o $a/rf64.pcm 2>/dev/null
ffmpeg -v error -y -i $a/rf64.wav -f s16le $a/rf64.ref
same "FFmpeg's RF64 WAV -> raw PCM identical" $a/rf64.pcm $a/rf64.ref

# Sample formats, converted as FFmpeg converts them.
for to in u8 s24le f32le; do
	$emu "$harness" convert $a/s16le.wav -o $a/to_$to.pcm \
		--sample-format ${to%le} 2>/dev/null
	ffmpeg -v error -y -i $a/s16le.wav -f $to $a/to_$to.ref
	same "s16 -> $to as FFmpeg converts" $a/to_$to.pcm $a/to_$to.ref
done
$emu "$harness" convert $a/f32le.wav -o $a/f2s.pcm --sample-format s16 \
	2>/dev/null
ffmpeg -v error -y -i $a/f32le.wav -f s16le $a/f2s.ref
same "f32 -> s16 as FFmpeg converts" $a/f2s.pcm $a/f2s.ref

# An AVI with video and PCM audio: either stream, and trimmed audio.
ffmpeg -v error -y -f lavfi -i "$src" -f lavfi -i "$tone" -c:v rawvideo \
	-pix_fmt bgr24 -c:a pcm_s16le -ac 2 $a/av.avi
$emu "$harness" avi $a/av.avi >$a/av.txt
if grep -q "PCM s16, 2 channels, 48000 Hz" $a/av.txt; then
	pass "seance avi shows the PCM stream"
else
	fail "seance avi doesn't show the PCM stream"
fi
$emu "$harness" convert $a/av.avi -o $a/av.wav 2>/dev/null
ffmpeg -v error -y -i $a/av.avi -map 0:a -f s16le $a/av.ref
ffmpeg -v error -y -i $a/av.wav -f s16le $a/av.wav.pcm
same "AVI PCM -> WAV" $a/av.wav.pcm $a/av.ref
$emu "$harness" convert $a/av.avi -o $a/av.y4m 2>/dev/null
expect "the same AVI to Y4M is its video" "$(frames $a/av.y4m)" 10
$emu "$harness" convert $a/av.avi -o $a/trim.pcm --audio --start 0.25s \
	--duration 12000 2>/dev/null
ffmpeg -v error -y -i $a/av.avi -map 0:a -af atrim=start_sample=12000:end_sample=24000 \
	-f s16le $a/trim.ref
same "--start 0.25s --duration 12000 trims to the sample" $a/trim.pcm $a/trim.ref
$emu "$harness" convert $a/av.wav -o $a/av2.avi 2>/dev/null
ffmpeg -v error -y -i $a/av2.avi -f s16le $a/av2.pcm
same "WAV -> our AVI, as FFmpeg reads it" $a/av2.pcm $a/av.ref

# MP3 in AVI, copied without decoding: FFmpeg decodes the same audio.
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libmp3lame; then
	ffmpeg -v error -y -f lavfi -i "$src" -f lavfi -i "$tone" -c:v rawvideo \
		-pix_fmt bgr24 -c:a libmp3lame -b:a 128k $a/mp3.avi
	$emu "$harness" avi $a/mp3.avi >$a/mp3.txt
	if grep -q "MP3 (0x0055)" $a/mp3.txt; then
		pass "seance avi names MP3"
	else
		fail "seance avi doesn't name MP3"
	fi
	ffmpeg -v error -y -i $a/mp3.avi -map 0:a -f s16le $a/mp3.ref
	$emu "$harness" convert $a/mp3.avi -o $a/mp3copy.avi 2>/dev/null
	$emu "$harness" convert $a/mp3.avi -o $a/mp3.wav 2>/dev/null
	ffmpeg -v error -y -i $a/mp3copy.avi -f s16le $a/mp3copy.pcm
	ffmpeg -v error -y -i $a/mp3.wav -f s16le $a/mp3wav.pcm
	same "MP3 AVI -> AVI, decoded the same" $a/mp3copy.pcm $a/mp3.ref
	same "MP3 AVI -> WAV, decoded the same" $a/mp3wav.pcm $a/mp3.ref
else
	echo "skip MP3 cases: no libmp3lame in this FFmpeg"
fi
# TSTA through testfilter.ax: FFmpeg's PCM back exactly, and FFmpeg reads
# the AVI the encoder's packets went into.
filter=$build/fixtures/testfilter.ax
if [ -f "$filter" ]; then
	$emu "$harness" encode "$filter" $a/s24le.wav -o $a/tsta.avi 2>/dev/null ||
		fail "encode TSTA"
	$emu "$harness" decode "$filter" $a/tsta.avi -o $a/tsta.pcm \
		--sample-format s24 2>/dev/null || fail "decode TSTA"
	same "s24 WAV -> TSTA -> s24 identical" $a/tsta.pcm $a/s24le.ref
	expect "TSTA AVI: FFmpeg sees its packets" \
		"$(ffprobe -v error -select_streams a:0 -show_entries packet=size \
		-of csv=p=0 $a/tsta.avi | wc -l | tr -d ' ')" 47
	expect "TSTA AVI: tag and rate" \
		"$(audio $a/tsta.avi codec_tag_string,sample_rate,channels)" \
		"Ts[0][0],48000,2"
	$emu "$harness" encode "$filter" $a/six.wav -o $a/six.avi 2>/dev/null
	$emu "$harness" decode "$filter" $a/six.avi -o $a/six-tsta.pcm 2>/dev/null
	same "6-channel WAV -> TSTA -> PCM identical" $a/six-tsta.pcm $a/six.ref
	dmo=$build/fixtures/testdmo.dll
	$emu "$harness" encode "$dmo" $a/s24le.wav -o $a/tsta-dmo.avi 2>/dev/null
	same "TSTA DMO's AVI is the filter's" $a/tsta-dmo.avi $a/tsta.avi
	$emu "$harness" decode "$dmo" $a/six.avi -o $a/six-dmo.pcm 2>/dev/null
	same "6-channel TSTA -> DMO -> PCM identical" $a/six-dmo.pcm $a/six.ref
fi
echo

codec=$build/fixtures/testcodec.dll
if [ -f "$codec" ]; then
	echo "== encode and decode with testcodec.dll"
	e=$work/enc
	mkdir -p $e
	ffmpeg -v error -y -f lavfi -i "$src" -pix_fmt yuv420p $e/in.y4m
	$emu "$harness" encode $codec $e/in.y4m -o $e/t.avi --keyint 5 2>/dev/null ||
		fail "encode Y4M"
	expect "encoded AVI: packets and keyframes" \
		"$(packets $e/t.avi | cut -d' ' -f1-2)" "10 2"
	expect "encoded AVI: size" "$(size $e/t.avi)" 64x48
	expect "encoded AVI: FOURCC" "$(ffprobe -v error -select_streams v:0 \
		-show_entries stream=codec_tag_string -of csv=p=0 $e/t.avi)" TST0
	$emu "$harness" decode $codec $e/t.avi -o $e/out.y4m 2>/dev/null || fail "decode"
	at_least "Y4M -> TST0 -> Y4M" "$(psnr $e/out.y4m $e/in.y4m)" inf
	filter=$build/fixtures/testfilter.ax
	if [ -f "$filter" ]; then
		$emu "$harness" decode "$filter" $e/t.avi -o $e/dshow-out.y4m \
			2>/dev/null || fail "DirectShow decode"
		at_least "VfW encode -> DirectShow decode" \
			"$(psnr $e/dshow-out.y4m $e/in.y4m)" inf
		$emu "$harness" encode "$filter" $e/in.y4m -o $e/dshow.avi \
			--keyint 5 2>/dev/null || fail "DirectShow encode"
		expect "DirectShow encoded AVI: packets and keyframes" \
			"$(packets $e/dshow.avi | cut -d' ' -f1-2)" "10 2"
		$emu "$harness" decode $codec $e/dshow.avi \
			-o $e/dshow-vfw-out.y4m 2>/dev/null || fail "VfW decode"
		at_least "DirectShow encode -> VfW decode" \
			"$(psnr $e/dshow-vfw-out.y4m $e/in.y4m)" inf
	fi
	$emu "$harness" encode $codec - -o $e/stdin.avi <$e/in.y4m 2>/dev/null
	expect "Y4M from stdin" "$(packets $e/stdin.avi | cut -d' ' -f1)" 10
	$emu "$harness" encode $codec $e/in.y4m -o $e/odml.avi --odml 2>/dev/null
	expect "OpenDML output" "$(packets $e/odml.avi | cut -d' ' -f1)" 10

	# BMP files numbered from 1, and an AVI, both BGR24: exact.
	ffmpeg -v error -y -f lavfi -i "$src" -pix_fmt bgr24 "$e/f%03d.bmp"
	ffmpeg -v error -y -f lavfi -i "$src" -f rawvideo -pix_fmt bgr24 $e/ref.bgr
	$emu "$harness" encode $codec "$e/f%03d.bmp" -o $e/bmp.avi 2>/dev/null
	$emu "$harness" decode $codec $e/bmp.avi -o $e/bmp.bgr 2>/dev/null
	if cmp -s $e/bmp.bgr $e/ref.bgr; then
		pass "BMP files -> TST0 -> BGR24 identical"
	else
		fail "BMP files -> TST0 -> BGR24 differ"
	fi
	$emu "$harness" encode $codec $f/bgr24_odd.avi -o $e/odd.avi 2>/dev/null
	$emu "$harness" decode $codec $e/odd.avi -o $e/odd.bgr 2>/dev/null
	if cmp -s $e/odd.bgr $f/bgr24_odd.ref.bgr; then
		pass "odd-sized BGR24 AVI -> TST0 -> BGR24 identical"
	else
		fail "odd-sized BGR24 AVI -> TST0 -> BGR24 differs"
	fi
fi

echo
if [ $failures -ne 0 ]; then
	echo "validate_media: $failures check(s) failed"
	exit 1
fi
echo "validate_media: all checks passed"
