#!/usr/bin/env python3
# abwin.py "<arm a heap.txt ...>" "<arm b heap.txt ...>": A/B on race windows matched by draws per frame
# (a run's timeline drifts, so whole-race fps compares different stretches). Per draws/frame band: mean
# fps of each arm's windows, window counts, and the MainThread's GPU-fence wait (bound.py's columns).
import collections, statistics, subprocess, sys
bands = [(1500, 1800), (1800, 2100), (2100, 2500), (2500, 2900), (2900, 4000)]
def windows(paths):
    out = []
    for p in paths:
        for line in subprocess.run([sys.executable, '-I', 'bound.py', p], capture_output=True, text=True).stdout.split('\n')[1:]:
            f = line.split()
            if len(f) > 12:
                out.append((float(f[1]), float(f[3]), float(f[12]) if f[11] == 'GPU' else float(f[13])))
    return out
arms = [windows(sys.argv[1].split()), windows(sys.argv[2].split())]
print('draws/frame band   arm a fps (n)      arm b fps (n)      b/a')
for lo, hi in bands:
    r = []
    for w in arms:
        v = [x[0] for x in w if lo <= x[1] < hi]
        r.append((statistics.mean(v) if v else 0, len(v)))
    ratio = r[1][0] / r[0][0] if r[0][0] and r[1][0] else 0
    print(f'{lo:5}-{hi:<5}         {r[0][0]:6.1f} ({r[0][1]:2})        {r[1][0]:6.1f} ({r[1][1]:2})        {ratio:.3f}')
