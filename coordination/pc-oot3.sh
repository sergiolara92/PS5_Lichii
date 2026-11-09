#!/usr/bin/env bash
# pc-oot.sh <tag> [seconds]: one PC run of the OOT NUP with frames at 60/100/140 s; prints the end of
# the game's log, the first guest fault's context and the unimplemented services.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; P=$C/../ProsperoEden
tag=${1:?tag}; secs=${2:-150}
cd $C && ./pc-run.sh $P NS2_GAME="/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029/ROMS - GITIGNORE/oot.nup" EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 \
    EDEN_NX2_TC_EXPECTED_MIB=4096 EDEN_NX2_TC_CRITICAL_MIB=5120 EDEN_HOST_CAPTURE="${PC_OOT_CAPTURE:-60,100,140}" NS2_RUN_SECONDS=$secs > runs/$tag.out 2>&1
R=$(cat $P/.local/headless-cache)/ns2-host/run; L=$R/user/log/eden_log.txt
mkdir -p runs/$tag; cp $R/stderr.log runs/$tag/stderr.txt; cp $L runs/$tag/eden_log.txt; mv $R/frame-*.ppm runs/$tag/ 2>/dev/null
echo "== $tag: $(tail -1 runs/$tag.out)"; ls runs/$tag/*.ppm 2>/dev/null | sed 's#.*/##' | tr '\n' ' '; echo
echo "log lines $(wc -l < $L), last at $(tail -1 $L | cut -c1-14)"
grep -a -E 'PANIC|SetTerminateResult|Cannot execute|session failed' $L | head -3 | cut -c1-170
n=$(grep -a -n -m1 -E 'Unmapped Read|Unmapped Write|PANIC|Cannot execute' $L | cut -d: -f1)
[ -n "$n" ] && { echo "-- before the first fault:"; sed -n "$((n-8)),$((n))p" $L | sed -E 's#/home/user/[^ ]*/(src|headless)/##g' | cut -c1-200; }
echo "-- unimplemented:"; grep -a -o "unimplemented function '[^']*'[^=]*port='[^']*'" $L | sort | uniq -c | sort -rn | head -12
