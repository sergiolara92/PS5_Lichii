#!/usr/bin/env bash
# pc-oot-prof.sh <tag> <checkout> [FROM=240] [SECONDS=400]: PC run of OOT that loads the backed-up
# save into gameplay (as pc-oot-load.sh) with wallprof sampling the guest cores and both GPU
# threads from FROM seconds. Samples in runs/<tag>/wp*, report with wallprof/report.py.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; cd $C
T=${1:?tag}; P=$(realpath ${2:?checkout}); FROM=${3:-240}; SECONDS_RUN=${4:-400}
GAME="${PCDRIVE_GAME:-$C/../ROMS - GITIGNORE/oot.nup}"
R=$(cat $P/.local/headless-cache)/ns2-host/run; L=$R/user/log/eden_log.txt; O=$C/runs/pcdrive
SAVE=${PCDRIVE_SAVE:-$(ls -d save-backups/oot-0400486024DEC000-*-ps5 | tail -1)}
# The save goes into every profile the PC build has made (each fresh run folder makes its own).
mkdir -p $O runs/$T $R/user/nand/user/save/0000000000000000
for profile in $R/user/nand/user/save/0000000000000000/*/; do
    mkdir -p "$profile/0400486024DEC000"; cp -r "$SAVE"/. "$profile/0400486024DEC000"/
done
echo "save $SAVE into $(ls $R/user/nand/user/save/0000000000000000 | wc -l) profile(s)"
rm -f "$L" "$R/pad.txt" "$R/capture-now.txt" $R/wp $R/wp.*; cp wallprof/wallprof.so $R/
setsid ./pc-run.sh $P NS2_GAME="$GAME" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 \
    EDEN_NX2_TC_EXPECTED_MIB=2048 EDEN_NX2_TC_CRITICAL_MIB=3072 EDEN_HOST_PAD_FILE=/work/pad.txt \
    EDEN_HOST_CAPTURE_FILE=/work/capture-now.txt EDEN_HOST_PRESENT=immediate NS2_RUN_SECONDS=$SECONDS_RUN \
    "NS2_WRAPPER=env LD_PRELOAD=/work/wallprof.so WALLPROF_THREADS=CPUCore_0,CPUCore_1,CPUCore_2,CPUCore_3,CPUCore_4,CPUCore_5,GPU,GPUBack WALLPROF_FROM=$FROM WALLPROF_US=1000 WALLPROF_OUT=/work/wp" \
    > runs/$T.out 2>&1 &
run=$!
until grep -a -q "vdecsvc configure" "$L" 2>/dev/null; do sleep 2; kill -0 $run 2>/dev/null || { echo "run ended early"; exit 1; }; done
echo "title at $(date +%T)"; sleep 30
PCDRIVE_CHECKOUT=$P ./pcdrive.sh shot $T-title >/dev/null
for step in 1 2 3; do PCDRIVE_CHECKOUT=$P ./pcdrive.sh press 0x2000 250; sleep 4; done
sleep 35; PCDRIVE_CHECKOUT=$P ./pcdrive.sh shot $T-game1 >/dev/null; echo "gameplay at $(date +%T)"
wait $run
mv $R/wp $R/wp.maps $R/wp.tags runs/$T/ 2>/dev/null; cp $R/stderr.log runs/$T/stderr.txt; cp "$L" runs/$T/
cp $P/build/ns2-host/eden-headless runs/$T/eden-headless
echo "samples $(stat -c %s runs/$T/wp 2>/dev/null)"; echo done
