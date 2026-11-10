# jit-quality: faster guest (A64 JIT) code for Bananza on the Pro (2026-11-07)

Worktree ProsperoEden-jit (branch jit-quality, from 7897329 + the full uncommitted state of
ProsperoEden-oot: patches 0124-0134). Own build cache (copied from oot's patched tree, since the .d
series does not replay from scratch in name order: 0127 fails before 0129).

## Where core 0's time goes (dkb-core0-1, tonight's oot build, ~50 fps)
Caveat (07:00): the old runner's A presses kept DK in a monkey dialog loop, so this profile and every
earlier dkb-console-run number measured that dialog, not gameplay. To be redone with the fixed runner.
- core 0: ~38% idle (Idle + its condition_variable wait), ~52% JIT code, ~10% emulator.
- JIT code: 94% in game blocks, ~3% of core 0 in the shared dispatch handlers (the fast dispatch
  table probe `cmp rbx,[r12]` alone ~1.6%: a 16 MiB table, TLB/cache misses).
- GetCNTPCT ~2.1% of core 0, but its samples sit on the `pause` the callback runs for reads < 1.7 us
  apart: it is the game spin-waiting on the counter. Inlining CNTPCT would only spin faster: not a lever.
- Game blocks: flat (hottest 0.8%: memset). In the 150 dumped hot blocks, page-table walk
  instructions take 9.5% of samples, window accesses 2.4%. Access sites (sample weighted): window 54%,
  page table 46%; of all memory instructions 19.6% are SP-based (never windowed unless
  fastmem64_stack=on), so ~26% of sites are demoted pointer accesses.
- Bananza's saved demotion list (/cache/jit/040074A01BF12000.fastmem64-sites2) holds 138,178 sites in
  44,194 blocks (a third of all compiled blocks); MKW's holds 54,669. Blocks with a demoted site carry
  42% of core 0's game-block samples. The list only grows (+~170 a session); this session's own
  refusals were few (faults=174). Backup: coordination/save-backups/fastmem-lists-20261107/.
- MainThread (threadrun.py): ~12 ms a frame not waiting at 50 fps; waits ~3.7 ms on sync objects,
  ~2.9 ms on arbitration (its workers), ~0.5 ms IPC.

## Changes (headless, uncommitted)
- lever-jitregs 9ec00fe merged (B.cond takes NZCV from the store's register; validated there, 220
  memory-check cases). dev-settings jit_nzcv_reuse=off.
- jit_dispatch_table=small: the fast dispatch mask is a run-time value (64K entries, 1 MiB, instead of
  1M, 16 MiB), lever-jitregs' unmeasured d391f2c as a dev setting.
- fastmem64_demote_after=N: a site leaves the window after N faults (default 1 = as before); faults
  before that are served by the site's fallback.

## Measurements
- jq-queue1 stopped after jq-base (old runner, dialog). jr-queue: the same 9 arms with the fixed runner
  (B twice, walk, settle, measure), runs/jr-queue.log.
