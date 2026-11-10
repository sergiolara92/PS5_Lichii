# Compound integration

This build retains ProsperoEden's Eden backend for Switch 1 games and adds an
experimental portable subset of [Compound](https://github.com/ovsky/compound),
pinned at `9ea3122589f1116838a5f2bef46dc5be9a79cba8`.

The linked Compound code executes its real integer AArch64 interpreter against a
small, self-authored instruction program. Its C interface also tests unsupported
instructions and memory failure paths. The built-in About screen reports the
actual self-test result, check counts and full upstream revision. The startup log
contains the same result under `PROSPERO_COMPOUND`. The test runs once per process;
a pass confirms the tested CPU subset only. A failure does not replace or disable
Eden.

**Switch 2 game launching is unavailable.** Compound is a work in progress. This
integration has no Compound game-loader or graphics path. Floating-point/SIMD,
atomics, a JIT, complete guest OS services and multiple executable modules are
unsupported by this portable subset. Neither Switch 1 nor Switch 2 commercial
titles can launch through Compound here. The existing library still launches
games through Eden; there is no Compound Play button. The OpenGL/Vulkan preference
selects Eden's graphics renderer and is independent of emulator-core diagnostics.

## Building

The complete application uses the existing Linux/WSL PS5 build. From this checkout:

```bash
make
```

`PROSPERO_ENABLE_COMPOUND=ON` is the default and statically links the Compound CPU
library into `eden-headless`. To omit it from a native build:

```bash
PROSPERO_ENABLE_COMPOUND=OFF make
```

The CPU bridge can be built and tested without Eden, console firmware, games,
keys, a Sony SDK or graphics dependencies:

```bash
cmake -S headless/compound -B build/compound-host
cmake --build build/compound-host
ctest --test-dir build/compound-host --output-on-failure
```

The public Payload SDK and patched PS5 graphics stack remain required for the
complete PS5 application; a successful desktop CPU test does not validate the PS5
frontend or a console run. See [Building ProsperoEden](BUILDING.md).

This checkout preserves the inherited `PPSA99009` package identity because the
bundled upstream filesystem helper and validation receipts are exact-title
inputs. It shares ProsperoEden's application and data locations. Do not install it
alongside an existing ProsperoEden under that same identity. A distinct title
requires coordinated changes to those inputs, not just a renamed folder.

## Interfaces and source

- `third_party/compound_portable`: the pinned portable CPU subset and upstream
  license/provenance.
- `headless/compound/compound_ps5.h`: version 1 C interface for capability reporting
  and the real CPU self-test. It exposes no game-launch API.
- `headless/compound_diagnostics.h`: cached frontend result and startup logging.
- `Services::emulator_diagnostic()`: read-only About data, also available to
  launcher packs as `status.emulator_core`.

Compound's source is GPL-3.0-only; Eden and ProsperoEden retain their respective
GPL-3.0-or-later notices. Preserve the corresponding source, build changes and
third-party notices when distributing a combined binary. No firmware, keys or
game files are included.
