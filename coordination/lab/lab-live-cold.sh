#!/usr/bin/env bash
# Runs ON the PS5 Linux lab, on a game already running with EDEN_JIT_CLEAR_FILE=/work/jit-clear-now.txt and the
# character where it should move. lab-live-cold.sh [rounds=8] [seconds=25]: live A/B of patch 0202 in one session:
# keeps the character walking forward and back with the camera panning, and each round clears the JIT with cold
# code off or on (off, on, on, off, ... ABBA), waits 8 s, then measures fps (average and median of 120-frame
# windows). Prints each round and the per-mode summary.
set -u
cd /data/lab/bin
rounds=${1:-8}; secs=${2:-25}
R=/data/lab/cache/ns2-host/run
frame() { grep -a "EDEN_TC frame" $R/stderr.log | tail -1 | grep -oE "frame=[0-9]+" | cut -d= -f2; }
( while :; do
    ./labdrive.sh press 0 2900 128 0 255 128; sleep 3.0
    ./labdrive.sh press 0 2900 128 255 0 128; sleep 3.0
  done ) & mover=$!
trap 'kill $mover 2>/dev/null' EXIT
out=/data/lab/shots/live-cold-$(date +%H%M).txt; : > $out
for ((i = 0; i < rounds; ++i)); do
    case $((i % 4)) in 0|3) mode=0;; *) mode=1;; esac
    before=$(grep -ac "EDEN_JIT_CLEAR_FILE clearing" /data/lab/shots/run.out)
    echo "cold=$mode" > $R/jit-clear-now.txt
    until (( $(grep -ac "EDEN_JIT_CLEAR_FILE clearing" /data/lab/shots/run.out) > before )); do sleep 0.5; done
    sleep 8
    f0=$(frame); until [[ $(frame) != "$f0" ]]; do sleep 0.05; done
    fa=$(frame); ta=$(date +%s%N); fb=$fa; tb=$ta; w=()
    while (( $(date +%s%N) - ta < secs * 1000000000 )); do
        f=$(frame); if [[ $f != "$fb" ]]; then now=$(date +%s%N); w+=("$(( (f - fb) * 1000000000000 / (now - tb) ))"); fb=$f; tb=$now; fi
        sleep 0.05
    done
    line=$(printf '%s\n' "${w[@]}" | sort -n | awk -v m=$mode -v n=$((fb - fa)) -v t=$((tb - ta)) '{x[NR] = $1 / 1000}
        END { printf "cold=%d avg %.2f median %.2f (%d frames, %d windows)", m, n / (t / 1e9), x[int((NR + 1) / 2)], n, NR }')
    echo "round $i $line" | tee -a $out
done
awk '{m = $3; a[m] += $5; md[m] += $7; c[m]++} END { for (k in a) printf "%s: avg %.2f, median %.2f over %d rounds\n", k, a[k] / c[k], md[k] / c[k], c[k] }' $out | sort | tee -a $out
