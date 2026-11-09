#!/usr/bin/env python3
# abcmp.py <run>...: per run (coordination/runs/<run>, dkb-windows.py's windows), the OOT house windows
# (900-1100 draws a frame: the fixed view before the runner walks, the steadiest comparison) and the
# measured windows (the last 18 but the final one: 90 s with camera turns) as mean/median/min fps.
import statistics, subprocess, sys

C = '/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination/'
for run in sys.argv[1:]:
    out = subprocess.run([sys.executable, '-I', C + 'dkb-windows.py', C + 'runs/' + run + '/heap.txt'],
                         capture_output=True, text=True).stdout.splitlines()
    rows = [tuple(float(x) for x in w.split('/')) for w in out[0].split()] if out else []
    house = [f for f, d, _ in rows if 900 <= d <= 1100]
    tail = rows[-19:-1]
    if not tail:
        print(f'{run:16s} no windows'); continue
    fps = [f for f, _, _ in tail]
    print(f'{run:16s} house {statistics.mean(house) if house else 0:5.1f} fps ({len(house)} windows)  measured '
          f'mean {statistics.mean(fps):5.1f} median {statistics.median(fps):5.1f} min {min(fps):5.1f} '
          f'draws {statistics.mean(d for _, d, _ in tail):5.0f}')
