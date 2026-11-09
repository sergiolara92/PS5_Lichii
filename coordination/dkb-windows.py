#!/usr/bin/env python3
# dkb-windows.py <heap.txt>: each 5 s window's fps, draws per frame and the GPU front thread's busy
# share (EDEN_VULKAN_FRAME and the EDEN_DEV_GPU line after it), then the median of the last 8.
import re
import statistics
import sys

rows, fps, prev = [], None, None
lean = []  # builds without dev counters: draws a frame from the frame line itself
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('EDEN_VULKAN_FRAME '):
        fps = float(re.search(r'fps=([0-9.]+)', line).group(1))
        if m := re.search(r'draws_per_frame=([0-9.]+)', line):
            lean.append((fps, float(m.group(1)), -1.0))
    elif line.startswith('EDEN_DEV_GPU ') and fps is not None:
        g = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)\b', line)}
        if prev:
            frames = max(1, g['frame'] - prev['frame'])
            dt = max(1, g['mono_ns'] - prev['mono_ns'])
            rows.append((fps, (g['draws'] - prev['draws']) / frames, (g['cpu_ns'] - prev['cpu_ns']) / dt))
        prev, fps = g, None
rows = rows or lean
print(' '.join(f'{f:.1f}/{d:.0f}/{c:.2f}' for f, d, c in rows))
tail = rows[-9:-1]
if tail:
    print(f'last windows: median fps={statistics.median(r[0] for r in tail):.1f} min={min(r[0] for r in tail):.1f} '
          f'draws/frame={statistics.median(r[1] for r in tail):.0f} front_busy={statistics.median(r[2] for r in tail):.2f}')
