#!/usr/bin/env python3
# blockmix.py <heap.txt> [cycles_per_guest_insn]: code expansion of the dev sampler's dumped hot blocks
# (pc_sample=on pc_fast=on: EDEN_PERF_BLOCK_GUEST/HOST, EDEN_PERF_GUEST_PC). Per block: guest
# instructions by class, and the host main path (block entry to the first int3; the slow paths follow it)
# by role. Prints, weighted by samples: host instructions per guest instruction, host instructions by role
# per guest instruction, per-guest-class host cost (least squares over blocks), and with the measured
# cycles per guest instruction the cycles per host instruction (~1 = expansion-bound, well above = stalls).
import collections, re, subprocess, sys, tempfile
import numpy as np

path = sys.argv[1]
cpg = float(sys.argv[2]) if len(sys.argv) > 2 else None
guest_words, host_code, weight = {}, {}, collections.Counter()
for line in open(path, errors='replace'):
    if line.startswith('EDEN_PERF_BLOCK_GUEST'):
        m = re.search(r'location=([0-9a-f]+) count=\d+ words=([0-9a-f]*)', line)
        guest_words.setdefault(int(m.group(1), 16), m.group(2))
    elif line.startswith('EDEN_PERF_BLOCK_HOST '):
        m = re.search(r'location=([0-9a-f]+) entry=[0-9a-f]+ size=\d+ bytes=([0-9a-f]*)', line)
        host_code.setdefault(int(m.group(1), 16), bytes.fromhex(m.group(2)))
    elif line.startswith('EDEN_PERF_GUEST_PC '):
        m = re.search(r'location=([0-9a-f]+) host_offset=[0-9a-f]+ count=(\d+)', line)
        weight[int(m.group(1), 16)] += int(m.group(2))

GCLASSES = ['load', 'store', 'flags-set', 'cond-select', 'branch', 'fp/simd', 'alu']
def guest_classes(text):
    data = b''.join(int(text[i:i + 8], 16).to_bytes(4, 'little') for i in range(0, len(text) - 7, 8))
    asm = subprocess.run(['llvm-mc', '--disassemble', '-triple=aarch64'], input=' '.join(f'0x{b:02x}' for b in data),
                         capture_output=True, text=True).stdout
    c = collections.Counter()
    for l in asm.splitlines():
        l = l.strip()
        if not l or l.startswith('.'): continue
        op = l.split()[0]
        if re.match(r'(b\.|b$|bl$|br$|blr$|ret|cbz|cbnz|tbz|tbnz)', op): c['branch'] += 1; break
        if re.match(r'(ld|ldr|ldur|ldp|ldx|ldax|ldar)', op): c['load'] += 1
        elif re.match(r'(st|stp|stx|stlx|stlr)', op): c['store'] += 1
        elif re.match(r'(cmp|cmn|tst|adds|subs|ands|bics|ccmp|ccmn|fcmp|fccmp|negs)', op): c['flags-set'] += 1
        elif re.match(r'(csel|csinc|csinv|csneg|cset|csetm|cinc|cinv|cneg|fcsel)', op): c['cond-select'] += 1
        elif re.search(r'\b[vqds]\d+', l) or op.startswith(('f', 'scvtf', 'ucvtf')): c['fp/simd'] += 1
        else: c['alu'] += 1
    return c

HROLES = ['guest state', 'flags', 'memory path', 'block end', 'compute']
MEMPAT = re.compile(r'\[r14\+|\[r13\+|0xfffffffff8000000|^shr\s+\w+,0xc$|^and\s+\w+,0xff[f8]$|^cmp\s+\w+,0xf[ef][08]$')
def host_roles(code):
    with tempfile.NamedTemporaryFile(suffix='.bin') as f:
        f.write(code); f.flush()
        out = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', f.name],
                             capture_output=True, text=True).stdout
    insns = []
    for l in out.splitlines()[7:]:
        m = re.match(r'\s*[0-9a-f]+:\t(?:[0-9a-f]{2} )+\s*\t(.*)', l)
        if not m: continue
        ins = re.sub(r'\s+', ' ', m.group(1)).strip().replace(' ', '\t', 1).replace('\t', ' ')
        if ins.startswith('int3'): break
        insns.append(ins)
    # The terminal: from the first halt check (cmp [r15+0x328]) on, only one exit path runs: count 3.
    term = next((i for i, x in enumerate(insns) if '[r15+0x328]' in x), len(insns))
    c = collections.Counter()
    prev = None
    for i, ins in enumerate(insns[:term]):
        mm = re.search(r'\[r15\+0x([0-9a-f]+)\]', ins)
        off = int(mm.group(1), 16) if mm else None
        op = ins.split(' ')[0]
        if off == 0x108 or op in ('lahf', 'sahf', 'cmc', 'seto'): role = 'flags'
        elif off is not None and off < 0x330: role = 'guest state'
        elif MEMPAT.search(ins) or (prev == 'memory path' and (op.startswith('j') or op == 'test' or
              (op == 'mov' and re.match(r'mov [re][a-z0-9]+,[re][a-z0-9]+$', ins)))): role = 'memory path'
        else: role = 'compute'
        c[role] += 1; prev = role
    c['block end'] += 3 if term < len(insns) else 0
    return c

rows = []
for loc, w in weight.items():
    if loc in guest_words and loc in host_code and w > 0:
        g = guest_classes(guest_words[loc]); h = host_roles(host_code[loc])
        if sum(g.values()) and sum(h.values()): rows.append((w, g, h))
W = sum(r[0] for r in rows)
G = sum(r[0] * sum(r[1].values()) for r in rows) / W
H = sum(r[0] * sum(r[2].values()) for r in rows) / W
print(f'{len(rows)} hot blocks, {W} samples; sample-weighted: {G:.1f} guest insns and {H:.1f} host insns '
      f'(main path) a block: {H / G:.2f} host insns per guest insn')
for role in HROLES:
    print(f'  {role:12} {sum(r[0] * r[2][role] for r in rows) / W / G:5.2f} host insns per guest insn')
gc = collections.Counter()
for w, g, _ in rows:
    for k, v in g.items(): gc[k] += w * v
print('guest mix: ' + ', '.join(f'{k} {100 * gc[k] / sum(gc.values()):.0f}%' for k in GCLASSES))
X = np.array([[r[1][k] for k in GCLASSES] for r in rows], float)
y = np.array([sum(r[2].values()) for r in rows], float)
sw = np.sqrt(np.array([r[0] for r in rows], float))
coef, *_ = np.linalg.lstsq(X * sw[:, None], y * sw, rcond=None)
print('host insns per guest insn of each class (least squares, sample-weighted): ' +
      ', '.join(f'{k} {c:.1f}' for k, c in zip(GCLASSES, coef)))
if cpg:
    print(f'measured {cpg:.2f} cycles per guest insn -> {cpg / (H / G):.2f} cycles per host insn '
          f'(the hot blocks\' mix; ~1 expansion-bound, well above stall-bound)')
