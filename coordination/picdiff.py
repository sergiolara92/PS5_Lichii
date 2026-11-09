#!/usr/bin/env python3
# picdiff.py <dir a> <dir b>: compare replay-rt-<n>.ppm pictures (P6, 8-bit). Per picture: mean
# absolute difference per channel and the share of pixels off by more than 24 in any channel.
# The Pro varies a little between runs; corruption (black or rainbow textures) shows as a large
# share of far-off pixels.
import os, sys

def read_ppm(path):
    data = open(path, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    assert fields[0] == b'P6' and fields[3] == b'255', path
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]

def compare(a, b):
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    if (wa, ha) != (wb, hb):
        return None
    total = sum(abs(x - y) for x, y in zip(pa, pb))
    far = sum(1 for i in range(0, len(pa), 3)
              if max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2])) > 24)
    return total / len(pa), far / (wa * ha)

if __name__ == '__main__':
    worst = 0.0
    for i in range(1, 22):
        name = f'replay-rt-{i}.ppm'
        a, b = os.path.join(sys.argv[1], name), os.path.join(sys.argv[2], name)
        if not (os.path.exists(a) and os.path.exists(b)):
            print(f'{i:2} missing')
            continue
        result = compare(a, b)
        if result is None:
            print(f'{i:2} size differs')
            continue
        mean, far = result
        worst = max(worst, far)
        print(f'{i:2} mean {mean:6.2f}  far {far * 100:6.2f}%')
    print(f'worst far {worst * 100:.2f}%')
