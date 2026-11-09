# dkb-look.py <run dir>: what the running Bananza session is doing (logs read over FTP, a screenshot).
import ftplib, io, re, sys, time, os
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=30); f.login(); f.voidcmd("TYPE I")
def get(remote, local, limit=None):
    buf = io.BytesIO()
    try:
        if limit is None:
            f.retrbinary("RETR " + remote, buf.write)
        else:
            conn = f.transfercmd("RETR " + remote)
            while buf.tell() < limit:
                c = conn.recv(65536)
                if not c: break
                buf.write(c)
            conn.close()
            try: f.voidresp()
            except ftplib.all_errors: pass
    except ftplib.all_errors: return b""
    open(os.path.join(out, local), "wb").write(buf.getvalue()); return buf.getvalue()
boot = get("/data/prosperoeden/logs/boot-trace.txt", "boot-trace.txt").decode("utf-8", "replace")
log = get("/data/prosperoeden/user/log/eden_log.txt", "eden_log-live.txt", 64 << 20).decode("utf-8", "replace")
heap = get("/data/prosperoeden/logs/heap.log", "heap-head.txt", 8 << 20).decode("utf-8", "replace")
frames = [l for l in heap.splitlines() if l.startswith("EDEN_VULKAN_FRAME ")]
print("boot:", (boot.strip().splitlines() or ["-"])[-1][:100])
print("log lines:", log.count("\n"), "last:", (log.strip().splitlines() or ["-"])[-1][:140])
for k in ("Unmapped", "No file system interface", "Rejected program", "exceeds maximum storage"):
    print(f"  {k}: {log.count(k)}")
print("pthread warnings:", heap.count("Internal Memory"), "| frame windows:", len(frames), (frames[-1][:90] if frames else ""))
# A picture: the dev build writes game-frame.ppm at its next 30 s segment after capture-once.txt.
if "--shot" in sys.argv:
    f.storbinary("STOR /data/homebrew/PPSA99008/capture-once.txt", io.BytesIO(b"1"))
f.quit()
