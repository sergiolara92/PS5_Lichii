#!/usr/bin/env bash
# PC: a race of the real game (scripted pad as pc-jitmix.sh) from a checkout's ns2-host build, run under
# pmc/pmctlb (NS2_WRAPPER) counting page walks, memory stalls and top-down slots per second on the
# guest cores and GPU threads from FROM seconds for SECONDS.
#   pc-tlb.sh <tag> <checkout> [FROM=300] [SECONDS=60] [VAR=value ...]
# Counts in runs/<tag>/tlb.txt; report: pmc/pmctlb.py runs/<tag>/tlb.txt.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; P=$(realpath ${2:?checkout}); FROM=${3:-300}; SECS=${4:-60}; shift 4 || shift $#
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
R=$(cat $P/.local/headless-cache)/ns2-host/run
mkdir -p $C/runs/$T $R; cp $C/pmc/pmctlb $R/; rm -f $R/tlb.txt
cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=1536 \
    EDEN_NX2_TC_CRITICAL_MIB=2560 EDEN_HOST_PAD="$PAD" NS2_RUN_SECONDS=$((FROM + SECS + 40)) \
    "NS2_WRAPPER=/work/pmctlb $FROM $SECS CPUCore_,GPU,VulkanWorker /work/tlb.txt" "$@" > runs/$T.out 2>&1
mv $R/tlb.txt runs/$T/ 2>/dev/null
cp $R/stderr.log runs/$T/stderr.txt; cp $R/user/log/eden_log.txt runs/$T/ 2>/dev/null
grep -c pmctlb: runs/$T/stderr.txt; grep -m1 -o "RTX[^,\"]*\|llvmpipe" runs/$T/eden_log.txt; echo done
