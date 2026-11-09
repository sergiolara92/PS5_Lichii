# ProsperoEden-cores (2026-10-31)

Thread placement and console build flags, measured on the Pro (dev settings as every run:
gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on), all on 85b0e2e. Race windows =
>1500 draws/frame in the race itself. us/draw = GPU thread CPU time per draw (EDEN_DEV_GPU cpu_ns /
EDEN_DEV_DRAW calls). Analysis: races.py runs/<tag>/heap.txt [-v] (fps, worst, GPU busy, us/draw per window).

**Result: nothing kept; eglantine-cores has no new commit.** One run per change; run-to-run
spread (~15% in us/draw, more in fps) hides anything smaller.

## SMT layout (measured, runs/cores-0, every pair timed)
- The title gets logical CPUs 0-12. Pairs (2n, 2n+1) take ~3.47 ms of shared multiply chains for
  n = 0..5, every other pair ~1.75 ms. Siblings: (0,1) (2,3) (4,5) (6,7) (8,9) (10,11); CPU 12 has
  none the title may use (13 is the system's). AdjacentCpusShareCore's (0,1) vs (0,2) check is right.
- Today: CPUCore_0-3 pinned to 0,2,4,6; GPU to 8; everything else (VulkanWorker, CPUCore_4/5,
  texture decoders, pipeline builders, audio...) on 7,9,10,11,12 (mask 1e80); 1,3,5 idle.

## Runs (race fps mean / worst_ms mean / GPU busy / us per draw)
| run | change | fps | worst | busy | us/draw | end |
| --- | --- | --- | --- | --- | --- | --- |
| cores-0 | baseline | 26.8 | 103 | 0.97 | 16.6 | clean |
| cores-A | 9 (GPU sibling) out of the secondary set (1c80) | 20.6 | 539 | 0.70 | 15.2 | clean |
| cores-Z | -march=znver2 for every target (inject.cmake) | 23.9 | 121 | 0.95 | 17.8 | OOM 414 s |
| cores-L | thin LTO on every Eden lib but core | 21.7 | 110 | 0.95 | 19.4 | OOM 423 s |
| cores-B | Z + secondary 1,3,5,7,10,11,12 (1caa), 9 idle | 1 window | | | | mutex EINVAL 363 s |
| cores-B2 | secondary 1caa, 9 idle (no Z) | 28.1 | 69* | 0.96 | 14.8 | clean |

- A: an idle sibling makes the GPU thread ~8% cheaper per draw, but four CPUs are too few for the
  other threads: the GPU thread then waits for work (busy 0.70) and hitches. The secondary set's
  size is the binding constraint, not SMT contention on the GPU core.
- B2 vs cores-0, races aligned by draws/frame: draws/s +2%, us/draw 14.8 vs 15.6 (cheaper in the
  heaviest windows, dearer later), *worst_ms only looks better because B2's race-start hitch fell
  in a window just under 1500 draws/frame. Within noise: not kept.
- B (on Z) crashed in TextureDecoder, "mutex lock failed: Invalid argument", seen in no other run;
  B2 (same placement) ran clean, so probably a timing-dependent lifetime bug, not placement.
- Z, L: no lower per-draw cost in matched windows. Not kept. (video_core already has
  -flto=thin -march=znver2; core cannot take LTO, see headless/CMakeLists.txt.)

## Not done / for others
- GPUBack (split on) placement not measured: needs eglantine-base 6329cab. Given A, pinning it to
  a core of its own (and idling its sibling) shrinks the secondary set further; expect it to only
  pay off together with a bigger secondary set (e.g. 1caa-style, guest siblings shared).
- RADV (libvulkan_radeon.ps5.a) is built `-Dbuildtype=debugoptimized` (-O2 -g) with no -march
  (mihawk-vulkan-review/tools/build-radv.sh). Its vkCmd* recording runs on the emulator's threads;
  a release/-O3 driver build is the coordinator's call.

## GPU thread's SMT sibling idle (2026-11-02, overnight-60fps)
Dev setting secondary_mask=<hex> (performance.cpp PlaceSecondary) overrides the secondary CPUs.
Race, split off, 4 interleaved pairs, same build:
| mask | fps | worst_ms mean | us/draw |
| --- | --- | --- | --- |
| 1e80 (7,9,10,11,12: GPU sibling 9 shared) | 23.1 / 23.2 / 24.5 / 28.6 | 103-113 (77) | 14.8-16.3 |
| 1caa (1,3,5,7,10,11,12: 9 idle) | 30.3 / 28.9 / crash / 29.9 | 57-73 | 12.9-13.3 |
+20% fps, -17% GPU-thread cost per draw, worst frames ~40% shorter. cores-B2 had shown the
direction (14.8 vs 15.6 us/draw) with one run each, inside the noise then. Now the default:
secondary threads take every CPU but the pinned ones and the GPU thread's whole core.
(place-b-3 crashed at 328 s: the TextureDecoder mutex crash, see gpu-replay.md.)
- Split on with its back pinned to CPU 10 (11 idle, secondary 10aa; dev settings back_cpu=10
  secondary_mask=10aa) against split off on the new default, 3 pairs: draws/s 68.4k / 63.4k /
  crash against 68.7k / crash / 67.6k. The front drops to 9.9 us per draw at 0.65-0.69 busy and the
  race does not draw faster: the back is the limit at the old cost. Not kept.

## Split GPU thread's back on a core of its own, again (2026-11-02 evening, after 0022-0026)
With the front's drains cut, the back pinned to CPU 10 (11 idle, secondary 10aa) races at
83.5k / 83.2k draws/s, 36.8 / 37.9 fps (rp-1/2) against 72.5k / 73.0k, 35.7 / 35.8 fps with the
back among the secondary threads (rc-1/2). Now the default (performance.cpp PlaceSecondary): with
the split on, the back takes the last core both of whose CPUs are secondary; back_cpu= and
secondary_mask= still override.
