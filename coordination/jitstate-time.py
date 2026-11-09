#!/usr/bin/env python3
# jitstate-time.py <heap.txt>...: samples that land inside the dumped hot blocks (EDEN_PERF_GUEST_PC
# host_offset), by the kind of host instruction at that offset: JitState access (by guest register),
# page-table memory check, guest memory access, other. Sampled PCs skid one instruction or so, so
# this is the time near each kind of instruction, not an exact cost.
import collections, re, subprocess, sys

host, samples = {}, collections.Counter()
for path in sys.argv[1:]:
    for line in open(path, errors='replace'):
        if line.startswith('EDEN_PERF_BLOCK_HOST'):
            m = re.search(r'location=([0-9a-f]+) entry=[0-9a-f]+ size=\d+ bytes=([0-9a-f]+)', line)
            host[m.group(1)] = m.group(2)
        elif line.startswith('EDEN_PERF_GUEST_PC '):
            m = re.search(r'location=([0-9a-f]+) host_offset=([0-9a-f]+) count=(\d+)', line)
            samples[(m.group(1), int(m.group(2), 16))] += int(m.group(3))
kinds, regs = collections.Counter(), collections.Counter()
for loc, code in host.items():
    out = subprocess.run(['llvm-mc', '--disassemble', '-triple=x86_64', '--output-asm-variant=1', '--show-encoding'],
                         input=' '.join('0x' + code[i:i + 2] for i in range(0, len(code), 2)),
                         capture_output=True, text=True).stdout
    offset = 0
    for line in out.splitlines():
        line = line.strip()
        if not line or line.startswith('.'): continue
        enc = re.search(r'encoding: \[([^\]]*)\]', line)
        size = len(enc.group(1).split(',')) if enc else 0
        ins = line.split('#')[0].strip()
        n = samples.get((loc, offset), 0)
        offset += size
        if not n: continue
        m = re.search(r'\[r15 \+ (\d+)\]', ins)
        if m:
            off = int(m.group(1))
            reg = f'x{off // 8}' if off < 248 else {248: 'sp', 256: 'pc', 264: 'nzcv', 808: 'halt'}.get(off, 'vec' if 288 <= off < 800 else f'+{off}')
            kinds['jitstate'] += n; regs[reg] += n
        elif 'rbp' in ins and ('rsp + 10' in ins): kinds['frame pointer'] += n
        elif '[r14 + 8*' in ins: kinds['page table load'] += n
        elif re.match(r'(test|cmp|and|shr|ja|jne|je|jb|jae)\b', ins): kinds['check/branch'] += n
        elif re.search(r'ptr \[r\w+ \+ r\w+', ins): kinds['guest memory access'] += n
        elif re.search(r'xmm|ymm', ins): kinds['vector/fp'] += n
        else: kinds['other'] += n
t = sum(kinds.values())
print(f'samples inside dumped blocks: {t}')
for k, v in kinds.most_common(): print(f'  {k:20s} {v * 100 / t:5.1f}%')
print('JitState samples by register:', ', '.join(f'{r} {v * 100 / t:.1f}%' for r, v in regs.most_common(12)))
