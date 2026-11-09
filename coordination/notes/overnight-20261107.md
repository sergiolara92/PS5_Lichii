# Overnight 2026-11-06/07: OOT and Bananza toward 60 fps (worktree ProsperoEden-oot)

All measured on the Pro with unattended runs (coordination/oot-console-run.sh, dkb-console-run.sh):
the script installs the dev build, loads the save by itself (A presses until gameplay), measures
5 s windows and ends through the emulator's quit. Patches are in
ProsperoEden-oot/patches/eglantine/eglantine-a42b4d4-ps5.d (uncommitted, recorded in the build
tree's .applied-patches).

## Ocarina of Time (Link's house, ~1,020 draws a frame)

| Change | fps |
|---|---|
| baseline (0124 AMD fixes) | 25.0-25.9 |
| 0125 acquire suspends without drain | no change |
| **0126 Lichii PR 103: CPU writes on GPU pages uploaded, not downloaded** | **45.0** (A/B 25.0 vs 45.0) |
| 0127 Lichii #446/#414/#146 + Djihads range/binding caches | 44.6 |
| 0128 Lichii #250 page filters | 45.3 |

After 0126 the GPU threads are no longer the limit in OOT: the front is ~half idle, the back ~half
idle, the game's threads 25-32% busy and waiting on each other (arbitration). The game reports
"game speed 76%": its frame (~22 ms) is its own job chain. Kokiri Forest (2,200-2,800 draws) not
re-measured with the new patches. A save at a forest spot (slot_01) was made; the game froze right
after saving (a write to a garbage address, with graphics memory short: a 300 MB allocation
failed). Saves are backed up in coordination/save-backups/.

## Donkey Kong Bananza (first area, ~3,850 draws a frame)

| Change | median fps |
|---|---|
| baseline (today's tree, 0124-0128) | 45.7 |
| **0129 viejunerd 0014 + 0130 heydemoura #50 + 0131 Orchid buffer-sync skips** | **50.2** |
| 0132 suspended-channel check without drain | 49.7 |
| 0133 skip empty draws (Lichii 4a865a829) | 49.4 |
| half internal resolution (test only) | 51.9 |

Where the time goes at ~50 fps:
- Front GPU thread ~89% busy; its waits on the back went down (0132) without an fps change.
- Back GPU thread ~85% busy, ~4.4 us a draw, spread over buffer binding (~25%: vertex/geometry
  buffers, stage buffers, multi-range storage, written ranges) and pipeline configure (~10%).
- Vulkan worker ~70% idle; present waits ~44 us on the GPU fence; half resolution +5% only: the
  PS5 GPU is not the limit.
- The game's main thread is 58% busy (mostly translated game code), waits 23% on sync objects and
  16% on its workers; presents alternate between 1 and 2 vblanks (frame ~20 ms): the game paces to
  vsync, so its frame has to fit under 16.7 ms for a locked 60.

## GPU command cache ("JIT for GPU"): the measurement

0134 (EDEN_NX2_SEGMENT_STATS=1) hashes every command-list entry the GPU thread parses. Bananza
gameplay: ~250,000 entries and ~14 million words a second; **98% of entries (92% of words) are
word-for-word repeats of an entry parsed in the last ~100 ms**, and only ~4% of entries at a known
address change between frames. The repeat rate the cache needs is there.

But **98% of entries run a macro** (Switch 2 draws go through macros): entries a cache could replay
without running anything (no method open at either end, no macro, no MME DMA read) are 1.8% of
entries and 0.8% of ProcessCommands time. A useful cache has to memoize macro runs (inputs: their
parameters, the registers they read, the memory their DMA reads see; outputs: the methods they
write) and, to pay off, the back's per-draw setup too, with the same kind of validation. Checking
those inputs costs about as much as running today's macro JIT, so the gain is uncertain: a
multi-week design, not an overnight change.

## Driver note (PS5_Vulkan)

radv_ps5_queue_wait_seq waits by polling os_time_get_nano in a loop (seen on the back thread while
it waited for downloads before 0126). Not a limit any more, but a sleeping wait belongs in the
driver.

## Where Bananza's frame goes now (what a locked 60 still needs)

The game paces to vsync and its frame averages ~20 ms (half 16.7 ms, half 33 ms). Its main thread
is 58% busy running translated game code (emulator overhead small), waits 23% on sync objects and
16% on its workers. The GPU threads are close to full (front ~89%, back ~85%). A locked 60 needs
the main thread's work ~20% faster (JIT code quality) and more GPU-thread headroom; the PS5 GPU
itself is not the limit (half resolution: +5%).

## OOT save after the freeze (user data, nothing deleted)

The save the game froze on wrote slot_01 (forest) and a slot_02; with either present the game no
longer boots (slot_02: a guest thread jumps to an unmapped address at boot; slot_01: stuck in the
intro). Both are renamed slot_01.aside and slot_02.aside on the console (slot_00, the 21:23 state,
boots and plays at 45 fps); copies of everything are in save-backups/ (…-20261106-222503-ps5-after-
freeze and …-20261106-212335-ps5). Renaming them back restores the exact files.

## Runners

dkb-console-run.sh <tag> <checkout> [MEASURE] [dev settings]: installs, presses A until two 5 s
windows exceed GAMEPLAY_DRAWS draws a frame (default 1000; 600 for OOT's house), measures, quits
through the emulator. Works for both titles (build the dev build with DEV_TITLE set).
