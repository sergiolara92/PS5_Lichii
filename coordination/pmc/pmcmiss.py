#!/usr/bin/env python3
# pmcmiss.py <out> <eden-headless> [thread name or tid prefix] [top]: pmcmiss samples by function:
# share of cycles (c), of L3-miss loads (m) and L2-miss loads (l). Addresses in the app's binary are
# symbolized with llvm-addr2line; other mappings by library name.
import collections, re, subprocess, sys
out, binary = sys.argv[1], sys.argv[2]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 30
maps = []
for line in open(out + '.maps'):
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \S+\s*(.*)', line)
    if m and 'x' in m.group(3):
        maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5).strip()))
tids = None
if len(sys.argv) > 3 and sys.argv[3] != '-':
    tids = set(sys.argv[3].split(','))
samples = []
for line in open(out):
    k, tid, ip = line.split()[:3]
    if tids is None or tid in tids:
        samples.append((k, int(ip, 16)))
def where(ip):
    for s, e, off, name in maps:
        if s <= ip < e:
            return name, ip - s + off
    return '?', ip
app = {}
for k, ip in samples:
    name, off = where(ip)
    if name.endswith('eden-headless'):
        app[ip] = off
# File offsets to ELF addresses through the LOAD segments (lld puts .text at vaddr != offset).
segs = []
for l in subprocess.run(['readelf', '-lW', binary], capture_output=True, text=True).stdout.split('\n'):
    f = l.split()
    if f and f[0] == 'LOAD':
        segs.append((int(f[1], 16), int(f[2], 16), int(f[4], 16)))
def vaddr(off):
    for o, v, size in segs:
        if o <= off < o + size:
            return off - o + v
    return off
app = {ip: vaddr(o) for ip, o in app.items()}
offs = sorted(set(app.values()))
names = {}
if offs:
    res = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', binary] + [hex(o) for o in offs], capture_output=True, text=True).stdout.split('\n')
    for i, o in enumerate(offs):
        names[o] = re.sub(r'\(.*', '', res[2 * i])[:90]
count = collections.defaultdict(collections.Counter)
for k, ip in samples:
    name, off = where(ip)
    fn = names.get(app.get(ip), '?') if name.endswith('eden-headless') else '[' + name.split('/')[-1] + ']'
    count[k][fn] += 1
tot = {k: sum(v.values()) or 1 for k, v in count.items()}
print(f"samples: cycles {tot.get('c',0)}, L3-miss loads {tot.get('m',0)}, L2-miss loads {tot.get('l',0)}")
print('  cyc%   L3m%   L2m%  function (by L3-miss loads)')
for fn, n in count['m'].most_common(top):
    print(f"{100*count['c'][fn]/tot['c']:6.1f} {100*n/tot['m']:6.1f} {100*count['l'][fn]/tot.get('l',1):6.1f}  {fn}")
