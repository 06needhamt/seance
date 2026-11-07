#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""LAV Video matrix: encode with ffmpeg, decode with ffmpeg (reference) and
with LAVVideo.ax in seance64, compare by PSNR. Aliases of one codec are the
same bitstream with the FOURCC patched."""
import os, re, subprocess, sys, json, shutil

SEANCE = os.environ.get("SEANCE", "build/seance64")
LAV = os.path.join(os.environ.get("LAV_DIR", "."), "LAVVideo.ax")
W = os.path.join(os.environ.get("WORK", "/tmp/lav_matrix"), "v")
os.makedirs(W, exist_ok=True)

def run(cmd, timeout=120, env=None):
    e = dict(os.environ)
    e["VFW_TIMEOUT"] = "60"
    if env:
        e.update(env)
    try:
        p = subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True,
                           text=True, timeout=timeout, env=e)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return 124, "timeout"

# (label, source size, source pix_fmt, ffmpeg encoder args, tag, [alias tags])
V = [
 ("h264",   "176x144", "yuv420p", "-c:v libx264 -preset ultrafast -g 5 -bf 0", "H264", ["h264","X264","x264"]),
 ("h264-avc1", "176x144", "yuv420p", "-c:v libx264 -preset ultrafast -g 5 -bf 0", "avc1", ["AVC1","CCV1"]),
 ("h264-bframes", "176x144", "yuv420p", "-c:v libx264 -preset fast -g 10 -bf 2", "H264", []),
 ("h264-10bit", "176x144", "yuv420p10le", "-c:v libx264 -preset ultrafast -profile:v high10 -g 5 -bf 0", "H264", []),
 ("hevc",   "176x144", "yuv420p", "-c:v libx265 -preset ultrafast -x265-params log-level=none:keyint=5:bframes=0 -f hevc", "HEVC", ["HVC1","HM10","H265"]),
 ("hevc-10bit", "176x144", "yuv420p10le", "-c:v libx265 -preset ultrafast -x265-params log-level=none:keyint=5:bframes=0 -f hevc", "HEVC", []),
 ("mpeg1",  "176x144", "yuv420p", "-c:v mpeg1video -g 5 -bf 0", "MPG1", ["mpg1"]),
 ("mpeg2",  "176x144", "yuv420p", "-c:v mpeg2video -g 5 -bf 0", "MPG2", ["mpg2","MP2V"]),
 ("mpeg4-asp", "176x144", "yuv420p", "-c:v mpeg4 -g 5 -bf 0 -q:v 3", "FMP4",
     ["XVID","xvid","DIVX","divx","Divx","DX50","dx50","MP4V","mp4v","M4S2","m4s2","MP4S","mp4s","3IVX","3ivx","3IV1","3iv1","3IV2","3iv2","BLZ0","GEOV","SEDG","SMP4","DXGM"]),
 ("xvid-enc", "176x144", "yuv420p", "-c:v libxvid -g 5 -bf 0", "XVID", []),
 ("msmpeg4v2", "176x144", "yuv420p", "-c:v msmpeg4v2 -g 5", "MP42", ["mp42","DIV2","div2"]),
 ("msmpeg4v3", "176x144", "yuv420p", "-c:v msmpeg4 -g 5", "MP43", ["mp43","DIV3","div3","MPG3","mpg3","DIV4","div4","DIV5","div5","DIV6","div6","DVX3","dvx3","3IVD"]),
 ("msmpeg4v1-tag", "176x144", "yuv420p", "-c:v msmpeg4v2 -g 5", "MPG4", ["mpg4","MP41","mp41","DIV1","div1"]),
 ("wmv1",   "176x144", "yuv420p", "-c:v wmv1 -g 5", "WMV1", ["wmv1"]),
 ("wmv2",   "176x144", "yuv420p", "-c:v wmv2 -g 5", "WMV2", ["wmv2","2VMW"]),
 ("flv1",   "176x144", "yuv420p", "-c:v flv -g 5", "FLV1", ["flv1"]),
 ("h263",   "176x144", "yuv420p", "-c:v h263 -g 5", "H263", ["h263","s263","I263","i263"]),
 ("h263p",  "176x144", "yuv420p", "-c:v h263p -g 5", "H263", []),
 ("h261",   "176x144", "yuv420p", "-c:v h261 -g 5", "H261", ["h261"]),
 ("mjpeg",  "176x144", "yuvj420p", "-c:v mjpeg -q:v 3", "MJPG", ["jpeg","MJPA","MJPB","mjpb"]),
 ("ljpeg",  "176x144", "bgr24", "-c:v ljpeg", "LJPG", []),
 ("jpegls", "176x144", "yuv420p", "-c:v jpegls", "MJLS", ["JPGL"]),
 ("jpeg2000", "176x144", "yuv420p", "-c:v jpeg2000", "mjp2", ["MJ2C","LJ2C","LJ2K","IPJ2"]),
 ("dv-pal", "720x576", "yuv420p", "-c:v dvvideo -pix_fmt yuv420p", "dvsd", ["DVSD","CDVH","CDVC","CDV5","dv25","DV25","dvcp","dvpp","dvc ","AVdv","AVd1"]),
 ("dv-ntsc", "720x480", "yuv411p", "-c:v dvvideo -pix_fmt yuv411p", "dvsd", []),
 ("dv50",   "720x576", "yuv422p", "-c:v dvvideo -pix_fmt yuv422p", "dv50", ["DV50","dv5p","dv5n"]),
 ("svq1",   "176x144", "yuv410p", "-c:v svq1", "SVQ1", []),
 ("rv10",   "176x144", "yuv420p", "-c:v rv10", "RV10", []),
 ("rv20",   "176x144", "yuv420p", "-c:v rv20", "RV20", []),
 ("theora", "176x144", "yuv420p", "-c:v libtheora", "THEO", ["theo"]),
 ("vp8",    "176x144", "yuv420p", "-c:v libvpx -deadline realtime -cpu-used 8 -g 5", "VP80", ["VP70"]),
 ("vp9",    "176x144", "yuv420p", "-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -g 5 -lag-in-frames 0", "VP90", []),
 ("vp9-10bit", "176x144", "yuv420p10le", "-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -g 5 -lag-in-frames 0", "VP90", []),
 ("av1-aom","176x144", "yuv420p", "-c:v libaom-av1 -cpu-used 8 -g 5 -lag-in-frames 0 -b:v 0 -crf 40", "AV01", []),
 ("av1-svt","176x144", "yuv420p", "-c:v libsvtav1 -preset 12 -g 5", "AV01", []),
 ("prores-422hq", "176x144", "yuv422p10le", "-c:v prores_ks -profile:v 3", "apch", []),
 ("prores-422", "176x144", "yuv422p10le", "-c:v prores_ks -profile:v 2", "apcn", []),
 ("prores-lt", "176x144", "yuv422p10le", "-c:v prores_ks -profile:v 1", "apcs", []),
 ("prores-proxy", "176x144", "yuv422p10le", "-c:v prores_ks -profile:v 0", "apco", []),
 ("prores-4444", "176x144", "yuva444p10le", "-c:v prores_ks -profile:v 4", "ap4h", ["ap4x"]),
 ("huffyuv", "176x144", "yuv422p", "-c:v huffyuv", "HFYU", []),
 ("ffvhuff", "176x144", "yuv420p", "-c:v ffvhuff", "FFVH", []),
 ("ffv1",   "176x144", "yuv420p", "-c:v ffv1", "FFV1", []),
 ("ffv1-rgb", "176x144", "bgr0", "-c:v ffv1", "FFV1", []),
 ("snow",   "176x144", "yuv420p", "-c:v snow -strict -2", "SNOW", []),
 ("cinepak","176x144", "rgb24", "-c:v cinepak", "cvid", []),
 ("msvideo1","176x144", "rgb555le", "-c:v msvideo1", "CRAM", ["MSVC","WHAM"]),
 ("qtrle",  "176x144", "rgb24", "-c:v qtrle", "rle ", []),
 ("rpza",   "176x144", "rgb555be", "-c:v rpza", "rpza", []),
 ("smc",    "176x144", "pal8", "-c:v smc", "smc ", []),
 ("zlib",   "176x144", "bgr24", "-c:v zlib", "ZLIB", []),
 ("zmbv",   "176x144", "pal8", "-c:v zmbv", "ZMBV", []),
 ("utvideo-rgb", "176x144", "bgr24", "-c:v utvideo -pred median", "ULRG", []),
 ("utvideo-rgba","176x144", "bgra", "-c:v utvideo", "ULRA", []),
 ("utvideo-420", "176x144", "yuv420p", "-c:v utvideo", "ULY0", []),
 ("utvideo-422", "176x144", "yuv422p", "-c:v utvideo", "ULY2", []),
 ("utvideo-444", "176x144", "yuv444p", "-c:v utvideo", "ULY4", []),
 ("magicyuv","176x144", "yuv422p", "-c:v magicyuv", "MAGY", []),
 ("cfhd",   "176x144", "yuv422p10le", "-c:v cfhd", "CFHD", []),
 ("asv1",   "176x144", "yuv420p", "-c:v asv1", "ASV1", []),
 ("asv2",   "176x144", "yuv420p", "-c:v asv2", "ASV2", []),
 ("hap1",   "176x144", "rgba", "-c:v hap -format hap", "Hap1", []),
 ("hap5",   "176x144", "rgba", "-c:v hap -format hap_alpha", "Hap5", []),
 ("hapy",   "176x144", "rgba", "-c:v hap -format hap_q", "HapY", []),
 ("png",    "176x144", "rgb24", "-c:v png", "png ", []),
 ("tiff",   "176x144", "rgb24", "-c:v tiff", "TIFF", []),
 ("bmp",    "176x144", "bgr24", "-c:v bmp", "BMP ", []),
 ("gif",    "176x144", "pal8", "-c:v gif", "GIF ", []),
 ("targa",  "176x144", "bgr24", "-c:v targa", "TGA ", []),
 ("webp",   "176x144", "yuv420p", "-c:v libwebp", "WEBP", ["VP8X"]),
 ("v210",   "176x144", "yuv422p10le", "-c:v v210", "v210", []),
 ("roq",    "176x144", "yuv444p", "-c:v roqvideo", "RoQV", []),
 ("dnxhd",  "1920x1080", "yuv422p", "-c:v dnxhd -b:v 36M", "AVdn", ["AVdh"]),
 ("vc2",    "176x144", "yuv422p10le", "-c:v vc2 -b:v 20M", "drac", ["vcrd"]),
 ("cljr",   "176x144", "yuv411p", "-c:v cljr", "CLJR", []),
 ("rawvideo-yuy2", "176x144", "yuyv422", "-c:v rawvideo", "YUY2", []),
 ("rawvideo-uyvy", "176x144", "uyvy422", "-c:v rawvideo", "UYVY", []),
 ("rawvideo-yv12", "176x144", "yuv420p", "-c:v rawvideo", "YV12", []),
 ("rawvideo-nv12", "176x144", "nv12", "-c:v rawvideo", "NV12", []),
 ("rawvideo-bgr24", "176x144", "bgr24", "-c:v rawvideo", "\0\0\0\0", []),
]

RATE25 = {"mpeg1", "mpeg2"}
FORCE = {"rv10": "rv10", "rv20": "rv20", "bmp": "bmp", "webp": "webp", "roq": "roqvideo"}
RGBISH = {"png","tiff","targa","bmp","zlib","zmbv","qtrle","cinepak","msvideo1","rpza","smc","gif","ffv1-rgb",
          "utvideo-rgb","utvideo-rgba","hap1","hap5","hapy","mjpeg","ljpeg","webp","roq"}

def patch_tag(src, dst, old, new):
    b = bytearray(open(src, "rb").read())
    hdr = b.find(b"movi")
    o = old.encode("latin-1"); n = new.encode("latin-1")
    cnt = 0
    i = 0
    while True:
        i = b.find(o, i, hdr)
        if i < 0: break
        b[i:i+4] = n; cnt += 1; i += 4
    open(dst, "wb").write(b)
    return cnt

def frames_of(path):
    b = open(path, "rb").read()
    return b.count(b"FRAME\n")

def psnr(a, r):
    rc, out = run(["ffmpeg", "-hide_banner", "-nostats", "-y", "-i", a, "-i", r,
                   "-lavfi", "[0:v][1:v]psnr", "-f", "null", "-"], 120)
    m = re.search(r"average:(\S+)", out)
    return m.group(1) if m else None

def lav_decode(avi, out):
    if os.path.exists(out): os.remove(out)
    rc, o = run([SEANCE, "decode", LAV, avi, "-o", out], 150)
    ok = rc == 0 and os.path.exists(out)
    return ok, rc, o.strip().splitlines()[-1] if o.strip() else ""


def rgb_compare(avi, refsrc, d, n):
    """Decode to BGR24 through LAV and with ffmpeg; byte compare (also vertically flipped)."""
    lav = f"{d}/lav_{n}.bgr"; ref = f"{d}/ref_{n}.bgr"
    if os.path.exists(lav): os.remove(lav)
    rc, o = run([SEANCE, "decode", LAV, avi, "--output-format", "BGR24", "-o", lav], 150)
    if rc != 0 or not os.path.exists(lav):
        return "FAILED " + (o.strip().splitlines() or [""])[-1][:90]
    inp = refsrc if refsrc.startswith("-c:v") else "-i " + refsrc
    rc, o = run(f"ffmpeg -hide_banner -loglevel error -y {inp} -pix_fmt bgr24 -fps_mode passthrough -f rawvideo {ref}", 120)
    if rc != 0: return "NO-REF"
    a = open(lav, "rb").read(); b = open(ref, "rb").read()
    if a == b: return "IDENTICAL (BGR24)"
    m = re.search(r"(\d+)x(\d+)", d)
    # rows flipped?
    import glob
    w = 176; h = 144
    if len(a) == len(b) and len(a) % (w*3*h) == 0:
        fr = w*3*h; flipped = b"".join(
            b"".join(a[f*fr + (h-1-y)*w*3 : f*fr + (h-y)*w*3] for y in range(h)) for f in range(len(a)//fr))
        if flipped == b: return "IDENTICAL (BGR24, rows flipped)"
        diff = sum(1 for x, y in zip(a, b) if x != y)
        return f"DIFFERS in {diff}/{len(a)} bytes (BGR24)"
    return f"SIZE {len(a)} vs {len(b)} (BGR24)"

def classify(p, nf_lav, nf_ref):
    if p is None: return "NO-COMPARE"
    if nf_lav != nf_ref: tag = f"frames {nf_lav}/{nf_ref}"
    else: tag = ""
    if p == "inf": v = "IDENTICAL"
    else:
        try:
            f = float(p)
            v = "CLOSE" if f >= 40 else ("LOSSY" if f >= 25 else "DIFFERENT")
            v += f" ({f:.1f} dB)"
        except ValueError:
            v = "NO-COMPARE"
    return (v + (" " + tag if tag else "")).strip()

results = []
only = set(sys.argv[1:])
for (label, size, pix, enc, tag, aliases) in V:
    if only and label not in only: continue
    d = f"{W}/{label}"
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    src = f"{d}/src.nut"
    # source: 10 frames of testsrc2, converted to the codec's pixel format
    rc, o = run(f"ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size={size}:rate={25 if label in RATE25 else 10}:duration=1 -pix_fmt {pix} -c:v ffv1 {src}", 60)
    row = {"label": label, "tag": tag, "stage": "", "result": "", "detail": ""}
    if rc != 0 or not os.path.exists(src):
        row.update(stage="source", result="NOT GENERATED", detail=o.strip()[-120:]); results.append(row); print(row, flush=True); continue
    base = f"{d}/base.avi"
    rate = 25 if label in RATE25 else 10
    ftag = tag if tag != "\0\0\0\0" else None
    tagarg = f"-tag:v '{tag}'" if ftag else ""
    fmt = "" if "-f hevc" not in enc else ""
    if "-f hevc" in enc:
        raw = f"{d}/raw.hevc"
        rc, o = run(f"ffmpeg -hide_banner -loglevel error -y -i {src} {enc} {raw}", 120)
        if rc == 0:
            rc, o = run(f"ffmpeg -hide_banner -loglevel error -y -framerate 10 -f hevc -i {raw} -c:v copy {tagarg} -r {rate} {base}", 60)
    else:
        rc, o = run(f"ffmpeg -hide_banner -loglevel error -y -i {src} {enc} {tagarg} -r {rate} {base}", 120)
    if rc != 0 or not os.path.exists(base) or os.path.getsize(base) < 100:
        row.update(stage="encode/mux", result="NOT GENERATED", detail=o.strip().splitlines()[-1][:110] if o.strip() else ""); results.append(row); print(row, flush=True); continue
    # reference decode: from the AVI, else with the decoder forced by name
    ref = f"{d}/ref.y4m"
    refsrc = base
    rc, o = run(f"ffmpeg -hide_banner -loglevel error -y -i {base} -pix_fmt yuv420p -fps_mode passthrough {ref}", 120)
    if rc != 0 and label in FORCE:
        rc, o = run(f"ffmpeg -hide_banner -loglevel error -y -c:v {FORCE[label]} -i {base} -pix_fmt yuv420p -fps_mode passthrough {ref}", 120)
        refsrc = f"-c:v {FORCE[label]} -i {base}"
    if rc != 0:
        row.update(stage="ref decode", result="NO REFERENCE", detail="ffmpeg can't decode the AVI it writes: " + o.strip()[-70:]); results.append(row); print(row, flush=True)
        # still see what LAV does with it
        out = f"{d}/lav_0.y4m"
        ok, rc, last = lav_decode(base, out)
        r = {"label": label, "tag": tag, "stage": "lav", "result": ("LAV decodes " + str(frames_of(out)) + " frames (unverified)") if ok else "LAV FAILED", "detail": last[:140]}
        results.append(r); print(r, flush=True)
        continue
    cases = [(tag, base)]
    for a in aliases:
        p = f"{d}/alias_{aliases.index(a)}.avi"
        if patch_tag(base, p, tag, a) >= 1:
            cases.append((a, p))
    for (t, avi) in cases:
        out = f"{d}/lav_{cases.index((t, avi))}.y4m"
        ok, rc, last = lav_decode(avi, out)
        r = {"label": label, "tag": t, "stage": "lav", "result": "", "detail": last[:140]}
        if not ok:
            r["result"] = "FAILED" + (" (crash/timeout)" if rc >= 124 or rc < 0 else "")
        else:
            nl, nr = frames_of(out), frames_of(ref)
            r["result"] = classify(psnr(out, ref), nl, nr)
            if label in RGBISH:
                r["rgb"] = rgb_compare(avi, refsrc, d, cases.index((t, avi)))
        results.append(r); print(r, flush=True)

json.dump(results, open(os.path.join(os.environ.get("WORK", "/tmp/lav_matrix"), "video_results.json" if not only else "video_results_partial.json"), "w"), indent=1)
