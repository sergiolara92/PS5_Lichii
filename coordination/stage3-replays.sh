#!/usr/bin/env bash
# lever-stage3 replay checks for one option on the Pro (notes/lever-stage3.md): interleaved timing
# pairs, split verify, and pictures, the option on against off (OFF_ENV), split on throughout.
#   stage3-replays.sh <prefix> <OFF_ENV, e.g. EDEN_NX2_REG_BLOCKS=0> [pairs=3]
# Results: runs/<prefix>-*.out and a summary in runs/<prefix>.res.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
pre=${1:?prefix}; off=${2:?off env}; pairs=${3:-3}
CAP=/data/lichii/captures/mkw-race-5.bin
BASE="gpu_accuracy=low cram_fine=on cram_dedup=on"
res=$C/runs/$pre.res; : > $res
run() {  # run <tag> <settings...>
    local t=$1; shift
    $C/replay-run-reserved.sh $t ../ProsperoEden-stage3 $CAP "$@" > $C/runs/$t.out 2>&1
    { echo "== $t: $*"; grep -a "second half (\|front's, second\|us per draw\|draws (capture\|register checks" $C/runs/$t/stderr.txt $C/runs/$t/eden_log.txt 2>/dev/null | sed 's|^.*runs/||'; } >> $res
}
pictures() {  # pictures <tag> <settings...>
    local t=$1; shift
    run $t "$@" env=EDEN_GPU_REPLAY_HASH=1 env=EDEN_GPU_REPLAY_DUMP=all
    mkdir -p $C/runs/$t/rt
    timeout 600 python3 -I - $C/runs/$t/rt <<'PY'
import ftplib, sys, os
f=ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"],2121,timeout=30); f.login(); f.voidcmd("TYPE I")
for i in range(1, 22):
    n=f"replay-rt-{i}.ppm"
    try:
        with open(os.path.join(sys.argv[1], n),"wb") as o: f.retrbinary("RETR /data/lichii/logs/"+n, o.write)
        try: f.delete("/data/lichii/logs/"+n)
        except ftplib.all_errors: pass
    except ftplib.all_errors as e: print(n, e)
f.quit()
PY
    grep -a "checksums" $C/runs/$t/stderr.txt >> $res
}
for i in $(seq 1 $pairs); do
    run $pre-on$i $BASE
    run $pre-off$i $BASE env=$off
done
run $pre-ver-on $BASE env=EDEN_NX2_SPLIT_VERIFY=1
run $pre-ver-off $BASE env=EDEN_NX2_SPLIT_VERIFY=1 env=$off
pictures $pre-pic-on $BASE
pictures $pre-pic-off $BASE env=$off
python3 $C/picdiff.py $C/runs/$pre-pic-off/rt $C/runs/$pre-pic-on/rt >> $res 2>&1
echo "== done" >> $res
