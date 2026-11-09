#!/usr/bin/env python3
# jiteff.py <heap.txt> [house_lo house_hi]: JIT efficiency from EDEN_PERF_JITEFF (dev-settings
# jit_count_insns=on; Eden 0140 + headless block-entry counter). Per 5 s poll and guest core: guest
# instructions run, host core cycles in translated code (TSC ticks in JIT Run minus callbacks and block
# compilation, scaled by the measured core clock EDEN_PERF_CPU_MHZ over the TSC rate), cycles per guest
# instruction, and the callbacks' share of JIT time. Polls are split by the frame's draws (house view vs
# camera) when a range is given.
import collections, re, statistics, sys

path = sys.argv[1]
lo, hi = (float(sys.argv[2]), float(sys.argv[3])) if len(sys.argv) > 3 else (None, None)
tsc_hz, mhz = None, []
polls, cur, compile_ns = [], {}, {}
frames, prev_gpu = None, None
for line in open(path, errors='replace'):
    if line.startswith('EDEN_PERF_CLOCK '):
        tsc_hz = int(re.search(r'tsc_hz=(\d+)', line).group(1))
    elif line.startswith('EDEN_PERF_CPU_MHZ'):
        mhz.append(int(re.search(r'mhz=(\d+)', line).group(1)))
    elif line.startswith('EDEN_PERF_JITEFF '):
        d = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
        cur[d['core']] = d
    elif line.startswith('EDEN_PERF_PROGRESS '):
        m = re.search(r'core=(\d+) .*compile_ns=(\d+)', line)
        if m: compile_ns[int(m.group(1))] = int(m.group(2))
    elif line.startswith('EDEN_VULKAN_FRAME '):
        frames = int(re.search(r'frames=(\d+)', line).group(1))
    elif line.startswith('EDEN_DEV_GPU ') and frames is not None:
        g = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)\b', line)}
        dpf = (g['draws'] - prev_gpu['draws']) / max(1, g['frame'] - prev_gpu['frame']) if prev_gpu else None
        prev_gpu = g
        polls.append((frames, dpf, dict(cur), dict(compile_ns)))
        frames = None
if not polls or tsc_hz is None:
    sys.exit('no EDEN_PERF_JITEFF / EDEN_PERF_CLOCK lines')
core_mhz = statistics.median(mhz) if mhz else None
scale = core_mhz * 1e6 / tsc_hz if core_mhz else 1.0
print(f'tsc {tsc_hz / 1e6:.0f} MHz, measured core clock median {core_mhz} MHz (min {min(mhz) if mhz else "-"}, '
      f'max {max(mhz) if mhz else "-"}), {len(polls)} polls')

groups = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
for (f0, _, a, c0), (f1, dpf, b, c1) in zip(polls, polls[1:]):
    if not a or not b: continue
    if lo is not None and dpf is not None:
        group = 'house' if lo <= dpf <= hi else ('camera' if dpf > hi else 'other')
    else:
        group = 'all'
    for core in b:
        if core not in a: continue
        g = groups[group][core]
        for key in ('insns', 'run_tsc', 'mem_tsc', 'cntpct_tsc', 'other_tsc', 'mem_calls', 'cntpct_calls', 'other_calls'):
            g[key] += b[core][key] - a[core][key]
        g['compile_tsc'] += (c1.get(core, 0) - c0.get(core, 0)) * tsc_hz / 1e9
        g['frames'] += f1
for group, cores in groups.items():
    print(f'== {group}')
    total = collections.Counter()
    for core, g in sorted(cores.items()):
        total.update(g)
        cb = g['mem_tsc'] + g['cntpct_tsc'] + g['other_tsc']
        code = g['run_tsc'] - cb - g['compile_tsc']
        if g['insns'] == 0 or g['run_tsc'] == 0: continue
        print(f'core {core}: {g["insns"] / g["frames"] / 1e6:6.2f} M guest insns/frame, '
              f'{code * scale / g["insns"]:5.2f} cycles/insn in translated code '
              f'(incl. callbacks {g["run_tsc"] * scale / g["insns"]:5.2f}); callbacks {100 * cb / g["run_tsc"]:4.1f}% of JIT time '
              f'(memory {100 * g["mem_tsc"] / g["run_tsc"]:4.1f}%, cntpct {100 * g["cntpct_tsc"] / g["run_tsc"]:4.1f}%, '
              f'other {100 * g["other_tsc"] / g["run_tsc"]:4.1f}%), compile {100 * g["compile_tsc"] / g["run_tsc"]:4.1f}%; '
              f'mem calls {g["mem_calls"] / g["frames"]:.0f}/frame')
    cb = total['mem_tsc'] + total['cntpct_tsc'] + total['other_tsc']
    code = total['run_tsc'] - cb - total['compile_tsc']
    frames = max(g['frames'] for g in cores.values())
    if total['insns']:
        print(f'all cores: {total["insns"] / frames / 1e6:.2f} M guest insns/frame, {code * scale / total["insns"]:.2f} '
              f'cycles/insn in translated code, callbacks {100 * cb / total["run_tsc"]:.1f}% of JIT time, '
              f'JIT time {total["run_tsc"] / tsc_hz * 1e3 / frames:.1f} ms/frame summed over cores')
