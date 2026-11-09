#!/usr/bin/env python3
"""lab-mhfps.py <MangoHud log folder> [seconds=40]: fps over the last <seconds> of the newest MangoHud CSV in the folder:
average (frames / time), median and minimum of 2-second windows, and the median frame time. Works for any Vulkan
program the lab runs with MANGOHUD=1 MANGOHUD_CONFIG=no_display,autostart_log=1,log_interval=0,output_folder=<dir>."""
import csv, glob, os, statistics, sys
folder = sys.argv[1]; seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 40
logs = sorted(glob.glob(os.path.join(folder, "*.csv")), key=os.path.getmtime)
if not logs: sys.exit("no MangoHud log in " + folder)
rows = list(csv.reader(open(logs[-1])))
head = next(i for i, r in enumerate(rows) if r and r[0] == "fps")
cols = rows[head]; t_col = cols.index("elapsed"); ft_col = cols.index("frametime")
frames = [(float(r[t_col]) / 1e9, float(r[ft_col])) for r in rows[head + 1:] if len(r) > ft_col]
end = frames[-1][0]; frames = [f for f in frames if f[0] >= end - seconds]
span = frames[-1][0] - frames[0][0]
windows = {}
for t, _ in frames:
    k = int((t - frames[0][0]) // 2); windows[k] = windows.get(k, 0) + 1
rates = [n / 2 for k, n in windows.items() if k < max(windows)] or [len(frames) / span]
print(f"fps avg {len(frames) / span:.2f} median {statistics.median(rates):.2f} min {min(rates):.2f} "
      f"({len(frames)} frames in {span:.1f} s; frame time median {statistics.median(f[1] for f in frames):.1f} ms)")
