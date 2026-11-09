#!/usr/bin/env bash
# pro-smt.sh <tag> <checkout> [pairs=3]: Pro A/B of the critical guest core's SMT sibling. Arm a: default
# placement (CPUCore_4/5 float on the secondary CPUs 1,3,5,7,12; CPU 1 is guest core 0's sibling).
# Arm b: pin=CPUCore_4:5 pin=CPUCore_5:7 (siblings of guest cores 2/3), CPU 1 left to light threads.
# Interleaved pairs, each a race with the scheduler trace from 230 s for 15 s after the guest's first thread switch (later windows end after the scripted return to menu at 325 s, and the trace is written only by a switch after its end).
# Summary per run in runs/<tag>.res: races.py, bound.py's guest-bound windows, ticks.py.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
T=${1:?tag}; P=$(realpath ${2:?checkout}); PAIRS=${3:-3}
cd $C
for i in $(seq 1 $PAIRS); do
    for arm in ${ARMS:-a b}; do
        t=$T-$arm$i; extra=(); [[ $arm == b ]] && extra=(pin=CPUCore_4:5 pin=CPUCore_5:7)
        ./scout-trace.sh $t $P 230 15 "${extra[@]}" > runs/$t.sh.out 2>&1
        g=$(python3 -I bound.py runs/$t/heap.txt 2>/dev/null | awk '/guest-bound/{n++; f+=$2} END{if(n) printf "guest-bound windows %d mean fps %.1f", n, f/n}')
        f=$(grep -m1 -o "exception: 0x[0-9a-f]* ([A-Z_]*)" runs/$t/klog.log); echo "$t $(tail -2 runs/$t/run.out | head -1); fault=${f:-none}; $(python3 -I races.py runs/$t/heap.txt 2>&1 | tail -1); $g; $(python3 -I ticks.py runs/$t/sched-trace.bin 2>&1 | sed -n '2p;3p;4p' | tr '\n' ' ')" | tee -a runs/$T.res
    done
done
echo done
