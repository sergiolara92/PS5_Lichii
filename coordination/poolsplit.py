#!/usr/bin/env python3
# poolsplit.py <run dir> [every windows=2]: the Pro's shared memory pool over a run, from the emulator's
# own accounting: guest_held and graphics_held (EDEN_PERF_DIRECT), the texture cache's usage
# (EDEN_VULKAN_TEXTURE_BUDGET, every 300 frames), the GPU-only regions in use (eden_log, nx2:gpu), and
# the rest of graphics_held (buffer cache, staging, pipelines, descriptors, driver) by difference.
import re, sys
run = sys.argv[1]; every = int(sys.argv[2]) if len(sys.argv) > 2 else 2
start = None; wins = []; tex = None; draws = None
for line in open(f'{run}/heap.txt', errors='replace'):
    m = re.search(r'name=CPUCore_0 .*registered_mono_ns=(\d+)', line)
    if m and start is None: start = int(m.group(1))
    m = re.match(r'EDEN_VULKAN_TEXTURE_BUDGET .*usage=(\d+)', line)
    if m: tex = int(m.group(1))
    m = re.match(r'EDEN_VULKAN_FRAME frames=(\d+) .*fps=([\d.]+)', line)
    if m: fps = float(m.group(2))
    m = re.match(r'EDEN_PERF_SAMPLE mono_ns=(\d+)', line)
    if m: mono = int(m.group(1))
    m = re.match(r'EDEN_PERF_DIRECT .*largest_free=(\d+) free=(\d+) .*guest_held=(\d+) graphics_held=(\d+)', line)
    if m and start is not None:
        wins.append(((mono - start) / 1e9, int(m.group(3)), int(m.group(4)), tex, int(m.group(2)), int(m.group(1)), fps))
gpuonly = []
for line in open(f'{run}/eden_log.txt', errors='replace'):
    m = re.match(r'\[ *([\d.]+)\].*?(\d+) MiB of GPU-only memory in use', line)
    if m: gpuonly.append((float(m.group(1)), int(m.group(2))))
def gpu_at(t):
    v = 0
    for tt, mib in gpuonly:
        if tt > t: break
        v = mib
    return v
G = 2**20
print(' time   fps  guest  VMA blocks  texture est.  GPU-only    free  largest free   (MiB)')
for i, (t, guest, gfx, tx, free, largest, fps) in enumerate(wins):
    if i % every and i != len(wins) - 1: continue
    g = gpu_at(t); txm = (tx or 0) / G
    print(f'{t:5.0f} {fps:5.1f} {guest/G:6.0f} {gfx/G:11.0f} {txm:13.0f} {g:9.0f} {free/G:7.0f} {largest/G:7.0f}')
