#!/usr/bin/env bash
# pc-trace.sh <tag> <checkout> [start=325] [seconds=20] [VAR=value ...]: one PC race (scripted pad as
# pc-jitmix.sh, fastmem on like the console) with the guest scheduler trace from <start> s and
# screenshots at start+5 and start+25 s; the run stops at start+35 s, before the video-memory
# watchdog's usual stop (~330-340 s at caps 1536/2560). Logs and trace in runs/<tag>/; then
# ticks.py / critpath.py runs/<tag>/sched-trace.bin.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; P=$(realpath ${2:?checkout}); START=${3:-325}; SECS=${4:-20}; shift 4 || shift $#
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
R=$(cat $P/.local/headless-cache)/ns2-host/run
mkdir -p $C/runs/$T; rm -f $R/sched-trace.bin $R/frame-*.ppm
cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=1536 \
    EDEN_NX2_TC_CRITICAL_MIB=2560 EDEN_HOST_PAD="$PAD" NS2_RUN_SECONDS=$((START + 35)) EDEN_HOST_A64_FASTMEM=1 \
    EDEN_HOST_CRAM_OFF=1 EDEN_NX2_SCHED_TRACE=$START:$SECS:/work/sched-trace.bin \
    EDEN_HOST_CAPTURE="$((START + 5)),$((START + 25))" "$@" > runs/$T.out 2>&1
mv $R/sched-trace.bin $R/frame-*.ppm runs/$T/ 2>/dev/null
cp $R/stderr.log runs/$T/stderr.txt; cp $R/user/log/eden_log.txt runs/$T/ 2>/dev/null
tail -2 runs/$T.out; grep -m1 -o "RTX[^,\"]*\|llvmpipe" runs/$T/eden_log.txt; ls runs/$T
