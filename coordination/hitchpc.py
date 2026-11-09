#!/usr/bin/env python3
# hitchpc.py runs/<tag>/heap.txt <checkout>/build/headless-native/llvm-pie.elf [present...]: the
# EDEN_HITCH_PC samples (hitch_pc=on) of each long frame, symbolized (code at 0x400000).
import collections, re, subprocess, sys
BASE = 0x400000
want = set(sys.argv[3:])
frames = collections.defaultdict(list)
heads = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'EDEN_HITCH present=(\d+) frame_ms=([\d.]+)', line)
    if m: heads[m.group(1)] = float(m.group(2))
    m = re.match(r'EDEN_HITCH_PC present=(\d+) worker=(\S+) count=(\d+) pc=([0-9a-f]+) callers=(\S*)', line)
    if m and (not want or m.group(1) in want):
        frames[m.group(1)].append((m.group(2), int(m.group(3)), [int(m.group(4), 16)] + [int(x, 16) for x in m.group(5).split(',') if x]))
addrs = sorted({a for v in frames.values() for _, _, chain in v for a in chain if a >= BASE})
out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', sys.argv[2]] + [hex(a - BASE) for a in addrs],
                     capture_output=True, text=True).stdout.split('\n')
name = {a: re.sub(r'\(.*', '', out[2 * i])[:90] for i, a in enumerate(addrs)}
for present, samples in frames.items():
    print(f'== present {present} ({heads.get(present, 0):.0f} ms), {sum(c for _, c, _ in samples)} samples')
    for worker, count, chain in sorted(samples, key=lambda s: -s[1])[:int(__import__("os").environ.get("TOP", 6))]:
        print(f'  {worker} x{count}: ' + ' <- '.join(name.get(a, hex(a)) for a in chain if a)[:900])
