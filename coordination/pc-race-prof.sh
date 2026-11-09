#!/usr/bin/env bash
# PC: build ns2-host, then a race of the real game (scripted pad as pc-capture-race.sh, split on)
# with wallprof sampling the guest cores, the GPU thread and its back from FROM seconds.
# pc-race-prof.sh <tag> [FROM=300] [VAR=value ...]; samples in runs/<tag>/wp*, report with wallprof/report.py.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; FROM=${2:-300}; shift; shift || true
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
cd $P && make ns2-host > $G/scratch-a42/build-host-$T.log 2>&1 || { echo build=FAIL; exit 1; }
R=$(cat $P/.local/headless-cache)/ns2-host/run; cp $C/wallprof/wallprof.so $R/; rm -f $R/wp $R/wp.*
cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=2048 EDEN_NX2_TC_CRITICAL_MIB=3072 \
    EDEN_HOST_PAD="$PAD" NS2_RUN_SECONDS=$((FROM + 120)) \
    "NS2_WRAPPER=env LD_PRELOAD=/work/wallprof.so WALLPROF_THREADS=CPUCore_0,CPUCore_1,CPUCore_2,CPUCore_3,CPUCore_4,CPUCore_5,GPU,GPUBack WALLPROF_FROM=$FROM WALLPROF_US=1000 WALLPROF_OUT=/work/wp" \
    "$@" > runs/$T.out 2>&1
mkdir -p runs/$T; mv $R/wp $R/wp.maps $R/wp.tags runs/$T/ 2>/dev/null; cp $R/stderr.log runs/$T/stderr.txt; cp $R/user/log/eden_log.txt runs/$T/
cp $P/build/ns2-host/eden-headless runs/$T/eden-headless
echo "samples $(stat -c %s runs/$T/wp 2>/dev/null)"; echo done
