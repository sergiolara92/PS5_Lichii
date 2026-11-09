#!/usr/bin/env python3
# parse-probe.py <klog>: the budget/mapper probe lines (EDEN_PROBE_BUDGET), each answering call's
# first-word argument and what it wrote, as little-endian 64-bit words.
import re, sys

rows = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'EDEN_PROBE_BUDGET   (\w+) id=(\d+) width=(\d+) -> (\S+) written=(\d+) last=(\d+) data=(\S+)', line)
    if m:
        fn, idv, w = m.group(1), int(m.group(2)), int(m.group(3))
        rows[(fn, w, idv)] = (m.group(4), int(m.group(5)), m.group(7))

def words(h):
    if h == '-':
        return []
    b = bytes.fromhex(h)
    return [int.from_bytes(b[i:i+8], 'little') for i in range(0, len(b) - 7, 8)]

for fn in ('MapperGetParam', 'MapperGetUsageStatsData'):
    for w in (4, 8):
        ids = sorted(i for (f, ww, i) in rows if f == fn and ww == w)
        if not ids:
            print(f'{fn} width={w}: all EINVAL')
            continue
        print(f'{fn} width={w}: {len(ids)} ids answered, {ids[0]}..{ids[-1]}')
        for i in (ids[0], ids[len(ids) // 2], ids[-1]):
            rc, written, data = rows[(fn, w, i)]
            ws = words(data)
            print(f'  arg0={i} rc={rc} bytes_written={written} words=' + ' '.join(hex(x) for x in ws[:12]))
