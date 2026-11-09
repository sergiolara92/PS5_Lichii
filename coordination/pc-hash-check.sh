#!/usr/bin/env bash
# PC: build ns2-host, then replays with color-target checksums: split off twice (are the checksums
# repeatable?) and split on once. pc-hash-check.sh <tag>
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; T=${1:?tag}
cd $P && make ns2-host > $G/scratch-a42/build-host-$T.log 2>&1 || { echo build=FAIL; grep -m5 " error" $G/scratch-a42/build-host-$T.log; exit 1; }
echo build=ok
for run in off1:0 off2:0 on:2; do
  echo "== ${run%%:*}"; $C/pc-replay.sh $T-${run%%:*} EDEN_NX2_SPLIT=${run#*:} EDEN_GPU_REPLAY_HASH=1 | grep "checksums\|draws (capture"
done
echo done
