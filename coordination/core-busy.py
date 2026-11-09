#!/usr/bin/env python3
"""core-busy.py <run dir>...: each emulated core's busy share (host CPU time / wall time) over the last
SECONDS (default 90) of a console run, from EDEN_PERF_CPU_POINT lines (heap.txt / heap.poll). With fps
pinned by pacing (a 30 fps view), a CPU gain shows here as less busy time instead of more frames."""
import os, re, sys
secs = float(os.environ.get("SECONDS_WINDOW", 90))
pat = re.compile(r"EDEN_PERF_CPU_POINT core=(\d+) .*?mono_ns=(\d+) cpu_ns=(-?\d+)")
for run in sys.argv[1:]:
    points = {}
    for name in ("heap.txt", "heap.poll"):
        path = os.path.join(run, name)
        if os.path.exists(path):
            for line in open(path, errors="replace"):
                m = pat.search(line)
                if m: points.setdefault(int(m[1]), []).append((int(m[2]), int(m[3])))
            if points: break
    out = []
    for core in sorted(points):
        pts = sorted(set(points[core]))
        end_t = pts[-1][0]; window = [p for p in pts if p[0] >= end_t - secs * 1e9]
        if len(window) < 2: continue
        (t0, c0), (t1, c1) = window[0], window[-1]
        out.append(f"core{core} {100 * (c1 - c0) / (t1 - t0):5.1f}%")
    print(f"{os.path.basename(run.rstrip('/')):12s} " + "  ".join(out))
