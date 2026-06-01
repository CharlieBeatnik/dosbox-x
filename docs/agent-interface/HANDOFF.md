# HANDOFF — Proposal 4.9 (Observability & Trust)

Leave-behind for the next agent continuing the **proposal-4.9** work on the
`agent-interface` branch. For the original Phase-1 development handover see
[`HANDOVER.md`](HANDOVER.md); to *use* the channel see [`USAGE.md`](USAGE.md).
Source proposal:
`X2RE/.claude/notes/dosbox-x-fixes/PROPOSAL_4.9_observability_and_trust.md`.

## What shipped this iteration (4.9.5 — multi-sentinel watches)

`farcall.watch`, `cpu.watch_target`, and `cpu.watch_range` now each hold a
**set** of sentinels instead of a single one, and every emitted event carries
a `which` index naming the sentinel that fired. Built, 104 unit tests pass,
and verified end-to-end (a new OBSTEST phase 6). 4.9.5 complete.

| # | Item | Proves | Status |
|---|------|--------|--------|
| 4.9.5 | multi-sentinel `farcall.watch` / `cpu.watch_target` / `cpu.watch_range` + `which` on events + per-sentinel `hits` | "*which* of N watched targets fired, and how often each?" in one run | ✅ |

### Design / code map

- **State → containers.** The scalar `g_{far,target,range}WatchSeg/Off/Hits/
  Enabled` globals are gone. Each watch is now a `std::vector` of a small
  sentinel struct (`AgentFarSentinel{seg,hits}`,
  `AgentTargetSentinel{seg,off,hits}`, `AgentRangeSentinel{seg,lo,hi,hits}`,
  defined in `src/agent/agent_internal.h`). `armed` == `!empty()`; the
  per-watch total `hits` is the sum across the set.
