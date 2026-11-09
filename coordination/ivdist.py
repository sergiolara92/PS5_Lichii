#!/usr/bin/env python3
# ivdist.py <run>...: the measured windows' (abcmp.py's: the last 18 but the final one) present-interval
# distribution in vblanks at 60 Hz (EDEN_VULKAN_INTERVALS v1..v4plus after each EDEN_VULKAN_FRAME) and per-window fps.
import re, sys
C = '/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination/runs/'
for run in sys.argv[1:]:
    wins, fps = [], None
    for line in open(C + run + '/heap.txt', errors='replace'):
        if line.startswith('EDEN_VULKAN_FRAME '):
            fps = float(re.search(r'fps=([0-9.]+)', line).group(1))
        elif line.startswith('EDEN_VULKAN_INTERVALS ') and fps is not None:
            wins.append((fps, [int(x) for x in re.findall(r'v(?:1|2|3|4plus)=(\d+)', line)])); fps = None
    tail = wins[-19:-1]
    tot = [sum(w[1][i] for w in tail) for i in range(4)]
    n = sum(tot)
    ms = sum(c * v for c, v in zip(tot, (16.7, 33.3, 50, 75))) / n if n else 0
    print(f'{run:12s} frames {n:5d}  16.7ms {tot[0]/n*100:4.1f}%  33ms {tot[1]/n*100:4.1f}%  50ms {tot[2]/n*100:4.1f}%  '
          f'67ms+ {tot[3]/n*100:4.1f}%  ~mean {ms:4.1f} ms')
    print('   windows fps:', ' '.join(f'{f:.0f}' for f, _ in tail))
