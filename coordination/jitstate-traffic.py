#!/usr/bin/env python3
# jitstate-traffic.py <heap.txt>...: in the dev sampler's dumped hot blocks (EDEN_PERF_BLOCK_HOST/GUEST),
# the accesses to the A64 JitState ([r15 + offset]) by guest register, loads and stores apart, each
# block weighted by its samples; and the share of each block's instructions they are.
import collections, re, subprocess, sys

def name(off):
    if off < 248: return f'x{off // 8}'
    return {248: 'sp', 256: 'pc', 264: 'nzcv', 276: 'fpcr', 808: 'halt'}.get(off, f'vec' if 288 <= off < 800 else f'+{off}')

weight, host = {}, {}
for path in sys.argv[1:]:
    for line in open(path, errors='replace'):
        if line.startswith('EDEN_PERF_BLOCK_GUEST'):
            m = re.search(r'location=([0-9a-f]+) count=(\d+)', line)
            weight[m.group(1)] = weight.get(m.group(1), 0) + int(m.group(2))
        elif line.startswith('EDEN_PERF_BLOCK_HOST'):
            m = re.search(r'location=([0-9a-f]+) entry=[0-9a-f]+ size=\d+ bytes=([0-9a-f]+)', line)
            host[m.group(1)] = m.group(2)
loads, stores = collections.Counter(), collections.Counter()
total_ins = state_ins = 0
for loc, code in host.items():
    w = weight.get(loc, 0)
    asm = subprocess.run(['llvm-mc', '--disassemble', '-triple=x86_64', '--output-asm-variant=1'],
                         input=' '.join('0x' + code[i:i + 2] for i in range(0, len(code), 2)),
                         capture_output=True, text=True).stdout
    for ins in (a.strip() for a in asm.splitlines()):
        if not ins or ins.startswith('.'): continue
        total_ins += w
        m = re.search(r'\[r15 \+ (\d+)\]', ins) or (re.search(r'\[r15\]', ins) and re.match(r'(.*)', '0'))
        if not m: continue
        off = int(m.group(1)) if m.group(1).isdigit() else 0
        state_ins += w
        dst_mem = re.match(r'\w+\s+(qword|dword|byte|word|xmmword) ptr \[r15', ins)
        (stores if dst_mem else loads)[name(off)] += w
print(f'blocks {len(host)}, weighted JitState share of instructions {state_ins * 100 / max(total_ins, 1):.1f}%')
t = sum(loads.values()) + sum(stores.values())
print('register   loads  stores  (share of all JitState accesses)')
for reg in sorted(set(loads) | set(stores), key=lambda r: -(loads[r] + stores[r]))[:24]:
    print(f'{reg:8s} {loads[reg] * 100 / t:6.1f}% {stores[reg] * 100 / t:6.1f}%')
