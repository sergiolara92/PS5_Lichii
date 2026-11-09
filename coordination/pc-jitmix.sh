#!/usr/bin/env bash
# PC: a race of the real game (scripted pad as pc-race-prof.sh) from a checkout's ns2-host build, run
# under pmc/pmcjit (NS2_WRAPPER, the app's parent) taking precise cycle and instruction samples of the
# guest cores (CPUCore_*) from FROM seconds for SECONDS.
#   pc-jitmix.sh <tag> <checkout> [FROM=300] [SECONDS=60] [VAR=value ...]
# Samples in runs/<tag>/jit.txt; report: pmc/jitmix.py runs/<tag>/jit.txt. PMC=pmcbr samples branch
# misses (kind b) instead of instructions.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; P=$(realpath ${2:?checkout}); FROM=${3:-300}; SECS=${4:-60}; shift 4 || shift $#
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
R=$(cat $P/.local/headless-cache)/ns2-host/run
mkdir -p $C/runs/$T $R; PMC=${PMC:-pmcjit}; cp $C/pmc/$PMC $R/; rm -f $R/jit.txt $R/jit.txt.maps
cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=2048 \
    EDEN_NX2_TC_CRITICAL_MIB=3072 EDEN_HOST_PAD="$PAD" NS2_RUN_SECONDS=$((FROM + SECS + 40)) \
    "NS2_WRAPPER=/work/$PMC $FROM $SECS CPUCore_ /work/jit.txt" "$@" > runs/$T.out 2>&1
mv $R/jit.txt $R/jit.txt.maps runs/$T/ 2>/dev/null
cp $R/stderr.log runs/$T/stderr.txt; cp $R/user/log/eden_log.txt runs/$T/ 2>/dev/null
grep "$PMC:" runs/$T/stderr.txt; grep -m1 -o "RTX[^,\"]*\|llvmpipe" runs/$T/eden_log.txt; echo done
