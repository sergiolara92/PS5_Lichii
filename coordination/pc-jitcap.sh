#!/usr/bin/env bash
# pc-jitcap.sh <tag> <checkout> [TRACES=40] [INSNS=200000] [FROM=170] [EVERY=4] [CORE_MASK=0x3f]: PC run of OOT (as pc-oot-prof.sh:
# backed-up save, three Circle presses into gameplay) with the JIT replay-bench capture (Eden 0143,
# EDEN_JIT_CAPTURE): from FROM s, every EVERY s, one guest-core window of INSNS instructions, TRACES in all.
# Traces land in runs/<tag>/jitcap/*.ejt; replay: EDEN_BENCH_DIR=... dynarmic_tests "[bench]".
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; cd $C
T=${1:?tag}; P=$(realpath ${2:?checkout}); TRACES=${3:-40}; INSNS=${4:-200000}; FROM=${5:-170}; EVERY=${6:-4}; MASK=${7:-0x3f}
GAME="${PCDRIVE_GAME:-$C/../ROMS - GITIGNORE/oot.nup}"
R=$(cat $P/.local/headless-cache)/ns2-host/run; L=$R/user/log/eden_log.txt
SAVE=${PCDRIVE_SAVE:-$(ls -d save-backups/oot-0400486024DEC000-*-ps5 | tail -1)}
mkdir -p runs/$T $R/user/nand/user/save/0000000000000000
for profile in $R/user/nand/user/save/0000000000000000/*/; do
    mkdir -p "$profile/0400486024DEC000"; cp -r "$SAVE"/. "$profile/0400486024DEC000"/
done
rm -rf "$R/jitcap"; mkdir -p "$R/jitcap"
rm -f "$L" "$R/pad.txt" "$R/capture-now.txt"
setsid ./pc-run.sh $P NS2_GAME="$GAME" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 \
    EDEN_NX2_TC_EXPECTED_MIB=2048 EDEN_NX2_TC_CRITICAL_MIB=3072 EDEN_HOST_PAD_FILE=/work/pad.txt \
    EDEN_HOST_CAPTURE_FILE=/work/capture-now.txt EDEN_HOST_PRESENT=immediate NS2_RUN_SECONDS=$((FROM + TRACES * EVERY + 60)) \
    EDEN_JIT_CAPTURE=/work/jitcap:$TRACES:$INSNS:$FROM:$EVERY:$MASK \
    > runs/$T.out 2>&1 &
run=$!
until grep -a -q "vdecsvc configure" "$L" 2>/dev/null; do sleep 2; kill -0 $run 2>/dev/null || { echo "run ended early"; exit 1; }; done
echo "title at $(date +%T)"; sleep 30
for step in 1 2 3; do PCDRIVE_CHECKOUT=$P ./pcdrive.sh press 0x2000 250; sleep 4; done
sleep 35; PCDRIVE_CHECKOUT=$P ./pcdrive.sh shot $T-game1 >/dev/null; echo "gameplay at $(date +%T)"
# WALK=1: walk and turn the camera for the rest of the run (heavy views, as the console runs' camera walk): the
# pad file's sticks (lx ly rx ry, 0-255) held 2.5 s per step.
if [[ -n ${WALK:-} ]]; then
    ( step=0; while kill -0 $run 2>/dev/null; do
        case $((step % 6)) in
            0) s="128 0 128 128";; 1) s="128 0 255 128";; 2) s="0 128 128 128";;
            3) s="128 0 0 128";; 4) s="255 128 128 128";; 5) s="128 255 255 128";;
        esac
        printf '%s 0 2500 %s\n' "$(date +%s%N)" "$s" > "$R/pad.txt.new" && mv "$R/pad.txt.new" "$R/pad.txt"
        step=$((step + 1)); sleep 2.5
        [[ $step == 12 ]] && PCDRIVE_CHECKOUT=$P ./pcdrive.sh shot $T-walk >/dev/null
    done ) &
    echo "walking from $(date +%T)"
fi
wait $run
cp -r "$R/jitcap" runs/$T/
grep -a "EDEN_JIT_CAPTURE" runs/$T.out | tail -50
ls runs/$T/jitcap | wc -l
