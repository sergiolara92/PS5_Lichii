#!/usr/bin/env python3
# dispatchshare.py <pmcjit samples> <eden-headless>: share of cycles in the A64 fast-dispatch/RSB
# terminal handlers (the code around each `cmp rbx, [r12]` probe, 49 3b 1c 24) among JIT cycles, and
# the emulator's block-lookup functions (LookupBlock/GetBlock) among all cycles of the sampled threads.
import collections, re, subprocess, sys
rows = [l.split() for l in open(sys.argv[1])]
cyc = [r for r in rows if r[0] == 'c']
jit = [(int(r[2], 16), r[3]) for r in cyc if r[3] != '-']
probes = {ip for ip, b in jit if b.startswith('493b1c24')}
near = lambda ip: any(-0x90 <= ip - p < 0x30 for p in probes)
handler = sum(1 for ip, b in jit if near(ip))
maps = []
for line in open(sys.argv[1] + '.maps'):
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \S+\s*(.*)', line)
    if m and 'x' in m.group(3) and m.group(5).endswith('eden-headless'):
        maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16)))
segs = []
for l in subprocess.run(['readelf', '-lW', sys.argv[2]], capture_output=True, text=True).stdout.split('\n'):
    f = l.split()
    if f and f[0] == 'LOAD': segs.append((int(f[1], 16), int(f[2], 16), int(f[4], 16)))
def vaddr(ip):
    for s, e, off in maps:
        if s <= ip < e:
            o = ip - s + off
            for so, v, size in segs:
                if so <= o < so + size: return o - so + v
    return None
app = collections.Counter(vaddr(int(r[2], 16)) for r in cyc if r[3] == '-')
app.pop(None, None)
offs = sorted(app)
names = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', sys.argv[2]] + [hex(o) for o in offs], capture_output=True, text=True).stdout.split('\n')
lookup = sum(n for o, n in app.items() if re.search(r'LookupBlock|GetBlock|EdenLookup|FastDispatch', names[2 * offs.index(o)]))
print(f'cycles {len(cyc)}, JIT {len(jit)}; handlers {handler * 100 / max(len(jit), 1):.2f}% of JIT; '
      f'block lookup {lookup * 100 / len(cyc):.2f}% of all; probes at {len(probes)} sites')
