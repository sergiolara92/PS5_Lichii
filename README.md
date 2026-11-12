<p align="center">
  <img src="sce_sys/icon0.png" width="192" alt="PS5Lichii app logo">
</p>

<h1 align="center">PS5_Lichii</h1>

<p align="center">
  <strong>Nintendo Switch 2 emulation on the PlayStation 5, built on Fritchii v5.5</strong>
</p>

<p align="center">
  <img alt="Pre-release v1.20.00" src="https://img.shields.io/badge/release-v1.20.00%20pre--release-orange">
  <img alt="Base Fritchii v5.5" src="https://img.shields.io/badge/base-Fritchii%20v5.5%20(4012e52)-blue">
  <img alt="Target PS5 Pro" src="https://img.shields.io/badge/target-PS5%20Pro-black">
  <img alt="Renderer Vulkan (RADV)" src="https://img.shields.io/badge/renderer-Vulkan%20%2F%20RADV-red">
  <img alt="License GPL-3.0-or-later" src="https://img.shields.io/badge/license-GPL--3.0--or--later-green">
</p>

<p align="center">
  <a href="#-thank-you">Thanks</a> ·
  <a href="#-where-it-stands">Status</a> ·
  <a href="#-what-is-in-it">Features</a> ·
  <a href="#-install">Install</a> ·
  <a href="#-game-files">Game files</a> ·
  <a href="#-building">Building</a> ·
  <a href="#-tools">Tools</a> ·
  <a href="#-credits">Credits</a>
</p>

![Important warning](assets/readme/important-warning.png)

## PS5_Lichii is a passion project, not a piracy project.

It exists so you can play the games you own on the console you own.

- **Piracy is not condoned.**
- **No games, encryption keys or console firmware are included, and they never will be.**
- **Use only legally obtained backups of games you own**, dumped yourself from your own game cards or digital purchases.
- **Use only keys and firmware dumped from hardware you own.**
- **Requests for, or links to, games, keys or firmware are not welcome** in this project's issues or discussions.

---

