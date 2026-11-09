#!/usr/bin/env bash
# pc-oot-load.sh <tag>: PC run of OOT that loads the backed-up save (title -> Continue -> slot ->
# Load) and leaves the game running in gameplay; frames in runs/pcdrive/<tag>-*.png, the log's
# shader rejections printed. Drive further with pcdrive.sh press/shot.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; cd $C
tag=${1:?tag}
R=$(cat ${PCDRIVE_CHECKOUT:-../ProsperoEden}/.local/headless-cache)/ns2-host/run; L=$R/user/log/eden_log.txt
S=$R/user/nand/user/save/0000000000000000/5FE2297C8C154B2717FF0F9F06EBD468/0400486024DEC000
cp -r save-backups/oot-0400486024DEC000-20261104-220405/. "${S:?}"/
rm -f "$L"
./pcdrive.sh start 3600 >/dev/null
until grep -a -q "vdecsvc configure" "$L" 2>/dev/null; do sleep 2; done
sleep 30
./pcdrive.sh shot $tag-title >/dev/null
for step in 1 2 3; do ./pcdrive.sh press 0x2000 250; sleep 4; done
sleep 35
./pcdrive.sh shot $tag-game1; sleep 10; ./pcdrive.sh shot $tag-game2
grep -a "rejected" "$L" | sed -E 's/^\[ *[0-9.]+\] //; s/.*rejected: //; s/Native shader at 0x[0-9a-f]+, instruction 0x[0-9a-f]+: //' |
    sed -E 's/\[op ([0-9a-f]+) .*/[op \1]/' | sort | uniq -c | sort -rn
