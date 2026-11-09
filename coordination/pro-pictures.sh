#!/usr/bin/env bash
# Pro: replay a capture (split off) with the installed build/dev, a quarter-size picture at every
# checkpoint into runs/<tag>/rt/. pro-pictures.sh <tag> [capture name] (no build).
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; CAP=${2:-mkw-race-5}
bash $C/replay-run.sh $T $P /data/prosperoeden/captures/$CAP.bin gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on env=EDEN_GPU_REPLAY_HASH=1 env=EDEN_GPU_REPLAY_DUMP=all > $C/runs/$T.out 2>&1
mkdir -p $C/runs/$T/rt
timeout 600 python3 -I - $C/runs/$T/rt <<'PY'
import ftplib, sys, os
f=ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"],2121,timeout=30); f.login(); f.voidcmd("TYPE I")
for i in range(1, 22):
    n=f"replay-rt-{i}.ppm"
    try:
        with open(os.path.join(sys.argv[1], n),"wb") as o: f.retrbinary("RETR /data/prosperoeden/logs/"+n, o.write)
        try: f.delete("/data/prosperoeden/logs/"+n)
        except ftplib.all_errors: pass
    except ftplib.all_errors as e: print(n, e)
f.quit()
PY
grep -a "draws (capture" $C/runs/$T/stderr.txt | grep -o "[0-9]* draws (capture: [0-9]*)"
echo "$(ls $C/runs/$T/rt | wc -l) pictures"; echo done
