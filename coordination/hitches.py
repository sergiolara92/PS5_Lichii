#!/usr/bin/env python3
# hitches.py runs/<tag>/heap.txt [min_ms]: the EDEN_HITCH lines (lever-hitches, patch 0043) of a
# run: each frame over min_ms (default 50) with its largest contributors, then the totals.
# Values are ms except draws, fm_faults and fm_demotions; waits and idle sum over threads/cores,
# so they can exceed the frame.
import collections, re, sys

min_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 50
counts = {'draws', 'fm_faults', 'fm_demotions', 'present'}
rows = []
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('EDEN_HITCH '):
        d = {k: float(v) for k, v in re.findall(r'(\w+)=([\d.]+)', line)}
        if d.get('frame_ms', 0) >= min_ms:
            rows.append(d)
total = collections.Counter()
for d in rows:
    rest = {k: v for k, v in d.items() if k not in counts | {'frame_ms', 'prev_blocked_ms'}}
    top = sorted(rest.items(), key=lambda kv: -kv[1])[:8]
    print(f"#{int(d['present']):6d} {d['frame_ms']:6.1f} ms blocked {d.get('prev_blocked_ms', 0):5.1f} "
          f"draws {int(d.get('draws', 0)):5d} faults {int(d.get('fm_faults', 0))} dem {int(d.get('fm_demotions', 0))} | "
          + ' '.join(f'{k}={v:.1f}' for k, v in top))
    total.update(rest)
print(f'{len(rows)} frames >= {min_ms} ms, {sum(d["frame_ms"] for d in rows):.0f} ms in all; totals:')
print(' '.join(f'{k}={v:.0f}' for k, v in total.most_common(16)))
