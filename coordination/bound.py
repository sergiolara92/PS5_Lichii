#!/usr/bin/env python3
# bound.py heap.txt...: race windows (>1500 draws/frame): fps, draws/frame, GPU-thread busy ms per frame,
# MainThread sync wait (GPU fences) ms per frame -> which side limits each window.
import re, sys, statistics
for path in sys.argv[1:]:
    win = []; cur = {}
    for line in open(path, errors='replace'):
        if line.startswith('EDEN_VULKAN_FRAME ') and 'fps=' in line:
            cur['fps'] = float(re.search(r'fps=([\d.]+)', line).group(1)); cur['frames'] = int(re.search(r'frames=(\d+)', line).group(1))
        m = re.match(r'EDEN_DEV_GPU .*cpu_ns=(\d+) idle_ns=(\d+).* draws=(\d+)', line)
        if m: cur['gpu'] = [int(m.group(i)) for i in (1, 2, 3)]
        m = re.match(r'EDEN_DEV_WAIT_THREAD tid=\d+ name=MainThread .*rsn=(\S+)', line)
        if m: cur['main'] = [int(x) for x in m.group(1).split(',')]
        if line.startswith('EDEN_DEV_DRAW ') and 'gpu' in cur and 'main' in cur and 'fps' in cur:
            win.append(dict(cur))
    rows = []
    for a, b in zip(win[1:], win):
        fr = a['frames']
        if not fr: continue
        draws = (a['gpu'][2] - b['gpu'][2]) / fr
        if draws < 1500: continue
        idle = (a['gpu'][1] - b['gpu'][1]) / 1e6 / fr
        frame = 1000 / a['fps']
        sync = (a['main'][3] - b['main'][3]) / 1e6 / fr
        rows.append((a['fps'], draws, frame - idle, sync, frame))
    print(path)
    for r in rows:
        print('  fps %5.1f draws/f %5.0f  GPU thread busy %5.1f ms/frame  main waits GPU %4.1f ms/frame  -> %s' % (r[0], r[1], r[2], r[3], 'GPU-bound' if r[3] > 1 else 'guest-bound'))
