#!/usr/bin/env bash
# scout-trace.sh <tag> <checkout> [start s] [seconds] [dev settings...]: trace-race.sh for any checkout
# (a Pro race with the guest scheduler trace, usual settings plus the extra ones), fetched into
# runs/<tag>/sched-trace.bin. Per-frame tick analysis: pmc/../ticks.py runs/<tag>/sched-trace.bin.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; P=$(realpath ${2:?checkout}); START=${3:-230}; SECS=${4:-10}; shift 4 || shift $#
cd $C && ./console-run.sh $T $P gpu_accuracy=low cram_fine=on cram_dedup=on env=EDEN_NX2_SCHED_TRACE=$START:$SECS:/data/lichii/logs/sched-trace.bin "$@" > runs/$T.out 2>&1
timeout 300 python3 -I - $C/runs/$T/sched-trace.bin <<'PY'
import ftplib, sys
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=30); f.login(); f.voidcmd("TYPE I")
with open(sys.argv[1], "wb") as o: f.retrbinary("RETR /data/lichii/logs/sched-trace.bin", o.write)
try: f.delete("/data/lichii/logs/sched-trace.bin")
except ftplib.all_errors: pass
f.quit()
PY
ls -la $C/runs/$T/sched-trace.bin | awk '{print $5}'; python3 races.py runs/$T/heap.txt | tail -1; echo done
