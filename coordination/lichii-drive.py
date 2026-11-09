#!/usr/bin/env python3
"""Drive the Lichii PC build (reference renderer) through OOT with X test key presses.

lichii-drive.py start            launch eden-cli on the NUP under XWayland (SDL_VIDEODRIVER=x11)
lichii-drive.py key <name> [ms]  press a key (a = Switch A, s = B, Up/Down arrows = left stick)
lichii-drive.py shot <file.png>  save the X screen (the Lichii window is fullscreen)
lichii-drive.py stop             end eden-cli
"""
import ctypes, os, subprocess, sys, time

C = "/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination"
RUN = f"{C}/lichii-run"
CLI = f"{C}/lichii-oot/build/bin/eden-cli"
GAME = "/home/mihawk/Desktop/PS5_Homebrews/GITHUB_ProsperoEden_20261029/ROMS - GITIGNORE/oot.nup"


def key(name, ms):
    x11 = ctypes.CDLL("libX11.so.6")
    xtst = ctypes.CDLL("libXtst.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XStringToKeysym.restype = ctypes.c_ulong
    display = x11.XOpenDisplay(os.environ.get("DISPLAY", ":0").encode())
    if not display:
        sys.exit("cannot open the X display")
    code = x11.XKeysymToKeycode(ctypes.c_void_p(display), x11.XStringToKeysym(name.encode()))
    for down in (1, 0):
        xtst.XTestFakeKeyEvent(ctypes.c_void_p(display), code, down, 0)
        x11.XFlush(ctypes.c_void_p(display))
        if down:
            time.sleep(ms / 1000)
    x11.XCloseDisplay(ctypes.c_void_p(display))


command = sys.argv[1]
if command == "start":
    env = dict(os.environ); env.setdefault("SDL_VIDEODRIVER", "x11")
    with open(f"{RUN}/drive-stdout.log", "wb") as out:
        subprocess.Popen([CLI, "-g", GAME], cwd=RUN, env=env, stdout=out, stderr=subprocess.STDOUT,
                         start_new_session=True)
    print("started")
elif command == "key":
    key(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 150)
elif command == "shot":
    subprocess.run(["import", "-window", "root", sys.argv[2]], check=True)
    from PIL import Image
    image = Image.open(sys.argv[2]); image.resize((image.width // 2, image.height // 2)).save(sys.argv[2])
    print(sys.argv[2])
elif command == "stop":
    subprocess.run(["pkill", "-f", "^" + CLI])
