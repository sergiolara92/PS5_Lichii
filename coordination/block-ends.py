#!/usr/bin/env python3
# block-ends.py <heap.txt>...: the dev sampler's hot blocks (EDEN_PERF_BLOCK_GUEST words) by how they
# end (the first branch from the block's start: the translator stops there) and their length in
# guest instructions, weighted by samples.
import collections, re, subprocess, sys
words, weight = {}, collections.Counter()
for path in sys.argv[1:]:
    for line in open(path, errors='replace'):
        if line.startswith('EDEN_PERF_BLOCK_GUEST'):
            m = re.search(r'location=([0-9a-f]+) count=(\d+) words=([0-9a-f]*)', line)
            words.setdefault(m.group(1), m.group(3)); weight[m.group(1)] += int(m.group(2))
kinds, lengths = collections.Counter(), collections.Counter()
total = 0
for loc, text in words.items():
    data = b''.join(int(text[i:i + 8], 16).to_bytes(4, 'little') for i in range(0, len(text) - 7, 8))
    asm = subprocess.run(['llvm-mc', '--disassemble', '-triple=aarch64'], input=' '.join(f'0x{b:02x}' for b in data),
                         capture_output=True, text=True).stdout
    ins = [a.strip().split()[0] for a in asm.splitlines() if a.strip() and not a.strip().startswith('.')]
    w = weight[loc]; total += w
    for n, op in enumerate(ins, 1):
        if op in ('b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz', 'svc', 'braa', 'blraa', 'retaa') or op.startswith('b.'):
            kinds['b.cond' if op.startswith('b.') else op] += w
            lengths[min(n, 20)] += w
            break
    else:
        kinds['(none in 64 words)'] += w
print('block ends, weighted by samples:')
for k, v in kinds.most_common(): print(f'  {k:10s} {v * 100 / total:5.1f}%')
mean = sum(n * v for n, v in lengths.items()) / max(sum(lengths.values()), 1)
print(f'mean guest instructions per block (weighted): {mean:.1f}')
print('  ' + ', '.join(f'{n}:{v * 100 / total:.0f}%' for n, v in sorted(lengths.items())))
