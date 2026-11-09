#!/usr/bin/env python3
# rank.py <binary> <dump> [top]: live bytes by owner (first frame outside std/boost/allocator code).
import re, subprocess, sys
from collections import defaultdict

binary, dump = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
lines = open(dump).read().splitlines()
sites, head = [], []
for line in lines:
    if line.startswith('site '):
        parts = line.split()
        sites.append((int(parts[1]), int(parts[2]), parts[3:]))
    else:
        head.append(line)
offs = sorted({f.split('+')[1] for _, _, fr in sites for f in fr if f.startswith('eden-headless+')})
names = {}
if offs:
    out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', binary] + [hex(int(o, 16) - 1) for o in offs],
                         capture_output=True, text=True).stdout.splitlines()
    for i, o in enumerate(offs):
        names[o] = out[2 * i] + ' @' + out[2 * i + 1].split('/')[-1]
NOISE = re.compile(r'^(std::|boost::|__gnu_cxx|operator new|ankerl::|fmt::|Common::(?!Fiber)|tsl::|void std::|__)')

def owner(frames):
    chain = []
    for f in frames:
        if f.startswith('eden-headless+'):
            n = names.get(f.split('+')[1], f)
            chain.append(n)
        else:
            chain.append(f)
    for i, n in enumerate(chain):
        if not NOISE.match(n) and not n.startswith('lib'):
            return n, chain
    return chain[0] if chain else '?', chain

for h in head:
    if h.startswith('live') or h.startswith('class'):
        parts = h.split()
        if h.startswith('live'):
            print(f'live {int(parts[1]) / 2**20:.0f} MiB')
        else:
            print(f'  2^{parts[1]:>2}: {int(parts[2]) / 2**20:8.1f} MiB {parts[4]:>9} blocks')
by = defaultdict(lambda: [0, 0, None])
for b, n, fr in sites:
    o, chain = owner(fr)
    e = by[o]; e[0] += b; e[1] += n
    if e[2] is None: e[2] = chain
print('\nby owner:')
for o, (b, n, chain) in sorted(by.items(), key=lambda kv: -kv[1][0])[:top]:
    short = lambda s: re.sub(r'\(.*', '', s)[-70:]
    print(f'{b / 2**20:8.1f} MiB {n:>9}  {short(o)}')
    if '-v' in sys.argv:
        print('            ' + ' <- '.join(short(c) for c in chain[:6]))
