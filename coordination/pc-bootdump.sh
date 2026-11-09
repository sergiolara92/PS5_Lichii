#!/usr/bin/env bash
# pc-bootdump.sh <tag> <checkout> [VAR=value ...]: one PC boot of the game (scripted pad, fastmem arena and
# CRAM off as pc-vkmem.sh) with every thread's stack taken by gdb at 150 s and 250 s of the game process
# (EDEN_HOST_STOP_DUMP makes the process attachable), to see where a boot that stays on the loading map
# waits. Dumps and logs in runs/<tag>/.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; P=$(realpath ${2:?checkout}); shift 2
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
R=$(cat $P/.local/headless-cache)/ns2-host/run
mkdir -p $C/runs/$T
(
    # The game process (bwrap shows it as /app/eden-headless) whose mounts include this checkout's run folder.
    find_pid() { for p in $(pgrep -f "^/app/eden-headless"); do grep -qs "$R" /proc/$p/mountinfo && echo $p && return; done; }
    until pid=$(find_pid) && [[ -n $pid ]]; do sleep 2; done
    start=$(date +%s)
    for at in 150 250; do
        while (( $(date +%s) - start < at )); do kill -0 $pid 2>/dev/null || exit 0; sleep 2; done
        # gdb cannot open bwrap's /app/eden-headless: give it the host binary; the libraries are the host's.
        gdb -batch -ex 'set sysroot /' -ex 'info threads' -ex 'thread apply all bt' $P/build/ns2-host/eden-headless -p $pid > $C/runs/$T/dump-$at.txt 2>&1
        tail -1 $R/user/log/eden_log.txt | cut -c1-200 > $C/runs/$T/log-at-$at.txt
    done
) &
cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 \
    EDEN_NX2_TC_EXPECTED_MIB=1536 EDEN_NX2_TC_CRITICAL_MIB=2560 EDEN_HOST_PAD="$PAD" NS2_RUN_SECONDS=300 \
    EDEN_HOST_A64_FASTMEM=1 EDEN_HOST_CRAM_OFF=1 EDEN_HOST_STOP_DUMP=/work/stop-dump.txt "$@" > runs/$T.out 2>&1
wait
cp $R/stderr.log runs/$T/stderr.txt; cp $R/user/log/eden_log.txt runs/$T/ 2>/dev/null
tail -2 runs/$T.out; ls -la runs/$T
