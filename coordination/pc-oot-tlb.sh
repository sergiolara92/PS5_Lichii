#!/usr/bin/env bash
# pc-oot-tlb.sh <tag> <checkout> [FROM=240] [SECONDS=120] [VAR=value ...]: PC run of OOT loaded into gameplay
# from the backed-up save (as pc-oot-prof.sh) under pmc/pmctlb, counting page walks, memory stalls and top-down
# slots per second on the guest cores and GPU threads from FROM s for SECONDS. Report: pmc/pmctlb.py runs/<tag>/tlb.txt.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; cd $C
T=${1:?tag}; P=$(realpath ${2:?checkout}); FROM=${3:-240}; SECS=${4:-120}; shift 4 || shift $#
GAME="${PCDRIVE_GAME:-$C/../ROMS - GITIGNORE/oot.nup}"
R=$(cat $P/.local/headless-cache)/ns2-host/run; L=$R/user/log/eden_log.txt
SAVE=${PCDRIVE_SAVE:-$(ls -d save-backups/oot-0400486024DEC000-*-ps5 | tail -1)}
mkdir -p runs/$T $R/user/nand/user/save/0000000000000000
for profile in $R/user/nand/user/save/0000000000000000/*/; do
    mkdir -p "$profile/0400486024DEC000"; cp -r "$SAVE"/. "$profile/0400486024DEC000"/
done
rm -f "$L" "$R/pad.txt" $R/tlb.txt; cp pmc/pmctlb $R/
setsid ./pc-run.sh $P NS2_GAME="$GAME" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 \
    EDEN_NX2_TC_EXPECTED_MIB=2048 EDEN_NX2_TC_CRITICAL_MIB=3072 EDEN_HOST_PAD_FILE=/work/pad.txt \
    EDEN_HOST_PRESENT=immediate NS2_RUN_SECONDS=$((FROM + SECS + 30)) \
    "NS2_WRAPPER=/work/pmctlb $FROM $SECS CPUCore_,GPU /work/tlb.txt" "$@" > runs/$T.out 2>&1 &
run=$!
until grep -a -q "vdecsvc configure" "$L" 2>/dev/null; do sleep 2; kill -0 $run 2>/dev/null || { echo "run ended early"; exit 1; }; done
echo "title at $(date +%T)"; sleep 30
for step in 1 2 3; do PCDRIVE_CHECKOUT=$P ./pcdrive.sh press 0x2000 250; sleep 4; done
sleep 35; PCDRIVE_CHECKOUT=$P ./pcdrive.sh shot $T-game1 >/dev/null; echo "gameplay at $(date +%T)"
wait $run
mv $R/tlb.txt runs/$T/ 2>/dev/null; cp $R/stderr.log runs/$T/stderr.txt; cp "$L" runs/$T/
grep -c pmctlb: runs/$T/stderr.txt; grep -m1 -o "RTX[^,\"]*\|llvmpipe" runs/$T/eden_log.txt; echo done
