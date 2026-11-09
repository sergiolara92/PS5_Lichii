#!/usr/bin/env python3
# ticks.py <sched-trace.bin> [critical thread id]: Mario Kart World runs a fixed 60 Hz game tick and,
# when it falls behind, a frame with two ticks (presented with interval 2). Splits the traced frames
# (present to present) into the two groups (2-means on frame length) and prints each group's frame
# time, the critical thread's run time and all guest-core run time, so: render cost R = S - T, tick
# cost T = L - S. A locked 60 needs the 1-tick frame S under 16.7 ms.
import bisect, collections, struct, sys, statistics
data = open(sys.argv[1], 'rb').read()
recs = sorted(r for r in (struct.unpack_from('<qIIQ', data, i) for i in range(0, len(data) - 23, 24)) if r[0])
presents = [ns for ns, c, k, _ in recs if k == 1]
runs = collections.defaultdict(list)  # thread -> sorted (start, end)
allrun = []
last = {}
for ns, c, k, ident in recs:
    if k == 0:
        if c in last and last[c][1]:
            runs[last[c][1]].append((last[c][0], ns)); allrun.append((last[c][0], ns))
        last[c] = (ns, ident)
allrun.sort()

def busy(iv, b, e):
    i = bisect.bisect_left(iv, (b - 50_000_000, 0)); s = 0
    while i < len(iv) and iv[i][0] < e:
        s += max(0, min(iv[i][1], e) - max(iv[i][0], b)); i += 1
    return s / 1e6

frames = list(zip(presents, presents[1:]))
lens = [(e - b) / 1e6 for b, e in frames]
# critical thread: the one with the most run time
crit = int(sys.argv[2]) if len(sys.argv) > 2 else max(runs, key=lambda t: sum(e - s for s, e in runs[t]))
q = statistics.quantiles(lens, n=10); lo, hi = q[1], q[7]
for _ in range(20):
    cut = (lo + hi) / 2
    a = [x for x in lens if x < cut]; b = [x for x in lens if cut <= x < 2.2 * lo]
    lo, hi = statistics.mean(a), statistics.mean(b)
groups = {'S': [], 'L': []}
for (b, e), L in zip(frames, lens):
    if L >= 2.2 * lo: continue  # hitches
    groups['S' if L < cut else 'L'].append((L, busy(runs[crit], b, e), busy(allrun, b, e)))
print(f'{len(frames)} frames, mean {statistics.mean(lens):.2f} ms ({1000/statistics.mean(lens):.1f} fps), cut {cut:.1f} ms, critical thread {crit}')
m = {}
for g, v in groups.items():
    if not v: continue
    m[g] = [statistics.mean(x[i] for x in v) for i in range(3)]
    q = statistics.quantiles([x[0] for x in v], n=10) if len(v) > 10 else []
    print(f'{g}: {len(v)} frames ({100*len(v)/len(frames):.0f}%), frame {m[g][0]:.2f} ms (p10 {q[0] if q else 0:.1f}, p90 {q[-1] if q else 0:.1f}), '
          f'critical thread {m[g][1]:.2f} ms, all guest cores {m[g][2]:.1f} ms')
if len(m) == 2:
    S, L = m['S'][0], m['L'][0]; T = L - S
    print(f'tick T = {T:.2f} ms of frame ({m["L"][1]-m["S"][1]:.2f} ms critical thread, {m["L"][2]-m["S"][2]:.1f} ms core time); '
          f'render R = S - T = {S - T:.2f} ms; S must drop {100*(1 - 16.67/S):.1f}% for 60')
    # fps predicted by the tick model for S shrinking: p = (S-16.67)/(16.67-T) double frames
    for k in (1.0, 0.95, 0.9, 0.85):
        s, t = S * k, T * k
        p = min(1, max(0, (s - 16.67) / (16.67 - t))) if t < 16.67 else 1
        print(f'   all frame work x{k:.2f}: S {s:.1f} ms -> {1000 / (s + p * t):.1f} fps ({100*p:.0f}% double frames)')
