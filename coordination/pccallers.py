#!/usr/bin/env python3
# pccallers.py <heap.txt> <llvm-pie.elf> [top]: the PC sampler's library samples (EDEN_PERF_CORE_CALLERS,
# dev builds) by sampled PC and the first own callers, most samples first; plus where all samples
# fell (the build, system libraries, the JIT caches).
import collections, re, subprocess, sys
BASE = 0x400000
pcs = collections.Counter()
chains = collections.Counter()
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('EDEN_PERF_CORE_PC '):
        m = re.search(r'pc=([0-9a-f]+) count=(\d+)', line)
        pcs[int(m.group(1), 16)] += int(m.group(2))
    elif line.startswith('EDEN_PERF_CORE_CALLERS '):
        m = re.search(r'pc=([0-9a-f]+) count=(\d+) callers=(\S*)', line)
        if m:
            callers = tuple(int(x, 16) for x in m.group(3).split(',') if x)
            chains[(int(m.group(1), 16), callers)] += int(m.group(2))
where = collections.Counter()
for pc, n in pcs.items():
    where['build' if BASE <= pc < 0x8000000 else 'JIT' if 0x1200000000 <= pc < 0x1300000000 else 'system library'] += n
total = sum(pcs.values())
print(', '.join(f'{k} {v * 100 / total:.1f}%' for k, v in where.most_common()))
own = sorted({a for _, cs in chains for a in cs if BASE <= a < 0x8000000})
out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', sys.argv[2]] + [hex(a - BASE - 1) for a in own],
                     capture_output=True, text=True).stdout.split('\n')
name = {a: re.sub(r'\(.*', '', out[2 * i])[:70] for i, a in enumerate(own)}
grouped = collections.Counter()
for (pc, cs), n in chains.items():
    grouped[(hex(pc), ' < '.join([name[a] for a in cs if a in name][:4]))] += n
sampled = sum(chains.values())
print(f'{sampled} library samples with callers')
for (pc, frames), n in grouped.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 15):
    print(f'{n * 100 / sampled:5.1f}% {pc} {frames}')
