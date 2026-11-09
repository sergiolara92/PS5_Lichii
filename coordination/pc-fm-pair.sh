#!/usr/bin/env bash
# pc-fm-pair.sh <tag>: one PC race baseline (CRAM on, no fastmem) and one with the 64-bit fastmem arena
# (CRAM off, EDEN_HOST_A64_FASTMEM), texture caps 1536/2560; frame times from the scheduler trace (330 s)
# and screenshots at 360 s; summary lines into runs/pcfm2.res.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
R=$(cat $P/.local/headless-cache)/ns2-host/run
for v in base fm; do
    t=pc$v-${1:?tag}; extra=(); [[ $v == fm ]] && extra=(EDEN_HOST_A64_FASTMEM=1 EDEN_HOST_CRAM_OFF=1)
    rm -f $R/frame-*.ppm $R/sched-trace.bin
    cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=1536 \
        EDEN_NX2_TC_CRITICAL_MIB=2560 EDEN_HOST_PAD="$PAD" EDEN_HOST_GAME_SECONDS=380 NS2_RUN_SECONDS=480 \
        EDEN_NX2_SCHED_TRACE=330:40:/work/sched-trace.bin EDEN_HOST_CAPTURE=360 "${extra[@]}" > runs/$t.out 2>&1
    mkdir -p runs/$t; mv $R/sched-trace.bin $R/frame-*.ppm runs/$t/ 2>/dev/null; cp $R/stderr.log runs/$t/stderr.txt
    echo "$t $(python3 -I schedtrace.py runs/$t/sched-trace.bin 2>/dev/null | grep -m1 '^frames') $(grep -a -o 'EDEN_FASTMEM_FAULTS [0-9]*' runs/$t/stderr.txt) $(grep -a -o '== exit [0-9]*' runs/$t.out) nvrm=$(journalctl -k -b --no-pager | grep -c NV_ERR_NO_MEMORY)" | tee -a runs/pcfm2.res
done
