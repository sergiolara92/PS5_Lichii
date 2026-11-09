#!/usr/bin/env bash
# pro-radv.sh <tag> [pairs=3]: Pro A/B of RADV's build, shared archive (-O2 -g, debugoptimized) against the
# scout's release copy (-O3 -march=znver2, assertions off: ProsperoEden-scout/build/radv-o3). Both are
# packages of the same scout commit, build/pkg-o2 and build/pkg-o3 (linked with each driver's isolated
# copy); each run copies its arm into build/dev, which console-run.sh and replay-run.sh install.
# First 2 interleaved replay pairs of mkw-race-5 (split on; the back's CPU time, second half), then
# <pairs> interleaved race pairs (races.py, fault per run). Results in runs/<tag>.res.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; S=$C/../ProsperoEden-scout
T=${1:?tag}; PAIRS=${2:-3}
cd $C
# The queued SMT runs install build/dev too: wait until they are done.
until grep -q "b3r-done" runs/sc-smtr.sh.out 2>/dev/null; do sleep 30; done
use() { rm -rf $S/build/dev/PPSA99008; cp -a --reflink=auto $S/build/pkg-$1/PPSA99008 $S/build/dev/; }
for i in 1 2; do
    for arm in o2 o3; do
        t=$T-rep-$arm-$i; use $arm
        ./replay-run.sh $t $S /data/prosperoeden/captures/mkw-race-5.bin gpu_accuracy=low cram_fine=on cram_dedup=on > runs/$t.out 2>&1
        echo "$t $(grep -a -h 'second half (1050\|split: the GPU' runs/$t.out runs/$t/*.txt 2>/dev/null | sort -u | tr '\n' ' ' | cut -c1-300)" | tee -a runs/$T.res
    done
done
for i in $(seq 1 $PAIRS); do
    for arm in o2 o3; do
        t=$T-$arm-$i; use $arm
        ./console-run.sh $t $S > runs/$t.out 2>&1
        f=$(grep -m1 -o "exception: 0x[0-9a-f]* ([A-Z_]*)" runs/$t/klog.log)
        echo "$t $(tail -2 runs/$t/run.out | head -1); fault=${f:-none}; $(python3 -I races.py runs/$t/heap.txt 2>&1 | tail -1)" | tee -a runs/$T.res
    done
done
use o2
echo done >> runs/$T.res
