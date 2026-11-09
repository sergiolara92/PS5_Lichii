#!/usr/bin/env python3
# critpath.py <sched-trace.bin> [heap.txt] [presenter tid]: the critical path of each traced frame
# (present to present), walked backwards from the present through "ran", "waited for a core" and "was
# woken by" links. The trace has no waker, so each wake of thread X is attributed to one of the guest
# threads running at that instant, chosen by how often each one wakes X over the whole trace (EM over
# the candidate sets); a wake with no guest thread running is "external" (host: GPU fence, IPC reply,
# timer). Prints, for 1-tick (S) frames: the path's time per thread and per link kind, and the most
# common chains (thread <- waker).
import bisect, collections, re, struct, sys, statistics

data = open(sys.argv[1], 'rb').read()
recs = sorted(r for r in (struct.unpack_from('<qIIQ', data, i) for i in range(0, len(data) - 23, 24)) if r[0])
names = {}
if len(sys.argv) > 2:
    for line in open(sys.argv[2], errors='replace'):
        m = re.match(r'EDEN_DEV_WAIT_THREAD tid=(\d+) name=(\S*)', line)
        if m and m.group(2):
            names[int(m.group(1))] = m.group(2)
name = lambda t: f'{t}' + (f' {names[t]}' if t in names else '') if t != 'ext' else 'external'

core_segs = collections.defaultdict(list)  # core -> [(start, end, thread)]
last = {}
for ns, c, k, t in recs:
    if k == 0:
        if c in last and last[c][1]:
            core_segs[c].append((last[c][0], ns, last[c][1]))
        last[c] = (ns, t)
thread_segs = collections.defaultdict(list)
seg_core = {}  # (thread, start) -> core
for c, ss in core_segs.items():
    for s, e, t in ss:
        thread_segs[t].append((s, e))
        seg_core[(t, s)] = c
for t in thread_segs:
    v = sorted(thread_segs[t])
    # A migrating thread starts on its new core before its old core records the switch away: clip.
    thread_segs[t] = [(st, min(en, v[i + 1][0]) if i + 1 < len(v) else en) for i, (st, en) in enumerate(v)]
starts = {t: [s for s, _ in v] for t, v in thread_segs.items()}
runnable = collections.defaultdict(list)
for ns, c, k, t in recs:
    if k == 2:
        runnable[t].append(ns)
presents = [ns for ns, c, k, _ in recs if k == 1]

def running_at(ns, exclude):
    out = []
    for ss in core_segs.values():
        i = bisect.bisect_right(ss, (ns, 1 << 62, 0)) - 1
        if i >= 0 and ss[i][0] <= ns < ss[i][1] and ss[i][2] != exclude:
            out.append(ss[i][2])
    return out

# Presenter: the thread running at the most presents.
if len(sys.argv) > 3:
    presenter = int(sys.argv[3])
else:
    cnt = collections.Counter(t for p in presents for t in running_at(p, None))
    presenter = cnt.most_common(1)[0][0]

# EM: P(waker | woken thread), candidates = guest threads running at the wake, or external if none.
cands = {}
for t, rs in runnable.items():
    for r in rs:
        c = running_at(r, t)
        cands[(t, r)] = c if c else ['ext']
prior = collections.defaultdict(lambda: collections.defaultdict(lambda: 1.0))
for _ in range(8):
    acc = collections.defaultdict(lambda: collections.defaultdict(float))
    for (t, r), cs in cands.items():
        z = sum(prior[t][w] for w in cs)
        for w in cs:
            acc[t][w] += prior[t][w] / z
    prior = collections.defaultdict(lambda: collections.defaultdict(lambda: 1e-3))
    for t, ws in acc.items():
        for w, v in ws.items():
            prior[t][w] = v
waker = {key: max(cs, key=lambda w: prior[key[0]][w]) for key, cs in cands.items()}

def seg_at(t, ns):
    """Index of thread t's segment that contains ns or ends at/before it (the latest started before ns)."""
    return bisect.bisect_left(starts[t], ns) - 1

