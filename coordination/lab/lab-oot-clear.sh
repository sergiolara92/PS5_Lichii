#!/usr/bin/env bash
# Runs ON the PS5 Linux lab. lab-oot-clear.sh <tag> ["VAR=value ..."]: OOT gameplay measured, then the JIT group
# cleared once (EDEN_JIT_CLEAR_FILE) so gameplay recompiles its code in the order it runs, then measured again.
set -u
cd /data/lab/bin
tag=${1:?tag}; export LAB_EXTRA="${2:-} EDEN_JIT_CLEAR_FILE=/work/jit-clear-now.txt"
R=/data/lab/cache/ns2-host/run
./labdrive.sh stop >/dev/null 2>&1
./lab-oot-load.sh "$tag" 3600 >/dev/null 2>&1
sleep 8; ./labdrive.sh press 0x2000 300; sleep 40
./lab-measure.sh "$tag-before" 2>&1 | tee /data/lab/shots/$tag-before.txt
touch $R/jit-clear-now.txt
until grep -aq "EDEN_JIT_CLEAR_FILE clearing" /data/lab/shots/run.out; do sleep 1; done
sleep 45
./lab-measure.sh "$tag-after" 2>&1 | tee /data/lab/shots/$tag-after.txt
./labdrive.sh stop >/dev/null 2>&1
