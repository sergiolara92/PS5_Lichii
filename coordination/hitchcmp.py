#!/usr/bin/env python3
# hitchcmp.py runs/<tag>/heap.txt...: per run, the race (windows over 1500 draws a frame): fps,
# worst frame, windows whose worst is over 100 / 200 ms, and frames of 2, 3 and 4+ vblanks.
import re, statistics, sys
def kv(l): return {a: float(b) for a, b in re.findall(r'(\w+)=(-?[\d.]+)\b', l)}
for path in sys.argv[1:]:
    L = open(path, errors='replace').read().splitlines()
    fr = [kv(l) for l in L if l.startswith('EDEN_VULKAN_FRAME ')]
    dr = [kv(l) for l in L if l.startswith('EDEN_DEV_DRAW ')]
    gp = [kv(l) for l in L if l.startswith('EDEN_DEV_GPU ')]
    iv = [kv(l) for l in L if l.startswith('EDEN_VULKAN_INTERVALS ')]
    race = []
    for w in range(1, min(len(fr), len(dr), len(gp), len(iv))):
        frames = gp[w]['frame'] - gp[w - 1]['frame']
        if frames > 0 and (dr[w]['calls'] - dr[w - 1]['calls']) / frames > 1500:
            race.append((fr[w], iv[w]))
    if not race:
        print(f'{path}: no race'); continue
    f = [x['fps'] for x, _ in race]; worst = [x['worst_ms'] for x, _ in race]
    n = sum(x['frames'] for x, _ in race)
    v = {k: int(sum(i[k] for _, i in race)) for k in ('v1', 'v2', 'v3', 'v4plus')}
    print(f"{path.split('/')[-2]:10s} win={len(race):2d} fps={statistics.mean(f):5.2f} worst_max={max(worst):6.1f} "
          f">100ms={sum(x > 100 for x in worst):2d} >200ms={sum(x > 200 for x in worst):2d} frames={int(n)} "
          f"v2={v['v2']} v3={v['v3']} v4+={v['v4plus']} (per 1000: {1000*(v['v2']+v['v3']+v['v4plus'])/max(n,1):.0f})")
