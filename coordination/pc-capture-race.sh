#!/usr/bin/env bash
# The texture cache is held to 4-5 GB on the PC: running the RTX 3070 (8 GB) out of memory left
# its driver unable to create a device until a reboot (NVRM out of memory, then "State in use").
# PC: build ns2-host, record a GPU capture inside a race (scripted pad as the Pro's development
# replay: L+R once, A every 5 s through the menus, then A tapped to drive and skip cutscenes),
# then replay it dumping pictures at every checkpoint to check what it shows.
# pc-capture-race.sh <name> <after_s>
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
NAME=${1:?name}; AFTER=${2:-300}
XCI="/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci"
PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"
cd $P && make ns2-host > $G/scratch-a42/build-host-$NAME.log 2>&1 || { echo build=FAIL; grep -m5 " error" $G/scratch-a42/build-host-$NAME.log; exit 1; }
echo build=ok
R=$(cat $P/.local/headless-cache)/ns2-host/run; rm -f $R/capture.bin
cd $C && ./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=4096 EDEN_NX2_TC_CRITICAL_MIB=5120 EDEN_NX2_SPLIT=0 EDEN_HOST_PAD="$PAD" \
    EDEN_HOST_GPU_CAPTURE=/work/capture.bin EDEN_HOST_GPU_CAPTURE_AFTER_S=$AFTER EDEN_HOST_GPU_CAPTURE_AT=0 \
    EDEN_HOST_GPU_CAPTURE_COUNT=2100 NS2_RUN_SECONDS=$((AFTER + 150)) > runs/$NAME-cap.out 2>&1
grep -ah "GPU capture" $R/user/log/eden_log.txt | grep -v "not clean" | sed 's/.*GPU capture/GPU capture/' | tail -3
[ -s $R/capture.bin ] || { echo "no capture"; exit 1; }
mv $R/capture.bin $G/captures/$NAME.bin; ls -la $G/captures/$NAME.bin | awk '{print $5}'
# Replay it with pictures at every checkpoint.
ln -f $G/captures/$NAME.bin $R/capture.bin
./pc-run.sh $P NS2_GAME="$XCI" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_NX2_TC_EXPECTED_MIB=4096 EDEN_NX2_TC_CRITICAL_MIB=5120 EDEN_NX2_SPLIT=0 EDEN_HOST_GPU_REPLAY=/work/capture.bin \
    EDEN_GPU_REPLAY_HASH=1 EDEN_GPU_REPLAY_DUMP=all EDEN_GPU_REPLAY_DUMP_DIR=/work NS2_RUN_SECONDS=600 > runs/$NAME-replay.out 2>&1
rm -f $R/capture.bin
mkdir -p runs/$NAME-replay/rt; cp $R/stderr.log runs/$NAME-replay/stderr.txt
find $R -name "replay-rt-*.ppm" -exec mv {} runs/$NAME-replay/rt/ \;
grep -a "GPU replay:" runs/$NAME-replay/stderr.txt | grep -v "record [0-9]* at\|shut down\|checksums" | tail -3
echo "$(ls runs/$NAME-replay/rt | wc -l) pictures"; echo done
