#!/usr/bin/env python3
# wakelat.py <sched-trace.bin> [heap.txt]: wake-to-run latency of guest threads (Eden 0136 trace): for
# each wait end (kind 3), the time until that thread is next switched in, split by what the core it
# then ran on was doing at the wake (idle, or running another thread) and by waker (guest/host).
import bisect, collections, re, struct, sys
data = open(sys.argv[1], 'rb').read()
recs = sorted(struct.iter_unpack('<qIIQ', data[:len(data) // 24 * 24]))
names = {}
if len(sys.argv) > 2:
    for line in open(sys.argv[2], errors='replace'):
        m = re.match(r'EDEN_DEV_WAIT_THREAD tid=(\d+) name=(.*?) calls=', line)
        if m and m.group(2): names[int(m.group(1))] = m.group(2)
switch_in = collections.defaultdict(list)   # tid -> [(ns, core)]
core_seq = collections.defaultdict(list)    # core -> [(ns, tid)]
wakes = []
for ns, core, kind, ident in recs:
    if kind == 0:
        switch_in[ident].append((ns, core)); core_seq[core].append((ns, ident))
    elif kind == 3:
        wakes.append((ns, ident & ((1 << 56) - 1), core))
core_times = {c: [x[0] for x in v] for c, v in core_seq.items()}
span = (recs[-1][0] - recs[0][0]) / 1e9
stats = collections.defaultdict(lambda: [0, 0, []])   # class -> [count, total ns, samples]
for w, tid, by in wakes:
    v = switch_in.get(tid)
    if not v: continue
    i = bisect.bisect_left(v, (w, -1))
    if i >= len(v): continue
    s, core = v[i]
    lat = s - w
    if lat > 50_000_000: continue  # not a plain wake-to-run (thread stayed off for another reason)
    j = bisect.bisect_right(core_times[core], w) - 1
    occ = core_seq[core][j][1] if j >= 0 else 0
    cls = ('core idle' if occ == 0 else 'core busy') + (', host waker' if by >= 1000 else ', guest waker')
    st = stats[cls]; st[0] += 1; st[1] += lat; st[2].append(lat)
print(f'{span:.1f} s traced, {len(wakes)} wakes')
for cls, (n, tot, lats) in sorted(stats.items(), key=lambda kv: -kv[1][1]):
    lats.sort()
    print(f'{cls:28} {n:7d} wakes {tot / span / 1e6:7.1f} ms/s  median {lats[len(lats) // 2] / 1e3:7.1f} us  '
          f'p90 {lats[len(lats) * 9 // 10] / 1e3:8.1f} us')
