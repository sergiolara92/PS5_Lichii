#!/usr/bin/env bash
# pc-vkmem.sh <tag> <checkout> [seconds=400] [VAR=value ...]: one PC run of the game (scripted pad as
# pc-trace.sh, fastmem on) under the vkmem layer (vkmem/: live device memory per call stack, every
# second) and pc-run's video-memory trace; the watchdog may stop it. Report: vkmem/vkmem.py runs/<tag>.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; P=$(realpath ${2:?checkout}); SECS=${3:-400}; shift 3 || shift $#
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
R=$(cat $P/.local/headless-cache)/ns2-host/run
mkdir -p $C/runs/$T $R/vkmem; cp $C/vkmem/libvkmem.so $C/vkmem/VkLayer_scout_vkmem.json $R/vkmem/; rm -f $R/vkmem.txt*
cd $C && PC_VRAM_TRACE=$C/runs/$T/vram.txt ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 \
    EDEN_NX2_TC_EXPECTED_MIB=1536 EDEN_NX2_TC_CRITICAL_MIB=2560 EDEN_HOST_PAD="$PAD" NS2_RUN_SECONDS=$SECS \
    EDEN_HOST_A64_FASTMEM=1 EDEN_HOST_CRAM_OFF=1 VK_ADD_LAYER_PATH=/work/vkmem VK_INSTANCE_LAYERS=VK_LAYER_SCOUT_vkmem \
    VKMEM_OUT=/work/vkmem.txt "$@" > runs/$T.out 2>&1
mv $R/vkmem.txt $R/vkmem.txt.stacks $R/vkmem.txt.maps runs/$T/ 2>/dev/null
cp $R/stderr.log runs/$T/stderr.txt; cp $R/user/log/eden_log.txt runs/$T/ 2>/dev/null
tail -2 runs/$T.out; grep -m1 -o "RTX[^,\"]*\|llvmpipe" runs/$T/eden_log.txt; ls -la runs/$T | tail -5
