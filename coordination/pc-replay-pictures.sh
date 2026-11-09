#!/usr/bin/env bash
# PC: build ns2-host, replay captures/<name>.bin dumping a quarter-size picture at every checkpoint
# into runs/<name>-replay/rt/. pc-replay-pictures.sh <name> [VAR=value ...]
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; NAME=${1:?name}; shift
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
cd $P && make ns2-host > $G/scratch-a42/build-host-pictures.log 2>&1 || { echo build=FAIL; exit 1; }
R=$(cat $P/.local/headless-cache)/ns2-host/run; rm -f $R/replay-rt-*.ppm
ln -f $G/captures/$NAME.bin $R/capture.bin
cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=3072 EDEN_NX2_TC_CRITICAL_MIB=4096 \
    EDEN_NX2_SPLIT=0 EDEN_HOST_GPU_REPLAY=/work/capture.bin EDEN_GPU_REPLAY_HASH=1 EDEN_GPU_REPLAY_DUMP=all \
    EDEN_GPU_REPLAY_DUMP_DIR=/work NS2_RUN_SECONDS=600 "$@" > runs/$NAME-replay.out 2>&1
rm -f $R/capture.bin
mkdir -p runs/$NAME-replay/rt; cp $R/stderr.log runs/$NAME-replay/stderr.txt; mv $R/replay-rt-*.ppm runs/$NAME-replay/rt/ 2>/dev/null
echo "$(ls runs/$NAME-replay/rt | wc -l) pictures"; echo done
