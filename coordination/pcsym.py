#!/usr/bin/env python3
# pcsym.py <heap.txt> <llvm-pie.elf> [top]: the console PC sampler's EDEN_PERF_CORE_PC samples
# (dev settings pc_sample=on pc_core=<n> pc_fast=on) by function, most samples first. The build's
# code starts at 0x400000 (crash reports: eboot+offset is an address in llvm-pie.elf).
import collections, re, subprocess, sys

BASE = 0x400000
counts = collections.Counter()
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('EDEN_PERF_CORE_PC '):
        m = re.search(r'pc=([0-9a-f]+) count=(\d+)', line)
        counts[int(m.group(1), 16)] += int(m.group(2))
total = sum(counts.values())
if not total:
    sys.exit('no EDEN_PERF_CORE_PC samples')
inside = [pc for pc in counts if pc >= BASE]
out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', sys.argv[2]] + [hex(pc - BASE) for pc in inside],
                     capture_output=True, text=True).stdout.split('\n')
names = {pc: re.sub(r'\(.*', '', out[2 * i])[:110] for i, pc in enumerate(inside)}
functions = collections.Counter()
for pc, count in counts.items():
    functions[names.get(pc, 'outside the build (system library)')] += count
print(f'{total} samples')
for name, count in functions.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 40):
    print(f'{count * 100 / total:5.1f}%  {name}')
