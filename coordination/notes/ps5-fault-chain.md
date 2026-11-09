# ps5-fault-chain: what a register chain points at, recorded at the fault

A fault record says which address was touched and leaves the registers in the seh trace. It does not
say what the code was looking for. This adds one line to that record: a register + offset chain,
walked with every hop checked, printed where the seh trace already prints.

No game-specific code in the tree: the chain comes from an environment variable.

## The two files

| File | Applies at | Touches |
|---|---|---|
| `ps5-fault-chain-wine.patch` | `.deps/wine-ps5/source` | `dlls/ntdll/{ps5_fault_chain.c,ps5_fault_chain.h,exception.c,Makefile.in}` |
| `ps5-fault-chain-tree.patch` | the repo root (`PS5_Proton-payloads`) | `tests/test_ps5_fault_chain.c`, `tools/host-tests.sh` |

The wine patch is a `git format-patch`, authored as Mihawk, ready to join `wine/patches/` as the next
free number (it applies with `git am` as well as `git apply -p1`). Suggested name:
`2421-ntdll-ps5-a-fault-can-record-what-a-register-chain-points-at.patch`.

```bash
# the applied Wine tree a test build uses
cd .deps/wine-ps5/source && git am /path/to/ps5-fault-chain-wine.patch     # or: git apply -p1
# the repo
cd /path/to/PS5_Proton-payloads && git apply -p1 /path/to/ps5-fault-chain-tree.patch
tools/host-tests.sh fault_chain
```

The host test skips itself, with a message, until the wine patch is applied.

## The variable

```
WINE_PS5_FAULT_CHAIN=rdi+0xe0;+0x158;u32:+0;u8:+4;u64:+0x10;u64:+0x30
```

* `reg[+off]` — the start: that register plus the offset (hex or decimal, `+` or `-`).
* `+off` — read 64 bits there; that value becomes the current address.
* `u8:|u32:|u64:+off` — report that width there; the current address does not move.

Up to 8 steps. Register names are the x86_64 ones, lower case, including `rip`. On another
architecture the hook is a no-op and reads nothing.

It prints through the channel the registers already use, so it lands in the wine log:

```
warn:seh:dispatch_exception backtrace: chain: rdi+0xe0=0x1069ad3920;+0x158=0x00000000105b7000;u32+0x0=0x00000000;u8+0x4=0x01;u64+0x10=0x0000000000000008;u64+0x30=0x0000000000000000
```

## What it is for (RE9 demo, run 8)

The fault at `re9demo.exe+0x56A403D` reads a lookup table's bucket array through a null pointer. The
registers say `rax=0, rdx=3`, nothing about the table. The chain above reads, from that same record:

```
this      = rdi                       (the descriptor-pool record)
Q         = *(this + 0xE0)            (the object holding the table)
M         = *(Q + 0x158)              (the table header)
M+0x00    the generation dword
M+0x04    the "usable" byte
M+0x10    the capacity
M+0x30    the bucket array            <-- null at the fault
```

That separates "the storage was never attached on this device" from "it belonged to one of the two
devices the game created and destroyed a second earlier" — and says whether `Q` or `M` are honest
objects at all.

## Limits, stated

* **First-chance dispatch.** It runs where Wine dispatches the exception to the program, before the
  game's own handler. That is the record the review is built on; it is not a post-mortem of where the
  game ended up.
* **Checked, not guarded by a second fault.** Each hop is validated (`MEM_COMMIT`, readable at the
  width, inside one region) before the read. A mapping torn down by another thread between the check
  and the read would still fault; that is accepted for a diagnostic, and the alternative — nested SEH
  inside the dispatcher — is worse.
* **Reads the calling process only**, and only its own address space.
* **No behaviour change when the variable is unset** (the query runs once per access violation).
* Not a general dumper: 8 hops, one line, no dumps, no allocation.
