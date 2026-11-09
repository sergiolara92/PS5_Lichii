#!/usr/bin/env bash
# Starts the GPU capture of a play-capture.sh run at the next command list.
timeout 30 python3 -I - <<'PY'
import ftplib, io, time
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=20); f.login(); f.voidcmd("TYPE I")
f.storbinary("STOR /data/prosperoeden/capture-now.txt", io.BytesIO(str(time.time_ns()).encode()))
f.quit(); print("capture triggered")
PY
