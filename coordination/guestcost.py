#!/usr/bin/env python3
# guestcost.py <heap.txt> <sched-trace.bin> [tids]: emulator costs on the guest cores over the 5 s windows
# a scheduler trace covers, in ms per second (6 guest cores = 6000 ms/s): reads/writes of GPU-tracked
# memory (EDEN_DEV_GUEST), JIT compilation per core (EDEN_PERF_PROGRESS), fastmem faults/demotions,
# dispatcher lookups (EDEN_PERF_JITPATH), and IPC time of the given threads (EDEN_DEV_IPC_THREAD).
import re, struct, sys
heap, trace = sys.argv[1], sys.argv[2]
tids = set(sys.argv[3].split(',')) if len(sys.argv) > 3 else set()
d = open(trace, 'rb').read()
ns = [x for x in (struct.unpack_from('<q', d, i)[0] for i in range(0, len(d) - 23, 24)) if x]
t0, t1 = min(ns), max(ns)
wins = []; cur = {}; mono = None
for line in open(heap, errors='replace'):
    m = re.match(r'EDEN_PERF_SAMPLE mono_ns=(\d+)', line)
    if m:
        if cur and mono is not None: wins.append((mono, cur))
        mono = int(m.group(1)); cur = {}
        continue
    if line.startswith('EDEN_DEV_GUEST '):
        for k in ('cpu_write_ns', 'cpu_read_ns', 'cpu_write_calls', 'cpu_read_calls', 'dirty_replays', 'write_deferred'):
            cur[k] = int(re.search(k + r'=(\d+)', line).group(1))
    m = re.match(r'EDEN_PERF_PROGRESS .*core=(\d+) compilations=(\d+) compile_ns=(\d+)', line)
    if m: cur[f'compile{m.group(1)}'] = int(m.group(3)); cur[f'compiles{m.group(1)}'] = int(m.group(2))
    m = re.match(r'EDEN_PERF_JITPATH .*core=(\d+) runs=(\d+) thunks=(\d+) lookups=(\d+)', line)
    if m: cur[f'lookups{m.group(1)}'] = int(m.group(4)); cur[f'runs{m.group(1)}'] = int(m.group(2))
    m = re.match(r'EDEN_FASTMEM .*faults=(\d+) demotions=(\d+).*kernel_calls=(\d+) kernel_ns=(\d+)', line)
    if m: cur['fm_faults'], cur['fm_demotions'], cur['fm_kcalls'], cur['fm_kns'] = map(int, m.groups())
    if line.startswith('EDEN_FASTMEM64_WHY '):
        for k in ('track_mtsc', 'protect_mtsc', 'fault_mtsc', 'run_mtsc', 'protect_pieces', 'track_calls'):
            mm = re.search(r'\b' + k + r'=(\d+)', line)
            if mm: cur['fm64_' + k] = int(mm.group(1))
    m = re.match(r'EDEN_FASTMEM64 faults=(\d+)', line)
    if m: cur['fm64_faults'] = int(m.group(1))
    m = re.match(r'EDEN_DEV_IPC_THREAD slot=\d+ tid=(\d+) name=(.*) calls=(\d+) ns=(\d+)', line)
    if m and m.group(1) in tids: cur[f'ipc{m.group(1)}'] = int(m.group(4)); cur[f'ipcc{m.group(1)}'] = int(m.group(3))
if cur and mono is not None: wins.append((mono, cur))
# windows (previous sample, this sample] overlapping the trace
sel = [(a, b) for a, b in zip(wins, wins[1:]) if b[0] > t0 and a[0] < t1]
first, last = sel[0][0], sel[-1][1]
secs = (last[0] - first[0]) / 1e9
D = {k: last[1].get(k, 0) - first[1].get(k, 0) for k in last[1]}
print(f'{len(sel)} windows, {secs:.1f} s (trace {(t1 - t0) / 1e9:.1f} s)')
per = lambda k: D.get(k, 0) / 1e6 / secs
print(f'GPU-tracked memory: reads {per("cpu_read_ns"):.1f} ms/s ({D.get("cpu_read_calls",0)/secs/1e3:.0f}k/s), writes {per("cpu_write_ns"):.1f} ms/s ({D.get("cpu_write_calls",0)/secs/1e3:.0f}k/s), dirty replays {D.get("dirty_replays",0)/secs:.0f}/s, deferred writes {D.get("write_deferred",0)/secs:.0f}/s')
print('JIT compile per core ms/s: ' + ' '.join(f'{c}:{per(f"compile{c}"):.1f}({D.get(f"compiles{c}",0)/secs:.0f}/s)' for c in range(6) if f'compile{c}' in D))
print('dispatcher lookups per core /s: ' + ' '.join(f'{c}:{D.get(f"lookups{c}",0)/secs/1e3:.0f}k' for c in range(6) if f'lookups{c}' in D))
print(f'fastmem: faults {D.get("fm_faults",0)/secs:.0f}/s, demotions {D.get("fm_demotions",0)/secs:.0f}/s, kernel calls {D.get("fm_kcalls",0)/secs:.0f}/s {per("fm_kns"):.2f} ms/s')
run = D.get('fm64_run_mtsc', 0) or 1
print(f'fastmem64 census (share of guest run time): track {100*D.get("fm64_track_mtsc",0)/run:.2f}%, protect {100*D.get("fm64_protect_mtsc",0)/run:.2f}%, fault {100*D.get("fm64_fault_mtsc",0)/run:.2f}%; faults {D.get("fm64_faults",0)/secs:.1f}/s (count is not cumulative if it drops: {D.get("fm64_faults",0)}), protect pieces {D.get("fm64_protect_pieces",0)/secs:.0f}/s, track calls {D.get("fm64_track_calls",0)/secs/1e3:.0f}k/s')
for t in sorted(tids):
    if f'ipc{t}' in D:
        print(f'IPC thread {t}: {per(f"ipc{t}"):.2f} ms/s, {D.get(f"ipcc{t}",0)/secs:.0f} calls/s')
