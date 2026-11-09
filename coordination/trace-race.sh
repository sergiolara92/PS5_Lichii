#!/usr/bin/env bash
# trace-race.sh <tag> [start s] [seconds] [dev settings...]: a Pro race with the guest scheduler
# trace (EDEN_NX2_SCHED_TRACE, patch 0034), fetched into runs/<tag>/sched-trace.bin.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; P=$C/../ProsperoEden
T=${1:?tag}; START=${2:-230}; SECS=${3:-10}; shift; shift; shift
cd $C && ./console-run.sh $T $P gpu_accuracy=low cram_fine=on cram_dedup=on env=EDEN_NX2_SCHED_TRACE=$START:$SECS:/data/prosperoeden/logs/sched-trace.bin "$@" > runs/$T.out 2>&1
timeout 300 python3 -I - $C/runs/$T/sched-trace.bin <<'PY'
import ftplib, sys
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=30); f.login(); f.voidcmd("TYPE I")
with open(sys.argv[1], "wb") as o: f.retrbinary("RETR /data/prosperoeden/logs/sched-trace.bin", o.write)
try: f.delete("/data/prosperoeden/logs/sched-trace.bin")
except ftplib.all_errors: pass
f.quit()
PY
ls -la $C/runs/$T/sched-trace.bin | awk '{print $5}'; echo done
