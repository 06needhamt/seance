#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Makes src/win32/gdi_font.h, the glyphs gdi32 draws text with, from a PCF
# font: tools/pcf2c.py /usr/share/fonts/X11/cyrillic/koi8x13.pcf.gz
import gzip, struct, sys

data = gzip.open(sys.argv[1]).read()
assert data[:4] == b'\x01fcp'
count = struct.unpack_from('<i', data, 4)[0]
tables = {}
for i in range(count):
    t, fmt, size, off = struct.unpack_from('<iiii', data, 8 + 16 * i)
    tables[t] = (fmt, size, off)

def table(t):
    fmt, size, off = tables[t]
    f = struct.unpack_from('<i', data, off)[0]
    big = f & 4
    return f, (">" if big else "<"), off + 4

# Accelerators: font ascent and descent.
acc = tables.get(1 << 8) and (1 << 8) or (1 << 1)
f, e, p = table(acc)
ascent, descent = struct.unpack_from(e + 'ii', data, p + 8)

# Metrics.
f, e, p = table(1 << 2)
metrics = []
if f & 0x100:
    n = struct.unpack_from(e + 'h', data, p)[0]; p += 2
    for i in range(n):
        lsb, rsb, w, a, d = [b - 0x80 for b in data[p:p + 5]]; p += 5
        metrics.append((lsb, rsb, w, a, d))
else:
    n = struct.unpack_from(e + 'i', data, p)[0]; p += 4
    for i in range(n):
        lsb, rsb, w, a, d, _ = struct.unpack_from(e + 'hhhhhH', data, p); p += 12
        metrics.append((lsb, rsb, w, a, d))

# Bitmaps.
f, e, p = table(1 << 3)
pad = 1 << (f & 3)
msbit = f & 8
n = struct.unpack_from(e + 'i', data, p)[0]; p += 4
offsets = struct.unpack_from(e + '%di' % n, data, p); p += 4 * n
p += 16
bits = data[p:]

# Encodings.
f, e, p = table(1 << 5)
min2, max2, min1, max1, default = struct.unpack_from(e + 'hhhhh', data, p); p += 10
idx = struct.unpack_from(e + '%dH' % ((max2 - min2 + 1) * (max1 - min1 + 1)), data, p)

cell_w = max(m[2] for m in metrics)
out = []
for ch in range(0x20, 0x7f):
    g = idx[ch - min2]
    lsb, rsb, w, a, d = metrics[g]
    gw = rsb - lsb
    stride = ((gw + 7) // 8 + pad - 1) // pad * pad
    rows = []
    for y in range(ascent + descent):
        gy = y - (ascent - a)
        v = 0
        if 0 <= gy < a + d:
            row = bits[offsets[g] + gy * stride: offsets[g] + (gy + 1) * stride]
            for x in range(gw):
                byte = row[x // 8]
                bit = (byte >> (7 - x % 8)) & 1 if msbit else (byte >> (x % 8)) & 1
                if bit and 0 <= lsb + x < cell_w:
                    v |= 0x80 >> (lsb + x)
        rows.append(v)
    out.append((ch, rows))

print("/* SPDX-License-Identifier: GPL-2.0-only */")
print("/*")
print(" * The harness's one font: the ASCII glyphs of X11's misc-fixed 8x13")
print(" * (as koi8x13 in font-misc-cyrillic: \"Public domain font.  Share and")
print(" * enjoy.\"), a cell %d wide and %d high, %d above the baseline. Each" % (cell_w, ascent + descent, ascent))
print(" * glyph is its rows, top first, the leftmost pixel the top bit.")
print(" * Made by tools/pcf2c.py.")
print(" */")
print("#define GDI_FONT_WIDTH   %d" % cell_w)
print("#define GDI_FONT_HEIGHT  %d" % (ascent + descent))
print("#define GDI_FONT_ASCENT  %d" % ascent)
print("#define GDI_FONT_FIRST   0x20")
print("#define GDI_FONT_LAST    0x7e")
print("")
print("static const unsigned char gdi_font[][GDI_FONT_HEIGHT] = {")
for ch, rows in out:
    name = chr(ch) if ch not in (0x5c, 0x2a) else ('\\\\' if ch == 0x5c else '*')
    print("\t{ %s },\t/* %s */" % (", ".join("0x%02x" % r for r in rows),
                                   "'%s'" % ("backslash" if ch == 0x5c else chr(ch))))
print("};")
