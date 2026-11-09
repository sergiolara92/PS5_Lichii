#!/usr/bin/env python3
# threadrun.py <heap.txt> [windows=12] [top=8]: over the last N 5 s polls, each guest thread's time not
# waiting (running or runnable; EDEN_DEV_WAIT_THREAD ns are cumulative waits) in ms per frame
# (EDEN_VULKAN_FRAME), busiest first. A JIT-speed proxy that fps hides when the frame is paced.
import collections, re, sys

path = sys.argv[1]
windows = int(sys.argv[2]) if len(sys.argv) > 2 else 12
top = int(sys.argv[3]) if len(sys.argv) > 3 else 8
polls, current, frames = [], {}, []
for line in open(path, errors='replace'):
    if line.startswith('EDEN_DEV_WAIT_THREAD '):
        m = re.search(r'tid=(\d+) name=(.*?) calls=(\d+) ns=(\d+)', line)
        current[(int(m.group(1)), m.group(2))] = (int(m.group(3)), int(m.group(4)))
    elif line.startswith('EDEN_VULKAN_FRAME '):
        m = re.search(r'frames=(\d+) seconds=([0-9.]+)', line)
        frames.append((int(m.group(1)), float(m.group(2))))
        polls.append(current)
        current = {}  # only threads listed in a poll: one in a long wait is not, and is skipped
if len(polls) < windows + 1:
    sys.exit(f'only {len(polls)} polls')
first, last = polls[-windows - 1], polls[-1]
n_frames = sum(f for f, _ in frames[-windows:])
seconds = sum(s for _, s in frames[-windows:])
run = {}
for key, (calls, ns) in last.items():
    if key in first and calls > first[key][0]:  # threads that never wait here are host-side or parked
        run[key] = seconds * 1e3 - (ns - first[key][1]) / 1e6  # ms not waiting over the span
print(f'{windows} windows, {seconds:.1f} s, {n_frames} frames, {n_frames / seconds:.2f} fps')
for (tid, name), ms in sorted(run.items(), key=lambda kv: -kv[1])[:top]:
    print(f'{ms / n_frames:6.2f} ms/frame  {ms / seconds / 10:5.1f}%  {tid} {name}')
