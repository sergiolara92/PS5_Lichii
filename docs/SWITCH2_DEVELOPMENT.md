# Switch 2 development (the Fritchii base)

This guide is for developers working on ProsperoEden's Switch 2 build: what it is made of, how to
build and run it on a Linux PC, how to put it on a PS5, and what is known to limit it. The release
build of ProsperoEden (Switch games) is covered in [BUILDING.md](BUILDING.md); read its toolchain
section first, everything here builds on it.

No game, key or firmware file is part of this source, and none may ever be committed. You need
your own dump of the game, your own `prod.keys` and your own firmware files.

## What the build is made of

| Part | Where | What it is |
|---|---|---|
| Emulator source | `tools/deps.json` entry `fritchii` | [Fritchii](https://github.com/Frizti/Fritchii) v5.3 (GPL-3.0), an Eden-derived emulator with Switch 2 services, fetched as an archive at a pinned commit. It is the only source |
| PS5 patch | `patches/fritchii/fritchii-<commit>-ps5.patch` and `.d/` | The few source changes the PS5 layer needs (development loader, read errors as values, the JIT's fiber depth) |
| PS5 layer | `headless/` (and `src/`) | The PS5 frontend, memory, JIT, threads, files, pad, audio, launcher. It *derives* about 90 Eden/Eglantine files at configure time (`headless/CMakeLists.txt`) |
| Console Vulkan edits | `tools/prepare-vulkan-port.py` | Edits to the Vulkan renderer for the PS5 driver (RADV on the PS5), applied at configure time |
| Vulkan driver | `ps5-mesa` and `ps5-vulkan-tools` in `tools/deps.json` | RADV with a PS5 winsys, built from pinned revisions |

Every target (`make package`, `dev`, `test`, `ns2-host`) builds on it; `ns2` and `ns2-dev` are older names.

### How the PS5 layer changes upstream code

The PS5 layer never edits the fetched source in place. `headless/CMakeLists.txt` reads an upstream
file, replaces exact pieces of text with `string(REPLACE)` and writes a derived copy into the build
folder, which is compiled instead of the original. Each important piece is *pinned*: if the text is
not found, configuration stops with `Pinned <what> changed`. `tools/prepare-vulkan-port.py` does the
same for the console's Vulkan renderer (`Pinned Vulkan anchor changed: <file>: <text>`).

So when upstream changes a file the PS5 layer derives, the build tells you exactly which piece moved.
You then look at the new upstream code and move the edit there; you never copy an old upstream
function back in.

## Setting up

1. A Linux PC (Ubuntu 26.04, Arch or CachyOS). On CachyOS/Arch run `tools/setup-cachyos.sh` once.
2. `make toolchain` checks the host tools (clang-18 family, cmake, ninja, meson, glslang, ...).
3. For PC runs you also need `bwrap` (bubblewrap) and, for a window, an X11 session and a Vulkan GPU.
4. `make deps` fetches everything at its pinned revision (a few GB; once).

The build cache lives in `~/.cache/ps5-eden-headless.<hash>/`; its path is written to
`.local/headless-cache`. The prepared Switch 2 source is in `<cache>/source`, the PC build in
`<cache>/ns2-host/build`, and the PC run folder in `<cache>/ns2-host/run`.

## The PC build

Most work is faster on the PC: a change is built and tried in minutes, and the console is used to
confirm a milestone.

```bash
make ns2-host
```

builds `build/ns2-host/eden-headless`, the same PS5 layer and emulator compiled for Linux (`-O3`).
Running a game:

```bash
EDEN_HOST_VULKAN=1 NS2_RUN_SECONDS=300 tools/run-ns2-host.sh <keys folder> <firmware folder> <game folder>
```

- `<keys folder>` holds `prod.keys` (and `title.keys` if you have it).
- `<firmware folder>` holds the firmware's NCA files.
- `<game folder>` is a loose dump with `exefs/` (at least `main`) and `romfs/`.

Your files are only read. The app runs in a private bubblewrap sandbox where `/data/prosperoeden`
is the run folder; nothing is written outside the build cache. The run ends after
`NS2_RUN_SECONDS` (default 120) or at a guest fault.

### Environment for PC runs

| Variable | Effect |
|---|---|
| `EDEN_HOST_VULKAN=1` | Render with the PC's GPU in an X11 window (otherwise a null renderer: no picture) |
| `NS2_RUN_SECONDS=<n>` | How long the run lasts |
| `EDEN_HOST_CAPTURE="40,90,150"` | Save the frame shown at those seconds as `<run>/frame-<seconds>.ppm` |
| `EDEN_HOST_PAD="start:end:period:hold:mask"` | Press buttons: `mask` for `hold` seconds every `period` seconds between `start` and `end` (seconds from start) |
| `EDEN_HOST_MEMORY_12GB=0` | The 4 GiB memory layout instead of 12 GiB (the runner's default is 12 GiB) |
| `EDEN_HOST_CRAM_OFF=1` | Turn off compact memory (the guest's memory is then backed when handed out) |
| `EDEN_HOST_JIT_SHARED_OFF=1` | Every guest core compiles its own JIT blocks (diagnostic) |
| `EDEN_LOG_FILTER=...` | Eden's log filter, e.g. `*:Info HW.GPU:Debug` |
| `NS2_WRAPPER="gdb -batch -ex run -ex bt --args"` | Run the app under another command |

`EDEN_HOST_PAD` masks are **PS5 controller bits**, not Switch ones: Cross `0x4000`, Circle `0x2000`,
Square `0x8000`, Triangle `0x1000`, L1 `0x400`, R1 `0x800`, L2 `0x100`, R2 `0x200`, Options `0x8`,
Up/Right/Down/Left `0x10/0x20/0x40/0x80`, touch pad `0x100000`. With the default mapping Circle is
the game's A. Example: tap A and B at the title screen from 45 s to 150 s:
`EDEN_HOST_PAD="45:150:4:0.25:0x6000"`.

Frames can be viewed with any PPM viewer, or converted: `magick frame-90.ppm frame-90.png`.

### Logs

- `<run>/stderr.log`: the PS5 layer's lines (`EDEN_*`), startup steps, crashes.
- `<run>/user/log/eden_log.txt`: the emulator's log (services, GPU, Vulkan).

Lines worth knowing:

- `Report: Device: ...` (eden_log): the GPU the run uses. **It must be your real GPU.** If it says
  `llvmpipe`, the run rendered in software and every timing (and the "freezes" that come with it)
  is meaningless.
- `nx2:vi2 present #N`: frames handed to the display (the first eight, then only delays).
  `present #N fence (...) not reached in 4 s` means the GPU stopped producing frames.
- `BackSparseWrite: nx2:gpu N sparse blocks backed (X MiB)`: the game's loading milestones; useful
  for comparing timelines between builds.
- `EDEN_CRAM now held=... own=... zero=... shown=... page_tables_free=a/b`: compact memory (below).

### Reference timeline (Mario Kart World, PC, RTX 3070)

First frame after about 19 s; 1,275 MiB of GPU memory backed at about 27 s; the title map at 40 s;
in a race at about 90 s with Cross/Circle tapped from 45 s. Eglantine's own build has the same
timeline, so a build that is much slower is broken somewhere (see Pitfalls).

### Host tests

```bash
export PATH=$PWD/.local/toolchain/bin:$PATH
ninja -C "$(cat .local/headless-cache)/ns2-host/build" eden-memory-check
"$(cat .local/headless-cache)/ns2-host/build/bin/eden-memory-check"
```

checks the sparse tables, compact memory (including slots that keep no mapping), the JIT's
exclusive and LSE atomics (`--exclusive-pressure`: up to six cores), shared JIT blocks
(`--shared-jit`, slow) and more. Run it after any change to `src/memory_pages.cpp`,
`src/guest_memory_service.inc` or `headless/dynarmic/`.

## The console build

You need a PS5 able to run homebrew, with an FTP server (port 2121), an ELF loader (port 9021) and
klog (port 3232); see [INSTALL.md](INSTALL.md).

```bash
make ns2-dev                    # development build that boots Mario Kart World: build/dev/PPSA99008
make ns2                        # the packaged build with the launcher: build/release/PPSA99008
make install PS5_HOST=<console address> APP=build/dev/PPSA99008
```

Close ProsperoEden on the console before installing. `DEV_TITLE=<16-digit title ID>` makes
`ns2-dev` boot another game (Mario Kart World is `0400C3F00006E000`).

### Files on the console

| Path | What |
|---|---|
| `/data/prosperoeden/keys/` | `prod.keys` (and `title.keys`) |
| `/data/prosperoeden/firmware/` | the firmware NCAs |
| `/data/prosperoeden/roms/<game>/` | the loose dump (`exefs/`, `romfs/`) and its `<name>.eden` descriptor |
| `/data/prosperoeden/logs/` | `stderr.log`, `boot-trace.txt`, `crash-<time>.txt` and the logs kept with each crash |
| `/data/prosperoeden/user/log/eden_log.txt` | the emulator's log |
| `/data/homebrew/PPSA99008/` | the installed app |

A Switch 2 game is described by a JSON `.eden` file next to its dump:

```json
{
  "schema": 1,
  "enabled": true,
  "mode": "execute",
  "exefs": "/data/prosperoeden/roms/<game>/exefs",
  "romfs": { "kind": "loose", "path": "/data/prosperoeden/roms/<game>/romfs" },
  "required_modules": ["rtld", "main", "subsdk0", "sdk"]
}
```

A game card image works in place of the extracted folders: `"xci": "/data/prosperoeden/roms/<game>/<game>.xci"`
instead of `exefs` and `romfs`. ExeFS and RomFS then come from its program NCA, decrypted with the
keys in `/data/prosperoeden/keys` as the game reads them, so no RomFS is built from a folder at
start (Mario Kart World on the Pro: the game loaded at 52 s instead of 157 s).

Two more single-file forms work the same way, one key instead of `exefs` and `romfs`:

- `"nsp": "/data/prosperoeden/roms/<game>/<game>.nsp"`: a submission package, its program NCA's
  ExeFS and RomFS (the base game; an update in its own NSP is not applied).
- `"nup": "/data/prosperoeden/roms/<game>/<game>.nup"`: a NUP container. The program comes from its
  newest update (else its base), the RomFS is the update's over the base's; add
  `"nup_update": false` to boot the base alone. Add-on content inside it is not loaded yet.

A loose dump of tens of thousands of files can exhaust the console's file and thread resources;
prefer one of the single-file forms. The Switch 2 memory layout (12 GiB) is chosen from the
program's NPDM inside the container, so no `main.npdm` beside the file is needed.

On this branch the descriptor must **not** have a `"profiles"` object (the older base used
research profiles; this build refuses them with `Native startup research profiles are disabled in
this build`). `main`'s build still needs them, so keep a copy of each form if you switch between
the two.

### Controlling a development build remotely

A development build reads two files in its app folder (`/data/homebrew/PPSA99008/`), so a test can
be driven over FTP without touching the controller:

- `compat-input.txt`: `<id> <mask> <milliseconds> [lx ly rx ry]` presses the PS5 buttons in `mask`
  (same bits as above) for that long; each new command needs a larger `id`. Sticks are 0–255, 128 is
  centred.
- `quit-app.txt`: in the launcher, ends the app normally when its content changes (write a new
  token each time, e.g. the time; the app cannot delete the file, its folder is read-only to it).

During a game, touch pad + L1 (`0x100400`) returns to the launcher. **End test runs this way**
(touch pad + L1, then `quit-app.txt`) rather than killing the app: killed runs have been followed by
console power-offs in the past.

### Reference result (Mario Kart World, PS5 Pro)

First frame after about 169 s (the console boots much slower than the PC), then about 52 frames a
second at 1080p, and gameplay reached with compact memory on.

## Known limits and open work

### The title's page tables

The PS5 gives a title **16,384 CPU page tables**, and every 2 MiB the title maps takes one, whatever
the memory type or protection (measured on the Pro). That is about 32 GiB of mappings for
*everything*: the guest's memory, GPU memory, sparse resources, the JIT. When they run out, the
console logs `page table exhausted` and the allocation fails (Mario Kart World then ended with
`VK_ERROR_OUT_OF_DEVICE_MEMORY` while creating a 6 GiB sparse texture array).

Compact memory (`src/guest_memory_service.inc`) therefore keeps no mapping for a guest slot nothing
has read (`zero=`); a first read maps the shared zero block there (`shown=`), and a first write
gives it memory of its own (`own=`). `page_tables_free=` in the `EDEN_CRAM` line shows what is left
(console only). In Mario Kart World it falls to about 2,800 free after a few minutes of play and
keeps falling as the game writes more memory, so long sessions may still run out. Reducing it
further (the sparse texture array alone takes about 2,900 tables with nothing bound) is the next
stability task.

A GPU access to a sparse range that is not mapped at all is a fatal GPU page fault on the PS5, so
the driver has to map unbound sparse ranges (to a zero block) and cannot make them free.

### Performance

The console's first frame comes at about 169 s against 19 s on the PC; the Pro holds about 52 FPS
at 1080p in the first scenes. Both are open work.

### Upstream updates

To move to a newer Fritchii commit:

1. Download `https://github.com/Frizti/Fritchii/archive/<commit>.tar.gz` into `.deps/`, record its
   SHA-256, and update the `fritchii` entry in `tools/deps.json`, `UPSTREAM.json` and the defaults at
   the top of `tools/prepare-build.sh`.
2. Rename the patch and its `.d` folder to the new short commit, and check that they still apply
   (`patch -p1 --dry-run` in an extracted copy).
3. `make ns2-host`. A `Pinned ... changed` error names the piece that moved: find it in the new
   source and move the edit (keep what the edit is for; drop it if upstream now does the same).
4. For the console, also run `make ns2-dev`; `tools/prepare-vulkan-port.py` names moved anchors the
   same way.
5. Compare a PC run against the reference timeline above, then try the console.

## Pitfalls that cost days

- **Software rendering on the PC.** The runner gives the sandbox `/sys`; without it the NVIDIA
  driver finds no GPU and Vulkan silently uses `llvmpipe`. Always check `Report: Device:`.
- **An unoptimised PC build.** `tools/build-ns2-host.sh` passes `-O3` explicitly; a CMake cache
  with empty release flags once built everything at `-O0`, about ten times slower than Eglantine.
- **PC pad masks are PS5 bits**, not Switch bits (Cross is `0x4000`, not `0x1`).
- **Six guest cores.** Eglantine emulates six cores; per-core state in the PS5 layer is sized by
  `kGuestCores` (`headless/performance.h`), never by a literal 4.
- **Descriptor profiles.** See above: none on this branch.

## Licence

ProsperoEden and Eglantine are GPL-3.0-or-later. Keep upstream's copyright lines in any code you
move or adapt, and never add game data, keys, firmware or anything taken from the console's system
software to the repository.
