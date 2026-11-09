#!/usr/bin/env bash
# Build both versions, then pictures at checksum 6: PC (GPU ASTC), Pro twice with GPU ASTC, Pro once with CPU ASTC.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
cd $P && make ns2-host > $G/scratch-a42/build-host-dump.log 2>&1 && make ns2-dev > $G/scratch-a42/build-dev-dump.log 2>&1 || { echo build=FAIL; exit 1; }
echo build=ok
( $C/pc-replay.sh pcdump EDEN_NX2_SPLIT=0 EDEN_GPU_REPLAY_HASH=1 EDEN_GPU_REPLAY_DUMP=6 | grep checksums | cut -c1-200
  cp $(cat $P/.local/headless-cache)/ns2-host/run/data/logs/replay-rt-6.ppm $C/runs/pcdump/ 2>/dev/null || find $(cat $P/.local/headless-cache)/ns2-host/run -name "replay-rt-6.ppm" -exec cp {} $C/runs/pcdump/ \; ) > $C/runs/pcdump.res 2>&1 &
for t in rd-gpu1:"" rd-gpu2:"" rd-cpu:astc=cpu; do
  T=${t%%:*}
  bash $C/replay-run.sh $T $P /data/prosperoeden/captures/mkw-race-4.bin gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on ${t#*:} env=EDEN_GPU_REPLAY_HASH=1 env=EDEN_GPU_REPLAY_DUMP=6 > $C/runs/$T.out 2>&1
  timeout 120 python3 -I - $C/runs/$T/replay-rt-6.ppm <<'PY'
import ftplib, sys
f=ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"],2121,timeout=30); f.login(); f.voidcmd("TYPE I")
with open(sys.argv[1],"wb") as o: f.retrbinary("RETR /data/prosperoeden/logs/replay-rt-6.ppm", o.write)
f.delete("/data/prosperoeden/logs/replay-rt-6.ppm"); f.quit()
PY
  echo "$T $(grep -a checksums $C/runs/$T/stderr.txt | cut -c1-120)"
done
wait; echo done
