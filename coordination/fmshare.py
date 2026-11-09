#!/usr/bin/env python3
# fmshare.py <heap.txt>: how guest memory accesses go in the hot JIT blocks of a pc_sample run
# (dev settings pc_sample=on pc_core=0 pc_fast=on): the window ([r13 + ...]) or the page table
# (a load from [r14 + 8*reg], then the access through the entry). Static: per hot block the count
# of each kind of access site, weighted by the block's samples. Dynamic: which kind of instruction
# the samples landed on (walk = the table load with its checks around it).
import collections, re, subprocess, sys, tempfile

host, samples = {}, collections.defaultdict(collections.Counter)
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('EDEN_PERF_BLOCK_HOST '):
        m = re.search(r'location=([0-9a-f]+) entry=[0-9a-f]+ size=\d+ bytes=([0-9a-f]*)', line)
        host.setdefault(int(m.group(1), 16), bytes.fromhex(m.group(2)))
    elif line.startswith('EDEN_PERF_GUEST_PC '):
        m = re.search(r'location=([0-9a-f]+) host_offset=([0-9a-f]+) count=(\d+)', line)
        samples[int(m.group(1), 16)][int(m.group(2), 16)] += int(m.group(3))

def disassemble(code):
    with tempfile.NamedTemporaryFile(suffix='.bin') as f:
        f.write(code); f.flush()
        out = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', f.name],
                             capture_output=True, text=True).stdout
    insns = []
    for l in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\t(?:[0-9a-f]{2} )+\s*\t(.*)', l)
        if m: insns.append((int(m.group(1), 16), m.group(2)))
    return insns

def classify(insns):
    kinds = ['other'] * len(insns)
    for i, (_, text) in enumerate(insns):
        if '[r14+' in text.replace(' ', '') and '*8]' in text:
            kinds[i] = 'walk'
            reg = text.split(',')[0].split()[-1]
            for j in range(max(0, i - 5), i):  # the bounds and alignment checks before it
                if re.search(r'\b(shr|test|jne|and|cmp|ja)\b', insns[j][1]) and kinds[j] == 'other': kinds[j] = 'walk'
            for j in range(i + 1, min(len(insns), i + 6)):
                t = insns[j][1].replace(' ', '')
                if kinds[j] == 'other' and re.search(r'\b(test|je)\b', insns[j][1]) and '[' not in t:
                    kinds[j] = 'walk'
                elif f'[{reg}+' in t:
                    kinds[j] = 'walk_access'
                    break
        elif re.search(r'\[r13\+r\w+\*1', text.replace(' ', '')):
            kinds[i] = 'window'
    return kinds

static = collections.Counter()
dynamic = collections.Counter()
covered = 0
total = sum(sum(c.values()) for c in samples.values())
for location, code in host.items():
    counts = samples.get(location)
    if not counts: continue
    insns = disassemble(code)
    kinds = classify(insns)
    weight = sum(counts.values())
    covered += weight
    n = collections.Counter(kinds)
    static['window'] += weight * n['window']
    static['walk'] += weight * n['walk_access']
    starts = [o for o, _ in insns]
    for offset, count in counts.items():
        k = 0
        while k + 1 < len(starts) and starts[k + 1] <= offset: k += 1
        dynamic[kinds[k] if starts else 'other'] += count
print(f'samples in game blocks {total}, in the {len(host)} dumped blocks {covered} ({covered * 100 / max(total, 1):.0f}%)')
sites = static['window'] + static['walk']
print(f'access sites, sample-weighted: window {static["window"] * 100 / max(sites, 1):.1f}%  page table {static["walk"] * 100 / max(sites, 1):.1f}%')
print('samples by instruction kind: ' + '  '.join(f'{k} {v * 100 / max(covered, 1):.1f}%' for k, v in dynamic.most_common()))
