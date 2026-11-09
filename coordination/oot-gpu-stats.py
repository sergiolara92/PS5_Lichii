#!/usr/bin/env python3
# oot-gpu-stats.py <heap.txt> [windows=12]: gameplay fps and the GPU front thread's cost over the
# last stretch of 5 s windows under 40 fps (EDEN_VULKAN_FRAME, EDEN_DEV_GPU, EDEN_DEV_WAIT_THREAD).
import re
import sys

lines = open(sys.argv[1], errors='replace').read().splitlines()
count = int(sys.argv[2]) if len(sys.argv) > 2 else 12


def fields(line):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)\b', line)}


# Pair each frame window with the EDEN_DEV_GPU line that follows it.
pairs, fps = [], None
for line in lines:
    if line.startswith('EDEN_VULKAN_FRAME '):
        fps = float(re.search(r'fps=([0-9.]+)', line).group(1))
    elif line.startswith('EDEN_DEV_GPU ') and fps is not None:
        pairs.append((fps, fields(line)))
        fps = None
all_fps = [f for f, _ in pairs]
print('fps windows:', ' '.join(f'{f:.1f}' for f in all_fps))
# The gameplay stretch: after the title screen (the last window of 55 fps or more) and the loading
# dip after it, up to the window before the last (the quit), at most `count` windows.
title = max((i for i, (f, _) in enumerate(pairs) if f >= 55), default=0)
dip = min(range(title, len(pairs)), key=lambda i: pairs[i][0]) if title < len(pairs) else title
start = dip + 1
end = min(len(pairs) - 2, start + count)
if end - start < 2:
    sys.exit('no gameplay stretch')
(_, a), (_, b) = pairs[start], pairs[end]
dt = (b['mono_ns'] - a['mono_ns']) / 1e9
draws = b['draws'] - a['draws']
frames = b['frame'] - a['frame']
print(f'gameplay {dt:.0f} s: fps={frames / dt:.1f} median_window={sorted(all_fps[start + 1:end + 1])[(end - start) // 2]:.1f} '
      f'draws/frame={draws / max(1, frames):.0f} front_cpu={(b["cpu_ns"] - a["cpu_ns"]) / 1e9 / dt:.2f} '
      f'idle={(b["idle_ns"] - a["idle_ns"]) / 1e9 / dt:.2f} drain={(b["drain_ns"] - a["drain_ns"]) / 1e9 / dt:.2f} '
      f'us/draw={(b["cpu_ns"] - a["cpu_ns"]) / 1e3 / max(1, draws):.1f}')