PS5_Lichii is the Switch 2 line of [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden), the
PS5 port of the Eden emulator. It runs [Fritchii](https://github.com/Frizti/Fritchii) v5.5, an
Eden-derived Switch 2 emulator, on the PS5 through ProsperoEden's native frontend, launcher and
Vulkan driver. Everything here is aimed at the **PS5 Pro**: its larger memory pool is what lets a
Switch 2 game fit at all.

> [!WARNING]
> **This is an early pre-release.** One game has been measured end to end (Ocarina of Time on the
> PS5 Pro). Expect hitches, out-of-memory stops in heavy areas, and games that do not start.
> Since v1.20.00 it installs as its own title, **PS5Lichii (`PPSA99009`)**, with its data in `/data/lichii`: it
> no longer replaces ProsperoEden or shares its folder. Coming from an earlier pre-release, rename
> `/data/prosperoeden` to `/data/lichii` before the first start.

## 🤝 Thank you

To the people whose tests, logs and fixes shaped the current release (their reports are in
[issue #1](https://github.com/mihawk-99/PS5_Lichii/issues/1)):

| Who | What they brought |
|---|---|
| [**Vampinity**](https://github.com/Vampinity) | Ocarina of Time on a base PS5, and the causes of three fixes: the light border around the title movie, the rumble that never fired, the intro at about 80% speed. Also why plants can go missing, and the base PS5 freeze reports |
| [**GrowingBlackBeans**](https://github.com/GrowingBlackBeans) | The trusted command copy, the reading of the crash logs (the 6144x6144 texture, the shared memory pool running short), and the submit thread finding for the graphics driver |
| [**yokozuna47**](https://github.com/yokozuna47) | PS5 Pro tests of v1.00.00 and v1.10.00 with full logs; the crashes they caught set the memory work of v1.20.00 |
| [**quatrixone**](https://github.com/quatrixone) | The title ID and data folder of its own, and the release build and check fixes ([#2](https://github.com/mihawk-99/PS5_Lichii/pull/2)) |

The projects this build stands on are in [Credits](#-credits).

## 📊 Where it stands

Measured on the PS5 Pro from per-frame times, frame generation off.

**Ocarina of Time** (`0400486024DEC000`), Hyrule Field, running with the camera and character moving
(run 50, the build pre-release v1.00.00 is made from):

| Window | Average | Median | 10% low | 1% low | Worst frame |
|---|---:|---:|---:|---:|---:|
| Whole gameplay stretch | **29.4 fps** | 28.2 fps | 18.9 fps | 11.5 fps | 624 ms |
| Last minute | **32.7 fps** | 33.9 fps | 22.8 fps | 18.8 fps | 58 ms |

**Mario Kart World** (`0400C3F00006E000`), a race from the start line, on the current `main`
(Fritchii v5.5). Restoring our PS5 renderer edits left the average where it was and took most of the
long stalls out:

| Race | Average | Median | 10% low | 1% low | Worst frame |
|---|---:|---:|---:|---:|---:|
| Fritchii v5.5 alone | 35.4 fps | 37.7 fps | 22.8 fps | 9.7 fps | 1,157 ms |
| **With the PS5 edits** | **34.7 fps** | **36.4 fps** | **24.5 fps** | **15.3 fps** | **290 ms** |

The *lows* are the average frame rate of the slowest 10% and 1% of frames: they are what a hitch
feels like, which an average hides.

| | Status |
|---|---|
| 🟢 Boots to gameplay | Ocarina of Time and Mario Kart World, on the PS5 Pro |
| 🟢 No GPU hangs | AMD ASTC decoder edge writes fixed by Fritchii; texture conversion in batches |
| 🟡 Out of memory | None on the measured Hyrule Field route; Ocarina of Time can still run out coming into Hyrule Field from Kokiri Forest |
| 🟡 Hitches | OoT's worst frame down from 2.3 s to 0.62 s, MKW's from 1.16 s to 0.29 s; some stalls remain at area changes |
| 🟡 30 fps + MihawkGen 60 fps | 30 fps base nearly held; frame generation to 60 is the next goal |
| 🟡 Known issue | Ocarina of Time's shader cache takes about four minutes to load at every start |
| ⚪ Other games | Donkey Kong Bananza reached gameplay on an earlier base; not re-measured on Fritchii |
| ⚪ Base PS5 | Not measured; its smaller memory pool is unlikely to be enough |

### Why memory is the whole game

A Switch 2 title expects 12 GB. The PS5 Pro gives an app a pool of about **13.3 GiB**, and in
Hyrule Field it holds roughly:

| Who | Size |
|---|---:|
| The game's own Switch 2 memory | 5–6 GiB |
| Graphics (textures, render targets, pipelines) | 3.3–4 GiB |
| Emulator heap and large blocks | ~2.1 GiB |
| JIT code | ~0.4 GiB |

An area change asks for about 2.5 GiB more graphics memory at once. Most of the work in this
build goes into keeping that headroom, without stalling the game:

- **Compact guest memory.** Slots of the game's memory that nothing has read keep no memory at all;
  cold slots are packed with LZ4 into a **slab store** (2 MiB blocks) that is compacted in the
  background, so packing never grows the heap.
- **A background scanner** keeps at least 2.5 GiB free, every 250 ms when short, and a two-tier
  emergency path packs only as much as an allocation needs.
- **ASTC textures converted in 64 MiB layer batches** instead of all layers at once (one terrain
  array alone needed a 967 MiB decode image).
- **Allocator recovery**: a failed graphics allocation frees idle memory and packs guest memory,
  for up to about 3 s, before it gives up.
- **Heap in 32 MiB pieces** (was 128 MiB), so a few long-lived blocks pin less.

## ✨ What is in it

<table>
<tr><td width="50%" valign="top">

**Emulation**
- Fritchii v5.5 (Switch 2 services, six guest cores)
- Shared, ahead-of-time JIT: blocks from earlier sessions compiled at start (**block list**, on by default)
- Shader cache across sessions
- Compute pipelines rebuilt on a worker thread (ported from Nolar-Hack/Lichii)

</td><td width="50%" valign="top">

**Game files**
- **XCI**, **NUP** and **NSP** files started as they are, no descriptor
- XCI read in place; NUP's newest update over its base
- Loose dumps (`exefs/` + `romfs/`) through a `.eden` file
- 12 GiB memory layout chosen from the game's own NPDM

</td></tr>
<tr><td valign="top">

**Graphics**
- Vulkan on [PS5 Mesa](https://github.com/mihawk-99/PS5_Mesa) (RADV with a PS5 winsys)
- ASTC kept compressed as BC formats
- **MihawkGen** frame generation is included in every PS5 package (30 → 60 fps), off by default
- **Frame rate limit** (30 / 40 / 60 FPS): a steady base, and frame generation's **Auto** target fills the output (30 → 120 at 120 Hz)
- Fast GPU accuracy on by default

</td><td valign="top">

**Launcher (from ProsperoEden)**
- Library with covers, profiles, per-game settings
- Button mapping, 29 languages, accessibility
- Performance overlay (**Touchpad + R1**)
- Crash reports with the session's logs

</td></tr>
</table>

**Defaults in this build** (the settings run 50 was measured with): **Fast GPU** on, **Compact
memory** on, **Block list** on. Frame generation is off; turn on **MihawkGen** in
**Settings > Video > Frame generation** for 60 fps output. The update check is off: this build has
no entry of its own on homebrew.page yet.

## 📦 Install

> [!TIP]
> Use [ShadowMountPlus 1.7beta4](https://github.com/drakmor/ShadowMountPlus/releases/tag/1.7beta4) or newer: it mounts the app and gives it `/data` and your drives.

1. Download `PS5_Lichii-v1.20.00.zip` from the [releases](../../releases) and extract it.
2. Copy the `PPSA99009` folder to `/data/homebrew/PPSA99009` on the PS5 (close PS5Lichii first).
3. Put your own `prod.keys` in `/data/lichii/keys/` and your own firmware files in `/data/lichii/firmware/`.
4. Put each game's file (`.xci`, `.nup` or `.nsp`) in `/data/lichii/roms/`, or in a folder of its own there, then open **Library**.

| Shortcut | Action |
|---|---|
| Touchpad + R1 | Performance overlay on/off |
| Touchpad + L1 | End the game and return to the library |

## 🎮 Game files

Copy the game file into `roms/` as it is: no extra file to write. The Library lists it, and the app
starts it as a Switch 2 game when it is one (a `.nup` always; an `.xci` or `.nsp` by its title ID).
Switch 1 games in the same folder keep starting as before.

```
/data/lichii/roms/
├── Mario Kart World.xci
├── oot/
│   └── Ocarina of Time.nup
└── Donkey Kong Bananza.nsp
```

| File | What is read |
|---|---|
| **XCI** (game card image) | ExeFS and RomFS from the program NCA as the game asks, decrypted with your keys; nothing is unpacked at start (Mario Kart World loaded at 52 s instead of 157 s) |
| **NUP** | The program from the newest update (else the base), the RomFS of the update over the base. Add-on content inside is not loaded yet |
| **NSP** | The base game's program NCA; an update in an NSP of its own is not applied |

A single file is best: a loose dump of tens of thousands of files can exhaust the console's file and
thread limits.

<details>
<summary>Loose dumps and special cases: a <code>.eden</code> descriptor</summary>

A small JSON `.eden` file beside a game is still read, for what a game file alone cannot say: a
loose dump (`exefs/` and `romfs/` folders), or a NUP started without its update (`"nup_update": false`).

```json
{
  "schema": 1,
  "enabled": true,
  "mode": "execute",
  "exefs": "/data/lichii/roms/<game>/exefs",
  "romfs": { "kind": "loose", "path": "/data/lichii/roms/<game>/romfs" }
}
```

`"xci"`, `"nup"` or `"nsp"` with a file's path take the place of `exefs` and `romfs`.

</details>

> [!IMPORTANT]
> Use only games, keys and firmware you dumped yourself from hardware you own. None of them are, or
> will ever be, part of this repository or its releases.

## 🛠️ Building

On Linux (Ubuntu 26.04, Arch or CachyOS; on CachyOS/Arch run `tools/setup-cachyos.sh` once):

```bash
make deps        # every input at its pinned revision
make package     # the app: build/release/PPSA99009
make release     # dist/: the ZIP, SHA256SUMS and release notes
make test        # the PC test suites
make ns2-host    # a Linux build of the same emulator, for fast PC runs
```

The build needs five sibling checkouts at the commits [`tools/deps.json`](tools/deps.json) pins:
[ps5-native-app-boilerplate](https://github.com/leolobato/ps5-native-app-boilerplate),
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa),
[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) and
[PS5_MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen). They are expected beside this
repository; to keep them elsewhere, write their folder to `.local/siblings` (or set
`PROSPERO_SIBLINGS`).

| Piece | Where |
|---|---|
| Emulator source | Fritchii `4012e52` (v5.5), fetched by `make deps`; the PS5 patch and our series in [`patches/fritchii/`](patches/fritchii) |
| PS5 frontend, memory, JIT, launcher | [`headless/`](headless), [`src/`](src) |
| Console Vulkan edits | [`tools/prepare-vulkan-port.py`](tools/prepare-vulkan-port.py) |
| Developer guide | [`docs/SWITCH2_DEVELOPMENT.md`](docs/SWITCH2_DEVELOPMENT.md), [`docs/BUILDING.md`](docs/BUILDING.md) |

## 🧰 Tools

[`coordination/`](coordination) holds the tools the measurements above come from:

| Tool | What it does |
|---|---|
| **Console runs** — `oot-console-run.sh`, `dkb-console-run.sh` | Install a build on the console, reach gameplay unattended through the live pad, walk a route, measure, quit cleanly, fetch the logs |
| **Frame lows** — `oot-frame-lows.py` | Average, median, 10% and 1% lows and worst frames from the per-frame histogram (`EDEN_FRAME_TIMES`) |
| **GPU recorder** — `capture-now.sh`, `play-capture.sh` | Record a game's GPU command stream on the console |
| **GPU replayer** — `replay-run.sh` (PS5), `pc-replay.sh` (PC) | Replay a capture deterministically to measure GPU-thread cost and check pictures |
| **PC runs** — `pc-run.sh`, `pc-oot-load.sh` | The Linux build driven into gameplay with a scripted pad, frame dumps and profiling |
| **PS5 Linux lab** — `lab/` | Profiling on a PS5 running Linux (same Zen 2 + GDDR6), with perf |
| **Profilers** — `wallprof/`, `pmc/`, `vkmem/`, `heapsite/` | Wall-clock sampling, hardware counters, Vulkan memory census, heap growth sites |
| **Stall analysis** — `hitches.py`, `hitchpc.py`, `hitchcmp.py` | Where long frames spend their time |

Console scripts read the console's address from `PS5_HOST`. Notes from the work are in
[`coordination/notes/`](coordination/notes).

## 🗺️ What comes next

1. **Zero hitches** on the Hyrule Field route and in races: unpacking texture sources on the GPU
   thread, emergency packing at area changes, the field-entry load.
2. **Lichii's latest GPU-thread work** (async read-back: OoT +17%; macros on the back thread: +8%),
   once Fritchii takes it in; then the per-draw work of our earlier MKW builds where it still wins.
3. **30 fps held, MihawkGen to 60 fps**, judged by the 1% low, not the average.
4. **Less memory**: re-evaluating the base against the next Lichii build (announced with large RAM
   savings), and a lower render resolution with FSR as an option.
5. More games re-measured on this base.

## 🙏 Credits

| Project | What it gives |
|---|---|
| [Eden](https://github.com/eden-emulator/mirror) | The emulator everything here derives from |
| [Fritchii](https://github.com/Frizti/Fritchii) by Frizti | The Switch 2 emulator this build runs (v5.5) |
| [Lichii](https://github.com/zortos293/Lichii) by zortos293 | Fritchii's base, and this repository's earlier base |
| [Eglantine](https://github.com/Djihads80/Eglantine) by Djihads80 | The first Switch 2 base of this work |
| [Nolar-Hack/Lichii](https://github.com/Nolar-Hack/Lichii) | BC-250 (PS5-class GPU) fixes; the compute-pipeline rebuild is ported from it |
| [EglantinePS5](https://github.com/heydemoura/EglantinePS5) by heydemoura | The index buffer span cap is ported from it |
| [Hookshot](https://github.com/NotRexed/Hookshot) by NotRexed | Invariant positions and NaN-guarded colour outputs on AMD are ported from it |
| [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden) by BlackBearReloaded and contributors | The PS5 frontend, launcher, packaging and everything around the emulator |
| [PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate), [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) | The native app foundation and the launcher's OpenGL |
| [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) by John Törnblom | The toolchain and system interfaces |
| [PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon) by ArkSama (and [mpereiraesaa's helper](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon)) | Filesystem elevation |

ProsperoEden's own contributors and testers are thanked in its
[README](https://github.com/blackbearreloaded/ProsperoEden#thanks). Third-party components, authors
and licences are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## ⚖️ License and disclaimer

GPL-3.0-or-later; see [LICENSE](LICENSE). ProsperoEden is Copyright © 2026 BlackBearReloaded;
Fritchii, Lichii and Eden keep their authors' copyright. Third-party components keep their own
licences. Binary releases are built from the tagged source in this repository.

- **No affiliation.** Independent homebrew, not affiliated with or endorsed by Sony Interactive
  Entertainment, Nintendo, or the Eden, Fritchii, Lichii or ProsperoEden projects. "PlayStation"
  and "PS5" are trademarks of Sony Interactive Entertainment Inc.; "Nintendo Switch" is a trademark
  of Nintendo.
- **No proprietary material.** No Sony or Nintendo SDK, firmware, encryption keys, system modules
  or game data are included.
- **No warranty, use at your own risk.** Provided "as is" (GPL sections 15 and 16). Running homebrew
  requires a modified console, which may void its warranty or cause data loss.
- **Legal use only.** Use it only with hardware and content you own. Do not ask for, post or link
  to game files, keys or firmware.

This project was developed with AI assistance from Anthropic tools.
