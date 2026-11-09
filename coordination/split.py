import re, sys, collections
# Per-window deltas of every *ns field on single-per-window EDEN_DEV_* lines (cumulative counters).
path = sys.argv[1]; lo, hi = int(sys.argv[2]), int(sys.argv[3])
keys = ['EDEN_DEV_GPU','EDEN_DEV_DRAW','EDEN_DEV_RAST','EDEN_DEV_CFG','EDEN_DEV_BIND','EDEN_DEV_UPD','EDEN_DEV_PUSH','EDEN_DEV_GEO','EDEN_DEV_WAIT','EDEN_DEV_GC_DRAIN','EDEN_DEV_GUEST','EDEN_DEV_HLE']
seq = collections.defaultdict(list)
for line in open(path, errors='replace'):
    k = line.split(' ', 1)[0]
    if k in keys:
        seq[k].append({a: int(b) for a, b in re.findall(r'(\w+)=(-?\d+)\b', line)})
fps = [re.search(r'fps=([\d.]+) worst_ms=([\d.]+)', l).groups() for l in open(path, errors='replace') if l.startswith('EDEN_VULKAN_FRAME ')]
tot = collections.Counter()
for w in range(lo, hi):
    for k in keys:
        if k not in seq or w + 1 >= len(seq[k]): continue
        a, b = seq[k][w], seq[k][w + 1]
        for f in b:
            if ('ns' in f) and f in a: tot[k + '.' + f] += b[f] - a[f]
n = hi - lo
print('windows', lo, hi, 'fps', [f[0] for f in fps[lo:hi]])
for f, v in tot.most_common(40):
    print(f'{f:45s} {v / n / 5e9 * 100:7.1f}% of 5s')
