#!/usr/bin/env bash
# fms.sh <tag> [variants...]: Pro races, 64-bit fastmem with 16 KiB pieces for regions that cannot be
# shown whole. Variants: base (no window), learn (fresh demotion list), use (list from the last run).
# Summaries into runs/fms.res, refusal census lines into runs/fms.why.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=${CHECKOUT:-$G/ProsperoEden}; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination  # CHECKOUT=<worktree> to race another build
cd $C
tag=${1:?tag}; shift
for v in "$@"; do
    t=$tag-$v; extra=()
    extra=(fastmem64=on); [[ $v == base* ]] && extra=(fastmem64=off); [[ $v == def* ]] && extra=(); [[ $v == stk* ]] && extra=(fastmem64=on fastmem64_stack=on)  # on by default since 7af096d; def = no setting, stk = stack accesses too
    if [[ $v == learn* ]]; then
        timeout 60 python3 -I - <<'PY'
import ftplib
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=30); f.login()
for name in ("fastmem64-sites", "fastmem64-code-sites", "fastmem64-sites2"):
    try: f.delete("/data/prosperoeden/user/cache/jit/0400C3F00006E000." + name)
    except Exception as e: print("no demotion list:", e)
PY
    fi
    ./console-run.sh $t $P gpu_accuracy=low cram_fine=on cram_dedup=on "${extra[@]}" > runs/$t.out 2>&1
    h=runs/$t/heap.txt
    echo "$t $(python3 -I races.py $h 2>/dev/null | head -1) end=\"$(grep -a -h -o 'run [a-z ]* after [0-9.]* s' runs/$t.out | tail -1)\" \
$(grep -a -h -o 'demoted_[a-z]*=[0-9]*' $h 2>/dev/null | sort -u | tr '\n' ' ') ptf_min=$(grep -a -h -o 'page_tables_free=[0-9]*' runs/$t/* 2>/dev/null | cut -d= -f2 | sort -n | head -1) \
$(grep -a 'EDEN_FASTMEM64 faults' $h | tail -1) $(grep -a -h -o 'track_calls=[0-9]* track_mtsc=[0-9]* protect_mtsc=[0-9]* fault_mtsc=[0-9]* run_mtsc=[0-9]*' $h | tail -1) pieces=\"$(grep -a -h -o '562 MiB in [0-9]* pieces' runs/$t/stderr.txt | head -1)\" in_use=$(grep -a -h -c 'is in use (placed at' runs/$t/stderr.txt) gpufault=$(grep -a -c GPU_FAULT_PAGE_FAULT runs/$t/klog.log)" | tee -a runs/fms.res
    echo "$t $(grep -a 'EDEN_FASTMEM64_WHY' $h | tail -1)" >> runs/fms.why
    ping -c1 -W3 "$PS5_HOST" > /dev/null || { echo "$t CONSOLE OFFLINE" | tee -a runs/fms.res; exit 1; }
done
echo "done $tag" >> runs/fms.res
