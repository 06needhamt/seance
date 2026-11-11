#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""An AVI of audio ffmpeg won't write into one (ALAC, TrueHD, MLP: no wave
format tag stands for them): ffmpeg encodes into Matroska, and the packets
and the codec's extra data go into an AVI by hand, one packet a chunk,
under a tag of no meaning. seance's decode --subtype says what it is.

usage: mkavi_audio.py <in.mka> <out.avi>"""
import json, struct, subprocess, sys

TAG = 0x4143  # nobody's


def chunk(name, data):
    return name + struct.pack("<I", len(data)) + data + b"\0" * (len(data) & 1)


def build(src, dst):
    info = json.loads(subprocess.check_output(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_streams",
         "-show_packets", "-show_data", "-of", "json", src]))
    st = info["streams"][0]
    rate, channels = int(st["sample_rate"]), int(st["channels"])
    bits = int(st.get("bits_per_raw_sample") or 0) or 16
    extra = b""
    for line in st.get("extradata", "").splitlines():
        # "00000000: 0000 0024 616c 6163 ...  ...$alac"
        if ":" in line:
            extra += bytes.fromhex(line.split(":", 1)[1][:41].replace(" ", ""))
    sizes = [int(p["size"]) for p in info["packets"]]
    # Matroska gives no durations and rounds times to a millisecond:
    # the packets' length is the average over them all.
    pts = [float(p["pts_time"]) for p in info["packets"]]
    frame = round((pts[-1] - pts[0]) * rate / (len(pts) - 1))
    data = subprocess.check_output(
        ["ffmpeg", "-v", "fatal", "-i", src, "-map", "0:a:0", "-c", "copy",
         "-f", "data", "-"])
    assert sum(sizes) == len(data), "packets and data disagree"

    movi, index, at = b"movi", b"", 4
    for n in sizes:
        index += struct.pack("<4sIII", b"00wb", 0x10, at, n)
        c = chunk(b"00wb", data[:n])
        data = data[n:]
        movi += c
        at += len(c)

    byte_rate = sum(sizes) * rate // (len(sizes) * frame)
    wfx = struct.pack("<HHIIHHH", TAG, channels, rate, byte_rate, frame,
                      bits, len(extra)) + extra
    strh = struct.pack("<4s4sIHHIIIIIIIIHHHH", b"auds", b"\0\0\0\0", 0, 0, 0,
                       0, frame, rate, 0, len(sizes), max(sizes), 0xffffffff,
                       0, 0, 0, 0, 0)
    strl = b"strl" + chunk(b"strh", strh) + chunk(b"strf", wfx)
    avih = struct.pack("<14I", 1000000 * frame // rate, byte_rate, 0, 0x10,
                       len(sizes), 0, 1, max(sizes), 0, 0, 0, 0, 0, 0)
    hdrl = b"hdrl" + chunk(b"avih", avih) + chunk(b"LIST", strl)
    riff = (b"AVI " + chunk(b"LIST", hdrl) + chunk(b"LIST", movi) +
            chunk(b"idx1", index))
    with open(dst, "wb") as f:
        f.write(chunk(b"RIFF", riff))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2])
