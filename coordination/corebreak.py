#!/usr/bin/env python3
# corebreak.py <heap.txt> <llvm-pie.elf> [frame_ms]: one guest core's PC samples (dev settings pc_sample=on
# pc_core=<n> pc_fast=on) by what the core was doing: idle, game code (JIT), the emulator's own code by area
# (scheduler/locks, SVC and services, memory callbacks and GPU write tracking, GPU submits, dynarmic runtime,
# allocation, the profiler itself) and system-library time attributed to its first own caller (the sampled caller
# chains, EDEN_PERF_CORE_CALLERS). With frame_ms (the run's heavy-frame time), also ms of this core per frame.
import collections, re, subprocess, sys

BASE = 0x400000
JIT_LO, JIT_HI = 0x1100000000, 0x1300000000  # the code caches (0x11d...-0x125... in the OOT runs)
pcs, chains = collections.Counter(), collections.Counter()
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('EDEN_PERF_CORE_PC '):
        m = re.search(r'pc=([0-9a-f]+) count=(\d+)', line)
        pcs[int(m.group(1), 16)] += int(m.group(2))
    elif line.startswith('EDEN_PERF_CORE_CALLERS '):
        m = re.search(r'pc=([0-9a-f]+) count=(\d+) callers=(\S*)', line)
        if m:
            chains[(int(m.group(1), 16), tuple(int(x, 16) for x in m.group(3).split(',') if x))] += int(m.group(2))
total = sum(pcs.values())
if not total:
    sys.exit('no core samples')

own = sorted({pc for pc in pcs if BASE <= pc < 0x8000000} | {a for _, cs in chains for a in cs if BASE <= a < 0x8000000})
out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', sys.argv[2]] + [hex(a - BASE) for a in own],
                     capture_output=True, text=True).stdout.split('\n')
name = {a: re.sub(r'\(.*', '', out[2 * i]) for i, a in enumerate(own)}

RULES = [  # first match wins; tested against the function names of a chain, innermost first
    ('idle', r'PhysicalCore::Idle|IdleThreadFunction'),
    ('present wait (vi)', r'WaitForFrame|PresentClientBuffer|Vi2Session'),
    ('profiler', r'Eden::Performance'),
    ('memory callbacks / GPU write tracking', r'DynarmicCallbacks64::(Memory|Check)|Memory::Impl|HandleRasterizerWrite|OnCPUWrite|'
                                              r'DeviceMemoryManager|GPUDirtyMemoryManager|RasterizerMarkRegion'),
    ('GPU submit (nvdrv)', r'nvhost|Nvidia|Tegra::'),
    ('scheduler / sync / locks', r'KScheduler|KSpinLock|SchedulerLock|KAddressArbiter|KConditionVariable|KSynchronization|'
                                 r'KThread::|KHardwareTimer|CoreTiming|Fiber|GetCurrentEmuThread|tls_data|GetCurrentHostThread'),
    ('SVC / services', r'Svc::|Service::|KServerSession|KClientSession|ServerManager|HLERequest'),
    ('dynarmic runtime', r'Dynarmic::|ArmDynarmic64'),
    ('allocation', r'eden_heap|malloc|operator new'),
]
def category(names):
    for label, pattern in RULES:
        if any(re.search(pattern, n) for n in names):
            return label
    return 'other emulator code'

where = collections.Counter()
for pc, n in pcs.items():
    if JIT_LO <= pc < JIT_HI:
        where['game code (JIT)'] += n
    elif BASE <= pc < 0x8000000:
        where[category([name.get(pc, '')])] += n
    else:
        where['library'] += n
library = where.pop('library', 0)
sampled = collections.Counter()
for (pc, cs), n in chains.items():
    if BASE <= pc < 0x8000000 or JIT_LO <= pc < JIT_HI:
        continue
    names = [name[a] for a in cs if a in name]
    sampled['library: ' + (category(names) if names else 'no own caller (system)')] += n
ssum = sum(sampled.values())
for label, n in sampled.items():
    where[label] += library * n / ssum if ssum else 0
if not ssum and library:
    where['library: unattributed'] += library
frame_ms = float(sys.argv[3]) if len(sys.argv) > 3 else None
print(f'{total} core samples; library samples with callers {ssum}')
for label, n in sorted(where.items(), key=lambda x: -x[1]):
    share = n / total
    print(f'{share * 100:5.1f}%' + (f'  {share * frame_ms:5.2f} ms/frame' if frame_ms else '') + f'  {label}')
