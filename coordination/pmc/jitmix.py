#!/usr/bin/env python3
# jitmix.py <pmcjit samples> [tid]: the retired-instruction mix (precise instruction samples) and the
# cycle mix (precise cycle samples, charged at retirement) of the JIT's code, by instruction kind, for
# one thread (the one with the most JIT samples unless given) and all threads together.
import collections, re, subprocess, sys, tempfile

rows = [l.split() for l in open(sys.argv[1])]
jit = [(k, int(t), b) for k, t, ip, b in rows if b != '-']
per_tid = collections.Counter(t for k, t, b in jit if k == 'c')
tid = int(sys.argv[2]) if len(sys.argv) > 2 else per_tid.most_common(1)[0][0]

keys = sorted({b for k, t, b in jit})
with tempfile.NamedTemporaryFile(suffix='.bin') as blob:
    for b in keys: blob.write(bytes.fromhex(b) + b'\x90' * 16)  # 32-byte slots: resync on the nops
    blob.flush()
    dis = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', '--no-show-raw-insn',
                          blob.name], capture_output=True, text=True).stdout
at = {}
for line in dis.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
    if m and int(m.group(1), 16) % 32 == 0: at[int(m.group(1), 16) // 32] = re.sub(r'\s+', ' ', m.group(2)).strip()
text = {b: at.get(i, '?') for i, b in enumerate(keys)}

def kind(ins):
    m = re.search(r'\[r15\+0x([0-9a-f]+)\]|\[r15\]', ins)
    if m:
        off = int(m.group(1), 16) if m.group(1) else 0
        store = re.match(r'\w+ \w+ PTR \[r15', ins) is not None
        if off < 256: return 'guest GPR/SP ' + ('store' if store else 'load')
        if off == 256: return 'PC store'
        if off == 264: return 'NZCV ' + ('store' if store else 'load')
        if off == 808: return 'halt check'
        if 288 <= off < 800: return 'guest vector ' + ('store' if store else 'load')
        return f'other JIT state'
    if re.search(r'rbp', ins) and re.search(r'rsp\+0x41[08]', ins): return 'frame pointer (rbp)'
    if 'rsp+0x414' in ins: return 'check bit'
    if '[r14+r' in ins and '*8]' in ins: return 'page table load'
    if re.match(r'shr r\w+,0xc$', ins) or re.match(r'test r\w+,0xfffffffff8000000$', ins): return 'page table check'
    if re.match(r'test [re]\w+,0x7$|test [re]\w+,0x[13f]$', ins): return 'alignment check'
    if re.search(r'\[rsp', ins): return 'spill/stack'
    m = re.match(r'mov (e\w+),(e\w+)$', ins)
    if m and m.group(1) == m.group(2): return 'mov e?x,e?x (zero-extend)'
    if re.match(r'movzx (e\w+),(\w)l$', ins): return 'movzx'
    if re.match(r'mov e\w+,0x[0-9a-f]+$', ins): return 'mov imm32 (PC value?)'
    if re.match(r'(cmp|test|bt) ', ins): return 'cmp/test/bt'
    if re.match(r'set\w+ ', ins): return 'setcc'
    if re.match(r'j[a-z]+ ', ins) and not ins.startswith('jmp'): return 'jcc'
    if ins.startswith('jmp'): return 'jmp'
    if ins.startswith('call'): return 'call'
    if ins.startswith('ret'): return 'ret'
    if re.search(r'PTR \[r\w+\+r\w+(\*1)?(\+0x[0-9a-f]+)?\]', ins): return 'guest memory access'
    if re.match(r'(v?mov\w*|v?p\w+|v?add[sp][sd]|v?mul|v?cvt|v?sub[sp]|v?xorp|v?and[np]|v?shufp|v?unpck|v?div[sp])\b', ins) and 'xmm' in ins: return 'SSE/AVX'
    if re.match(r'mov ', ins): return 'mov (other)'
    if re.match(r'lea ', ins): return 'lea'
    return 'ALU/other'

def report(title, which):
    for k, label in (('i', 'retired instructions'), ('c', 'cycles')):
        c = collections.Counter(kind(text[b]) for kk, t, b in which if kk == k)
        total = sum(c.values())
        print(f'{title}: {label} in JIT code, {total} samples')
        for name, v in c.most_common(): print(f'  {name:28s} {v * 100 / max(total, 1):5.1f}%')
    ci = collections.Counter(kk for kk, t, b in which)
    print(f'  IPC in JIT code {ci["i"] / max(ci["c"], 1):.2f}')

all_c = collections.Counter(k for k, t, ip, b in rows if int(t) == tid)
print(f'thread {tid}: {all_c["c"]} cycle samples, {sum(1 for k, t, b in jit if t == tid and k == "c")} in JIT code')
report(f'thread {tid}', [x for x in jit if x[1] == tid])
report('all threads', jit)
top = collections.Counter(text[b] for k, t, b in jit if t == tid and k == 'c')
print('hottest instructions (cycles, that thread):')
for ins, v in top.most_common(30): print(f'  {v:6d} {kind(ins):26s} {ins}')
