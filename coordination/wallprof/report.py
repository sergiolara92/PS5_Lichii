import struct, sys, collections, subprocess, re
# wallprof.so samples -> self and inclusive time per function, per sampled thread.
base = sys.argv[1]; binary = sys.argv[2]; top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
maps = []
for line in open(base + '.maps'):
    p = line.split()
    if len(p) >= 6 and 'x' in p[1]:
        lo, hi = (int(x, 16) for x in p[0].split('-'))
        maps.append((lo, hi, int(p[2], 16), p[5]))
tags = {int(l.split()[0]): l.split()[2] for l in open(base + '.tags')}
loads = {}  # module -> load bias (start of its first mapping minus file offset)
for line in open(base + '.maps'):
    p = line.split()
    if len(p) >= 6:
        lo = int(p[0].split('-')[0], 16)
        loads.setdefault(p[5], lo - int(p[2], 16))
def where(a):
    for lo, hi, off, name in maps:
        if lo <= a < hi: return name, a - loads[name]
    return '?', a
raw = open(base, 'rb').read()
size = 8 + 8 * 64
samples = collections.defaultdict(list)
for i in range(0, len(raw) - size + 1, size):
    tag, depth = struct.unpack_from('ii', raw, i)
    pcs = struct.unpack_from('64Q', raw, i + 8)[:depth]
    samples[tag].append(pcs)
want = set()
for tag, ss in samples.items():
    for pcs in ss:
        for k, a in enumerate(pcs):
            m, o = where(a if k == 0 else a - 1)
            if m.endswith('eden-headless'): want.add(o)
want = sorted(want)
out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', binary] + [hex(o) for o in want],
                     capture_output=True, text=True).stdout.split('\n')
names = {o: re.sub(r'\(.*', '', out[2 * i])[:120] for i, o in enumerate(want)}
def fn(a, k):
    m, o = where(a if k == 0 else a - 1)
    if m.endswith('eden-headless'): return names.get(o, hex(o))
    return m.rsplit('/', 1)[-1]
for tag, ss in sorted(samples.items()):
    n = len(ss); selfc = collections.Counter(); incl = collections.Counter()
    for pcs in ss:
        frames = [fn(a, k) for k, a in enumerate(pcs[2:])]  # skip handler + signal trampoline
        if frames: selfc[frames[0]] += 1
        for f in set(frames): incl[f] += 1
    print(f'==== {tags.get(tag, tag)}: {n} samples')
    print('-- self'); [print(f'{c*100/n:5.1f}%  {f}') for f, c in selfc.most_common(top)]
    print('-- inclusive'); [print(f'{c*100/n:5.1f}%  {f}') for f, c in incl.most_common(top)]
