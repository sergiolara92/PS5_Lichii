#!/usr/bin/env python3
# frametrace.py <sched-trace.bin> [heap.txt]: where each frame's time goes on its critical path, from
# the scheduler trace with Eden 0136's records (EDEN_NX2_SCHED_TRACE; record kinds in sched_trace.h).
# Each frame (present to present) is walked back from the present: a guest thread that ran is "run";
# before that it was runnable ("wait for a core" after a wake, "preempted" otherwise) or waiting, and
# a wait leads to whoever ended it: a guest thread (continue on it), a host thread (GPU front/back,
# Vulkan; continue on it through its busy time and the wait zone before it) or nobody known
# ("external": timers, vsync). Prints the ranked mean ms a frame per category, for all frames and
# for frames over 20 ms.
import bisect, collections, re, struct, sys

data = open(sys.argv[1], 'rb').read()
recs = sorted(struct.iter_unpack('<qIIQ', data[:len(data) // 24 * 24]))
REASONS = ['none', 'sleep', 'ipc', 'sync', 'condvar', 'arbitration', 'suspended', 'other']
ZONES = {1: 'idle', 2: 'pending sleep', 31: 'split drain', 32: 'split wait', 33: 'split full',
         34: 'drain sync', 35: 'drain puller', 36: 'drain mme dma read'}
names = {}
if len(sys.argv) > 2:
    for line in open(sys.argv[2], errors='replace'):
        m = re.match(r'EDEN_DEV_WAIT_THREAD tid=(\d+) name=(.*?) calls=', line)
        if m and m.group(2): names[int(m.group(1))] = m.group(2)
host_names = {}
segs = collections.defaultdict(list)      # guest tid -> [[start, end]]
waits = collections.defaultdict(list)     # guest tid -> [(begin, end, waker, reason)]
wait_open = {}
zones = collections.defaultdict(list)     # host tag -> [(begin, end, zone)]
zone_open = {}
presents, submits, fences = [], {}, []
timer_late, svc_open, svc_spans = [], {}, collections.defaultdict(list)  # 0183 records
SVCS = {0x01: 'SetHeapSize', 0x0b: 'SleepThread', 0x0c: 'GetThreadPriority', 0x0d: 'SetThreadPriority', 0x11: 'SignalEvent',
        0x12: 'ClearEvent', 0x16: 'CloseHandle', 0x17: 'ResetSignal', 0x18: 'WaitSynchronization', 0x19: 'CancelSynchronization',
        0x1a: 'ArbitrateLock', 0x1b: 'ArbitrateUnlock', 0x1c: 'WaitProcessWideKeyAtomic', 0x1d: 'SignalProcessWideKey',
        0x1e: 'GetSystemTick', 0x21: 'SendSyncRequest', 0x22: 'SendSyncRequestWithUserBuffer', 0x25: 'GetThreadId', 0x26: 'Break',
        0x27: 'OutputDebugString', 0x29: 'GetInfo', 0x34: 'WaitForAddress', 0x35: 'SignalToAddress', 0x36: 'SynchronizePreemptionState',
        0x40: 'CreateSession', 0x43: 'ReplyAndReceive'}
core_cur = {}
core_seq = {}                             # core -> [(ns, thread switched to)]
wait_begins = collections.defaultdict(list)
for ns, core, kind, ident in recs:
    if kind == 0:
        if core in core_cur:
            tid, start = core_cur[core]
            if tid: segs[tid].append([start, ns, core])
        core_cur[core] = (ident, ns)
        core_seq.setdefault(core, []).append((ns, ident))
    elif kind == 1:
        presents.append((ns, core))
    elif kind == 4:
        wait_open[ident] = ns
        wait_begins[ident].append(ns)
    elif kind == 3:
        tid, reason = ident & ((1 << 56) - 1), ident >> 56
        waits[tid].append((wait_open.pop(tid, ns), ns, core, reason))
    elif kind == 5:
        tag, zone = core & 0xffff, core >> 16
        if ident > 1: zones[tag].append((ident, ns, zone))  # one record at the end, id = entry time
        elif ident == 0: zone_open[tag] = (ns, zone)
        elif tag in zone_open:
            b, z = zone_open.pop(tag)
            zones[tag].append((b, ns, z))
    elif kind == 7:
        if ident & 3 == 0: submits[ident >> 2] = (ns, core)
        else: fences.append((ns, ident >> 2, core))
    elif kind == 6:
        host_names[core] = ident.to_bytes(8, 'little').rstrip(b'\0').decode(errors='replace')
    elif kind == 8:  # 0183: a CoreTiming event fired this late (ns)
        timer_late.append(ident)
    elif kind == 9:  # 0183: SVC entry (thread | number << 48)
        svc_open[ident & ((1 << 48) - 1)] = (ns, ident >> 48)
    elif kind == 10 and (ident & ((1 << 48) - 1)) in svc_open:
        b, num = svc_open.pop(ident & ((1 << 48) - 1))
        svc_spans[ident & ((1 << 48) - 1)].append((b, ns, num))
for v in list(segs.values()) + list(waits.values()) + list(zones.values()) + list(svc_spans.values()): v.sort()
svc_starts = {t: [x[0] for x in v] for t, v in svc_spans.items()}

def svc_split(who, lo, hi):
    # The part of [lo, hi] (thread `who` running) spent inside SVCs, by SVC (0183 records).
    v = svc_spans.get(who)
    parts = collections.Counter()
    if not v: return parts
    i = max(bisect.bisect_right(svc_starts[who], lo) - 1, 0)
    while i < len(v) and v[i][0] < hi:
        a, b = max(v[i][0], lo), min(v[i][1], hi)
        if b > a: parts[SVCS.get(v[i][2], hex(v[i][2]))] += b - a
        i += 1
    return parts
seg_starts = {t: [s[0] for s in v] for t, v in segs.items()}
wait_ends = {t: [w[1] for w in v] for t, v in waits.items()}
zone_ends = {t: [z[1] for z in v] for t, v in zones.items()}
core_times = {c: [x[0] for x in v] for c, v in core_seq.items()}

def core_wait_kind(who, w, t):
    # The core `who` ran on from t: idle at the wake w, or held by a thread that was preempted at t
    # (no wait of its own just before t: preemption latency) or that left by itself (queued behind it).
    i = bisect.bisect_left(seg_starts.get(who, []), t)
    if i >= len(segs.get(who, [])) or segs[who][i][0] != t: return 'wait for a core'
    core = segs[who][i][2]
    j = bisect.bisect_right(core_times[core], w) - 1
    occ = core_seq[core][j][1] if j >= 0 else 0
    if occ == 0: return 'wait for a core: idle core'
    k = bisect.bisect_left(core_times[core], t) - 1
    prev = core_seq[core][k][1] if k >= 0 else 0
    wb = wait_begins.get(prev, []); q = bisect.bisect_right(wb, t) - 1
    if prev and not (q >= 0 and wb[q] >= core_seq[core][k][0]):
        return 'wait for a core: preemption latency'
    return 'wait for a core: behind ' + gname(occ)

fence_at = {}
for ns, tick, tag in fences: fence_at.setdefault(tag, []).append((ns, tick))
tag_by_name = {n: t for t, n in host_names.items()}
FRONT, BACK = tag_by_name.get('GPU'), tag_by_name.get('GPUBack')
gname = lambda t: names.get(t, str(t))
hname = lambda t: host_names.get(t, f'host{t}')

def running_guest(t, prefer):
    best = None
    for tid, v in segs.items():
        i = bisect.bisect_right(seg_starts[tid], t) - 1
        if i >= 0 and v[i][0] <= t <= v[i][1]:
            if tid == prefer: return tid
            best = best or tid
    return best

preempted_by = collections.Counter()  # (critical-path thread, the guest thread on its core meanwhile) -> ns
def walk(fs, fe, presenter):
    out = collections.Counter()
    who, kind, t = presenter, 'g', fe
    for _ in range(4000):
        if t <= fs: break
        if kind == 'g':
            v = segs.get(who, [])
            i = bisect.bisect_left(seg_starts.get(who, []), t) - 1
            seg = v[i] if i >= 0 else None
            if seg and seg[1] >= t:
                lo = max(seg[0], fs)
                inside = svc_split(who, lo, t)
                for name_, v_ in inside.items(): out[('svc ' + name_, gname(who))] += v_
                out[('run', gname(who))] += t - lo - sum(inside.values()); t = seg[0]; continue
            run_end = seg[1] if seg else fs
            j = bisect.bisect_right(wait_ends.get(who, []), t) - 1
            w = waits[who][j] if j >= 0 else None
            if w and w[1] > run_end:
                out[(core_wait_kind(who, w[1], t), gname(who))] += t - max(w[1], fs); t = w[1]
                b, e, by, reason = w
                if by >= 1000 and (by in zones or by in fence_at):  # a GPU thread: follow it
                    who, kind = by, 'h'
                elif by >= 1000:  # another host thread (timers, audio, services): its wait is external
                    out[('external ' + REASONS[reason] + ' via ' + hname(by), gname(who))] += t - max(b, fs); t = b
                    segment_count['external ' + REASONS[reason] + ' via ' + hname(by)] += 1
                elif by in segs and by != who: who = by
                else:
                    out[('external ' + REASONS[reason], gname(who))] += t - max(b, fs); t = b
                continue
            out[('preempted', gname(who))] += t - max(run_end, fs)
            # Who held the core it resumed on meanwhile (guest threads, by their time in the gap).
            i2 = bisect.bisect_left(seg_starts.get(who, []), t)
            if i2 < len(segs.get(who, [])) and segs[who][i2][0] == t:
                c2 = segs[who][i2][2]; lo = max(run_end, fs)
                k2 = bisect.bisect_right(core_times[c2], lo) - 1
                for idx in range(max(k2, 0), len(core_seq[c2])):
                    ns0, occ = core_seq[c2][idx]
                    if ns0 >= t: break
                    ns1 = core_seq[c2][idx + 1][0] if idx + 1 < len(core_seq[c2]) else t
                    a, b = max(ns0, lo), min(ns1, t)
                    if b > a: preempted_by[(gname(who), gname(occ) if occ else 'idle')] += b - a
            t = run_end
        else:
            z = zones.get(who)
            if not z:
                f = [x for x in fence_at.get(who, []) if x[0] <= t]
                if f and f[-1][1] in submits:
                    sub_ns, sub_tag = submits[f[-1][1]]
                    out[('GPU execution', 'submit to fence')] += t - max(sub_ns, fs); t = sub_ns
                    if BACK is None: break
                    who = BACK; continue
                out[('host other', hname(who))] += t - fs; break
            i = bisect.bisect_right(zone_ends[who], t) - 1
            if i < 0:
                out[('host busy', hname(who))] += t - fs; break
            b, e, zone = z[i]
            out[('host busy', hname(who))] += t - max(e, fs); t = e
            if zone == 32 and FRONT is not None: who = FRONT
            elif zone in (31, 33, 34, 35, 36) and BACK is not None: who = BACK
            else:
                g = running_guest(e, presenter)
                if g: who, kind = g, 'g'
                else:
                    out[('host wait ' + ZONES.get(zone, str(zone)), hname(who))] += t - max(b, fs); t = b
    return out

frames = list(zip(presents, presents[1:]))
segment_count = collections.Counter()  # critical-path waits ended by a host thread, by kind (all frames walked)
if timer_late:
    late = sorted(timer_late)
    q = lambda f: late[min(len(late) - 1, int(f * len(late)))] / 1e3
    print(f'== timer events (0183): {len(late)}, late by p50 {q(0.5):.0f} us, p90 {q(0.9):.0f} us, p99 {q(0.99):.0f} us, mean {sum(late) / len(late) / 1e3:.0f} us')
for label, pick in (('all frames', lambda d: True), ('frames over 20 ms', lambda d: d > 20e6)):
    total, n, ms = collections.Counter(), 0, 0
    for (fs, _), (fe, presenter) in frames:
        if not pick(fe - fs): continue
        total.update(walk(fs, fe, presenter)); n += 1; ms += (fe - fs) / 1e6
    if not n: continue
    print(f'== {label}: {n} frames, mean {ms / n:.2f} ms')
    by_kind = collections.Counter()
    for (k, w), v in total.items(): by_kind[k] += v
    for k, v in by_kind.most_common():
        print(f'{v / n / 1e6:7.2f} ms {v / n / 1e4 / (ms / n):5.1f}%  {k}')
    if label == 'all frames':
        for k, c in segment_count.most_common(4): print(f'   {c / n:6.1f} a frame  {k} (waits on the critical path)')
    print('-- top items')
    for (k, w), v in total.most_common(15):
        print(f'{v / n / 1e6:7.2f} ms  {k}: {w}')
    print('-- preempted: who held the core meanwhile (ms a frame)')
    for (victim, by), v in preempted_by.most_common(12):
        print(f'{v / n / 1e6:7.2f} ms  {victim} <- {by}')
    preempted_by.clear()
