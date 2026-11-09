#!/usr/bin/env bash
# Fetch the newest crash report (and its heap/stderr logs) and boot-trace.txt into $1.
timeout 120 python3 -I - "$1" <<'PY'
import ftplib, sys, os
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=30); f.login(); f.voidcmd("TYPE I")
f.cwd("/data/prosperoeden/logs"); lines = []; f.retrlines("LIST", lines.append)
names = [l.split()[-1] for l in lines]
c = sorted(n for n in names if n.startswith("crash-") and n.endswith(".txt") and n.count("-") == 2)[-1]
for n in (c, c[:-4] + "-heap.log", c[:-4] + "-stderr.log", "boot-trace.txt"):
    try:
        with open(os.path.join(sys.argv[1], n), "wb") as o: f.retrbinary("RETR " + n, o.write)
    except ftplib.all_errors as e: print(n, e)
f.quit()
print(c)
PY
