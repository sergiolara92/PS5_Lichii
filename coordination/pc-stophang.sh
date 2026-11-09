#!/usr/bin/env bash
# pc-stophang.sh <tag> <runs>: PC races ended by a timed stop, until one hangs at the stop limit;
# that run leaves every thread's stack (EDEN_HOST_STOP_DUMP) in runs/<tag>-<n>/stop-dump.txt.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
R=$(cat $P/.local/headless-cache)/ns2-host/run
cd $C
for n in $(seq 1 ${2:-5}); do
    t=${1:?tag}-$n; rm -f $R/stop-dump.txt
    ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=1536 \
        EDEN_NX2_TC_CRITICAL_MIB=2560 EDEN_HOST_PAD="$PAD" EDEN_HOST_GAME_SECONDS=300 NS2_RUN_SECONDS=420 \
        EDEN_HOST_STOP_DUMP=/work/stop-dump.txt > runs/$t.out 2>&1
    mkdir -p runs/$t; cp $R/stderr.log runs/$t/stderr.txt; mv $R/stop-dump.txt runs/$t/ 2>/dev/null
    echo "$t stopped=$(grep -a -c 'Game stopped' runs/$t/stderr.txt) dump=$([[ -s runs/$t/stop-dump.txt ]] && echo yes || echo no) $(grep -a -o '== exit [0-9]*' runs/$t.out | tail -1) gpu=$(grep -a -m1 -o 'Device: [^,]*' runs/$t/stderr.txt)" | tee -a runs/pcstop.res
    [[ -s runs/$t/stop-dump.txt ]] && break
done
echo "done $1" >> runs/pcstop.res
