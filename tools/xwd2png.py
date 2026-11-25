#!/usr/bin/python3 -I
"""xwd2png.py in.xwd out.png: convert a 32-bit xwd window dump to PNG."""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
h = struct.unpack('>20I', d[:80])
hs, w, hh, bo, bpl, ncol = h[0], h[4], h[5], h[7], h[12], h[19]
off = hs + ncol * 12
rows = []
for y in range(hh):
    r = d[off + y * bpl: off + y * bpl + w * 4]
    px = bytearray(w * 3)
    if bo == 0:
        px[0::3], px[1::3], px[2::3] = r[2::4], r[1::4], r[0::4]
    else:
        px[0::3], px[1::3], px[2::3] = r[1::4], r[2::4], r[3::4]
    rows.append(b'\x00' + bytes(px))
raw = b''.join(rows)
def chunk(t, c):
    return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' +
    chunk(b'IHDR', struct.pack('>IIBBBBB', w, hh, 8, 2, 0, 0, 0)) +
    chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
print(w, hh, 'distinct colours in a sample:', len(set(raw[i:i + 3] for i in range(1, len(raw), 997))))
