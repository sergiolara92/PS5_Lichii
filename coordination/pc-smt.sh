#!/usr/bin/env bash
# pc-smt.sh <tag> <checkout> [pairs=2]: PC A/B of the critical guest core's SMT sibling, in the Pro's
# layout (pcplace.py): guest cores 0-3 alone on cores 0-3, GPU front/back on cores 4/5 with their
# siblings idle, every other thread on 6-9 (the guest cores' siblings). Arm a: CPUCore_4/5 float there
# too; arm b: CPUCore_4/5 pinned to 8 and 9 (siblings of guest cores 2/3), so 6 (guest core 0's
# sibling) carries only light threads: the Pro's pin=CPUCore_4:5 pin=CPUCore_5:7. Race scheduler trace
# 285-315 s per run; ARMS="n a b" adds arm n, no placement at all (the PC race; its course load is ~240-280 s); summary lines (ticks.py) in runs/<tag>.res.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; P=$(realpath ${2:?checkout}); PAIRS=${3:-2}
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
R=$(cat $P/.local/headless-cache)/ns2-host/run
BASE="CPUCore_0:0+CPUCore_1:1+CPUCore_2:2+CPUCore_3:3+GPUBack:5+GPUFencing:6,7,8,9+GPU:4"
cp $C/pcplace.py $R/
for i in $(seq 1 $PAIRS); do
    for arm in ${ARMS:-a b}; do
        t=$T-$arm$i; extra=""; [[ $arm == b ]] && extra="+CPUCore_4:8+CPUCore_5:9"
        wrap="NS2_WRAPPER=python3 -I /work/pcplace.py $BASE$extra+rest:6,7,8,9"; [[ $arm == n ]] && wrap="NS2_NOWRAP=1"
        rm -f $R/sched-trace.bin
        # A killed run frees its video memory minutes after pc-run lets the lock go: wait for it.
        for w in $(seq 60); do f=$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits | head -1); (( f >= 6000 )) && break; sleep 5; done
        cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=1536 \
            EDEN_NX2_TC_CRITICAL_MIB=2560 EDEN_HOST_PAD="$PAD" NS2_RUN_SECONDS=320 EDEN_HOST_A64_FASTMEM=1 EDEN_HOST_CRAM_OFF=1 \
            EDEN_NX2_SCHED_TRACE=285:30:/work/sched-trace.bin \
            "$wrap" > runs/$t.out 2>&1
        mkdir -p runs/$t; mv $R/sched-trace.bin runs/$t/ 2>/dev/null; cp $R/stderr.log runs/$t/stderr.txt; cp $R/user/log/eden_log.txt runs/$t/ 2>/dev/null
        echo "$t $(grep -c 'pcplace:' runs/$t/stderr.txt) placed; $(python3 -I ticks.py runs/$t/sched-trace.bin 2>&1 | sed -n '1p;2p;3p;4p' | tr '\n' ' ') $(grep -m1 -o 'RTX[^,\"]*\|llvmpipe' runs/$t/eden_log.txt)" | tee -a runs/$T.res
    done
done
echo done
