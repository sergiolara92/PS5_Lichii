#!/usr/bin/env python3
# ppm2png.py <in.ppm> <out.png>: a replay picture as PNG, to look at.
import struct, sys, zlib, importlib.util, os
s = importlib.util.spec_from_file_location('pd', os.path.join(os.path.dirname(__file__), 'picdiff.py'))
pd = importlib.util.module_from_spec(s); s.loader.exec_module(pd)
w, h, p = pd.read_ppm(sys.argv[1])
raw = b''.join(b'\x00' + p[y * w * 3:(y + 1) * w * 3] for y in range(h))
chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
