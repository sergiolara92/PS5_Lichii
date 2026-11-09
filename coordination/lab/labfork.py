#!/usr/bin/env python3
"""Runs ON the PS5 Linux lab: drive another Eden fork (extracted AppImage) for comparisons with our build.
labfork.py start <fork>         eden-cli from /data/lab/forks/<fork>/dl/squashfs-root on the OOT dump, in
                                /data/lab/forks/<fork>/run (a fresh copy of ../user-template: keys, NAND, the
                                user's save; vsync off, GPU accuracy low, 1x), MangoHud logging every frame to
                                /data/lab/forks/<fork>/mh
labfork.py key <keys> [ms]      hold keys together via XTest, e.g. "a", "Down", "Up+l" (arrows = left stick,
                                i/j/k/l = right stick, a = A)
labfork.py shot <file.png>      screenshot of the X screen (half size)
labfork.py fps <fork> [seconds] fps from the newest MangoHud log over its last <seconds> (avg, median of 2 s windows)
labfork.py stop                 end every eden-cli
"""
import csv, ctypes, glob, os, shutil, statistics, subprocess, sys, time

FORKS = "/data/lab/forks"
GAME = "/data/lab/games/oot.nup"


def keys(names, ms):
    os.environ.setdefault("XAUTHORITY", sorted(glob.glob("/run/user/1000/xauth_*"))[0])  # over SSH too
    x11 = ctypes.CDLL("libX11.so.6"); xtst = ctypes.CDLL("libXtst.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p; x11.XStringToKeysym.restype = ctypes.c_ulong
    display = x11.XOpenDisplay(b":0")
    if not display: sys.exit("cannot open the X display")
    codes = [x11.XKeysymToKeycode(ctypes.c_void_p(display), x11.XStringToKeysym(n.encode())) for n in names]
    for c in codes: xtst.XTestFakeKeyEvent(ctypes.c_void_p(display), c, 1, 0)
    x11.XFlush(ctypes.c_void_p(display)); time.sleep(ms / 1000)
    for c in codes: xtst.XTestFakeKeyEvent(ctypes.c_void_p(display), c, 0, 0)
    x11.XFlush(ctypes.c_void_p(display)); x11.XCloseDisplay(ctypes.c_void_p(display))


def fps(fork, seconds):
    # The fork's window title carries its FPS counter (MangoHud does not hook the AppImage builds): one sample a second.
    os.environ.setdefault("XAUTHORITY", sorted(glob.glob("/run/user/1000/xauth_*"))[0]); os.environ["DISPLAY"] = ":0"
    pid = subprocess.run(["pgrep", "-x", "eden-cli"], capture_output=True, text=True).stdout.split()[0]
    win = subprocess.run(["xdotool", "search", "--pid", pid], capture_output=True, text=True).stdout.split()[0]
    samples = []
    for _ in range(int(seconds)):
        title = subprocess.run(["xdotool", "getwindowname", win], capture_output=True, text=True).stdout
        if "FPS: " in title: samples.append(int(title.split("FPS: ")[1].split()[0]))
        time.sleep(1)
    print(f"fps avg {statistics.mean(samples):.2f} median {statistics.median(samples):.2f} min {min(samples)} "
          f"({len(samples)} one-second samples of the title's FPS)")


cmd = sys.argv[1]
if cmd == "start":
    fork = sys.argv[2]; base = f"{FORKS}/{fork}"; run = f"{base}/run"
    shutil.rmtree(run, ignore_errors=True); os.makedirs(run)
    shutil.copytree(f"{FORKS}/user-template", f"{run}/user")
    os.makedirs(f"{base}/mh", exist_ok=True)
    root = f"{base}/dl/squashfs-root"
    env = dict(os.environ, DISPLAY=":0", SDL_VIDEODRIVER="x11", MANGOHUD="1",
               MANGOHUD_CONFIG=f"no_display,autostart_log=1,log_interval=0,output_folder={base}/mh",
               LD_LIBRARY_PATH=f"{root}/usr/lib")
    env.setdefault("XAUTHORITY", sorted(glob.glob("/run/user/1000/xauth_*"))[0])
    with open(f"{run}/stdout.log", "wb") as out:
        subprocess.Popen([f"{root}/usr/bin/eden-cli", "-g", GAME], cwd=run, env=env, stdout=out,
                         stderr=subprocess.STDOUT, start_new_session=True)
    print("started", fork)
elif cmd == "key":
    keys(sys.argv[2].split("+"), int(sys.argv[3]) if len(sys.argv) > 3 else 150)
elif cmd == "shot":
    # The composited screen through KDE (XWayland cannot read back a Vulkan window); prints the fork's title (FPS).
    env = dict(os.environ, XDG_RUNTIME_DIR="/run/user/1000", WAYLAND_DISPLAY="wayland-1", QT_QPA_PLATFORM="wayland",
               DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/1000/bus")
    subprocess.run(["timeout", "25", "spectacle", "-b", "-n", "-f", "-o", sys.argv[2]], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    xenv = dict(os.environ, DISPLAY=":0", XAUTHORITY=sorted(glob.glob("/run/user/1000/xauth_*"))[0])
    pid = subprocess.run(["pgrep", "-x", "eden-cli"], capture_output=True, text=True).stdout.split()
    title = ""
    if pid:
        wins = subprocess.run(["xdotool", "search", "--pid", pid[0]], capture_output=True, text=True, env=xenv).stdout.split()
        if wins: title = subprocess.run(["xdotool", "getwindowname", wins[0]], capture_output=True, text=True, env=xenv).stdout.strip()
    print(sys.argv[2], "|", title)
elif cmd == "fps":
    fps(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 40)
elif cmd == "stop":
    subprocess.run(["pkill", "-TERM", "-x", "eden-cli"])