- **`which` plumbing without touching the ~12 hot-path call sites.** The
  matchers (`AGENT_FarWatchMatches` / `AGENT_TargetWatchMatches` /
  `AGENT_RangeWatchEntry` in `agent.cpp`) now loop over their vector, bump the
  matched sentinel's own counter, and stash the matched index in a file-scope
  `g_{far,target,range}WatchWhich`. The emitter that runs *immediately after*
  (same CPU thread, no intervening code) reads that stash to append
  `"which":<n>`. So the CPU-core hooks in `cpu.cpp`, `callback.cpp`,
  `core_normal/support.h`, and `core_normal/prefix_none.h` are **unchanged**
  (the matcher/emitter signatures didn't change).
- **Handlers (`agent.cpp`).** `handleFarcallWatch` accepts `target_segs:[…]`,
  `handleCpuWatchTarget` accepts `targets:["SEG:OFF",…]` (the cpu.probe string
  form — new `parseSegOffArg` helper), `handleCpuWatchRange` accepts
  `ranges:[{seg,lo,hi},…]`. Each array form **replaces** the whole set; `[]`
  clears. The pre-4.9.5 scalar forms still work and are stored as a
  one-element set, so all existing callers/tests are byte-for-byte compatible.
- **`debug.status` (`agent_observe.cpp`).** Each watch now reports a
  `sentinels` array (`[{seg[,off|lo,hi],hits}, …]`); the array index == the
  event `which`. Top-level `hits` is the total, and the first sentinel's
  scalar fields are still mirrored at top level for back-compat.
- **Events.** `farcall.transfer` / `cpu.transfer` / `cpu.range_enter` gained a
  trailing `"which":<n>` (buffers bumped 192→224). The `#else` (release-build)
  no-op stubs in `include/agent.h` are unchanged (the public signatures of the
  matchers/setters/emitters didn't change).

### Build note (read this — it bit this iteration)

`src/debug/debug.cpp` has grown (via the 4.9 agent hooks) past the COFF
`/JMC` section limit and now fails `C1128: number of sections exceeded
object file format limit` on a **fresh** compile. Prior iterations only ever
relinked a stale `debug.obj` built before it crossed the limit; a
partial-failure build this session invalidated that obj and exposed it. Fixed
by adding `/bigobj` to `debug.cpp`'s `ClCompile` entry in
`vs/dosbox-x.vcxproj` — the compiler-recommended, codegen-neutral fix. (If a
Unix build hits the same wall, `debug.cpp` may need a per-file flag in
`Makefile.am` too — untested here.)

### Tests

- **`tests/agent_observability_tests.cpp`** — 6 new gTests: per-sentinel
  `which` + counts for target / far / range, empty-array-clears, bad-entry
  rejection, and "scalar form is a one-element set". **104 Agent gTests total,
  all pass.** (Gotcha re-learned: bind `status()` to a local `JsonValue`
  before chaining `.get()` — `status().get(...)` dangles the temporary and
  faults.)
- **`tests/agent_live/obstest.asm` + `OBSTEST.COM` (rebuilt)** — Phase 6: two
  NEAR calls to `landmark_msa` / `landmark_msb`; landmark table grew to 11
  entries (`+18`/`+20`). **`tests/agent_live/test_observability.py`** Phase 6
  arms `cpu.watch_target targets=[msa,msb]`, taps '6', and asserts the two
  `cpu.transfer` events carry `which==0` then `which==1`, plus equal nonzero
  per-sentinel `hits` summing to the total. **All 7 phases pass.** (The phase
  may run >once via the '6' key auto-repeating before the slow paste-pump
  releases it — same harness quirk phases 4/5 tolerate — so the assertion
  checks per-sentinel *equality* + `which`, not an exact count.)

## Previously shipped (4.9.4-VGA — framebuffer write coverage)

Closed the last 4.9.4 gap: `mem.watch` now covers the **VGA framebuffer**
(A0000–BFFFF), not just normal RAM, while fixing a latent emulation-corruption
bug. Built, unit-tested, and verified end-to-end against a booted guest.

| # | Item | Proves | Status |
|---|------|--------|--------|
| 4.9.4-VGA | `mem.watch` fires on VGA framebuffer writes (`old:null`) | "*which* instruction writes the framebuffer?" in one run, no latch corruption | ✅ |

### The problem (subtler than "blind spot")

The write hook already fired for VGA writes — the CPU cores reach VGA memory
through `mem_write{b,w,d}_inline` (paging.h), the very functions the hook sits
in, before the page handler is dispatched. But `AGENT_MemWatchNote` then read
the old value back with `mem_readb(lin)`, and **a VGA read loads the plane
latches** (`vga.latch.d = …` in `VGA_Generic_Read_Handler`,
`src/hardware/vga_memory.cpp:319`). So arming `mem.watch` on the framebuffer
corrupted any latched copy the guest's next instruction relied on (mode-X
copies, "fast clears", page-flip tricks). That's why VGA was documented as a
gap — using it there was unsafe, not silent.

### The fix (one file, no VGA-handler edits)

`AGENT_MemWatchNote` now classifies the destination before reading: `bool
oldKnown = memReadIsSideEffectFree(lin)` = `MEM_GetPageHandler(lin>>12)
->getFlags() & PFLAG_READABLE`. Plain host RAM/ROM sets `PFLAG_READABLE`
(direct host read, side-effect-free) and is read back exactly as before. VGA
and MMIO handlers are constructed `PageHandler(PFLAG_NOCODE)` — no
`PFLAG_READABLE` — so for those the hook **skips the read-back**, evaluates the
new-value-only predicate `AGENT_MemWatchMatchNoOld`, and emits with `old:null`.
This catches both the page-handler path and the mapped-host fast path (the hook
is upstream of the TLB write check), without touching any of the ~20 VGA
handler classes. `lin` is treated as physical (mem.watch is a real-mode tool).

- `new` is the raw CPU store value (for planar modes, the byte the program
  wrote before the hardware bit-mask / map-mask / ALU plane expansion).
- `new_eq` / `new_and_mask_eq` work unchanged; `new_ne_old` degrades to
  "match every write" when `old` is unknown (can't prove idempotence).
- Files: `src/agent/agent_observe.cpp` (`memReadIsSideEffectFree`,
  `AGENT_MemWatchMatchNoOld`, reworked `AGENT_MemWatchNote`, `emitMemWrite`
  gains an `oldKnown` arg → `old:null`); `include/agent.h` (decl +
  `#else` no-op). `paging.h` untouched.

## Previously shipped (4.9.4 — `mem.watch` write-intercept, RAM)

The Tier-2 headline item: a real write-intercept (not a value-change poll)
that reports **the instruction that did the store**, the old/new value, with
an address range + access-size + value predicate. Built, unit-tested, and
verified end-to-end against a booted guest.

| # | Command | Proves | Status |
|---|---------|--------|--------|
| 4.9.4 | `mem.watch` / `mem.unwatch` + `mem.write` event | "*which* instruction stamps this value?" in one run | ✅ |

### How it works / code map

- **`include/paging.h`** — the hook lives at the top of `mem_writeb_inline` /
  `mem_writew_inline` / `mem_writed_inline` (the path the CPU cores reach via
  `SaveMb/Mw/Md`, and the path `mem_writeb` and friends funnel into). Guarded
  by `#if C_DEBUG` so it compiles out entirely in release. The fast gate is a
  global `extern bool AGENT_memWatchArmed` — a disarmed watch costs one bool
  load per guest write. On armed, it calls `AGENT_MemWatchNote(addr,val,size)`.
  Forward-declared locally in paging.h (not via an `agent.h` include) to keep
  that very wide header lean.
- **`src/agent/agent_observe.cpp`** — all the mem.watch logic is self-contained
  here:
  - file-scope state (range precomputed to a linear `[linLo,linHi]`, size
    filter, predicate enum/val/mask, hit counter);
  - `AGENT_MemWatchMatch(lin,new,old,size)` — the **pure** predicate (no memory
    read, no event, no counter bump), so the unit tests drive it directly with
    a synthesised old/new pair and no `MemBase`;
  - `AGENT_MemWatchNote` — the hook: cheap range/size pre-filter → read old
    value (`mem_readb/w/d`, a plain read, no recursion) → `AGENT_MemWatchMatch`
    → bump counter + `emitMemWrite`;
  - `handleMemWatch` / `handleMemUnwatch` dispatch handlers;
  - the `watches.mem` block added to `debug.status`.
- **`include/agent.h`** — public decls for `AGENT_memWatchArmed`,
  `AGENT_MemWatchMatch`, `AGENT_MemWatchNote` (+ the `#else` no-op pair; the
  armed bool has no no-op counterpart since its only reader is the C_DEBUG
  paging hook).
- **`src/agent/agent.cpp`** / **`agent_internal.h`** — two dispatch cases +
  two `handle*` prototypes. Old value is read *before* the store, so the event
  carries the true pre-write value; `from_cs:from_ip` comes from
  `DEBUG_GetPrevCS/IP` (the storing instruction's start — heavy-debug only).

### `mem.write` event shape

```json
{"event":"mem.write","seg":2084,"off":27164,"addr":"0824:6A1C","size":2,
 "old":1889,"new":2131,"from_cs":2084,"from_ip":4718,"from":"0824:126E",
 "from_text":"mov word [6a1c],0853"}
```

Predicate forms (`when`, optional, pick one): `{"new_eq":<u32>}` (a value
wider than `size` never matches, so no coincidental-low-byte false positives),
`{"new_ne_old":true}`, `{"new_and_mask_eq":{"mask":<u32>,"value":<u32>}}`.
Range `[lo,hi]` is matched against the write's **starting** real-mode linear
address `(seg<<4)+off`.

### Tests

- **`tests/agent_observability_tests.cpp`** — the RAM `mem.watch` iteration
  added 13 gTests driving the pure predicate `AGENT_MemWatchMatch`
  exhaustively (range edges, size filter, all three predicate forms,
  disarmed→false) plus dispatch arg-validation and `debug.status` reflection.
  **This iteration** added 6 more for `AGENT_MemWatchMatchNoOld` (the
  new-value-only predicate used when `old` is unsampled): scope/size gating,
  `new_eq`, `new_and_mask_eq`, `new_ne_old`→always-match, disarmed→false.
  **98 Agent gTests total, all pass.** The hook's destination classification
  (`MEM_GetPageHandler`/`PFLAG_READABLE`) and the `old:null` event field are
  MemBase-dependent, so that path is covered live (same split as disasm).
- **`tests/agent_live/obstest.asm` + `OBSTEST.COM` (rebuilt)** — Phase 4 is the
  RAM store (`mov word ptr [watch_target],0853h` at `landmark_store`).
  **This iteration** added Phase 5: switch to planar mode 12h, `mov es:[di],al`
  (al=`5Ah`) to `A000:0064` at `landmark_vga_store`, restore text mode. Grew
  the landmark table to 9 entries (`+16` = vga_store). Rebuilt via
  `build_masm.py` (toolchain present on this host: `MASM`/`TASM` env vars set).
- **`tests/agent_live/test_observability.py`** — Phase 4 covers the RAM store;
  **Phase 5** arms `mem.watch A000:0064 size=1 when new_eq=5A` (the value
  predicate filters the mode-set BIOS screen clear, which writes 0), taps '5',
  and asserts the `mem.write` event's `from_cs:from_ip == landmark_vga_store`,
  `new==0x5A`, **`old is None`**, `size==1`, and `debug.status`
  `watches.mem.hits` ≥ 1. **All 6 phases pass** (`VGA store at 0814:018D
  new=0x5a old=null 'mov  es:[di],al'`).

## Previously shipped (Tier 1 + disasm)

Four commands, each built, unit-tested **and** verified end-to-end against a
booted DOS guest with a purpose-built MASM program:

| # | Command | Proves | Status |
|---|---------|--------|--------|
| 4.9.1 | `debug.status` + per-BP/per-watch hit counters + `bytes_now` | "fired or never reached?" in one run | ✅ |
| 4.9.2 | `cpu.probe` (non-halting exec counter) | "is X on the path?" at full speed | ✅ |
| 4.9.3 | `cpu.trace_ring` + `cpu.traceback` | "how did the CPU get here?" in one capture | ✅ |
| 4.9.6 | `cpu.disasm` (structured) | removes unsafe hand-decoding | ✅ |

### Code map

- **`src/agent/agent_observe.cpp`** (new TU) — the four handlers, the probe
  table, the trace ring, and the hot-path hooks `AGENT_ProbeActive/Check`,
  `AGENT_TraceActive/Record` (public symbols at the bottom).
- **`include/agent.h`** — declarations + `#if C_DEBUG`/`#else` no-op pair for
  the four hooks.
- **`src/agent/agent.cpp`** — watch hit counters added to the
  `AGENT_*WatchMatches` matchers (bump on the same edge that fires the event);
  five new dispatch cases; `g_*WatchHits` globals.
- **`src/agent/agent_internal.h`** — externs for watch state + five `handle*`
  prototypes.
- **`src/debug/debug.cpp`** / **`include/debug.h`** — debugger-side bridge:
  `CBreakpoint::hits` (`BumpHits`/`GetHits`, bumped in `CheckBreakpoint` and
  `CheckIntBreakpoint`); `DEBUG_AgentForEachBreakpoint` (friend fn + the
  `AgentBreakpointInfo` struct, read-only BP iteration); `DEBUG_AgentDisasmOne`
  (wraps `DasmI386`). The per-instruction probe/trace hooks fire from
  `DEBUG_HeavyIsBreakpoint`, gated on a fast bool.
  - **Gotcha:** `include/debug.h` has *no* overall include guard (it is
    re-included on purpose — it was all prototypes). The new *types* live
    behind a local guard `DOSBOX_DEBUG_AGENT_TYPES` or you get C2011. Keep any
    future type additions inside that guard.
- **`vs/dosbox-x.vcxproj`** + **`.vcxproj.filters`** — `agent_observe.cpp`
  wired in. **`Makefile.am` was NOT touched** (see Follow-ups).

### Tests

- **`tests/agent_observability_tests.cpp`** — 20 gTests (registered in
  `tests/tests.h`). Probe + trace hooks are pure agent-side state, so these
  drive them directly the way the CPU core would and read back through
  `debug.status`. Anything needing `MemBase` (real disasm bytes, `bytes_now`)
  is only arg-validated here and covered live. **All 79 Agent gTests pass.**
- **`tests/agent_live/obstest.asm`** + **`OBSTEST.COM`** (committed) — MASM
  6.11 program, fixed landmark table at `CS:0103`, phases for
  probe/BP-counters, disasm fodder (never executed), and a distinctive trace
  chain.
- **`tests/agent_live/test_observability.py`** — drives OBSTEST.COM against a
  real `bin/x64/Debug/dosbox-x.exe`. **All 4 phases pass:**
  - phase1a: `cpu.probe` counts exactly 300 loop iterations (non-halting).
  - phase1b: BP `hits` ≥1 and `bytes_now` first byte == `90` (NOP) while
    paused at the BP.
  - phase2: `cpu.disasm` mnemonics in order **and** every instruction's
    reported `bytes` equals `mem.read` of the same address, landing exactly on
    `disasm_end`.
  - phase3: `cpu.traceback` shows `trace_a … trace_end` in order,
    most-recent-last.

### Building the MASM test program

`tests/agent_live/build_masm.py` is the in-repo analogue of X2RE's
`scripts/assemble_masm611.py`, trimmed for single-source tiny `.COM`s. It runs
the DOS toolchain **inside DOSBox-X** (the host is 64-bit Windows and can't
exec the 16-bit binaries). Two stages: `ML.EXE /c` then `TLINK.EXE /t /x`.

```
MASM=...\MASM611   TASM=...\TASM1   DOSBOXX=...\bin\x64\Debug
python tests/agent_live/build_masm.py            # builds OBSTEST.COM
```

**MASM build gotchas (do NOT relitigate — all learned the hard way here):**
- **Inline the build into `[autoexec]`, not a `call <file>.bat`.** Under the
  VS-built DOSBox-X, `call _masmbuild.bat` from autoexec silently did nothing
  (no log, no OBJ); the identical commands inlined into autoexec work. The
  current `build_masm.py` inlines them.
- **Invoke `M:\BIN\ML.EXE` / `T:\TLINK.EXE` by absolute path,** not via
  `PATH=` — PATH resolution was unreliable under this build.
- **No `/AT`.** The sources use full segment definitions (`code segment / org
  100h`), not `.MODEL TINY`; `/AT` then miscomputes label offsets and the
  landmark table comes out as garbage. `TLINK /t` makes the plain OBJ a `.COM`.
- **No `2>>` / `2>&1`** in the in-DOS batch — COMMAND.COM passes the `2` to ML
  as a literal filename ("Assembling: 2" → fatal A1000).
- `M:\BIN\ML.EXE` (protected-mode, needs the present `DOSXNT.EXE`) assembles
  fine under the VS Debug DOSBox-X build. `OBSTEST.COM` is committed so the
  live test runs without a DOS toolchain installed.

### Build / test recipe (this host)

```powershell
# import VS env (see the reference-windows-build memory), then:
msbuild vs\dosbox-x.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:PlatformToolset=v143 -m
```
```bash
# unit tests (quote the filter; XML because Win stdio drops late console writes):
./bin/x64/Debug/dosbox-x.exe -tests "--gtest_filter=Agent*" "--gtest_output=xml:out.xml"
# live-fire:
python tests/agent_live/test_observability.py
```

## Not done / deferred (pick up here, in proposal priority order)

- **4.9.4 and 4.9.5 are now complete.** Next items:
- **4.9.7 `cpu.step` / `cpu.step_over`** — structured single-step returning the
  post-step `regs.get` snapshot. (NOTES' suggested alternative if 4.9.5 had
  ballooned — it didn't, so this is simply the next item.)
- **4.9.8 `state.save` / `state.restore`** — agent entry points into the
  existing savestate subsystem for deterministic, instant iteration.
- **4.9.9 conditional / Nth-hit BP + on-hit command macro** — `bp.set {if, do,
  continue}`. Pairs with the per-BP hit counter already added (`hits==N`
  conditions are now cheap).

### Smaller follow-ups

- **`Makefile.am`** not updated for `agent_observe.cpp` — only the VS project
  was. A Linux/macOS build needs the agent TU list updated; verify on a Unix
  host.
- **`contrib/agent-client/dbxagent.py`** has no typed wrappers for the new
  commands yet (clients use `call("debug.status")` etc.). Add `debug_status()`,
  `cpu_probe()`, `cpu_trace_ring()`, `cpu_traceback()`, `cpu_disasm()` when
  convenient.
- **Heavy-debug only:** `cpu.probe` and `cpu.trace_ring` rely on
  `DEBUG_HeavyIsBreakpoint`, which only runs in a `C_HEAVY_DEBUG` build
  (`vs/config.h` has it on). A plain `C_DEBUG` build compiles them but they
  never count. `debug.status`, `cpu.disasm`, and the BP/watch counters work in
  any `C_DEBUG` build.
- **USAGE.md older sections** — the "Phase 2 roadmap" / "What is not
  capturable" sections may still list `disasm` / `cpu.step` as gaps; trim them
  to point at 4.9.7+ only.

## Acceptance-test mapping (proposal → here)

- 4.9.1/4.9.2 "C01E retrial" → `test_observability.py` phase1a+1b.
- 4.9.3 "traceback at the ARPL" → phase3.
- 4.9.6 hand-decode removal → phase2 (disasm bytes == `mem.read`).
- 4.9.4 "name the stamp site" → phase4 (`mem.write` `from_cs:from_ip` ==
  `landmark_store`, old/new == the known transition).
- 4.9.4-VGA "name the framebuffer writer" → phase5 (`mem.write`
  `from_cs:from_ip` == `landmark_vga_store`, `new==0x5A`, `old==null`).
- 4.9.5 "which of N watched targets fired" → phase6 (two-sentinel
  `cpu.watch_target`: `cpu.transfer` `which==0` for `landmark_msa`, `which==1`
  for `landmark_msb`; per-sentinel `hits` in `debug.status`).
- 4.9.8 acceptance test deferred with that item.

## Environment note

This session hit repeated harness instability (tool results duplicating/
truncating, an occasional PowerShell-classifier outage, large parallel
batches getting cancelled). Process exit codes and *file* contents stayed
authoritative even when stdout echo garbled. If it recurs: keep tool calls
small and sequential, and when in doubt write results to a file and read it
back rather than trusting inline stdout.
