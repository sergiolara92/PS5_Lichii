#!/usr/bin/env python3
# livestat.py <heap.txt or run stdout> <switch name> [skip windows after a flip, default 1]: the paired verdict of a
# live A/B run (dkb-console-run.sh LIVE_AB, pc-live-ab.sh). The emulator logs EDEN_LIVE_WINDOW every 5 s with the
# live values in force (headless/live_switches.inc). Windows in which the switch changed, and `skip` after each
# change (the JIT recompiling after its cache was cleared), are dropped; the rest form segments of one arm.
# Neighbouring segments of different arms are pairs (the runner flips ABBA, so drift along the route cancels):
# per pair, B minus A of the mean frame time and fps; then the mean difference with a 95% t interval. With
# EDEN_PERF_JITEFF polls (jit_count_insns=on), also TSC ticks per guest instruction on cores 0-2 per arm.
import collections, math, re, statistics, sys

path, name = sys.argv[1], sys.argv[2]
skip = int(sys.argv[3]) if len(sys.argv) > 3 else 1
T975 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23, 12: 2.18, 15: 2.13, 20: 2.09, 30: 2.04}

def t975(df):
    return next(v for k, v in sorted(T975.items()) if k >= df) if df <= 30 else 1.96

windows, polls, last_arm = [], [], None
prev, cur = {}, {}
for line in open(path, errors='replace'):
    if line.startswith('EDEN_LIVE_WINDOW '):
        f = dict(kv.split('=', 1) for kv in line.split()[1:] if '=' in kv and not kv.startswith('values='))
        vals = line.split('values=', 1)[1].strip()
        values = dict(kv.split('=', 1) for kv in vals.split(',') if '=' in kv) if vals != '-' else {}
        last_arm = values.get('arm', values.get(name))  # the runner's arm label, else the switch value
        windows.append({'presents': int(f['presents']), 'fps': float(f['fps']), 'changed': f['changed'] == '1', 'arm': last_arm,
                        'value': values.get(name, '-'), 'period': values.get('period')})
    elif line.startswith('EDEN_PERF_JITEFF '):
        d = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
        cur[d['core']] = d
        if d['core'] == 2 and 0 in cur and 1 in cur:  # one poll per 5 s: cores 0-2 seen
            if prev:
                polls.append((len(windows), {c: (cur[c]['insns'] - prev[c]['insns'], cur[c]['run_tsc'] - prev[c]['run_tsc']) for c in (0, 1, 2)}))
            prev = {c: dict(cur[c]) for c in (0, 1, 2)}

# Drop the window in which the arm flipped and `skip` after it (a period change within one arm drops nothing).
keep, cool, prev_arm = [], 0, None
for w in windows:
    if prev_arm is not None and w['arm'] != prev_arm:
        cool = skip + 1
    prev_arm = w['arm']
    w['kept'] = cool == 0 and w['arm'] is not None and w['presents'] > 0 and w['fps'] > 0  # no presents: paused or loading
    if cool > 0: cool -= 1
    keep.append(w)
# Segments: runs of kept windows of one arm and one period (the runner's ABBA periods).
segments = []
for w in keep:
    if not w['kept']:
        if segments: segments[-1]['open'] = False
        continue
    if not segments or not segments[-1]['open'] or (segments[-1]['arm'], segments[-1]['period']) != (w['arm'], w['period']):
        segments.append({'arm': w['arm'], 'period': w['period'], 'windows': [], 'open': True})
    segments[-1]['windows'].append(w)
arms = []
for s in segments:
    if s['arm'] not in arms: arms.append(s['arm'])
if len(arms) < 2:
    sys.exit(f'{name}: one arm only ({arms}), nothing to pair')

def seg_stats(s):
    presents = sum(w['presents'] for w in s['windows'])
    secs = sum(w['presents'] / w['fps'] for w in s['windows'] if w['fps'] > 0)
    return (1000 * secs / presents if presents else float('nan')), (presents / secs if secs else float('nan'))

# Disjoint neighbouring periods of opposite arms: with ABBA the order alternates, so a drift cancels.
a, b = arms[0], arms[1]
pairs, i = [], 0
while i + 1 < len(segments):
    s1, s2 = segments[i], segments[i + 1]
    if s1['arm'] == s2['arm']:
        i += 1
        continue
    first, second = (s1, s2) if s1['arm'] == a else (s2, s1)
    (ms1, fps1), (ms2, fps2) = seg_stats(first), seg_stats(second)
    pairs.append((ms2 - ms1, fps2 - fps1))
    i += 2
per_arm = collections.defaultdict(list)
for s in segments: per_arm[s['arm']].extend(s['windows'])
print(f'== {name}: {len(windows)} windows, {sum(w["kept"] for w in keep)} kept, {len(segments)} segments, {len(pairs)} pairs')
for arm in arms:
    ws = per_arm[arm]
    presents = sum(w['presents'] for w in ws); secs = sum(w['presents'] / w['fps'] for w in ws if w['fps'] > 0)
    print(f'  arm {arm} ({name}={",".join(sorted({w["value"] for w in ws}))}): {len(ws)} windows, {presents / secs:.2f} fps, {1000 * secs / presents:.2f} ms/frame')
if len(pairs) >= 2:
    for label, k in (('frame ms', 0), ('fps', 1)):
        d = [p[k] for p in pairs]
        m, sd = statistics.mean(d), statistics.stdev(d)
        half = t975(len(d) - 1) * sd / math.sqrt(len(d))
        print(f'  paired {label} ({b} minus {a}): {m:+.3f}, 95% CI [{m - half:+.3f}, {m + half:+.3f}] over {len(d)} pairs')
if polls:
    print('  cores 0-2 TSC ticks per guest insn by arm (JIT Run time, callbacks included):')
    for arm in arms:
        tot = {c: [0, 0] for c in (0, 1, 2)}
        for at, delta in polls:
            if at == 0 or at > len(keep): continue
            w = keep[at - 1]
            if not w['kept'] or w['arm'] != arm: continue
            for c in (0, 1, 2):
                tot[c][0] += delta[c][0]; tot[c][1] += delta[c][1]
        print(f'    arm {arm}: ' + '  '.join(f'core {c} {tot[c][1] / tot[c][0]:.2f}' if tot[c][0] else f'core {c} -' for c in (0, 1, 2)))
