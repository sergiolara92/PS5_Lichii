#!/usr/bin/env bash
# pc-live-ab.sh <tag> <checkout> <name=a|b> [MEASURE=300] [PERIOD=20] [VAR=value ...]: a live A/B in one PC run of
# OOT (headless/live_switches.inc): boots into gameplay from the backed-up save (as pc-oot-load.sh), turns the camera
# the same way all run, and from gameplay on writes the live switch file every PERIOD s in ABBA order (arm=A|B,
# period=k, name=value); afterwards livestat.py gives the paired verdict. EDEN_JIT_SWITCHES etc. as VAR=value.
# A/A check: name=v|v (the same value in both arms).
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; cd $C
T=${1:?tag}; P=$(realpath ${2:?checkout}); AB=${3:?name=a|b}; MEASURE=${4:-300}; PERIOD=${5:-20}; shift 5 2>/dev/null || shift $#
NAME=${AB%%=*}; VALUES=${AB#*=}; VA=${VALUES%%|*}; VB=${VALUES#*|}
GAME="${PCDRIVE_GAME:-$C/../ROMS - GITIGNORE/oot.nup}"
R=$(cat $P/.local/headless-cache)/ns2-host/run; L=$R/user/log/eden_log.txt; LIVE=$R/data/live-switches.txt
SAVE=${PCDRIVE_SAVE:-$(ls -d save-backups/oot-0400486024DEC000-*-ps5 | tail -1)}
mkdir -p runs/$T $R/data $R/user/nand/user/save/0000000000000000
for profile in $R/user/nand/user/save/0000000000000000/*/; do
    mkdir -p "$profile/0400486024DEC000"; cp -r "$SAVE"/. "$profile/0400486024DEC000"/
done
rm -f "$L" "$R/pad.txt" "$LIVE"
setsid ./pc-run.sh $P NS2_GAME="$GAME" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 \
    EDEN_NX2_TC_EXPECTED_MIB=2048 EDEN_NX2_TC_CRITICAL_MIB=3072 EDEN_HOST_PAD_FILE=/work/pad.txt \
    EDEN_HOST_PRESENT=immediate NS2_RUN_SECONDS=$((170 + MEASURE)) "$@" > runs/$T.out 2>&1 &
run=$!
until grep -a -q "vdecsvc configure" "$L" 2>/dev/null; do sleep 2; kill -0 $run 2>/dev/null || { echo "run ended early"; exit 1; }; done
echo "title at $(date +%T)"; sleep 30
for step in 1 2 3; do PCDRIVE_CHECKOUT=$P ./pcdrive.sh press 0x2000 250; sleep 4; done
sleep 35; echo "gameplay at $(date +%T)"
# Camera: right stick right 2.5 s, rest 2.5 s, all run (the same views in both arms).
( while kill -0 $run 2>/dev/null; do
    printf '%s 0 2500 128 128 255 128\n' "$(date +%s%N)" > "$R/pad.txt.new" && mv "$R/pad.txt.new" "$R/pad.txt"; sleep 5
  done ) &
order=(A B B A)
for ((k = 0; k * PERIOD < MEASURE; ++k)); do
    kill -0 $run 2>/dev/null || break
    arm=${order[k % 4]}; value=$VA; [[ $arm == B ]] && value=$VB
    printf 'arm=%s\nperiod=%d\n%s=%s\n' $arm $k "$NAME" "$value" > "$LIVE.new" && mv "$LIVE.new" "$LIVE"
    sleep $PERIOD
done
rm -f "$LIVE"
wait $run
cp "$L" runs/$T/eden_log.txt 2>/dev/null
grep -a "EDEN_LIVE_SWITCH refused" runs/$T.out | sort -u | head -3
python3 -I livestat.py runs/$T.out "$NAME"
