#!/usr/bin/env bash
# One PC replay of captures/mkw-race-4.bin through pc-run.sh (make ns2-host first; one PC GPU test
# at a time, never while anything builds). pc-replay.sh <tag> [VAR=value ...], e.g.
#   pc-replay.sh r1 EDEN_NX2_SPLIT=2 EDEN_GPU_REPLAY_HASH=1
# CAP=<capture name> picks another capture, CHECKOUT=<path> another checkout. Prints the replay's result lines; logs in runs/<tag>/.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=${CHECKOUT:-$G/ProsperoEden}; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
tag=${1:?tag}; shift
R=$(cat $P/.local/headless-cache)/ns2-host/run
ln -f $G/captures/${CAP:-mkw-race-4}.bin $R/capture.bin
cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=${TC_EXPECTED:-1536} EDEN_NX2_TC_CRITICAL_MIB=${TC_CRITICAL:-2560} EDEN_HOST_GPU_REPLAY=/work/capture.bin NS2_RUN_SECONDS=600 "$@" > runs/$tag.out 2>&1
rm -f $R/capture.bin
mkdir -p runs/$tag; cp $R/stderr.log runs/$tag/stderr.txt; cp $R/user/log/eden_log.txt runs/$tag/eden_log.txt 2>/dev/null
grep -a "GPU replay:" runs/$tag/stderr.txt | grep -v "record [0-9]* at\|shut down" | cut -c1-500
