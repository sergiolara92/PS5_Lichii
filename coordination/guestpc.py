#!/usr/bin/env python3
# guestpc.py <heap.txt> [top]: guest core 0's PC samples mapped to the game's own code (dev settings
# pc_sample=on pc_core=0 pc_fast=on, 64-bit block map in jit-compile-batch.inc): the hottest JIT
# blocks by guest PC, grouped by 4 KiB of guest code, and the hottest blocks' AArch64 instructions
# (EDEN_PERF_BLOCK_GUEST), to tell a busy-wait (a load/compare loop, YIELD/WFE) from real work.
import collections, re, subprocess, sys

blocks = collections.Counter()
words = {}
total_core = 0
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('EDEN_PERF_GUEST_PC '):
        m = re.search(r'location=([0-9a-f]+) host_offset=[0-9a-f]+ count=(\d+)', line)
        blocks[int(m.group(1), 16)] += int(m.group(2))
    elif line.startswith('EDEN_PERF_BLOCK_GUEST '):
        m = re.search(r'location=([0-9a-f]+) count=\d+ words=([0-9a-f]*)', line)
        words.setdefault(int(m.group(1), 16), m.group(2))
    elif line.startswith('EDEN_PERF_CORE_PC '):
        total_core += int(re.search(r'count=(\d+)', line).group(1))
mapped = sum(blocks.values())
print(f'core samples {total_core}, mapped to game code blocks {mapped} ({mapped * 100 / max(total_core, 1):.0f}%)')
pages = collections.Counter()
for location, count in blocks.items():
    pages[location >> 12] += count
print('-- by 4 KiB of game code')
for page, count in pages.most_common(15):
    print(f'{count * 100 / mapped:5.1f}%  {page << 12:#x}')
top = int(sys.argv[2]) if len(sys.argv) > 2 else 12
print('-- hottest blocks')
for location, count in blocks.most_common(top):
    print(f'{count * 100 / mapped:5.1f}%  {location:#x}')
for location, text in list(words.items())[:8]:
    data = b''.join(int(text[i:i + 8], 16).to_bytes(4, 'little') for i in range(0, len(text) - 7, 8))
    hexes = ' '.join(f'0x{b:02x}' for b in data[:64])
    asm = subprocess.run(['llvm-mc', '--disassemble', '-triple=aarch64'], input=hexes,
                         capture_output=True, text=True).stdout
    print(f'== block {location:#x} ({blocks.get(location, 0)} samples)')
    print('\n'.join('   ' + l.strip() for l in asm.splitlines() if l.strip())[:1400])
