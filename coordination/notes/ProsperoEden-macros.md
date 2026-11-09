# ProsperoEden-macros (2026-10-31) — PAUSED, measurement not finished

Branch eglantine-macros: c326908 (macro census, committed), merge of eglantine-base 6329cab (ab4b01c).
**Uncommitted in the worktree** (verified exact, not yet measured well enough to commit):
headless/macro_native.h, macro_native_hook.h, macro_native_check.cpp, edits to
headless/CMakeLists.txt (derived macro.cpp and maxwell_3d.cpp hooks), main.cpp (dev settings),
macro_census.h (cheaper off switch).

## What the macros cost (Pro, census run macros0, split off, race windows)
- Switch 2 macros are Turing MME code (shared code RAM, entered at a start address), not the old
  Maxwell MME: Eden's hash-keyed HLE table (GetHLEProgram) never applies. Replacements are matched
  by the exact code words at the start instead (re-checked when the code generation changes).
- ~200,000-400,000 macro calls/s in races. Macros' own time (JIT, excluding the method writes they
  make) 80-230 ms/s; writes plus the draws they start 150-720 ms/s. Own time per call 0.12-1.1 us,
  ~0.1 us of it fixed per-call setup (Execute prelude, JIT state, thunk+lambda per write).
- Hottest by own time: 0x75 const buffer select (~100k calls/s), 0x21 const buffer bind (~70k),
  0x319 vertex formats, 0x8 shader bind, 0x188 shader unbind; then 0x322, 0x181, 0x1e7 (the draw
  macro: its writes are the draws), 0x79, 0x7e.
- GPU-thread PC samples, race (pc_sample=on): split off, macro code 15.5% of the GPU thread
  (runs/macros-off). Split on: 27.7% of the front thread (runs/macros-split-off), mostly the JIT's
  generated code — the macros are the front's biggest single cost.

## Native replacements (uncommitted)
- Five macros above (0x75, 0x21, 0x8, 0x188, 0x319) natively, in MacroEngine::Execute (split off)
  and Maxwell3D::FrontRunMacro (split on). A replacement declines (JIT runs the call) before any
  effect when a parameter is missing or a data RAM index is out of range.
- Exactness: eden-macro-native-check (PC, no GPU needed): 600,000 random/edge calls against
  TuringMacroInterpreter and TuringMacroJit, 0 differences; two of three deliberate bugs caught
  (the third was an equivalent change). In game (runs/macros-check, mme_native=check): 5.0 million
  calls compared with the interpreter, 0 differences.
- Dev settings: mme_native=off|check, mme_census=on, mme_sample=<start>,...

## Measurements so far (not conclusive)
| run | split | native | race windows | GPU cpu us/draw | fps | macro share of samples |
| --- | --- | --- | --- | --- | --- | --- |
| macros-off | off | off | 8 | 14.86 | 29.5 | 15.5% |
| macros-on | off | on | 3 (crash 422 s) | 14.32 | 25.2 | 13.7% (293 samples) |
| macros-split-off | on | off | 6 | 11.24 (front) | 26.8 | 27.7% |
| macros-split-on | on | on | — | startup hang, rerun needed | | |

- Found on the way: the census's off check (function-local static) cost 1.6% of the GPU thread on
  the console; fixed in the worktree (plain variable), same for mme_native's mode.
- Crashes at 419-422 s in 3 of 5 runs (VK out of device memory; SIGSEGV with method 0x3a400040
  from the command stream; SIGSEGV in Fermi2D software blit), including the census run that had no
  replacements: the known late-race memory trouble, not the macros as far as the stacks show.

## To resume
Planned queue (cancelled by the pause, nothing ran): census pair `mme_census=on` with mme_native
off vs on (per-call own time of each replaced start, the direct macro-cost number), then
pc_sample pairs split on (native on / off) and split off (on2 / off2). Build in the worktree is
current (make ns2-dev done after the fixes). Tools: side-session notes folder macroshare.py
(macro share of GPU-thread samples in race windows), mmedis.py (MME disassembler for the census's
"MME code" lines).
- PC: the NVIDIA driver was wedged (vkCreateDevice fails); PC builds need
  PROSPERO_TOOLCHAIN_BIN=$HOME/.local/bin:<dir with gmake -> make> in this environment.