def walk(frame_start, end):
    kinds = collections.Counter()
    per_thread = collections.Counter()
    detail = collections.Counter()  # (kind, path thread, other thread) -> ns
    chain = []
    x, t = presenter, end
    for _ in range(100000):
        if t <= frame_start:
            break
        i = seg_at(x, t)
        if i < 0:
            kinds['unknown'] += t - frame_start
            break
        s, e = thread_segs[x][i]
        run = min(t, e) - max(s, frame_start)
        if t > e:  # between segments (should not happen on the path)
            kinds['gap'] += t - max(e, frame_start)
        if run > 0:
            per_thread[x] += run
            kinds['run'] += run
        if s <= frame_start:
            break
        pe = thread_segs[x][i - 1][1] if i > 0 else -1
        rs = runnable[x]
        j = bisect.bisect_right(rs, s) - 1
        if j >= 0 and rs[j] > pe:
            r = rs[j]
            kinds['wait for a core'] += s - max(r, frame_start)
            detail[('wait for a core', x, '')] += s - max(r, frame_start)
            w = waker[(x, r)]
            if w == 'ext':
                kinds['external wait'] += max(0, r - max(pe, frame_start))
                detail[('external wait', x, '')] += max(0, r - max(pe, frame_start))
                chain.append((x, 'ext'))
                t = pe
            else:
                chain.append((x, w))
                x, t = w, r
        else:
            kinds['preempted'] += s - max(pe, frame_start)
            # who ran on the core x left at pe, until x ran again
            lo_, hi_ = max(pe, frame_start), s
            ss = core_segs[seg_core[(x, thread_segs[x][i - 1][0])]] if i > 0 else []
            k0 = max(0, bisect.bisect_right(ss, (lo_, 1 << 62, 0)) - 1)
            for st, en, th in ss[k0:]:
                if st >= hi_:
                    break
                if en > lo_ and th != x:
                    detail[('preempted', x, th)] += min(en, hi_) - max(st, lo_)
            t = pe
    return kinds, per_thread, chain, detail

frames = list(zip(presents, presents[1:]))
lens = [(e - b) / 1e6 for b, e in frames]
q = statistics.quantiles(lens, n=10)
lo, hi = q[1], q[7]
for _ in range(20):
    cut = (lo + hi) / 2
    a = [x for x in lens if x < cut]; b = [x for x in lens if cut <= x < 2.2 * lo]
    lo, hi = statistics.mean(a), statistics.mean(b)
S = [(b, e) for (b, e), L in zip(frames, lens) if L < cut]
print(f'{len(frames)} frames, presenter {name(presenter)}, {len(S)} 1-tick frames (< {cut:.1f} ms), mean {statistics.mean(lens[i] for i in range(len(frames)) if lens[i] < cut):.2f} ms')
amb = collections.Counter(min(len(c), 3) if c != ['ext'] else 0 for c in cands.values())
print(f'wakes: {sum(amb.values())}; external {amb[0]}, 1 candidate {amb[1]}, 2 {amb[2]}, 3+ {amb[3]}')
K = collections.Counter(); P = collections.Counter(); C = collections.Counter(); D = collections.Counter(); links = []
for b, e in S:
    k, p, ch, d = walk(b, e)
    K.update(k); P.update(p); C.update(ch); D.update(d); links.append(len(ch))
n = len(S)
print(f'critical path per 1-tick frame ({statistics.mean(links):.0f} wake links on average):')
for kind, v in K.most_common():
    print(f'   {kind:16} {v / n / 1e6:6.2f} ms')
print('run time on the path by thread (ms per 1-tick frame):')
for t, v in P.most_common(12):
    print(f'   {name(t):28} {v / n / 1e6:6.2f}')
print('most common wake links (woken <- waker), per frame:')
for (x, w), v in C.most_common(14):
    print(f'   {name(x):24} <- {name(w):24} {v / n:5.1f}')
print('non-run links by path thread (ms per 1-tick frame; preempted: the thread that ran on its core instead):')
for (kind, x, o), v in D.most_common(16):
    print(f'   {kind:16} {name(x):24} {name(o) if o != "" else "":24} {v / n / 1e6:6.2f}')
