#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""LAV Audio matrix: encode with ffmpeg into AVI, decode with ffmpeg
(reference) and with LAVAudio.ax in seance64, compare the PCM."""
import os, re, subprocess, sys, json, shutil
from array import array

SEANCE = os.environ.get("SEANCE", "build/seance64")
LAV = os.path.join(os.environ.get("LAV_DIR", "."), "LAVAudio.ax")
W = os.path.join(os.environ.get("WORK", "/tmp/lav_matrix"), "a")
os.makedirs(W, exist_ok=True)

# LAV Audio has its WMA and TTA decoders off until the registry says otherwise.
REG = f"{W}/lav.reg"
with open(REG, "w") as f:
    f.write('Windows Registry Editor Version 5.00\n\n'
            '[HKEY_CURRENT_USER\\Software\\LAV\\Audio\\Formats]\n'
            '"wma"=dword:00000001\n'
            '"tta"=dword:00000001\n')

def run(cmd, timeout=120):
    e = dict(os.environ); e["VFW_TIMEOUT"] = "60"; e["VFW_REGISTRY"] = REG
    try:
        p = subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True, timeout=timeout, env=e)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return 124, "timeout"

# (label, encoder args (after -i src), sample rate / channels override args, aliases are not used)
A = [
 ("ac3",   "-c:a ac3 -b:a 192k", ""),
 ("ac3-51", "-c:a ac3 -b:a 384k", "-ac 6"),
 ("eac3",  "-c:a eac3 -b:a 192k", ""),
 ("mp2",   "-c:a mp2 -b:a 192k", ""),
 ("mp3",   "-c:a libmp3lame -b:a 192k", ""),
 ("mp3-mono-22k", "-c:a libmp3lame -b:a 64k", "-ac 1 -ar 22050"),
 ("aac",   "-c:a aac -b:a 128k", ""),
 ("aac-51","-c:a aac -b:a 320k", "-ac 6"),
 ("dts",   "-c:a dca -strict -2 -b:a 768k", ""),
 ("wmav1", "-c:a wmav1 -b:a 128k", ""),
 ("wmav2", "-c:a wmav2 -b:a 128k", ""),
 ("flac",  "-c:a flac", ""),
 ("flac-24","-c:a flac -sample_fmt s32", ""),
 ("vorbis","-c:a libvorbis -q:a 4", ""),
 ("adpcm_ima_wav", "-c:a adpcm_ima_wav", ""),
 ("adpcm_ms", "-c:a adpcm_ms", ""),
 ("g726",  "-c:a g726 -b:a 32k", "-ar 8000 -ac 1"),
 ("g722",  "-c:a g722", "-ar 16000 -ac 1"),
 ("adpcm_yamaha", "-c:a adpcm_yamaha", ""),
 ("speex", "-c:a libspeex", "-ar 16000 -ac 1"),
 ("pcm_s16le", "-c:a pcm_s16le", ""),
 ("pcm_s24le", "-c:a pcm_s24le", ""),
 ("pcm_s32le", "-c:a pcm_s32le", ""),
 ("pcm_f32le", "-c:a pcm_f32le", ""),
 ("pcm_f64le", "-c:a pcm_f64le", ""),
 ("pcm_u8", "-c:a pcm_u8", ""),
 ("pcm_alaw", "-c:a pcm_alaw", ""),
 ("pcm_mulaw", "-c:a pcm_mulaw", ""),
 # No wave format tag stands for these, and ffmpeg won't put them in an
 # AVI: mkavi_audio.py does, and --subtype says what they are.
 ("alac",  "-c:a alac", "", "alac"),
 ("truehd", "-c:a truehd -strict -2", "", "{EB27CEC4-163E-4CA3-8B74-8E25F91B517E}"),
 ("mlp",   "-c:a mlp -strict -2", "", "MLP "),
 ("ra_144", "-c:a real_144", "-ac 1 -ar 8000", "14_4"),
 # Matroska won't take Nellymoser: it comes from FLV, its own container.
 ("nellymoser", "-c:a nellymoser", "-ac 1 -ar 22050", "NELL", None, "flv"),
 # Under its own tag, with the TTA1 header ffmpeg leaves out of an AVI.
 ("tta",   "-c:a tta", "", None, 0x77a1),
]
MKAVI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mkavi_audio.py")

def load(path):
    a = array("h")
    with open(path, "rb") as f:
        a.frombytes(f.read())
    return a

def compare(ref, lav, ch):
    """ref, lav: arrays of interleaved s16."""
    if ref == lav:
        return "IDENTICAL", ""
    n = min(len(ref), len(lav)) // ch
    win = min(1500, n // 3)
    mid = n // 2
    best = None
    for sh in range(-4000, 4001):
        a0 = (mid + sh) * ch
        b0 = mid * ch
        if a0 < 0 or a0 + win * ch > len(lav) or b0 + win * ch > len(ref):
            continue
        e = 0
        for i in range(0, win * ch, 3):
            d = lav[a0 + i] - ref[b0 + i]
            e += d if d >= 0 else -d
        if best is None or e < best[0]:
            best = (e, sh)
            if e == 0:
                break
    if best is None:
        return "NO-COMPARE", f"frames lav {len(lav)//ch} ref {len(ref)//ch}"
    sh = best[1]
    s = sh * ch
    if s >= 0:
        x, y = lav[s:], ref
    else:
        x, y = lav, ref[-s:]
    m = min(len(x), len(y))
    tot = 0; mx = 0; same = 0
    for i in range(m):
        d = x[i] - y[i]
        if d < 0: d = -d
        tot += d
        if d > mx: mx = d
        if d == 0: same += 1
    mean = tot / m if m else 0
    peak = max((abs(v) for v in ref[:200000:11]), default=1) or 1
    note = f"shift {sh}, frames lav {len(lav)//ch}/ref {len(ref)//ch}, max diff {mx}, mean {mean:.2f}, peak {peak}"
    if mx == 0:
        return "IDENTICAL (offset/length differs)" if (sh or len(lav) != len(ref)) else "IDENTICAL", note
    if mx <= 2:
        return "CLOSE (<=2 LSB)", note
    if mean / peak < 0.002:
        return "CLOSE", note
    if mean / peak < 0.02:
        return "LOSSY-CLOSE", note
    return "DIFFERENT", note

results = []
only = set(sys.argv[1:])
os.makedirs(W, exist_ok=True)
src = f"{W}/src.wav"
run(f'ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1.5" -f lavfi -i "anoisesrc=color=pink:sample_rate=48000:duration=1.5:amplitude=0.1" -f lavfi -i "sine=frequency=1100:sample_rate=48000:duration=1.5" -filter_complex "[0][1]amix=inputs=2:duration=first[l];[2][1]amix=inputs=2:duration=first[r];[l][r]amerge=inputs=2[a]" -map "[a]" -c:a pcm_s16le {src}')
for (label, enc, extra, *by_hand) in A:
    # By hand: the subtype to name, the wave format tag, the container.
    subtype, tag, container = (by_hand + [None, None, "mka"][len(by_hand):]
                               if by_hand else (None, None, None))
    if only and label not in only: continue
    d = f"{W}/{label}"
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    avi = f"{d}/x.avi"
    # What ffmpeg reads back: the AVI, or the file the AVI is made from.
    media = f"{d}/x.{container}" if by_hand else avi
    rc, o = run(f"ffmpeg -hide_banner -loglevel error -y -i {src} {extra} {enc} {media}", 120)
    if rc == 0 and by_hand:
        rc, o = run([sys.executable, MKAVI, media, avi] +
                    ([hex(tag)] if tag else []), 120)
    row = {"label": label, "result": "", "detail": ""}
    if rc != 0 or not os.path.exists(avi) or os.path.getsize(avi) < 100:
        row.update(result="NOT GENERATED", detail=(o.strip().splitlines() or [""])[-1][:110]); results.append(row); print(row, flush=True); continue
    pr = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=channels,sample_rate,codec_tag", "-of", "csv=p=0", media], capture_output=True, text=True).stdout.strip()
    ref = f"{d}/ref.raw"
    rc, o = run(f"ffmpeg -hide_banner -loglevel error -y -i {media} -f s16le -acodec pcm_s16le {ref}", 120)
    if rc != 0:
        row.update(result="NOT GENERATED", detail="ffmpeg can't decode: " + o.strip()[-80:]); results.append(row); print(row, flush=True); continue
    lavwav = f"{d}/lav.wav"
    rc, o = run([SEANCE, "decode", LAV, avi, "--audio", "--sample-format", "s16", "-o", lavwav] +
                (["--subtype", subtype] if subtype else []), 150)
    last = (o.strip().splitlines() or [""])[-1][:140]
    if rc != 0 or not os.path.exists(lavwav):
        row.update(result="FAILED" + (" (crash/timeout)" if rc >= 124 or rc < 0 else ""), detail=(o.strip().splitlines() or [""])[0][:140]); results.append(row); print(row, flush=True); continue
    lavraw = f"{d}/lav.raw"
    rc, o2 = run(f"ffmpeg -hide_banner -loglevel error -y -i {lavwav} -f s16le -acodec pcm_s16le {lavraw}", 60)
    # channels of the reference stream
    cc = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=channels", "-of", "csv=p=0", media], capture_output=True, text=True).stdout.strip()
    ch = int(cc) if cc.isdigit() else 2
    lc = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=channels", "-of", "csv=p=0", lavwav], capture_output=True, text=True).stdout.strip()
    lch = int(lc) if lc.isdigit() else ch
    if lch != ch:
        row.update(result=f"CHANNELS {lch} vs {ch}", detail=last); results.append(row); print(row, flush=True); continue
    if rc != 0 or not os.path.exists(lavraw):
        row.update(result="FAILED", detail="ffmpeg can't read lav.wav: " + o2.strip()[-80:]); results.append(row); print(row, flush=True); continue
    v, note = compare(load(ref), load(lavraw), ch)
    row.update(result=v, detail=note or last); results.append(row); print(row, flush=True)

json.dump(results, open(os.path.join(os.environ.get("WORK", "/tmp/lav_matrix"), "audio_results.json" if not only else "audio_results_partial.json"), "w"), indent=1)
