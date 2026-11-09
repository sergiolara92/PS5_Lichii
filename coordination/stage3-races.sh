#!/usr/bin/env bash
# lever-stage3 race pairs on the Pro (notes/lever-stage3.md): the option on against off (OFF_ENV),
# interleaved, usual settings, split on. stage3-races.sh <prefix> <OFF_ENV> [pairs=3]
# Summary in runs/<prefix>.res: races.py's race-window fps, GPU faults in the klog, how it ended.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
pre=${1:?prefix}; off=${2:?off env}; pairs=${3:-3}
BASE="gpu_accuracy=low cram_fine=on cram_dedup=on"
res=$C/runs/$pre.res; : > $res
race() {  # race <tag> <settings...>
    local t=$1; shift
    (cd $C && ./console-run-reserved.sh $t ../ProsperoEden-stage3 "$@" > runs/$t.out 2>&1)
    {
        echo "== $t: $*"
        python3 $C/races.py $C/runs/$t/heap.txt 2>&1 | tail -3
        echo "gpu faults: $(grep -ac 'GPU_FAULT' $C/runs/$t/klog.log 2>/dev/null); crash notes: $(grep -ac 'after_crash=1\|EDEN_GUEST_EXCEPTION\|Userspace PANIC' $C/runs/$t/klog.log $C/runs/$t/stderr.txt 2>/dev/null | awk -F: '{s+=$NF} END {print s}'); frames: $(ls $C/runs/$t/frame-*.ppm 2>/dev/null | wc -l)"
        tail -2 $C/runs/$t/run.out
    } >> $res
}
for i in $(seq 1 $pairs); do
    race $pre-on$i $BASE
    race $pre-off$i $BASE env=$off
done
echo "== done" >> $res
