#!/usr/bin/env bash
# pc-fastmem-ab.sh <pairs>: PC Vulkan races of Mario Kart World (scripted pad), CRAM off, with and without
# the PC-only 64-bit fastmem arena (EDEN_HOST_A64_FASTMEM, src/host_memory.cpp). Frame times from the
# guest scheduler trace (presents, 40 s from 330 s: schedtrace.py) into runs/pcfm.res. Each run ends
# through the emulator's own stop (EDEN_HOST_GAME_SECONDS): killed runs wedged the PC's driver.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
R=$(cat $P/.local/headless-cache)/ns2-host/run
for i in $(seq 1 ${1:-2}); do
  for v in off on; do
    t=pcfm-$v-$i; extra=(); [[ $v == on ]] && extra=(EDEN_HOST_A64_FASTMEM=1)
    cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=2048 \
        EDEN_NX2_TC_CRITICAL_MIB=3072 EDEN_HOST_PAD="$PAD" EDEN_HOST_GAME_SECONDS=380 NS2_RUN_SECONDS=480 \
        EDEN_NX2_SCHED_TRACE=330:40:/work/sched-trace.bin EDEN_HOST_CRAM_OFF=1 \
        "${extra[@]}" > runs/$t.out 2>&1
    mkdir -p runs/$t; cp $R/stderr.log runs/$t/stderr.txt
    mv $R/sched-trace.bin runs/$t/ 2>/dev/null
    echo "$t $(python3 -I schedtrace.py runs/$t/sched-trace.bin 2>/dev/null | grep -m1 '^frames') stop=$(grep -a -c 'Game stopped' runs/$t/stderr.txt; grep -a -o '== exit [0-9]*' runs/$t.out) arena=$(grep -ac EDEN_A64_ARENA runs/$t/stderr.txt) fault=$(grep -ac 'GUEST_EXCEPTION\|FATAL' runs/$t/stderr.txt)" | tee -a runs/pcfm.res
  done
done
echo done | tee -a runs/pcfm.res
