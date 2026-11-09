#!/usr/bin/env bash
# pc-bootloop.sh <tag> <checkout> <boots> [EDEN_JIT_SWITCHES...]: boots OOT on the PC <boots> times (110 s each:
# boot, title, into gameplay with three Circle presses) with the given JIT switches (EDEN_JIT_SWITCHES, the
# console's jit_* dev settings), and reports each boot's guest crashes (EDEN_CRASH / ExceptionRaised in the log).
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; cd $C
T=${1:?tag}; P=$(realpath ${2:?checkout}); N=${3:?boots}; shift 3; SW="$*"
GAME="$C/../ROMS - GITIGNORE/oot.nup"
R=$(cat $P/.local/headless-cache)/ns2-host/run; L=$R/user/log/eden_log.txt
SAVE=$(ls -d save-backups/oot-0400486024DEC000-*-ps5 | tail -1)
mkdir -p runs/$T
for i in $(seq 1 $N); do
    mkdir -p $R/user/nand/user/save/0000000000000000
    for profile in $R/user/nand/user/save/0000000000000000/*/; do mkdir -p "$profile/0400486024DEC000"; cp -r "$SAVE"/. "$profile/0400486024DEC000"/; done
    rm -f "$L" "$R/pad.txt"
    ( sleep 70; for s in 1 2 3; do printf '%s 8192 250\n' "$(date +%s%N)" > "$R/pad.txt.new" && mv "$R/pad.txt.new" "$R/pad.txt"; sleep 4; done ) &
    EDEN_JIT_SWITCHES="$SW" ./pc-run.sh $P NS2_GAME="$GAME" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_HOST_PAD_FILE=/work/pad.txt \
        EDEN_HOST_PRESENT=immediate NS2_RUN_SECONDS=110 EDEN_JIT_SWITCHES="$SW" > runs/$T/boot-$i.out 2>&1
    wait
    cp "$L" runs/$T/boot-$i.log 2>/dev/null
    crash=$(grep -a -c "EDEN_CRASH\|ExceptionRaised" runs/$T/boot-$i.log 2>/dev/null)
    sw=$(grep -a -c "EDEN_JIT_SWITCH" $R/stderr.log)
    echo "boot $i: guest crash lines ${crash:-?}, switches seen $sw, $(grep -a '== exit' runs/$T/boot-$i.out)"
done
