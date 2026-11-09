import re, sys, statistics
# Race windows (>1500 draws/frame) of a heap.txt: fps, worst_ms, GPU thread cpu/wall.
def kv(l): return {a: float(b) for a, b in re.findall(r'(\w+)=(-?[\d.]+)\b', l)}
L = [l for l in open(sys.argv[1], errors='replace')]
fr = [kv(l) for l in L if l.startswith('EDEN_VULKAN_FRAME ')]
dr = [kv(l) for l in L if l.startswith('EDEN_DEV_DRAW ')]
gp = [kv(l) for l in L if l.startswith('EDEN_DEV_GPU ')]
rows = []
for w in range(1, min(len(fr), len(dr), len(gp))):
    frames = gp[w]['frame'] - gp[w - 1]['frame']
    if frames <= 0: continue
    dpf = (dr[w]['calls'] - dr[w - 1]['calls']) / frames
    busy = (gp[w]['cpu_ns'] - gp[w - 1]['cpu_ns']) / (gp[w]['mono_ns'] - gp[w - 1]['mono_ns'])
    us = (gp[w]['cpu_ns'] - gp[w - 1]['cpu_ns']) / max(1, dr[w]['calls'] - dr[w - 1]['calls']) / 1e3
    race = dpf > 1500
    rows.append((w, race, fr[w]['fps'], fr[w]['worst_ms'], dpf, busy, us))
    if '-v' in sys.argv: print(f"{w:3d} {'R' if race else ' '} fps={fr[w]['fps']:6.2f} worst={fr[w]['worst_ms']:7.1f} draws/f={dpf:6.0f} gpu_busy={busy:.3f} us/draw(cpu)={us:5.2f}")
r = [x for x in rows if x[1]]
if r:
    m = statistics.mean
    print(f"race windows={len(r)} fps={m([x[2] for x in r]):.2f} (median {statistics.median([x[2] for x in r]):.2f}) "
          f"worst_ms mean={m([x[3] for x in r]):.1f} max={max(x[3] for x in r):.1f} draws/f={m([x[4] for x in r]):.0f} "
          f"gpu_busy={m([x[5] for x in r]):.3f} us/draw={m([x[6] for x in r]):.2f} draws/s={m([x[4]*x[2] for x in r]):.0f}")
else: print('no race windows')
