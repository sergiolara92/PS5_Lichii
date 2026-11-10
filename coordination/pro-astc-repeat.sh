#!/usr/bin/env bash
# Pro: build ns2-dev, then two split-off replays with checksums and ASTC decoded on the CPU.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
cd $P && make ns2-dev > $G/scratch-a42/build-dev-astccpu.log 2>&1 || { echo build=FAIL; grep -m5 " error" $G/scratch-a42/build-dev-astccpu.log; exit 1; }
echo build=ok
for i in 1 2; do
  bash $C/replay-run.sh pac-$i $P /data/lichii/captures/mkw-race-4.bin gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on astc=cpu env=EDEN_GPU_REPLAY_HASH=1 > $C/runs/pac-$i.out 2>&1
  grep -a "checksums" $C/runs/pac-$i/stderr.txt | cut -c1-300
done
echo done
