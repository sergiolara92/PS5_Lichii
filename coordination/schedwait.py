#!/usr/bin/env python3
# schedwait.py <sched-trace.bin> [thread id]: waits from "runnable" (kind 2) to running (kind 0) in
# a guest scheduler trace (patches 0034/0036). For each wait: how long, and what the core it ran on
# did meanwhile (idle the whole time, or running other threads). Long waits on an idle core are
# wake-up delivery delays; on a busy core, the thread queued behind others.
import bisect, collections, struct, sys

data = open(sys.argv[1], 'rb').read()
records = sorted(struct.unpack_from('<qIIQ', data, i) for i in range(0, len(data) - 23, 24))
records = [r for r in records if r[0]]
focus = int(sys.argv[2]) if len(sys.argv) > 2 else None

# Each core's timeline: (start, thread) where thread 0 is idle.
timeline = collections.defaultdict(list)
for ns, core, kind, ident in records:
    if kind == 0:
        timeline[core].append((ns, ident))

def idle_share(core, begin, end):
    line = timeline[core]
    i = max(0, bisect.bisect_right(line, (begin, 1 << 64)) - 1)
    idle = 0
    while i < len(line) and line[i][0] < end:
        s = max(line[i][0], begin)
        e = min(line[i + 1][0] if i + 1 < len(line) else end, end)
        if line[i][1] == 0 and e > s:
            idle += e - s
        i += 1
    return idle / (end - begin) if end > begin else 1.0

pending = {}
buckets = collections.defaultdict(lambda: [0, 0, 0])  # range -> [count, idle-core count, total ns]
bounds = [(0, 50e3, '<50us'), (50e3, 200e3, '50-200us'), (200e3, 1e6, '0.2-1ms'), (1e6, 5e6, '1-5ms'), (5e6, 1e12, '>5ms')]
for ns, core, kind, ident in records:
    if kind == 2:
        pending.setdefault(ident, ns)
    elif kind == 0 and ident in pending:
        start = pending.pop(ident)
        if focus is not None and ident != focus:
            continue
        wait = ns - start
        for lo, hi, name in bounds:
            if lo <= wait < hi:
                b = buckets[name]
                b[0] += 1
                b[2] += wait
                if wait >= 50e3 and idle_share(core, start, ns) > 0.9:
                    b[1] += 1
seconds = (records[-1][0] - records[0][0]) / 1e9
print(f'{"thread " + str(focus) if focus is not None else "all threads"}, {seconds:.1f} s')
for lo, hi, name in bounds:
    count, idle, total = buckets[name]
    print(f'  {name:9s} {count / seconds:8.0f}/s  {total / 1e6 / seconds:7.1f} ms/s waiting'
          + (f'  core idle the whole time in {idle * 100 / count:.0f}%' if count and lo >= 50e3 else ''))
