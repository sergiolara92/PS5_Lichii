# Night of 2026-11-02 → 03: Mario Kart World on the Pro

## Where it stands
- **Races: about 41-43 fps** (3 races, medians 43.0 / 42.0 / 41.9), up from about 33 at the start of
  the night and 38-39 yesterday evening. Textures still look right (frames checked), no crash, no GPU
  fault in the last 9 races.
- Everything is committed on `overnight-60fps` (local only, nothing pushed). Tip: e9ed206.

## What limits the frame now (measured tonight)
- I mapped core 0's samples back to the game's own code (new: 64-bit JIT block map in the dev
  sampler). During a race, core 0 spends 63% of its time in the game's translated code, ~3% in the
  JIT's dispatcher, ~5% in the emulator (mostly game reads of GPU-tracked memory, ~2.5%), and ~23%
  idle, waiting for the game's other job threads.
- The game's code time is **flat**: the hottest block is 0.9%, the hottest 64 KiB is ~15%. No spin
  loop and no single routine to fix.
- All six of the game's job threads run 9-14 ms per frame, and every core idles 25-35% waiting on
  that job graph. Waiting for a core or for a wake-up is a small share.
- This matches the TotK study on the old port: the frame is the game's own code in the JIT, and 60
  fps needs that code to run about 1.4-1.5× faster. The old port's attempts found no such gain:
  64-bit fastmem got no faster, and more cores changed nothing.

## Honest outlook for locked 60 fps
With what is measured, I don't see a route to a locked 60 in races. The remaining known levers are
each a few percent: emulator overhead on the guest cores (~5%), the JIT dispatcher (~3%), and the heap cuts listed in notes/ProsperoEden-heap.md. A real 1.4× would need
deeper JIT work, such as keeping guest registers in host registers across blocks; about 13% of the
hot blocks' instructions are register save/reload. That is a large, risky project. I'd rather plan
it with you than start it unattended.

## Also tonight
- Ported fork fixes (0017-0020, 0032), many split-GPU-thread improvements (0021-0031), and a
  scheduler trace with runnable events (0034/0036). Lazy dense GPU-only regions save ~3 GB in races.
- Stop hang (a stop after a race times out and the restart hangs): the back thread is stuck inside
  the syncpoint signal. 0037/0038 now print which step it is in, so the next hang names the lock.
  Not fixed yet.
- Base-PS5 zip: deliverables/ProsperoEden-race-presets-base-ps5-ef54f78.zip (made before bit 27).

## Decisions for you
1. Keep bit 27 (high CPU class) as the default? I've adopted it. Same memory, +8-10%.
2. Do you want me to start the cross-block register work (large and risky, maybe +10-15%), or
   should I focus on the stop hang and robustness first?

## Console sharing note
Around 01:40-01:50 a MihawkOS session briefly defaulted to the Pro's address (it thought it was the
base PS5). It ran one short helper test and uploaded one 5 KB probe file, and touched nothing of
ProsperoEden's. It is fixed to target the base only. I paused for one race while we checked; every
ProsperoEden run tonight logged the 14.29 GB pool.
