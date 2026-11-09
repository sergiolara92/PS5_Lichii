#!/usr/bin/env bash
# fmp2.sh: Pro races, 64-bit fastmem with its demoted-block list kept across sessions (learn, then
# use) interleaved with baselines; summaries into runs/fmp2.res.
set -u
G=/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029; P=$G/ProsperoEden; C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
cd $C
for t in fmq-base1 fmq-learn fmq-use1 fmq-base2 fmq-use2; do
    extra=(); [[ $t != *base* ]] && extra=(fastmem64=on)
    ./console-run.sh $t $P gpu_accuracy=low cram_fine=on cram_dedup=on "${extra[@]}" > runs/$t.out 2>&1
    h=runs/$t/heap.txt
    echo "$t $(python3 -I races.py $h 2>/dev/null | head -1) crash=$(grep -c 'run crashed' runs/$t.out) \
$(grep -a -h -o 'demoted_[a-z]*=[0-9]*' $h runs/$t/stderr.txt 2>/dev/null | sort -u | tr '\n' ' ') \
$(grep -a 'EDEN_FASTMEM64 faults' $h | tail -1)" | tee -a runs/fmp2.res
    grep -a 'EDEN_FASTMEM64_WHY' $h | tail -1 >> runs/fmp2.why
done
echo done >> runs/fmp2.res
