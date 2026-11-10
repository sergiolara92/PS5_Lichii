#!/usr/bin/env bash
# replay2.sh <tag> [capture name] [dev settings...]: build ns2-dev, two Pro replays (split off),
# print the second-half GPU thread CPU and the draw check.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; CAP=${2:-mkw-race-5}; shift; shift || true
cd $P && make ns2-dev > $G/scratch-a42/build-dev-$T.log 2>&1 || { echo build=FAIL; grep -m5 " error" $G/scratch-a42/build-dev-$T.log; exit 1; }
for i in 1 2; do
  bash $C/replay-run.sh $T-$i $P /data/lichii/captures/$CAP.bin gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on "$@" > $C/runs/$T-$i.out 2>&1
  grep -a "second half (" $C/runs/$T-$i/stderr.txt | cut -c1-120; grep -a "draws (capture" $C/runs/$T-$i/stderr.txt | grep -o "[0-9]* draws (capture: [0-9]*)"
done
echo done
