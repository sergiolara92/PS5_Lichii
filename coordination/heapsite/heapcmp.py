#!/usr/bin/env python3
# heapcmp.py heap.log...: per run, heap live (in_use) and held (pieces) in menus and races, and race fps.
import re, statistics, sys

def kv(l): return {a: float(b) for a, b in re.findall(r'(\w+)=(-?[\d.]+)\b', l)}
M = 2 ** 20
for path in sys.argv[1:]:
    L = open(path, errors='replace').read().splitlines()
    loaded = [kv(l) for l in L if 'ps5-opengl-heap' in l and 'phase=game_loaded' in l]
    fr = [kv(l) for l in L if l.startswith('EDEN_VULKAN_FRAME ')]
    dr = [kv(l) for l in L if l.startswith('EDEN_DEV_DRAW ')]
    gp = [kv(l) for l in L if l.startswith('EDEN_DEV_GPU ')]
    use = [kv(l) for l in L if l.startswith('EDEN_PERF_HEAP_USE ')]
    heap = [kv(l) for l in L if l.startswith('EDEN_PERF_HEAP ') and 'pieces=' in l]
    race = []
    for w in range(1, min(len(dr), len(gp))):
        frames = gp[w]['frame'] - gp[w - 1]['frame']
        race.append(frames > 0 and (dr[w]['calls'] - dr[w - 1]['calls']) / frames > 1500)
    n = min(len(race), len(use), len(heap))
    rw = [w for w in range(n) if race[w]]
    mw = [w for w in range(n) if not race[w]]
    f = lambda xs: f'{min(xs) / M:.0f}-{max(xs) / M:.0f}' if xs else '-'
    print(path.split('/')[-2] if '/' in path else path)
    if loaded: print(f'  game_loaded live {loaded[0]["live_bytes"] / M:.0f} MiB in {loaded[0]["blocks"]:.0f} blocks')
    print(f'  windows {n}, race {len(rw)}')
    print(f'  in_use  menus {f([use[w]["in_use"] for w in mw])}  race {f([use[w]["in_use"] for w in rw])}  max {max(u["in_use"] for u in use) / M:.0f}')
    print(f'  held    menus {f([heap[w]["pieces"] for w in mw])}  race {f([heap[w]["pieces"] for w in rw])}')
    if rw:
        fps = [fr[w + 1]['fps'] for w in rw if w + 1 < len(fr)]
        print(f'  race fps mean {statistics.mean(fps):.1f} range {min(fps):.1f}-{max(fps):.1f}')
        print(f'  race in_use median {statistics.median(use[w]["in_use"] for w in rw) / M:.0f}, held median {statistics.median(heap[w]["pieces"] for w in rw) / M:.0f}')
