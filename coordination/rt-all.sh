#!/usr/bin/env bash
# Build ns2-dev, then Pro replays split off and split on dumping a quarter-size picture at every
# checkpoint (runs/<tag>/rt/). rt-all.sh <tag> [extra dev settings]
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; T=${1:?tag}; shift
cd $P && make ns2-dev > $G/scratch-a42/build-dev-$T.log 2>&1 || { echo build=FAIL; grep -m5 " error" $G/scratch-a42/build-dev-$T.log; exit 1; }
echo build=ok
for mode in off on; do
  extra=$([ $mode = off ] && echo gpu_split=off)
  bash $C/replay-run.sh $T-$mode $P /data/lichii/captures/mkw-race-4.bin gpu_accuracy=low cram_fine=on cram_dedup=on $extra "$@" env=EDEN_GPU_REPLAY_HASH=1 env=EDEN_GPU_REPLAY_DUMP=all > $C/runs/$T-$mode.out 2>&1
  mkdir -p $C/runs/$T-$mode/rt
  timeout 600 python3 -I - $C/runs/$T-$mode/rt <<'PY'
import ftplib, sys, os
f=ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"],2121,timeout=30); f.login(); f.voidcmd("TYPE I")
for i in range(1, 22):
    n=f"replay-rt-{i}.ppm"
    try:
        with open(os.path.join(sys.argv[1], n),"wb") as o: f.retrbinary("RETR /data/lichii/logs/"+n, o.write)
        try: f.delete("/data/lichii/logs/"+n)
        except ftplib.all_errors: pass
    except ftplib.all_errors as e: print(n, e)
f.quit()
PY
  echo "$mode $(ls $C/runs/$T-$mode/rt | wc -l) pictures"
done
echo done
