#!/usr/bin/env bash
# Drive a PC run of OOT live: pcdrive.sh start [seconds] | press <mask> <ms> | shot <name> | log
#   start: launches the PC build in the background (EDEN_HOST_PAD_FILE, EDEN_HOST_CAPTURE_FILE)
#   press: presses DualSense buttons (0x4000 Cross, 0x2000 Circle, 0x10/0x20/0x40/0x80 up/right/down/left)
#   shot:  saves the current frame as runs/pcdrive/<name>.png (half size) and prints its path
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; P=${PCDRIVE_CHECKOUT:-$C/../ProsperoEden}
R=$(cat $P/.local/headless-cache)/ns2-host/run; O=$C/runs/pcdrive; mkdir -p $O
case ${1:?command} in
start)
    rm -f "$R/pad.txt" "$R/capture-now.txt"
    cd $C && setsid ./pc-run.sh $P NS2_GAME="${PCDRIVE_GAME:-/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029/ROMS - GITIGNORE/oot.nup}" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 \
        EDEN_NX2_TC_EXPECTED_MIB=4096 EDEN_NX2_TC_CRITICAL_MIB=5120 EDEN_HOST_PAD_FILE=/work/pad.txt \
        EDEN_HOST_CAPTURE_FILE=/work/capture-now.txt EDEN_HOST_PRESENT=immediate NS2_RUN_SECONDS=${2:-1200} ${PCDRIVE_EXTRA:-} > $O/run.out 2>&1 &
    echo "started";;
press)
    printf '%s %s %s\n' "$(date +%s%N)" "${2:?mask}" "${3:-200}" > "$R/pad.txt.new" && mv "$R/pad.txt.new" "$R/pad.txt";;
shot)
    before=$(ls "$R"/frame-*.ppm 2>/dev/null | wc -l); touch "$R/capture-now.txt"
    for _ in $(seq 100); do sleep 0.2; [[ $(ls "$R"/frame-*.ppm 2>/dev/null | wc -l) -gt $before ]] && break; done
    sleep 0.5; f=$(ls -t "$R"/frame-*.ppm | head -1)
    python3 -I -c 'import sys; from PIL import Image; im=Image.open(sys.argv[1]); im.resize((im.width//2, im.height//2)).save(sys.argv[2])' "$f" "$O/${2:?name}.png"
    rm -f "$R"/frame-*.ppm; echo "$O/$2.png";;
log)
    tail -${2:-5} "$R/user/log/eden_log.txt" | cut -c1-220;;
esac
