# Console, PC and lab tools

Scripts that drive the PS5 Pro (`oot-console-run.sh`, `dkb-console-run.sh`, `console-run.sh`), the GPU
recorder and replayer (`capture-now.sh`, `play-capture.sh`, `replay-run.sh`, `pc-replay.sh`), PC runs
(`pc-run.sh`, `pc-oot-load.sh`), the PS5 Linux lab (`lab/`), profilers (`wallprof/`, `pmc/`, `vkmem/`,
`heapsite/`) and run analysis (`oot-frame-lows.py`, `hitches.py`, ...). Notes are in `notes/`.

Console scripts read the console's address from `PS5_HOST` (FTP on port 2121). Runs, the console lock
and saves are under this folder (`runs/`, `save-backups/`, not committed); game files and the older
checkouts that some scripts default to stay in `GITHUB_ProsperoEden_20261029/`.

# ProsperoEden parallel work: shared notes

Three worktrees of `ProsperoEden` (local branches cut from `eglantine-base` at 85b0e2e), one per
session. The coordinating session works in `ProsperoEden` itself and merges what passes.

| Worktree | Branch | Work |
| --- | --- | --- |
| `ProsperoEden-split` | `eglantine-split` | Eglantine's GPU thread split crashes on the PS5 Pro: find and fix it |
| `ProsperoEden-draws` | `eglantine-draws` | GPU thread cost per draw in races: measure and cut it |
| `ProsperoEden-heap` | `eglantine-heap` | The heap keeps 2.4 GB of pieces for 1.6 GB in use: give memory back |
| `ProsperoEden-macros` | `eglantine-macros` | Native replacements for Mario Kart World's hottest GPU macros |
| `ProsperoEden-cores` | `eglantine-cores` | GPU threads on physical cores of their own; a Zen 2 (and LTO) console build |

## Base
`eglantine-base` is at 62f77bf (2026-11-01: Eglantine a42b4d4, games from an .xci), on 1663940: 85b0e2e + eglantine-split (c0e2ac9) + eglantine-draws
(547c9cb, 8487106) + eglantine-heap (the pieces' free room given back). Merge it into your branch
when convenient (`git merge refs/heads/eglantine-base`); runs with the split on omit `gpu_split=off`.
PokeDinner fork commits were tried and shelved: shelved/pokedinner/README.md.

## The game is read from its .xci (2026-11-01)
The console's `Mario Kart World.eden` names the .xci (`"xci"` instead of `exefs`/`romfs`); the folder
form is beside it as `Mario Kart World.eden.folder`. Only builds with eglantine-base's XCI loader read
it: merge eglantine-base first. console-run.sh's timeline is 100 s shorter to match (the race starts
~107 s sooner); `SHIFT=0` restores the old one.

## Goals (in this order)
1. Smooth, constant 60 fps in Mario Kart World races on the PS5 Pro, no hitches.
2. Fast loading.
3. No texture glitches.

## Where things stand (2026-10-31)
- Races run at 17-25 fps; menus at 60. The GPU thread is the limit: CPU-bound at about 17.7 us
  per draw, 2,000-3,200 draws a frame (60 fps needs about 6.9 us per draw on one thread).
- About half the console runs end out of memory when a course loads (the app's pool is 14.3 GB:
  guest ~5.8 GB, graphics 3.3-4.7 GB, heap pieces ~2.4 GB + 0.3 GB large blocks, JIT ~0.35 GB).
- Dev settings that every run uses: `gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on`.
  `gpu_accuracy=low` alone (no split) took the PC from ~18 to ~29 fps; the split on top, ~34.

## Rules
- Local commits on your own branch only. Never push, never add a remote, never touch `main`.
  Commit with the repository's configured noreply identity; never write any other email.
- Only your own worktree's files. The coordinator merges branches into `eglantine-base`.
- No game data, keys or firmware in any repository. Mask LAN addresses in messages (192.168.x.y).
- Console runs only through `console-run.sh` (it holds a lock: one run at a time on the Pro, and
  it installs your build over the title, so never install any other way). A run ends through the
  emulator's own stop; the runner's kill is only its watchdog. `PRIORITY=1` is for the cores session's
  short measurement runs only.
- PC runs only through `pc-run.sh` (one GPU test at a time).
- Driver (PS5_Mesa / RADV) changes: tell the coordinator instead of making them.
- Write findings worth sharing in `notes/<your-worktree>.md` here (short, dated).

## Tools (this folder)
- `console-run.sh <tag> <checkout> [dev settings...]`: install `build/dev/PPSA99008` of the
  checkout (build it with `make ns2-dev` first; the first build of a worktree takes long) and
  run Mario Kart World for ~9 minutes (title, menus, a race). Logs in `runs/<tag>/`.
- `fetch-crash.sh runs/<tag>`: the newest crash report and its logs from the console.
  Symbolize with `llvm-addr2line -f -C -e <checkout>/build/headless-native/llvm-pie.elf <offsets>`
  (the `eboot+0x...` offsets as they are).
- `split.py runs/<tag>/heap.txt <first> <last>`: share of each GPU-thread phase over windows;
  also `tools/analyze-perf-log.py` in the repository.
- `pc-run.sh <checkout> VAR=value...`: one PC run (`make ns2-host` first). Useful:
  `EDEN_HOST_VULKAN=1 NS2_RUN_SECONDS=220 EDEN_HOST_PAD=45:150:4:0.25:0x6000 EDEN_HOST_GPU_LOW=1`.
- `wallprof/`: an LD_PRELOAD wall-clock stack sampler for named threads (PC), with `report.py`
  and `inside.py` to symbolize; use it through `NS2_WRAPPER` (see wallprof.c).
- `heapsite/heapsite.c`: LD_PRELOAD live-heap-by-call-site profiler for the PC (build: cc -O2 -shared -fPIC -o heapsite.so heapsite.c -ldl -lpthread; copy into the run folder; touching /work/heapsite.dump writes /work/heapsite.txt).

Editing a shared script: write a new file and `mv` it over the old one (running copies keep the old file); never rewrite it in place.
