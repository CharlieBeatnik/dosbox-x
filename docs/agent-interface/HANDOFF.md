# HANDOFF — Proposal 4.9 (Observability & Trust)

Leave-behind for the next agent continuing the **proposal-4.9** work on the
`agent-interface` branch. For the original Phase-1 development handover see
[`HANDOVER.md`](HANDOVER.md); to *use* the channel see [`USAGE.md`](USAGE.md).
Source proposal:
`X2RE/.claude/notes/dosbox-x-fixes/PROPOSAL_4.9_observability_and_trust.md`.

## What shipped this iteration (4.9.4 — `mem.watch` write-intercept)

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

- **`tests/agent_observability_tests.cpp`** — 13 new gTests (now 33 in the
  suite, **92 Agent gTests total, all pass**). The pure predicate
  `AGENT_MemWatchMatch` is exercised exhaustively (range edges, size filter,
  all three predicate forms, disarmed→false); plus dispatch arg-validation and
  `debug.status` reflection. The hook's memory-read + event emission is
  MemBase-dependent, so that path is covered live (same split as disasm).
- **`tests/agent_live/obstest.asm` + `OBSTEST.COM` (rebuilt)** — added Phase 4:
  a single `mov word ptr [watch_target], 0853h` at `landmark_store`, with the
  field starting at `0761h`. Repurposed the two reserved landmark-table slots
  (`+12` = watch_target offset, `+14` = store offset). Rebuilt via
  `build_masm.py` (toolchain present on this host: `MASM`/`TASM` env vars set).
- **`tests/agent_live/test_observability.py`** — Phase 4 arms
  `mem.watch ... when new_eq=0853`, taps '4', and asserts the `mem.write`
  event's `from_cs:from_ip == landmark_store`, `old==0761`, `new==0853`,
  `size==2`, `from_text` contains `mov`, and `debug.status` `watches.mem.hits`
  ≥ 1. **All 5 phases pass** (`stamp at 0814:016E old=0x0761->new=0x0853
  'mov  word [0115],0853'`).

### Known gap carried forward

`mem.watch` hooks the generic `mem_write*` path, which covers normal RAM but
**not the VGA framebuffer** (planar writes go through the VGA page handlers'
`writeb`/`writew`). Hooking those is the remaining piece of the proposal's
4.9.4 (the "VGA blind spot"); see USAGE.md §"BPM on VGA memory".

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

- **4.9.4 VGA sub-item** — `mem.watch` (RAM) shipped this iteration; the one
  remaining piece is hooking the VGA page handlers' `writeb`/`writew` so a
  planar framebuffer write reports its real writer CS:IP (the "VGA blind
  spot"). The RAM hook and event shape are done; this adds the same
  range/predicate check inside the VGA write callbacks and routes through the
  existing `emitMemWrite`.
- **4.9.5 multi-sentinel watches** — let `cpu.watch_target` / `farcall.watch`
  hold a *set* (and `cpu.watch_range` several ranges) with a `which` field on
  events. NOTES flags this ~20 lines (generalize `g_*WatchSeg` to a container).
  The per-watch hit-counter plumbing added here should extend to per-sentinel
  counts.
- **4.9.7 `cpu.step` / `cpu.step_over`** — structured single-step returning the
  post-step `regs.get` snapshot.
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
- 4.9.5 / 4.9.8 acceptance tests deferred with those items.

## Environment note

This session hit repeated harness instability (tool results duplicating/
truncating, an occasional PowerShell-classifier outage, large parallel
batches getting cancelled). Process exit codes and *file* contents stayed
authoritative even when stdout echo garbled. If it recurs: keep tool calls
small and sequential, and when in doubt write results to a file and read it
back rather than trusting inline stdout.
