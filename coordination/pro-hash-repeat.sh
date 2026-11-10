#!/usr/bin/env bash
# Pro: a second split-off replay with color-target checksums (installed build), to see whether the
# checksums repeat. pro-hash-repeat.sh <tag>
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; T=${1:?tag}
bash $C/replay-run.sh $T $P /data/lichii/captures/mkw-race-4.bin gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on env=EDEN_GPU_REPLAY_HASH=1 > $C/runs/$T.out 2>&1
grep -a "checksums\|draws (capture" $C/runs/$T/stderr.txt | cut -c1-500
echo done