- Bananza dropped as the test title by the user (07:20); OOT (0400486024dec000, slot_00 Link's house)
  instead, runner GAMEPLAY_DRAWS=600 (walk + camera turns). Queue runs/jo-queue.sh, compared with
  coordination/abcmp.py (house = the fixed 900-1100-draw view before the walk; measured = 90 s of camera).

| run | house fps | measured mean | median | min | draws |
| --- | --- | --- | --- | --- | --- |
| jo-base | 44.2 | 30.9 | 28.1 | 16.6 | 1871 |
| jo-nowin (fastmem64=off) | 37.1 | 27.0 | 26.8 | 15.3 | 1850 |

- Calibration: slower guest memory access (window off) costs OOT 13-16%: OOT is guest-bound, JIT
  gains will show in fps. Guest thread 174 (OptimizeChangeBanc...) is runnable 96-97% of the time,
  thread 100/101 ~70%, MainThread ~22%.
| jo-stack (fastmem64_stack=on) | 43.0 | 30.7 | 29.4 | 17.7 | 1957 |

- Stack window: neutral (house -1.2 fps, camera mean -0.2). 8,235 "partial" refusals (stack regions
  partly mapped) demoted ~8.3k stack sites into the saved list (132,657 -> 140,917; harmless for default
  runs, which never window SP accesses). Not a lever as it stands.
| jo-dsmall (jit_dispatch_table=small) | 43.4 | 29.9 | 26.8 | 17.1 | 1965 |

- Small dispatch table: neutral to slightly worse, within noise (~±1 fps). Not taken.
| jo-nzoff (jit_nzcv_reuse=off) | 41.5 | 27.6 | 24.4 | 15.5 | 1935 |

- NZCV reuse: off loses 2.7 fps in the house (-6%) and 3.3 on the camera mean, more than the ~1%
  lever-jitregs estimated; 62,609 B.cond sites fused in jo-base. To be confirmed by a repeat pair.
| jo-learn (fastmem64_learn=on) | 40.9 | 27.6 | 27.1 | 16.1 | 1954 |
| jo-fresh (fresh list) | 42.5 | 28.8 | 27.4 | 16.0 | 1887 |

- Learn: one session demotes 134,303 sites (the old list had ~132k): the list is not stale. Refusals:
  partial 126,320 of which piece_misaligned in the alias region (0x11) 62,750, piece_scattered 32,651,
  piece_alias_failed 28,652; blocked 13.9k, blocked_write 6.7k. EDEN_GUEST_LAYOUT alias=1106e00000:
  MapPhysicalMemory's region. -> patch 0135.
- jo-fresh vs jo-base: -1.7 house, -2.1 camera mean with the same list size: run-to-run noise is
  ~±2 fps; single runs cannot settle a 5% effect. Repeats needed (NZCV too).

## Patches (eglantine .d, recorded in the build tree's .applied-patches)
- 0135-physical-memory-window-pieces: MapPhysicalMemory allocates per free hole so 16 KiB pieces agree
  with their guest addresses (window on only; EDEN_PHYS_CONGRUENT=0 = before). Package build/pkg-0135.
- 0136-frame-trace-waits-gpu: scheduler-trace records for per-frame critical paths (presenting
  thread, guest wait begin/end with reason and waker, GPU front/back wait zones, Vulkan submit and
  fence, host thread names); headless/CMakeLists.txt KThread hooks; coordination/frametrace.py walks
  each frame back from the present and ranks where its time goes. Package build/pkg-0136.
- Queues: runs/jp-queue.sh (0135: learn, on1, on2, nzoff, on3), runs/jt-trace.sh (trace jt-1 after it).
- Placement (jo-base EDEN_PERF_WORKER): guest cores 0-3 on CPUs 0/2/4/6, GPU front 8, back 10; CPUCore_4/5,
  VulkanWorker, audio, decoders, VSync float on 1/3/5/7/12 (the guest cores' SMT siblings); 9 and 11 unused.
| jo-fresh-stack (fresh list, stack on) | 44.9 | 30.2 | 30.6 | 16.9 | 1911 |
| jo-fresh2 (fresh list) | 41.9 | 28.0 | 25.6 | 16.7 | 1960 |

- No-change runs (base, fresh, fresh2): house 41.9-44.2, camera mean 28.0-30.9; reference means 42.9
  and 29.2. Stack on (twice) and NZCV off (once) are inside that band: none settled by one run.
| jp-learn (0135, learn) | 39.5 | 26.7 | 24.4 | 16.5 | 1929 |

- 0135 learn vs jo-learn: demoted 96,949 vs 134,303 (-28%); partial refusals 90,131 vs 126,320;
  piece_misaligned (alias 0x11) 31,912 vs 62,750; piece_alias_failed 11,345 vs 28,652; but
  piece_scattered 44,270 vs 32,651 (small per-call mappings: each call's loose pages land anywhere).
| jp-on1 (0135, list 97k) | 43.0 | 31.0 | 32.8 | 19.3 | 1887 |

- jp-on1: camera mean +1.8 over the 29.2 reference (+6%), median 32.8, min 19.3; house unchanged. The
  saved list grew 96,949 -> 115,533 in this session (faults 3,010).
| jp-on2 (0135, list 116k) | 41.1 | 27.6 | 23.0 | 17.0 | 1933 |

- 0135 verdict so far: on1+on2 mean house 42.1, camera 29.3 vs reference 42.9 / 29.2: no measurable fps
  gain. The list settles at ~116k sites (-13% vs 134k). jp-on1's +6% was noise. Kept (general, fewer
  demotions, no regression seen) but not a lever; the deeper fix (reserving whole 16 KiB physical
  pieces for small per-call mappings) is not worth building without a predicted gain.
| jp-nzoff (0135, NZCV off) | 43.2 | 28.2 | 24.7 | 16.2 | 1842 |

- NZCV reuse: two off-runs 42.4 / 27.9 vs five on-runs 42.5 / 29.3: house equal, camera +1.4 (~1 standard
  error with ±1.5 fps single-run noise). Weak evidence of a few percent; kept (PC-validated, no harm).
| jp-on3 (0135) | 42.1 | 27.0 | 23.1 | 13.9 | 1954 |

- 0135 over three runs: house 42.1, camera 28.5 vs reference 42.9 / 29.2: no gain (within noise, if
  anything slightly lower). Not a lever. Whether to keep it in the tree is open (fewer demotions vs
  more CPU page tables for pieces); it is off with EDEN_PHYS_CONGRUENT=0.

## Frame trace jt-1 (0136; OOT after the walk, heavy camera view, 140-142.5 s: 69 frames, mean 36.2 ms)
The 1M-record buffer filled in 2.5 s (GPU wait-zone records 569k): enough frames, but shorter than asked.
frametrace.py critical path per frame (walked back from the Presentation Thread's present):

| link | ms a frame | share |
| --- | --- | --- |
| guest code running (many job threads: ModuleSystemWorker1/4/5, MainThread, 152, 156-160, 174) | 20.9 | 57.5% |
| guest thread preempted (runnable, another thread on its core) | 6.4 | 17.6% |
| guest thread woken, waiting for a core | 3.8 | 10.5% |
| GPU front/back busy (guest waiting on them) | 3.7 | 10.1% |
| external (timer sleeps 1.1, audio 0.4) | 1.5 | 4.2% |

- Contention: guest cores 0-2 ~50% busy, 3-5 85-89%. Job worker 158 (core 3) alone is runnable-not-running
  6.9 ms a frame on the path; 95% of that time thread 30 holds core 3. Thread 30: no IPC, ~9 ms a frame
  running, woken ~150x/s by the Presentation Thread (sync). Background threads 174 (OptimizeChangeBank,
  89% runnable, mostly real work between sleeps) and 100 (63%) hold cores 4/5 and delay 159/160/152.
- Job workers run ~22% on their own core whether full (1-3) or SMT-shared (4-5): SMT placement is not
  visibly slowing them.
- Next: core-3 PC samples (is thread 30 polling or working?) and a house-view trace (jt-queue2).
- Effective core sets (jt-1 switches): OptimizeChangeBank 3/4/5 only, thread 100 4/5, thread 30 core 3 only,
  job workers one core each (155/MainThread 0, 156 1, 157 2, 158/152 3, 159 4, 160 5). Cores 0-2 sit 50% idle
  and none of the busy threads ever goes there: the game's affinity masks (Eden's scheduler is the Switch
  kernel's and migrates within masks). The imbalance is the game's design, not an Eden scheduling fault.
- 0136 thinned (rebuilt as build/pkg-0136b): one record per GPU wait, waits under 20 us not recorded.

## Thread 30 settled (jt-core3: PC samples of guest core 3, 0136 package)
- Core 3: 54% of samples in system libraries; 41.8% of those (~22% of core 3) are one stack:
  condition_variable::__do_timed_wait < INx2Vi2Session::WaitForFrame < PresentClientBuffer <
  InvokeRequestTipc. Thread 30 is AM's HLE server thread (RunOnGuestCoreProcess: guest core 3,
  priority 16, above the game's threads). The nx2 graphics sessions (gsv from AM ISelfController 2000,
  and vi2/gpu/syncpt/... it opens) are served there; a present blocks until the previous frame's GPU
  fence (~6.4 ms a call), syncpt 201 ~12 ms a call, on a host condition variable while holding core 3.
  Not a guest spin: an emulator HLE wait on a guest core.
- Patch 0137-nx2-sessions-host-server: those sessions get a ServerManager of their own on a host
  thread (as vi/nvservices/FS); the client waits in the guest kernel. EDEN_NX2_HOST_SERVER=0 = before.
  Predicted: up to the ~6.9 ms a frame worker 158 queued behind thread 30 (jt-1 heavy view), i.e. well
  over 5%. Package build/pkg-0137 (0135 + thinned 0136 + 0137). A/B queue runs/j7-queue.sh (3 pairs +
  a trace with it on).
- jt-2 (meant as the house view; the trace's clock starts at its first event, after boot, so 85 s
  landed after the walk again): 96 heavy frames, 36.0 ms: guest code running 29.6 ms (82%), preempted
  2.1, wait for a core 1.7, timer sleeps 1.8, GPU threads 0.8. Contention varies by moment (28% in
  jt-1, 10% here); guest code speed is the bulk in both.
- 0137 (nx2 sessions on a host-thread server) froze OOT after the walk (~112 s): GPU channels waited on
  semaphores for 2 s, a present's fence "not reached in 4 s", the session ended through the launcher
  (j7-on1; no earlier run has those messages). Cause not found (no deferral, processes resolved from
  the client/application). Shelved: coordination/shelved/jit-quality/0137-nx2-sessions-host-server.diff,
  removed from the tree; j7-on2 reruns it once to confirm.
- 0138-present-waits-in-guest-kernel (replaces it): WaitForFrame waits with KSynchronizationObject::Wait
  on a KEvent the fence action signals, so AM's guest thread leaves core 3 to the game while it waits;
  request order unchanged. EDEN_NX2_PRESENT_GUEST_WAIT=0 = host wait. Package build/pkg-0138.
  Queue runs/j8-queue.sh: j7-on2 (0137 confirm), 3 interleaved 0138 pairs, a trace with it on.
- j7-on2 (0137 again): SIGBUS on GPUBack at 143 s (crash report runs/j7-on2/crash-20261107-100823.txt;
  symbolized against the 0138 elf, so approximate: Nx2VideoDecoder::Render). Two 0137 runs, two
  failures (freeze, crash): moving the nx2 sessions (video decode included) off AM's single thread
  breaks ordering they rely on. 0137 stays shelved. j7-off1 (0137 package, off): house 37.7, camera 27.5.
- j8-on1 (0138 on): stopped itself at 84 s before gameplay: VK_ERROR_INITIALIZATION_FAILED creating a
  texture-cache image (JoinImages -> SlotVector<Vulkan::Image>::insert; report runs/j8-on1/crash-...101125.txt),
  the graphics-memory failure seen overnight. Not obviously 0138's (it changes only the present's wait);
  the off/on pairs that follow tell.
- 0138 paused at the coordinator's request: the cache session's 0160 (ProsperoEden-gcache) fixes the same
  thread-30 wait (KThread::Sleep, fence polled every 100 us); its A/B decides. j8-on1's report: VK image
  creation failed (graphics memory), not the wait itself.

## JIT code speed: predicted gains (OOT, from jt-core3's dumped hot blocks and the frame traces)
Guest code running is 58% (jt-1) to 82% (jt-2) of the frame's critical path, spread over many threads
(no thread above ~5 ms a frame). Only broad JIT speed moves it; each 1% of JIT speed ~0.6-0.8% of a frame.

| item | evidence | predicted frame gain |
| --- | --- | --- |
| condition flags in-block: each cmp packs NZCV (cmc/lahf/seto) and stores it, each csel/cneg/ccmp/cset in the same block loads it back (sahf): a store-forward round trip on the dependency chain | core-3 hot blocks: pack 10.8%, sahf 3.1%, nzcv load/store 2.5% of samples (skid inflates pack); 117 of 117 nzcv loads follow an in-block store; Bananza core 0: 8% | 2-5% (OOT's flag-heavy string compares more, Bananza less) |
| guest GPR traffic at block boundaries (5.5 guest instructions a block, 51% end in b.cond) | 11.6% of hot-block samples (OOT core 3), 18% (Bananza core 0); lever-jitregs: realistic third to half of it | 2-5%, large risky change |
| fastmem coverage | 0135 cut demotions 13%: no measurable gain; page-walk now 0.5% of hot-block samples | <1% |
| dispatcher (fast-dispatch probe) | ~1.6% of core 0; small table measured neutral | <1% |
| NZCV at B.cond terminals (lever-jitregs, merged) | 2 off vs 5 on runs: camera +1.4 fps, ~1 SE | ~0-4% (kept) |

- The hottest OOT core-3 loop (pages 0x80fc7000-0x80fc9fff, 38% of core 3's game samples) is a
  string-keyed binary search (byte compares: ldurb/cmp/cneg/csel/ccmp) on thread 174
  (OptimizeChangeBank, background, cores 3-5); it delays the core-3-5 workers but is not itself on
  the path.
- No single JIT item reaches the 5% bar. The one >=5% guest-side lever found is the present's host wait
  on guest core 3 (0138 / the cache session's 0160).
- Cache session (0159 log): thread 30 = Eden's HLE "am" guest-core service thread (core 3, prio 16).
  PresentClientBuffer also host-sleeps in PaceToVblank (until the next vblank) after WaitForFrame; on
  the PC 44% of CPUCore_3 wall time is am -> PresentClientBuffer -> PaceToVblank. Their 0160 moves both
  waits into the guest scheduler (KThread::Sleep); 0138 covered only WaitForFrame: superseded if 0160's
  A/B (pw-hw-N vs pw-gs-N) holds.
- j8-off1 (0138 package, wait off): normal, house 41.2, camera 26.7, no crash. One clean off vs one
  VK-failure on: inconclusive; 0160 (cache session) owns the present-wait question now.

## 0139-a64-nzcv-forward (built, package build/pkg-0139; A/B runs/j9-queue.sh)
Conditional selects (csel/cset/cinc/cneg, ccmp's NZCV select) take the block's last stored NZCV as an IR
argument (new opcodes EdenConditionalSelect32/64/NZCV, pass EdenA64NZCVForward after get/set
elimination, then a second elimination drops the NZCV stores that only fed them). Off:
dev-settings jit_nzcv_forward=off. Both A/B arms run with EDEN_NX2_PRESENT_GUEST_WAIT=0 (0138 off).
Not memory-checked on a host build (none for this tree): correctness rests on the console runs.

## Block ends: what fewer of them would buy (hot-block samples)
| block-end cost | OOT core 3 | Bananza core 0 |
| --- | --- | --- |
| guest register reload at block start | 6.9% | 2.3% |
| guest register store at block end | 4.7% | 15.8% |
| NZCV load/store | 2.6% | 3.0% |
| halt check on each link | 2.3% | 1.5% |
| total (ceiling, every block end free) | 16.5% | 22.9% |
- Hot blocks end 93% in conditional branches (b.cond 52%, cbz 37%, cbnz/tbnz/tbz 5%); unconditional
  b/bl/ret/blr/br only ~4%: longer blocks through unconditional branches and calls buy <1% of a frame.
- Halt check only on backward links (every cycle has one; forward links need none): ~1% of a frame, cheap.
- Superblocks through conditional branches (fall-through continues, side exits) or linked blocks keeping
  guest registers in host registers: each removes perhaps a third to half of the ceiling on hot paths:
  ~5-8% of JIT cycles, 3-6% of a frame; multi-week, risky (lever-jitregs: fault/exception/SVC exactness,
  shared JIT contract). Neither is a safe >=5% prediction.

## Wake-to-run latency ("woken but waiting for its core"), from jt-1/jt-2 (coordination/wakelat.py, frametrace.py)
- All guest wakes: idle core 4 us median (guest waker) / 10 us (host waker); busy core 20-22 us median.
  Busy-core wakes where the occupant was preempted (pure preemption latency): median 5 us, p90 42-50 us,
  p99 0.2-0.5 ms; where the occupant left by itself (priority order): median 32 us, p99 4-8 ms.
- On the critical path (ms a frame, jt-1 / jt-2): behind thread 30 (am's present waits: the cache
  session's 0160) 3.10 / 0.48; idle-core wake-up 0.30 / 0.42; preemption latency 0.26 / 0.36; behind
  other game threads <=0.3.
- Ceiling without thread 30: ~0.6-0.8 ms of a 36 ms frame, ~2%. Below the 5% bar; not built.
  (Candidates if ever wanted: a short spin before an idle guest core blocks on its host condition
  variable; more frequent halt checks for the preemption tail.)

## JIT efficiency measurement (host cycles per guest instruction): built, queued
- Guest instructions: dev-settings jit_count_insns=on makes every A64 block entry add its instruction
  count to a per-core JIT-state counter (headless/dynarmic/shared-jit.cmake, reg_alloc.cmake, jit_impl.inc).
- Host time: Eden 0140-jit-time-counters: TSC ticks in each core's JIT Run (dev RunThread rewrite in
  headless/CMakeLists.txt) and in the callbacks translated code makes (memory slow path, CNTPCT,
  exceptions/SVC/cache ops), with call counts; block compilation from EDEN_PERF_PROGRESS compile_ns.
- Report: EDEN_PERF_JITEFF per core every 5 s poll; coordination/jiteff.py -> guest insns per frame,
  core cycles per guest instruction in translated code (scaled by the measured clock), callbacks' share.
- Packages build/pkg-eff-oot and build/pkg-eff-dkb (0135, 0136, 0138 in tree but run off, 0139 on, 0140).
  Queue runs/je-queue.sh after the CPU-class runs (jc).

### First result: je-oot (OOT, pkg-eff-oot, jit_count_insns=on; jiteff.py 900-1100)
- Measured core clock (dependent-add chain, 39 polls): median 3,795 MHz, 3,716-3,835. The cores already
  run at the Pro's high frequency (~3.8 GHz) in our "High CPU Frequency" class; the klog's 3200 table is
  a snapshot from before the class applies. (The CPU-class A/B still checks the other classes.)
- Camera windows (gameplay, ~25 fps), per guest core:

| core | M guest insns / frame | cycles / guest insn (translated code) | incl. callbacks | callbacks share of JIT time |
| --- | --- | --- | --- | --- |
| 0 | 6.13 | 9.03 | 9.67 | 6.5% |
| 1 | 6.82 | 8.93 | 9.45 | 5.4% |
| 2 | 5.52 | 9.19 | 9.88 | 6.8% |
| 3 | 13.44 | 5.88 | 6.13 | 4.1% |
| 4 | 27.31 | 4.44 | 4.55 | 2.4% |
| 5 | 29.19 | 4.12 | 4.25 | 3.1% |
| all | 88.4 | 5.51 | | 4.2% |

- Callbacks are the memory slow path (~6-7k calls a frame per core); CNTPCT ~0.1%, SVC/exceptions ~0.1%.
  Block compilation ~0% in gameplay (9-11% in the house windows right after the load). JIT time 134 ms a
  frame summed over the 6 cores.
- House windows (right after the load, compile-heavy): 6.81 cycles/insn overall (cores 0-2 ~9-10).
- The counter itself adds one memory add per block (~5.5 guest insns): roughly +0.2 cycles/insn.
- Reading: 4-9 host cycles per guest instruction, 5.5 on average, is far above ~2: per the agreed rule
  there is a large general JIT excess. JIT code IPC on x86 is ~1.1 (lever-jitregs), so this is ~6 host
  instructions per guest instruction: expansion, not only stalls. Cores 0-2 (MainThread and the
  pointer-chasing job workers) run at ~9; cores 4-5 (OptimizeChangeBank's tight string search and
  workers) at ~4.3. Cause breakdown next (block ends, flags, memory, plain codegen).

### Cause breakdown (coordination/blockmix.py on the dumped hot blocks; interim: OOT core 3, Bananza core 0)
Host main path per block (entry to the terminal; one exit path counted), sample-weighted:

| | OOT core 3 (jt-core3, 5.88 cyc/insn) | Bananza core 0 (dkb-core0-1) |
| --- | --- | --- |
| host insns per guest insn | 7.7 | 7.0 |
| guest-register traffic (block start/end) | 1.05 | 0.96 |
| flags (pack/unpack, NZCV) | 1.03 | 0.34 |
| memory-access path (walk, window checks) | 1.15 (+~0.5 of test/jcc) | 0.85 |
| block end (halt check, link) | 0.47 | 0.14 |
| "compute" | 3.99 | 4.73 |
| cycles per host insn (core 3) | 0.76 (IPC ~1.3): expansion-bound | |

- Core 3's "compute" by opcode: reg-reg mov (copies, zero-extends) 28%, test/cmp + jcc 25%, lea 10%,
  vector 9%, the guest data access itself 7%, cmov/setcc 5%, ALU ~13%.
- Per guest instruction on core 3: real work ~2.3 host instructions; overhead ~5.4 (memory path ~1.6,
  register copies ~1.1, guest-register traffic ~1.05, flags ~1.0, block end ~0.5).
- Guest mix (core 3): loads 29%, flag-setting 17%, branches 16%, ALU 15%, conditional selects 12%, stores
  7%, FP/SIMD 5%. Per guest class (least squares): conditional select ~22 host insns (pre-0139 build),
  load ~10, store ~9, ALU ~7, flag-setting ~6.
- Cores 0-2 (~9 cycles/insn): jb-c0 (PC samples of guest core 0, 0139 build) decides whether their extra
  is more expansion or memory stalls.

## Ultimate goal (user, 2026-11-07): every emulated core below 2 PS5 cycles per guest instruction, all games
Five JIT items, built one at a time behind dev switches, each measured with the cycles-per-instruction
counter (per core) plus OOT/Bananza fps, correctness first:
1. register allocation across blocks/linked jumps; 2. trace/superblock compiler with side exits;
3. flag liveness across traces; 4. cheaper memory accesses (addressing modes, checks dropped where proven,
fastmem coverage); 5. tiered JIT (hot traces re-optimised in the background, e.g. LLVM).
- Starting point (OOT camera): 5.5 cycles/insn average, ~9 on cores 0-2, ~4.3 on cores 4-5, at ~3.8 GHz.
- Conformance harness: the PC host build (make ns2-host) compiles the same derived dynarmic (all headless
  cmake text patches) for Linux; dynarmic's own A64 tests (tests/A64: a64.cpp, fuzz_with_unicorn.cpp,
  misaligned_page_table.cpp, ...) are to be built against it. Host build started 11:06.
- Order: set by the cause breakdown once cores 0-2's samples (jb-c0) are in.

### je-dkb (Bananza, new save, gameplay windows > 2,000 draws, ~53 fps median)
- Core clock median 3,829 MHz. All cores: 19.2 M guest insns a frame, **7.85 cycles/insn** in translated
  code, callbacks 4.2% of JIT time (core 0: CNTPCT 2.1%, the spin seen before), compile ~1-2%.
- Per core (cycles/insn, M insns a frame): 0: 7.42 (4.99), 1: 7.43 (3.19), 2: 7.47 (3.27), 3: 7.58 (2.48),
  4: 10.18 (2.10), 5: 8.04 (3.18). Bananza's code is uniformly ~7.5 cycles/insn; OOT's job workers on
  cores 4-5 are the cheap outlier (tight string search).

### 0139 A/B (3 interleaved pairs, OOT): on 40.8 house / 27.4 camera vs off 41.5 / 28.3: no fps gain (within
noise). To be re-judged with the per-core cycles/insn counter (more sensitive) before it stays on.

### Cause breakdown, slow cores (jb-c0: PC samples of OOT guest core 0, 0139 build; 9.03 cycles/insn measured)
(pc_core=4 is the GPU front thread, not guest core 4: jb-c4 has no guest blocks.)
- 8.72 host insns per guest insn at 1.04 cycles each (IPC ~1): expansion-bound like core 3 (0.76), slightly
  more stall. Blocks are long here (21 guest insns), the guest code FP/SIMD-heavy (fp/simd 25%, loads 27%,
  ALU 22%, stores 11%).
- Per guest instruction: guest-register traffic 1.00, memory path 1.06 (+~0.4 of test/jcc), flags 0.40,
  block end 0.12, "compute" 6.14 = vector/FP 35% (vector register moves ~12%, NaN checks vcmpunordps/ptest
  ~5% + their jcc), reg-reg mov 18%, lea 11%, jcc 11%, ALU 11%, test/cmp 7%, data access 5%.

## The five items: order and predictions (host insns per guest insn removed on cores 0-2; CPI ~1, so ~cycles)
Start 8.7 host insns / 9.0 cycles per guest insn on cores 0-2 (5.5 average, OOT; 7.85 Bananza).

| item | what it removes on cores 0-2 | predicted | notes |
| --- | --- | --- | --- |
| 1 register allocation across blocks/links (+ better in-block allocation) | guest-register traffic 1.0, part of the copies/vector moves (~1.8) | ~1.4 | biggest; prerequisite for 2-3 |
| 4 cheaper memory accesses | walk/checks ~1.5 | ~1.0 | coverage + addressing modes |
| 5 tiered JIT (optimising backend for hot traces) | copies, vector moves, lea/ALU codegen, NaN/flag fallout | ~1.5-2 | the only route to near 2 host insns per guest insn |
| 3 flag liveness | flags 0.4 (core 3: 1.0) | ~0.3 (core 3 ~0.7) | |
| 2 traces/superblocks | block ends 0.12 (core 3: 0.5) and extends 1/3 across b.cond | ~0.2 here, more on short-block code | |
| (extra) inaccurate-NaN FP mode | NaN checks ~0.4 | ~0.3-0.4 | accuracy trade-off, dev switch |

- All together ~4.5-5 of 8.7: ~4 cycles per guest insn on cores 0-2. Below 2 needs code close to one or two
  host instructions per guest instruction: only an optimising tier (item 5) on top of 1-4 gets there.
- Order: 1, 3 (cheap once 1's liveness exists), 4, 2, 5; the inaccurate-NaN mode measured early as a dev switch.
- Conformance: dynarmic_tests (PC host build, Unicorn installed in ~/.local/opt) with the new differential
  test "Eden: differential random code" (0141): random straight-line and control-flow programs, switches
  off vs on, exact state compare. Unicorn's own fuzzers stop one instruction earlier than the test expects
  (Eden's cycle counting), so they cannot be the oracle; the off-vs-on differential test is.

## Item 1/2, step 1: fall-through superblocks (Eden 0142, dev-settings jit_superblocks=N)
- Global pinning (lever-jitregs' reg_pin.cmake, 2 registers in R12/R13) was measured null: each pinned
  read is still a copy and guest-register traffic is spread over ~25 registers. Cross-block allocation
  needs a compilation unit wider than a block: the superblock.
- 0142: a block passes up to N conditional branches as side exits and translates on along the
  fall-through. IR ops A64EdenSideExitCond/IfSet/IfClear; get/set elimination keeps the stores before
  an exit but keeps the values known after it (no reload on the fall-through); the emitter jumps over a
  linked exit with the halt/cycle check (headless/dynarmic/jit_links.inc, jit_side_exits.inc). The
  block-entry instruction counter is corrected on a taken exit.
- Conformance: dynarmic_tests "[eden]" (0141, PC host build): random straight-line and control-flow
  programs (forward branches + a counted loop, run to the end), all switches off vs on, exact state;
  test RNG reproducible with EDEN_DIFF_SEED. 6,000 programs: 0 mismatches (0139 + 0142 on). One earlier
  2,000-program run had one "memory differs" in a straight-line program that did not reproduce (the RNG
  was not seedable then); a 30,000-program hunt is running.
- Harness fixes: A64TestEnv interrupts list restored; SM4 instructions excluded (rip-relative S-box out of
  range on the PC host); trial translations in a child process (Eden's fork asserts on some encodings,
  e.g. half-precision FP); an exclusive monitor for atomics.
- Console A/B queued: runs/js1-queue.sh (OOT, superblocks 0 vs 4, cycles/insn counter, 2 pairs).

## 2026-11-07: register-allocator miscompile found by the differential oracle (fixed)

The 0141 differential test (30000 programs, then 4 x 15000 seeded) found stores going to 0x200 / 0x301 / 0x8300
with NZCV forwarding (0139) on: those are x64 LAHF/SETO flag words. Root cause is not 0139 but our
free-move change in headless/dynarmic/reg_alloc.cmake (since the build-g snapshot): MoveOutOfTheWay moves an
evicted GPR value into any free register, caller-saved ones included. Emitters that place a call argument
first and then call HostCall (Use(vaddr, ABI_PARAM2) + Use(value, XMM1) + EndOfAllocScope + HostCall: the
128-bit callback write, and the 128-bit exclusive read/write that games run through LDXP/STXP-style code)
leave RSI "empty" to the allocator; HostCall's first step, ScratchGpr(RAX), then moved the live value from RAX
into RSI and overwrote the address. 0139 only made it frequent (it keeps NZCV live in RAX across stores).
Fix (reg_alloc.cmake): during HostCall's sweep free moves only target callee-saved RBX/R12/R13, the stack
otherwise (upstream's behaviour). Both reproducer cases now match across all switch settings.
Affects every ProsperoEden build with free moves on (console included) for 128-bit exclusive accesses;
the fix is in the jit-quality worktree only so far.
Test changes (0141): a mismatch prints EDEN_DIFF_CASE <hex line>; EDEN_DIFF_CASE=<line> replays it
(EDEN_DIFF_VERBOSE=1 dumps IR); control-flow programs also keep x28 out of the Rt2 field (ldp wrote the loop
counter, so the run stopped on ticks at a block-boundary-dependent place: a test artifact, not a 0142 bug).

## 2026-11-07: js1 console A/B, 0142 superblocks (jit_superblocks=4 vs 0), OOT camera walk, pkg-0142-oot
| run | all cores cycles/insn | cores 0-2 | core 3 | house cycles/insn | median fps |
|---|---|---|---|---|---|
| off1 | 5.54 | 9.02 / 8.92 / 9.06 | 5.73 | 6.81 | 23.9 |
| on1  | 5.31 | 8.89 / 8.78 / 8.83 | 5.17 | 6.21 | 26.0 |
| off2 | 5.85 | 8.95 / 8.87 / 9.13 | 6.04 | - | 30.0 |
| on2  | 5.27 | 8.92 / 8.68 / 8.85 | - | - | 27.7 |
Superblocks: about -7% cycles/insn over all cores (mostly cores 3-5; cores 0-2 about -2%), no crash; fps noise
larger than the effect. Note the package still had the free-move HostCall bug (fixed since) and 0139 on.

## 2026-11-07: JIT replay bench, first numbers (OOT on the PC, jc-oot3: 200 windows, 1 per s from 170 s)
Capture (0143, EDEN_JIT_CAPTURE) at RunThread entry; every window ends at the thread's next SVC (Env::Stop now
halts the JIT; counts come from AddTicks, `consumed`). Replay (0141 "[bench]") through a page-table JIT set up
like the game's, 20 reps, perf_event_open user-space counts around Run on the PC's i5-10600K (Intel, so
cycles are not the PS5's; host instructions per guest instruction are the same x64 code and deterministic).
All 200 windows end in exactly the captured state, with every switch combination tried (default, nzcv off,
superblocks 4): the bench is also a differential test on real game code.
Windows are short: median 121 guest insns (117 of 200 are the same 121-insn wake-and-SVC window), mean 1390,
max 31034. Sampling is uniform over Run calls, which is what the console's Σcycles/Σinsns counter weighs.
| core | windows | host insns/guest | PC cycles/guest | console cycles/insn (js1-off1) | console host insns/guest (sampler) |
|---|---|---|---|---|---|
| 0 | 75 | 10.65 | 15.0-16.2 | 9.02 | 8.7 (core 0) |
| 1 | 61 | 17.62 | 20.8-21.7 | 8.92 | |
| 2 | 36 | 19.66 | 22.5-24.0 | 9.06 | |
| 3 | 12 | 11.52 | 8.6-9.0 | 5.73 | 7.7 (core 3) |
| 4 | 6 | 10.49 | 9.2-9.8 | ~5.5 | |
| 5 | 10 | 10.30 | 9.3 | ~3.9 | |
| all | 200 | 13.36 | 13.4-14.0 | 5.54 | |
Same ordering as the console (cores 0-2 worst), but higher: the bench counts everything inside Run (entry/exit,
SVC/halt, memory and CNTPCT callbacks) while the console figure excludes callbacks and the sampler counts
translated code only; short windows make the per-Run cost weigh. Next for the bench: per-Run fixed cost
measured on its own (empty window), translated-code-only count, more varied windows (dedupe, longer captures,
Bananza). Switch effects on host insns/guest: 0139 nzcv forward 0.0% (it shortens a dependency chain, not the
count), 0142 superblocks=4 -3.0% (13.36 -> 12.96); cycles noise between runs about 5%.

## 2026-11-07: bench calibration (coordinator: make sb4-vs-default on the bench track the console's ~7%)
Gaps found and closed, in order of size:
1. Memory setup. The game runs dynarmic in Auto: fastmem over the whole space (no range check), the direct
   page table behind it (mask 0), wall-clock CNTPCT and NO cycle counting (multicore). The first bench used
   the masked table + cycle counting: 11.8 -> 5.8 host insns/guest (console mix) once it matches the game.
2. Callbacks: the console's counter excludes them; the bench's Env callbacks (byte loops) cost ~340 host insns
   each. Bench now pauses the counters inside callbacks (code_insns) - small after (1).
3. Warm repetition. Each window repeated back to back hides cache and predictor effects: ~2.9 cycles/guest.
   "[bench-mix]": one JIT per core shared by its windows (as the game's), all windows once per round in capture
   order, fastest of 9 rounds per window: ~6.7 cycles/guest on short windows, repeatable to ~2%.
4. Core balance. Capture now takes windows from the cores in turn (40 per core in jc-oot5); before, cores 0-2
   took 86% of windows while the console runs 63% of its instructions on cores 4-5.
5. Window weighting (open): windows are sampled per Run call and long Runs are cut at the window size (200k).
   Short (SVC-ended) windows alone: 7.40 host insns, 6.7 cycles/guest; with the cut long ones: 4.91, 2.4.
   EDEN_JIT_RUNS (capture log) now measures each core's real share of instructions in long Runs; the bench
   combines the two kinds in that share (EDEN_BENCH_LONG_SHARE, "stratified").
The block-entry counter (console jit_count_insns) is exact with and without superblocks (bench check, +0.00%).
sb4 vs default on the bench, every variant: -2.4 to -3.4% host insns and about -3% cycles (Intel). Console: -7%
with ~4 points of run-to-run noise (two "off" runs differ by 5.6%), on Zen 2.
Instruction mix of the bench's JIT code (pmcjit on [bench-mix], jc-oot3): guest GPR/SP loads+stores 16.7% of
instructions / 23.9% of cycles; guest memory 10.8% / 19.8%; PC store, halt check, NZCV, PC immediates 12% / 12%.
Calibration result (jc-oot8: 240 windows, 40 per core, EDEN_JIT_RUNS long shares from the same run):
| core | long share | default host insns / cycles | sb4 host insns / cycles | console cycles (js1-off1) |
|---|---|---|---|---|
| 0 | 0.35 | 7.22 / 8.46 | 7.14 / 8.86 | 9.02 |
| 1 | 0.003 | 6.53 / 9.93 | 6.49 / 9.92 | 8.92 |
| 2 | 0.025 | 6.62 / 9.03 | 6.48 / 8.86 | 9.06 |
| 3 | 0.99 | 6.88 / 2.76 | 6.10 / 2.42 | 5.73 |
| 4 | 0.986 | 7.25 / 2.86 | 6.12 / 2.45 | 4.55 |
| 5 | 0.944 | 7.43 / 3.08 | 6.22 / 2.65 | 4.22 |
| weighted | | 7.15 / 4.21, 4.61 | 6.27 / 3.91, 4.42 | 5.54 |
sb4 vs default: host insns -12.3% (deterministic); cycles -7.1% and -4.1% in two pairs (PC noise ~5%) against
the console's -7%: the bench tracks the console. The per-core shape matches too (cores 0-2 ~9 cycles, 3-5 ~3).
Use: EDEN_BENCH_DIR=runs/jc-oot8/jitcap EDEN_BENCH_WEIGHTS=6.24,6.99,5.73,14.43,28.44,30.31
EDEN_BENCH_LONG_SHARE=0.35,0.003,0.025,0.99,0.986,0.944 dynarmic_tests "[bench-mix]" ("stratified and weighted").
Warning for the metric: cores 3-5 spend 94-99% of their instructions in Runs of >= 200k guest instructions and
run 40-50x core 0's instruction count on the PC (311 G on core 4 over the capture). These look like polling
loops; instruction-weighted cycles/insn is dominated by them, and speeding them up need not raise fps. Item
choices should be judged on cores 0-2 (the game's work) as well as the weighted figure.
Correction to the warning above: the long Runs on cores 3-5 are real work, not polling. trace-5-17 et al. run an
unrolled byte-by-byte string compare (ldrb/cmp/cneg/csel/cbz/b.ne, the 0139 pattern); every branch ends a block,
which is why superblocks pay most there.
NB: zsh does not word-split "$e": `env $e` with two settings passed one variable (cnt-sb4 above ran without
superblocks); use `env ${=e}`.

## 2026-11-07: item 1 (registers across blocks), first steps on the calibrated bench (jc-oot8, stratified+weighted)
Guest GPR/SP loads+stores are 16.7% of JIT instructions (24% of cycles); flat over registers (x8 1.8%, x9 1.6%,
x0 1.2%, ...), stores > loads (9.6% vs 7.1%): pinning a few registers cannot take much (as lever-jitregs found).
- 0144 dead exit stores (drop a block's final store when every direct successor writes the register first,
  successors translated read-only): correct after fixing in-block re-reads (SetW then GetX reads the state), but
  only 8% of block-end stores are dead (values flow into the next block): -0.15% host insns. SHELVED
  (coordination/shelved/jit-quality/0144-dead-exit-stores.diff).
- 0145 superblock exit stores (stores a side exit needs moved onto its exit path; the fall-through keeps the
  values in host registers; kept only where the main-path store is later replaced): all 240 windows end in
  their captured state with every combination.
| config | host insns/guest (weighted) | cores 0-5 |
|---|---|---|
| superblocks 0 | 7.15 | 7.22 6.53 6.62 6.88 7.25 7.43 |
| superblocks 8 | 6.17 | 7.14 6.49 6.48 6.03 5.98 6.07 |
| superblocks 8 + 0145 | 5.81 | 7.07 6.44 6.44 5.81 5.52 5.56 |
| superblocks 16 + 0145 | 5.70 | 7.06 6.44 6.44 5.76 5.36 5.39 |
(cycles not usable in this round: the PC was loaded by other sessions, 3.8-8.8 between runs of one config.)
Bugs met on the way: Xbyak "label is too far" (EmitCond's short jump over a longer exit path: near now); a capture
ExclusiveMonitor{9} overflow (the monitor holds 6 processors: abort in jc-oot6; fixed).
- 0146 follow short forward unconditional branches inside a block: correct, no gain (5.70 with or without): hot
  code closes loops with backward conditional branches and leaves through calls/returns. SHELVED.
- Mix after superblocks 16 + 0145 (pmcjit on [bench-mix], jc-oot8): ALU 24.8%, register copies 12.1%, guest
  memory 8.1%, lea 6.5%, guest GPR/SP stores 5.9% + loads 3.8%, spills 4.6% (new: longer superblocks keep more
  live), setcc 4.0%, NZCV store 3.1% + load 1.7%, zero-extends 2.3%, movzx 3.7%.
- 0147 (item 3, first try) side exits test the forwarded NZCV value + NZCV stores sunk to exit paths: correct, no
  gain (5.70 -> 5.69; cycles 3.77 vs 3.73 in three pairs): a register move replaces the reload one for one.
  SHELVED (shelved/jit-quality/0147-superblock-exit-flags.diff).
- 0148 (item 3) branch on live host flags: after an add/subtract that set NZCV the host flags still hold it
  (CF = ARM C); conditional side exits and If terminals jump on them while nothing was emitted since (marks, as
  the NZCV-reuse and CheckBit fast paths do). Host insns 5.70 -> 5.53 (-3.0%), every core (cores 0-2 7.06 ->
  6.93); all windows match. jit_flags_fuse=on.
Console js2 (pkg-0145-oot: superblocks 8 + 0145 vs off): off1 5.67 cycles/insn camera, on1 5.01 (-11.6%; house
6.90 -> 5.75, -16.7%); bench predicted -19% host insns for this pair. Median fps noisy (26.1 vs 21.9); guest
insns/frame rose 82 -> 105 M (the faster loops run more).
Console js2 so far: off1 5.67, off2 5.73, on1 5.01 cycles/insn (camera, all cores): superblocks 8 + 0145 about -12%.
Mix with the full stack (superblocks 16 + 0145 + 0148, pmcjit, jc-oot8): mov 40.6% (memory 24.3%, reg-reg 15.0%,
imm 1.3%), movzx 7.0%, lea 6.4%, cmp 4.1%, cmc 3.5%, lahf 2.9%, seto 2.9%, xor 2.8%, sahf 1.7% (was 3.5%).
Next lever (item 3 proper): NZCV materialisation (xor/cmc/lahf/seto ~12% of instructions) happens for every flag
setter whose NZCV is stored or tested; most guest flags die at the next compare. Plan: NZCV liveness (a block's
final SetNZCV, and side exits' needs, dead when the successors set flags before reading them; 0144's successor
scan fits flags far better than GPRs) + forwarded exits (0147's IR) + lazy materialisation in the backend
(skip lahf/seto when every use is a live-flags branch).
Queued: js3 (pkg-0148-oot: superblocks 16 + 0145 + 0148 vs off) after js2.
- 0149 NZCV liveness (drop a block's last NZCV store when every successor sets flags first; If terminals take it
  from live flags or the register): correct, no gain (5.53 -> 5.53): with superblocks 16 only 147 of 2472 blocks
  have a last SetNZCV after their last side exit (26 dead); most successors end undecided. SHELVED.
Console js2 final (pkg-0145-oot, superblocks 8 + 0145 vs off; camera, all cores cycles/insn): off 5.67, 5.73;
on 5.01, 4.84: -13.4% (bench: -19% host insns for the same pair). House: off 6.90, on 5.75, 6.38. Median fps not
better (26.1/24.2 off vs 21.9/23.7 on): the game's frame rate here is not set by guest-core instruction cost alone
(more guest insns per frame ran: 82-86 M off vs 102-105 M on).
JIT-state traffic left with the full stack (pmcjit, share of all JIT instructions): GPR stores 6.4%, GPR loads 3.9%,
NZCV stores 3.6%, vectors 1.2%, PC 0.6%. Flag materialisation (xor/cmc/lahf/seto) ~12%.
State of items: 1 (registers) superblock exit-store sinking (0145) done; dead exit stores (0144) and branch
following (0146) do not pay; what is left is the block-boundary GPR traffic (needs a block-entry register
contract: a real cross-block allocator). 3 (flags) branches on live flags (0148) done; flag forwarding to exits
(0147) and NZCV liveness (0149) do not pay; the materialisation cost needs flags kept as a value in EFLAGS by the
allocator (lazy materialisation), a deeper backend change.
Item 4 (memory), analysis on the bench (full stack): guest memory accesses are already one mov each through
fastmem ([r13+reg]); their cost (~20% of JIT cycles) is cache misses in the game's data. Redundant loads in a
block (same base+offset, no store between): 41 of 12193 reads (0.3%) with superblocks 16, 3 of a just-stored
address: not worth a pass. Address folding: `lea r,[r+imm]` feeding the next [r13+r] access is 3.0% of
instructions (fold imm into the access's displacement); it touches the fastmem/page-reuse/checked-fastmem
emitters (other sessions' code) and the fault path (fallback needs base+imm) for at most ~3% instructions.
Console js3-on1 (pkg-0148-oot, superblocks 16 + 0145 + 0148): 4.97 cycles/insn camera (js2 on: 5.01, 4.84).

## 2026-11-07: item 4, 0180 address-displacement fold (patch range now 0180-0199)
`lea r,[r+imm]` feeding the next fastmem access ([r13+r]) was 3.0% of all JIT instructions. 0180: an IR pass (after
page reuse) gives such an access its base and an A64EdenAddrDisp(imm) before it; the emitter overlay
(headless/dynarmic/addr-fold.cmake) folds imm into [r13 + base + imm]; non-fastmem paths compute base+imm into a
scratch; the fastmem fault path gets a stub (base+imm around the fallback call, stack moved by 8 for the inner
call) as the handler's fake-call target. jit_addr_fold=on.
Bench (full stack): 5.53 -> 5.34 host insns/guest (-3.4%); all windows match, also with fastmem faults forced
(EDEN_BENCH_HOLES=7: 276 pages, =3: 621 pages served only through the page table; the bench now resumes Run
after a demotion halt, as the game's core loop does). First stub version crashed in the fallback (8-byte stack
misalignment), caught by the holes test.
Console js3 (full stack vs off): on 4.97, 4.73; off1 5.65 cycles/insn (camera). js4 (full stack + 0180 vs off)
queued after js3 on pkg-0180-oot.
Where the extra guest instructions go (coordinator's question; camera, M guest insns/frame and cycles/insn per core):
| run | core 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| js3-off1 | 6.76 / 9.16 | 7.64 / 9.02 | 6.46 / 8.89 | 14.70 / 5.92 | 27.72 / 4.65 | 31.34 / 4.17 |
| js3-on1 | 6.21 / 8.77 | 7.31 / 8.39 | 5.86 / 8.63 | 16.60 / 4.80 | 28.59 / 3.91 | 30.12 / 3.74 |
| js3-on2 | 6.96 / 8.79 | 7.95 / 8.59 | 6.57 / 8.61 | 18.57 / 4.76 | 36.35 / 3.72 | 38.97 / 3.51 |
The extra instructions are all on cores 3-5 (the string-compare loops); cores 0-2 run the same ~6-8 M/frame and
gained only ~4% (9.0 -> 8.7 cycles/insn). Cores 3-5 do more of the same work when it is faster (throughput-bound
background or polling work, not the frame's critical path); cores 0-2 (FP/SIMD-heavy game threads) are the
likely limit. Unsafe FP options (InaccurateNaN + ReducedErrorFP) on the bench: core 0 6.94 -> 6.86 host insns
(-1%); but the bench's core 0-2 windows are mostly short wake/SVC stubs, so they may under-represent the
console's FP-heavy main thread. Queued js5 (after js4): 0136 frame trace on/off + PC samples of guest core 3 on/off.

## 2026-11-07: cores 0-2 retarget (coordinator): what core 0's time is
Console PC samples of guest core 0 (jb-c0, 0139 build, OOT camera): only 31% of the samples are in the game's JIT
blocks (13529 of 44319). By address: 40.5% in the JIT code region (31% game blocks + ~9.5% dispatcher/thunks), 27.0%
in the emulator's own C++ (eboot, below 0x10000000: HLE services, kernel emulation, memory callbacks), ~31% in system
libraries (0x11d...-0x125...: kernel waits/locks/memcpy, to be named). So the JIT's code is at most ~40% of core 0's
time: the full JIT stack (-4% on cores 0-2's translated code) moves core 0 by ~1.5%. js6 (PC samples of cores 0 and
1 with the full stack, pkg-0180-oot; ELF saved as build/pkg-0180-oot.llvm-pie.elf for pcsym.py) will name the C++.
PC captures (jc-oot9, cores 0-2, 300 windows, no camera movement): cores 0-1 cost only 4-5 host insns/guest on the
bench vs 8.7 in the console's hot blocks; cores 1-2's real work is short Runs (87 of 100 windows < 1000 guest insns;
core 1 averages ~3200 guest insns per Run). Added stick input to the PC pad file (headless mock_devices.cpp:
"<id> <mask> <ms> lx ly rx ry") and WALK=1 in pc-jitcap.sh (walk + camera turns): jc-oot10 captures heavy views.
js5 frame trace (frametrace.py, heavy frames > 20 ms, OOT camera), full stack vs off (pkg-0180-oot):
| category (ms/frame) | on (js5-trace-on) | off (js5-trace-off) |
|---|---|---|
| frame | 28.88 | 30.77 |
| run (guest threads on the critical path) | 22.64 | 23.06 |
| preempted | 2.80 | 2.74 |
| host busy (GPU back) | 0.46 | 1.76 |
| external sleep (HostTiming) | 1.04 | 1.33 |
| wait for a core behind thread 30 | 1.03 | 1.05 |
Critical-path threads: MainThread ~3.0 ms, ModuleSystemWorker1-5 (job workers) 1.2-3.7 ms each, 150/152/155-160
1-3 ms each. The JIT stack takes ~0.4 ms off "run"; the critical path is ~23 ms of guest threads running. js6 (PC
samples of cores 0/1 with the full stack) splits "run" into JIT code vs C++ (services, scheduler, callbacks).
PC heavy-view capture: jc-oot10 (WALK=1) crashed at ~60 s of walking, SIGSEGV on the GPU thread in
Tegra::DmaPusher::Step() <- DispatchCalls <- Control::Scheduler::ResumePending (core dump via coredumpctl, PID
3403858; texture cache passing its critical size as new areas load). Not JIT code; jc-oot4's segfault (frame ~3120,
textures jumping) is likely the same. For the GPU/command-stream owners. jc-oot11 captures earlier (from 105 s).
!! js5-pc3-on (pkg-0180-oot, full stack superblocks 16 + 0145 + 0148 + 0180, plus pc_sample on guest core 3):
GUEST CRASH at boot (19.8 s): the guest executes data (UnallocatedEncoding exceptions at sequential PCs from
0x114B82A7A4, in the heap alias region), EDEN_CRASH_CODE frames, terminate 2010-0212. The same package and switches
booted fine in js4-on1, js4-on2, js5-trace-on (and js3-on1/2 without 0180): intermittent, or tied to the sampler's
SIGUSR2 on a guest core. Treated as a JIT bug until shown otherwise; js6 stopped. Boot code is not in the bench
(captures are gameplay) and the differential test never folds addresses (callback memory): next, PC runs with the
full stack (dev settings on the PC: tools/run-ns2-host.sh NS2_DEV_SETTINGS) to reproduce, then bisect the switches.

## 2026-11-07 MILESTONE: cores 0-1, what the time is (js6, all JIT switches off, OOT camera walk, heavy frames ~30.8 ms)
coordination/corebreak.py <heap.txt> <elf> [frame_ms]: a guest core's PC samples by activity; library time
attributed to its first own caller. JIT code cache = 0x11d...-0x125... (pccallers.py's 0x12... range missed
half of it; corebreak agrees with guestpc.py's block mapping: 32% vs 31%, 37% vs 34%).
| activity | core 0 | core 1 |
|---|---|---|
| idle (PhysicalCore::Idle + its cond wait) | 58.8% = 18.1 ms/frame | 53.1% = 16.4 |
| game code (JIT) | 32.4% = 10.0 | 36.6% = 11.3 |
| scheduler / sync / locks | 3.7% = 1.15 | 4.9% = 1.50 |
| memory callbacks + GPU write tracking (OnCPUWrite) | 1.9% = 0.57 | 1.4% = 0.45 |
| dynarmic runtime | 0.6% = 0.20 | 0.9% = 0.28 |
| profiler itself | 0.6% = 0.19 | 0.5% = 0.14 |
| SVC / services / nvdrv submits | 0.1% = 0.03 | 0.3% = 0.09 |
The non-JIT ~60% is idle, not services: SVC/HLE, nvdrv and kernel work are < 0.1-0.3 ms/frame. With js5's trace
(critical path 78% "run", hopping between MainThread and ModuleSystemWorker1-5 and threads 150-160), the frame is a
latency-bound chain of guest threads: the cores idle half the time while the chain runs on one at a time. On the
busy part of cores 0-1, game code is ~85%: the speed of the critical-path threads' guest code (FP/SIMD-heavy, ~9
cycles/insn on the console) is what sets fps. The JIT stack so far helps that code only ~4%.
Core 3 (js5-pc3-off): 45% game code; library waits are vi WaitForFrame/PresentClientBuffer (present, 34% of its
library samples) and idle.
Bench note: jc-oot11's 6 "superblock mismatches" were a bench artifact (a superblock compiled for one window kept a
"no code" tail from that window's partial page set; the shared code cache served it to another window). Fixed in
the bench (code reads fall back to every window's pages); all windows of jc-oot8/9/11 match with the full stack.
The console boot crash (js5-pc3-on) is therefore still unexplained.
Heavy-view windows (jc-oot11, WALK=1, cores 0-2, off): core 0 6.22 host insns/guest at 10.8 PC cycles/guest, core 2
6.10 at 15.7: ~1.7-2.6 cycles per host instruction, i.e. memory stalls dominate there (unlike the light windows).

## 2026-11-07: where the critical path's time could come from (coordinator items 1-2)
(1) Walking-capture windows (jc-oot11, cores 0-2, all off), cycles in JIT code (pmcjit; PC loaded, load 15-22):
guest memory accesses 24.3%, guest GPR/SP loads 14.8%, ALU 12.6%, cmp/test 12.0% (stalls charged after loads),
GPR/SP stores 5.7%, SSE/AVX 4.9%, halt check 3.2%, page table ~1% (fastmem works), spills 1.2%, dispatch small.
Unsafe FP (InaccurateNaN + ReducedErrorFP) on these windows: host insns core 0 6.22 -> 6.22, core 1 4.83 -> 4.58,
core 2 6.10 -> 6.10: FP/NaN handling is not a lever here. Memory accesses are cache misses in game data (the JIT
cannot remove them). JIT-removable: block-boundary guest-register traffic (~20% of cycles; a real cross-block
allocator might take half) + halt checks (3%) => at most ~10-13% of the chain's game code, ~2-2.5 ms of 23 ms per
heavy frame. The 1.75x the chain needs for 60 fps cannot come from JIT code quality alone.
(2) Preemption on the critical path (frametrace.py now attributes the core's holders; heavy frames, full stack):
158 <- thread 30 0.68 ms, Presentation Thread <- idle core 0.61 ms (host-side latency: the guest core's host
thread not running), MainThread <- 155 0.23 ms, Presentation Thread <- 155-158 ~0.6 ms (the game's own threads,
by priority). With "wait for a core: behind 30" 1.03 ms, thread 30 (am's HLE present waits, the cache session's
0160) costs ~1.75 ms per heavy frame on the critical path: the largest single non-JIT item. Host latency ~0.6 ms.
External sleep via HostTiming ~1.0 ms (guest timed sleeps).

## 2026-11-07: placement and thread 30 (coordinator items 1a, 2)
Placement now (js6 logs): CPUCore_0-3 pinned to CPUs 0,2,4,6 (physical cores 0-3, CCX 0); GPU front 8, GPU back 10
(CCX 1, siblings 9 and 11 idle); everything else on mask 10aa = CPUs 1,3,5,7 (the SMT siblings of guest cores 0-3)
+ 12: CPUCore_4/5 (always busy: the string-compare loops, plus job workers 153/159/154/160), VulkanWorker,
VulkanPresent, pipeline builders, decoders, services. So the critical-path guest cores 0-2 share their physical cores
with busy threads; the cores notes measured a busy sibling halving compute throughput. Job workers 150-160 are
pinned two per guest core over all six cores (trace: 155/86 core 0, 150/156 core 1, 151/157 core 2, 152/158 core 3,
153/159 core 4, 154/160 core 5), so no guest core is free of critical-path work.
Thread 30 (am, guest core 3): its present cost is the fence wait in nx2:vi2 WaitForFrame (PaceToVblank only
sleeps above 60 fps). My 0138 (guest-kernel KEvent wait there; on by default, my runs had it off with
EDEN_NX2_PRESENT_GUEST_WAIT=0 after one inconclusive VK_ERROR crash) frees core 3 during it. The gcache session's
notes: freeing core 3 (their 0160 variants) let the game run ahead into Eden's Switch 2 semaphore-model stalls.
js7 (queued, all JIT off, frame trace): base vs place (CPUCore_4 -> 7, CPUCore_5 -> 12, other secondaries on 1,3,5:
cores 0-2's siblings for light threads) vs gw (0138 on), two rounds interleaved. Large pages: EDEN_FASTMEM large=0 on
the console (no 2 MB pages behind guest memory): a TLB lever for the payload SDK/memory owners.

### js7-gw1 (0138 on, EDEN_NX2_PRESENT_GUEST_WAIT=1): crash at 127 s
- SIGSEGV on GPUBack in Tegra::MemoryManager::IsFullyMappedRange (eboot+0x271dff2), fault address 0x714a042160,
  rsi = 0x0308e4210a33cb00: a GPU virtual address far outside the 40-bit space, so the page-table index runs off the
  table. A garbage GPU address from the command stream: gcache's predicted failure (freeing core 3 -> semaphore
  mirror / garbage pushbuffer). eden_log was empty (the app restarted), so no "waits on semaphore" tells.
- General robustness point for the GPU owners: IsFullyMappedRange (and its siblings) should reject an address
  beyond the address space instead of indexing the page table with it.

### js7 placement, full frame-interval distribution (ivdist.py: measured windows, present intervals in 60 Hz vblanks)
| run | 16.7 ms | 33 ms | 50 ms | 67 ms+ | mean interval |
|---|---|---|---|---|---|
| base1 | 1.7% | 43.3% | 34.9% | 20.1% | 47.2 ms |
| place1 | 1.2% | 43.7% | 35.4% | 19.7% | 47.2 ms |
| base2 | 1.4% | 60.4% | 22.2% | 16.0% | 43.4 ms |
| place2 | 1.1% | 42.5% | 36.7% | 19.8% | 47.5 ms |
Round 1 identical; base2 is the outlier (a good run), not place2 a bad one. Light frames did not get slower. The
per-window fps sequences follow the runner's path (content), and window phase shifts between runs, so median fps
over 18 windows is noisy by several fps. Frame-trace heavy frames: base 28.98/31.01, place 26.86/30.23 ms (-1.5 avg)
on a 168-frame trace window each; cores 0-2 cycles/insn unchanged. Verdict: no measurable placement effect; not a lever.

### Large pages, bench (EDEN_BENCH_HUGE=1: each captured page's 2 MiB granule made accessible + MADV_HUGEPAGE)
jc-oot11 walking windows, 200 rounds, 3 interleaved pairs, cycles/guest cores 0/1/2 weighted:
4K 3.52/3.73/4.26, 2M 3.63/3.65/3.49: noise (+-10%) larger than any effect. Expected: windows touch few pages and
the fastest round runs TLB-warm. The real measure is pc-oot-tlb.sh (pmctlb page-walk cycles on the guest cores).

### TLB (coordinator's large-page question), PC OOT gameplay, pc-oot-tlb.sh tlb-oot4k (4 KiB guest pages, 120 s)
Guest cores (pmctlb, ~981 G cycles): dTLB load walks 0.1%, store 0.0%, iTLB 0.2% of cycles; memory stalls 3.1%,
L3-miss stalls 0.5%; retiring 41%, front-end 16%, back-end 42%. 2 MiB pages can save at most the walk share,
<0.1 ms on the chain: not a lever, no PayloadSDK change proposed. (2 MiB PC run skipped; the PC-only EDEN_HOST_HUGE
madvise was reverted.)

## 2026-11-07: 0181 blocks follow calls, stubs and returns (item 1, cross-block registers)
Dynamic mix of the walking capture (bench EDEN_BENCH_STEPMIX=1, jc-oot11), per 1000 guest insns:
core 0: bl 21.6, blr 2.0, ret 15.3 (matched to a call 15.0; within 16 insns of it 12.9), br 21.8, b 6.1, taken
conditional 37.9 (backward 20.1); core 2: bl 14.7, ret 8.3, br 16.1, cond taken 59.2 (backward 43.9); core 1 mostly
one long loop. 152 of 169 BR sites are dynamic-linking stubs (adrp x16; ldr x17,[x16,#]; add x16,#; br x17).
0181 (jit_follow_calls=N): a block follows BL into the callee, BR/BLR whose target the translation knows (the stub's
GOT slot, read at compile time: register checked at run time, mismatch leaves to the branch), and RET x30 of a
followed call to its return address (checked unless x30 is known). Followed calls skip the return-stack push (put
back at the call when the block ends before the return). Callee ranges register for invalidation (jit_group.h).
BL alone gave only -1.4% (the callee is usually a stub that ended the block at its BR); with stubs:
| capture (superblocks 16 + 0145 + 0148 + 0180) | follow calls 0 | follow calls 8 |
|---|---|---|
| jc-oot11 walking, core 0 host insns / cycles | 5.56 / 3.63, 3.69 | 4.86 / 3.16, 3.13 |
| jc-oot11 core 1 | 4.10 / 1.80 | 4.07 / 1.76 |
| jc-oot11 core 2 | 5.45 / 4.90, 4.85 | 5.10 / 4.23, 4.12 |
| jc-oot11 stratified + weighted | 4.99 / 3.34 | 4.64 / 2.95 (-7.0% / -12%) |
| jc-oot8 (all cores) s+w | 5.34 / 3.72, 3.75 | 5.11 / 3.39, 3.41 (-4.3% / -9%) |
| jc-oot9 s+w | 5.05 / 3.03, 3.00 | 4.84 / 2.86, 2.84 (-4.2% / -5%) |
All windows end in their captured state. follow 16/32 = follow 8. dynarmic_tests "[eden]": new call/return/stub
programs (leaf, x30 saved/restored, return elsewhere, nested, branch inside, block ending inside the callee, GOT stubs,
GOT slot rewritten between two calls); 0 mismatches in 3000 + 6000 programs; mutations (no x30 check, no GOT check)
give 50 and 29 mismatches attributed to the switch. Test env: a guest write to the code range now reads back.
Prediction for the chain (heavy frame ~23 ms "run", ~78% of the busy time of cores 0-1 is JIT code => ~18 ms):
cores 0-2 host insns -12.6% / -0.7% / -6.4% (avg ~6.6%); the console has given ~0.6-0.7x the bench's host-insn cut
in cycles (js2: bench -19%, console -13.4%) => ~4.5% of 18 ms ~= 0.8 ms; using bench cycles (-12% weighted) => up to
~1.5 ms. Predicted 0.8-1.5 ms per heavy frame. Not measured on the console (full stack off it until the boot crash
is understood).

### 2026-11-07 evening: restarts, 0166, console, cold sweep
- App restarts (~18:29, ~20:4x) killed every background job, setsid included: long jobs now run as systemd user
  units (systemd-run --user --collect --unit=jq-...).
- PC boots, full stack + jit_follow_calls=8 (bl-calls): 2/2 clean, switches confirmed in stdout (boot 3 lost).
- gcache's 0166 (span contiguity) on the PC: jc-0166-1/2 (WALK=1 as jc-oot10, 300 windows) no crash (jc-oot10 crashed
  ~60 s into walking without it). 0166 reverted from the jit tree afterwards.
- Fuzz OOM (48 GB): generator bug. A GOT slot rewritten to a subroutine that calls the stub again = guest infinite
  recursion; with 0181 the loop is all direct links (followed stub, the fork's LinkBlockFast BL), which count no
  ticks, so the JIT pushed stack bytes forever. Real code loops like this only in infinite recursion (the fork's
  LinkBlockFast B/BL already allow unchecked infinite loops). Generator: rewrite targets call nothing. EDEN_DIFF_PRINT_EACH=1
  prints each case first (found it at iteration 7607 of seed 77181).
- Console jcb-1..10 INVALID: gcache's ml-on-2 runner died in the restart and left PPSA99009 running with the lock free;
  dkb refused to launch but had installed pkg-0181-oot over the running title first. dkb-console-run.sh now checks
  procs before installing. jq-jcb2 reruns the 10 boots once the console is idle.
- Cold-cache sweep (EDEN_BENCH_COLD=<MiB> written before each window; jc-oot11 full stack, s+w cycles/guest, quiet
  PC, two passes): warm 3.04/3.28 (calls 8: 2.86/2.91); cold 2 MiB 3.56/3.98; 4 MiB 5.34/5.40; 8 MiB 5.36/3.65;
  16 MiB 4.19/6.78; 64 MiB 5.58/5.53 (noisy). Console/bench ratio is ~1.95 even on compute-bound loops (cores 3-5:
  4-5 vs 2.1-2.7) and ~2.7 on cores 0-2 (9 vs 3.3): ~28% of the console's cores 0-2 JIT cycles look like memory
  latency, ~72% compute. Item 5 needs the compute part 1.67x faster for 1.4x on the console, ~2.5x for the chain's
  1.75x. Prediction for offline LLVM -O2: 1.5-2x on warm traces -> 1.3-1.55x on the console, ~4-5 ms of the ~18 ms
  of JIT time in the ~23 ms chain.

## 2026-11-07 night: item 5 feasibility (second tier), offline compile of the hot blocks
Hot blocks of the walking capture (bench EDEN_BENCH_STEPMIX=1 EDEN_BENCH_HOT=50, full stack + 0181): the top 50 of
1474 compiled blocks carry <=90% of the stepped guest instructions (one 398-insn superblock of core 1's long loop
~59%); their IR uses 99 opcodes (Get/Set, Add64, memory, a few vector/FP ops dominate).
Test (EDEN_BENCH_TIER=50; 0182 + tests/A64/eden_tier.inc): each block's IR (Translate, before dynarmic's passes) as
C, compiled with clang-18 -O2 -march=znver2 (registers as locals -> SSA, memory via the fastmem base, exits write
back changed registers; FP as host IEEE), called by the JIT in place of the block (A64EdenExternal); 41 of 50
translate (rest: exclusive accesses); all 60 windows end in their captured state.
| jc-oot11, host insns / cycles per guest insn | core 0 | core 1 | core 2 | s+w |
|---|---|---|---|---|
| dynarmic (full stack + 0181) | 4.86 / 4.14 | 4.07 / 1.89 | 5.10 / 5.26 | 4.64 / 3.65 |
| clang per block, exits via dispatcher | 5.22 / 3.40 | 3.83 / 1.46 | 6.93 / 4.66 | 5.22 / 3.07 |
| clang per block, chained (tail calls) | 4.95 / 4.08 | 3.54 / 1.47 | 5.49 / 5.08 | 4.59 / 3.42 |
| clang one region (all blocks, registers kept) | 6.52 / - | 3.53 / - | 5.42 / - | 5.09 / - |
(cycles noisy: another session's PC emulator ran at ~4.5 cores; host insns are exact.) Core 1's long loop is the
best case: -13% host insns, ~-22% cycles (1.15x / ~1.25x). Cores 0-2's short blocks gain nothing: crossing into
native code and back costs about what the block saves. The region version shows the cost of a tier that loads every
register at entry. clang's code for the 398-insn block: 1430 x86 insns (3.6 per guest insn); the remaining waste is
the flags (signed compares rebuilt from packed NZCV, ~8%).
Verdict: offline LLVM -O2 beats dynarmic by ~1.15-1.25x on the hottest code and not at all on the short blocks;
far below the 1.4x bar. With ~28% of the console's cycles in memory latency, a ~1.3x compute gain is ~1.2x on the
console (~3 ms of the ~18 ms JIT time), at the cost of LLVM ORC in the payload, executable memory for it and a
compile thread. The JIT route alone does not reach 60.
