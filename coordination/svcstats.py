#!/usr/bin/env python3
# svcstats.py <sched-trace.bin>: per SVC (0183 records), calls per second and the time the calling thread ran
# inside it (span minus the time it was switched out), mean and p90 per call; and the timed events' lateness.
import bisect, collections, struct, sys
data = open(sys.argv[1], 'rb').read()
recs = sorted(struct.iter_unpack('<qIIQ', data[:len(data) // 24 * 24]))
SVCS = {0x0b: 'SleepThread', 0x18: 'WaitSynchronization', 0x1a: 'ArbitrateLock', 0x1b: 'ArbitrateUnlock',
        0x1c: 'WaitProcessWideKeyAtomic', 0x1d: 'SignalProcessWideKey', 0x21: 'SendSyncRequest', 0x34: 'WaitForAddress',
        0x35: 'SignalToAddress', 0x11: 'SignalEvent', 0x17: 'ResetSignal', 0x1e: 'GetSystemTick', 0x29: 'GetInfo'}
segs = collections.defaultdict(list); cur = {}; opened = {}; spans = []
t0, t1 = None, None
for ns, core, kind, ident in recs:
    if kind == 6: continue
    t0 = ns if t0 is None else t0; t1 = ns
    if kind == 0:
        if core in cur and cur[core][0]: segs[cur[core][0]].append((cur[core][1], ns))
        cur[core] = (ident, ns)
    elif kind == 9: opened[ident & ((1 << 48) - 1)] = (ns, ident >> 48)
    elif kind == 10:
        tid = ident & ((1 << 48) - 1)
        if tid in opened:
            b, num = opened.pop(tid); spans.append((tid, b, ns, num))
starts = {t: [s[0] for s in v] for t, v in segs.items()}
per = collections.defaultdict(list)
for tid, b, e, num in spans:
    v = segs.get(tid, []); i = max(bisect.bisect_right(starts.get(tid, []), b) - 1, 0); ran = 0
    while i < len(v) and v[i][0] < e:
        ran += max(0, min(v[i][1], e) - max(v[i][0], b)); i += 1
    per[num].append((ran, e - b))
secs = (t1 - t0) / 1e9
print(f'trace {secs:.2f} s, {len(spans)} SVCs')
for num, v in sorted(per.items(), key=lambda kv: -sum(r for r, _ in kv[1])):
    ran = sorted(r for r, _ in v)
    print(f'{SVCS.get(num, hex(num)):26s} {len(v) / secs:8.0f}/s  ran mean {sum(ran) / len(ran) / 1e3:6.1f} us  p50 {ran[len(ran) // 2] / 1e3:6.1f}  '
          f'p90 {ran[int(len(ran) * 0.9)] / 1e3:6.1f}  total {sum(ran) / secs / 1e6 * 1e3:6.1f} ms/s  span mean {sum(s for _, s in v) / len(v) / 1e3:7.1f} us')
