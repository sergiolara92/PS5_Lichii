#!/usr/bin/env python3
# schedtrace.py <sched-trace.bin> [heap.txt]: guest thread switches and presents recorded with
# EDEN_NX2_SCHED_TRACE (patch 0034), per frame (present to present): frame time, each core's busy
# time, the threads that ran longest and their names (EDEN_DEV_WAIT_THREAD lines of heap.txt).
import collections, re, statistics, struct, sys

records = []
data = open(sys.argv[1], 'rb').read()
for i in range(0, len(data) - 23, 24):
    ns, core, kind, ident = struct.unpack_from('<qIIQ', data, i)
    if ns:
        records.append((ns, core, kind, ident))
records.sort()
names = {}
if len(sys.argv) > 2:
    for line in open(sys.argv[2], errors='replace'):
        if line.startswith('EDEN_DEV_WAIT_THREAD '):
            m = re.search(r'tid=(\d+) name=(\S*)', line)
            if m and m.group(2):
                names[int(m.group(1))] = m.group(2)

presents = [ns for ns, core, kind, _ in records if kind == 1]
switches = [(ns, core, ident) for ns, core, kind, ident in records if kind == 0]
print(f'{len(switches)} switches, {len(presents)} presents over {(records[-1][0] - records[0][0]) / 1e9:.2f} s')
if len(presents) < 3:
    sys.exit('too few presents')

# Run intervals per core: from a switch to the core's next switch.
intervals = []  # (start, end, core, thread)
last = {}
for ns, core, ident in switches:
    if core in last:
        start, thread = last[core]
        if thread:
            intervals.append((start, ns, core, thread))
    last[core] = (ns, ident)

frames = list(zip(presents, presents[1:]))
per_thread_frame = collections.defaultdict(list)
core_busy = collections.defaultdict(list)
frame_ms = []
longest = collections.Counter()
for begin, end in frames:
    run = collections.Counter()
    busy = collections.Counter()
    for s, e, core, thread in intervals:
        overlap = min(e, end) - max(s, begin)
        if overlap > 0:
            run[thread] += overlap
            busy[core] += overlap
    length = end - begin
    frame_ms.append(length / 1e6)
    for core in range(6):
        core_busy[core].append(busy[core] / length)
    for thread, t in run.items():
        per_thread_frame[thread].append(t / 1e6)
    if run:
        longest[run.most_common(1)[0][0]] += 1

print(f'frames {len(frames)}: mean {statistics.mean(frame_ms):.1f} ms, median {statistics.median(frame_ms):.1f}')
print('core busy share per frame: ' + ' '.join(f'{c}:{statistics.mean(v) * 100:.0f}%' for c, v in sorted(core_busy.items())))
print('threads by mean run time per frame (ms):')
rows = sorted(per_thread_frame.items(), key=lambda kv: -sum(kv[1]) / len(frames))
for thread, values in rows[:14]:
    mean = sum(values) / len(frames)
    print(f'  {thread:6d} {names.get(thread, "?"):24s} {mean:6.2f} ms  max {max(values):6.2f}  longest in {longest[thread]} frames')
