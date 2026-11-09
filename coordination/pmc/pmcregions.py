#!/usr/bin/env python3
# pmcregions.py <samples> [binary] [top]: pmcsample's cycle and instruction samples by code region
# (JIT code = anonymous executable memory; the emulator binary by function when [binary] is given;
# other files by name): share of cycles and IPC per region (both events use the same period).
import collections, re, subprocess, sys

maps = []
for line in open(sys.argv[1] + '.maps'):
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+\s*(.*)', line)
    if m and 'x' in m.group(3):
        maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5).strip()))
binary = sys.argv[2] if len(sys.argv) > 2 else None
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25

def region(ip):
    for start, end, offset, name in maps:
        if start <= ip < end:
            if not name or name.startswith('['): return ('JIT code', None)
            if binary and name.endswith('eden-headless'): return ('emulator', ip - start + offset)
            return (name.rsplit('/', 1)[-1], None)
    return ('unmapped', None)

counts = collections.defaultdict(lambda: [0, 0])  # region -> [cycles, instructions]
emu = collections.defaultdict(lambda: [0, 0])
for line in open(sys.argv[1]):
    kind, ip = line.split()
    name, offset = region(int(ip, 16))
    idx = 0 if kind == 'c' else 1
    counts[name][idx] += 1
    if offset is not None:
        emu[offset][idx] += 1
total_c = sum(c for c, _ in counts.values())
print(f'cycle samples {total_c}')
for name, (c, i) in sorted(counts.items(), key=lambda kv: -kv[1][0])[:12]:
    print(f'  {c * 100 / total_c:5.1f}% of cycles  IPC {i / max(c, 1):.2f}  {name}')
if binary and emu:
    # Symbolize the emulator's samples by function (llvm-addr2line on the binary's file offsets).
    offsets = sorted(emu)
    # File offsets to link-time addresses through the executable LOAD segment.
    seg = re.search(r'LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+\S+\s+\S+\s+\S+\s+R E',
                    subprocess.run(['llvm-readelf', '-lW', binary], capture_output=True, text=True).stdout)
    delta = int(seg.group(2), 16) - int(seg.group(1), 16) if seg else 0
    emu = {o + delta: v for o, v in emu.items()}
    offsets = sorted(emu)
    out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', binary] + [hex(o) for o in offsets],
                         capture_output=True, text=True).stdout.splitlines()
    funcs = collections.defaultdict(lambda: [0, 0])
    for k, o in enumerate(offsets):
        name = out[2 * k] if 2 * k < len(out) else '??'
        funcs[name][0] += emu[o][0]; funcs[name][1] += emu[o][1]
    print('emulator functions:')
    for name, (c, i) in sorted(funcs.items(), key=lambda kv: -kv[1][0])[:top]:
        print(f'  {c * 100 / total_c:5.1f}%  IPC {i / max(c, 1):.2f}  {name[:110]}')
