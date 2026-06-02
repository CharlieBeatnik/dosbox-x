# HANDOFF — Proposal 4.9 (Observability & Trust)

Leave-behind for the next agent continuing the **proposal-4.9** work on the
`agent-interface` branch. For the original Phase-1 development handover see
[`HANDOVER.md`](HANDOVER.md); to *use* the channel see [`USAGE.md`](USAGE.md).
Source proposal:
`X2RE/.claude/notes/dosbox-x-fixes/PROPOSAL_4.9_observability_and_trust.md`.

## What shipped this iteration (4.9.9 — conditional / Nth-hit BP + on-hit macro)

`bp.set` / `bp.clear`: a conditional breakpoint that pairs an execution address
with an optional **`if` condition** (register / memory word / Nth-hit), an
optional **`do` command macro** (a read-only snapshot captured *atomically at
the trigger instant*, before the emulator advances), and a **`continue`** flag
(run the macro then auto-resume instead of halting). Built, **121 unit tests
pass** (+10), and verified end-to-end (a new OBSTEST **phase 9**, reusing the
phase-1 loop with no `.COM` rebuild). 4.9.9 complete.

| # | Item | Proves | Status |
|---|------|--------|--------|
| 4.9.9 | `bp.set {addr, if, do, continue}` / `bp.clear` → `bp.cond` event | "halt only on the interesting case, snapshot it atomically, optionally don't even stop" | ✅ |

### Design / code map

- **Approach: an agent-side conditional-BP table checked from the heavy-debug
  per-instruction hook** — the *same* slot `cpu.probe` / `cpu.trace_ring` use —
  not a field threaded through `CBreakpoint`. One line added to
  `DEBUG_HeavyIsBreakpoint` (`src/debug/debug.cpp`, right after the watch
  fallback): `if (AGENT_CondBpActive() && AGENT_CondBpCheck(cur_cs, cur_ip,
  prev_cs, prev_ip)) return true;`. Returning true enters the debugger exactly
  as a `CheckBreakpoint` match does. This keeps the debugger core almost
  untouched, is fully introspectable via `debug.status`, and is naturally
  heavy-debug-scoped (a condition-false reach just *doesn't halt this
  instruction* — the per-instruction check is what makes that clean; a non-heavy
  0xCC-trap BP can't be un-halted, so `bp.set` returns `unsupported` there).
- **Everything else lives in `src/agent/agent_cpu.cpp`** (no new TU): the
  condition mini-language parser + evaluator, the macro runner (an allowlist of
  read-only commands — `regs.get`, `mem.read`, `cpu.disasm`, `cpu.traceback`,
  `debug.status` — dispatched to the existing handlers), `handleBpSet` /
  `handleBpClear`, the `g_condBps` table, and the `AGENT_CondBpActive` /
  `AGENT_CondBpCheck` global bridges. Types (`CondBp`, `BpCondition`, …) and the
  externs are in `agent_internal.h`; two dispatch cases in `agent.cpp`; the
  `bp.cond` event and the `cond_breakpoints` section of `debug.status`
  (`agent_observe.cpp`).
- **Condition language** (one comparison, `operand [& mask] op operand`):
  operands are a register, the literal `hits` (this BP's reach count), a number
  (decimal or `0x`-hex), or `[byte|word|dword] [seg:off]` (bare-hex halves, the
  agent's SEG:OFF convention — implemented by reusing `parseHexU32` for memref
  halves while standalone immediates stay C-style decimal/0x). The parser is a
  hand tokenizer + recursive descent; it is **pure** (no CPU state) and
  unit-tested directly. The evaluator reads `reg_e*` (all sub-registers derived
  by mask/shift, no reg_ax/al macros needed), `SegValue`, `reg_flags`, and
  `phys_readb` for memory; comparisons are unsigned.
- **Atomicity / safety.** The macro runs *inside* `AGENT_CondBpCheck` on the CPU
  thread at the trigger, so the snapshot is of the exact pre-instruction state —
  no command-poll round-trip. The allowlist is enforced at `bp.set` time (clear
  error up front) so a macro can never run `cpu.run` / `state.restore` / anything
  that recurses the CPU or mutates run state from the hot path. `bp.cond` is
  emitted via `serverBroadcastLine` (enqueue only — same as `AGENT_EmitBpHit`).

### Reply / event shape

```json
bp.set → {"bp_id":1,"addr":"0824:6F8E","seg":2084,"off":28558,
          "condition":"sp==0x0200","macro_len":2,"continue":false}

bp.cond event → {"event":"bp.cond","bp_id":1,"addr":"0824:6F8E","seg":2084,
   "off":28558,"hits":7,"halted":true,"from":"0824:6F10","from_cs":2084,
   "from_ip":28432,"results":[{"cmd":"regs.get","ok":true,"result":{…}}, …]}
```

`debug.status` gains `cond_breakpoints:[{bp_id,addr,seg,off,condition,macro_len,
continue,hits,fires}]` — `hits` = every reach (the `hits` operand), `fires` =
reaches where the condition held. Errors: `bad_args` (addr / condition / macro /
continue / caps), `unsupported` (non-heavy build), `not_found` (`bp.clear` of a
gone id).

### Tests

- **`tests/agent_observability_tests.cpp`** — **10 new gTests** (parser accepts
  the common forms + rejects malformed; evaluator over immediates/hits and
  registers/mask; `bp.set` arg validation; `bp.set` → `debug.status` reflection;
  `bp.clear` all / by-id / not_found; and the hot-path worker via
  `AGENT_CondBpCheck` directly — counts/fires/halt-vote, condition gating,
  continue=run-and-resume). All headless (parser is pure; the evaluator reads
  reg globals; the worker's macro is empty/`regs.get` so no MemBase). **121
  Agent gTests total, all pass.**
- **`tests/agent_live/test_observability.py` Phase 9** — reuses the phase-1
  bounded loop (`mov cx,iters` / `nop`@loopbody / `loop`), where at the Nth hit
  `cx == iters+1-N`, so a register condition and an Nth-hit condition pick the
  *same* iteration. **9a**: `bp.set if=hits==150 do=regs.get` halts on the 150th
  hit; the `bp.cond` event says `hits==150, halted==true`, and the macro's
  `regs.get` captured `cx==151` (taken before `loop` decrements it — proves the
  atomic-at-trigger snapshot). **9b**: `bp.set if=cx==0x97 continue=true` fires
  once *without halting* (no `state.paused`), and `debug.status` shows
  `hits == iters*fires` — the reach-vs-fire distinction. **All 12 phases pass.**

#### Phase ordering note — read this

Phase 9 runs **before** phase 8 in `run()`, on purpose. See the next section.

## ⚠️ Discovered pre-existing bug: savestate restore → resume crashes (NOT 4.9.9)

While adding phase 9 (which runs right after phase 8 in proposal order) the
live suite crashed. Root-caused to a **pre-existing 4.9.8 / savestate defect**,
not this iteration's code:

- **Repro (zero 4.9.9 surface):** `state.save` → `state.restore` → `cpu.run`,
  then merely keep the CPU running — the emulator dies **~0.5 s later** with an
  access violation. Reproduced with **no** `bp.set`/`bp.clear` and the cond-BP
  hook gated off (`AGENT_CondBpActive()==false`), so 4.9.9 is not involved.
- **Signature:** WER `BEX64`, faulting module `unknown`, near-NULL call (fault
  address `0x8`) **from `ntdll`** — i.e. a Windows callback thread invoking a
  function pointer the restore left stale. Strong suspect: a **MIXER channel
  handler** (or similar device callback) that the savestate serializes/restores
  but does not re-bind to a live function after load.
- **Why it was never caught:** phase 8 was the last phase, and its own
  assertions all complete *while paused* (before its `finally` resumes the CPU);
  the async crash then lands in ignored teardown. Nothing ran after it.
- **Impact:** breaks the proposal's headline 4.9.8+4.9.9 pairing ("restore to a
  snapshot, then arm a conditional BP for the Nth hit"). A restore followed by
  *paused* inspection (regs/mem/step/disasm/status) is fine; restore → `cpu.run`
  is not.
- **Workarounds in place:** phase 9 is ordered before phase 8 so the suite
  passes and 4.9.9 is fully exercised on a healthy machine; `USAGE.md`
  (`state.save`/`state.restore` notes) carries a user-facing warning.
- **Next iteration should fix this** before the restore-and-rerun loop is
  trusted. Start at the mixer/device savestate components (look for a serialized
  callback / `MixerChannel` handler pointer that isn't re-pointed on load), or
  bisect which POD component's load makes a subsequent run crash. A minimal
  repro is trivial to recreate: save, restore, run, wait 1 s.

## Previously shipped (4.9.8 — `state.save` / `state.restore`)

Headless agent entry points into the existing savestate subsystem, so an agent
can snapshot the bug window once and re-run from it instantly — deterministic,
byte-identical iteration instead of the ~25 s re-drive (boot/mount/launch/
navigate) every loop. Built, **111 unit tests pass**, and verified end-to-end (a
new OBSTEST phase 8 that round-trips the full register file). 4.9.8 complete.

| # | Item | Proves | Status |
|---|------|--------|--------|
| 4.9.8 | `state.save` / `state.restore` `{slot}` → headless snapshot/restore (restore returns `{regs, cs_ip, insn}`) | "snapshot here, re-run from here instantly and deterministically" | ✅ |

### Design / code map

- **Reuses `SaveState::instance().save/load/isEmpty/getName`** (`include/dosbox.h`
  / `src/misc/savestates.cpp`) — the same slot-based subsystem the *Capture →
  Save/Load state* menu drives. No new serialization; the headline value is the
  *headless, deterministic* wrapping, not a new format.
- **Handlers in `src/agent/agent_cpu.cpp`** (`handleStateSave` /
  `handleStateRestore`), next to `cpu.step` because they share its "must be
  paused" gate and the `buildStepResult()` reply builder. Dispatch: two cases in
  `agent.cpp`; two prototypes in `agent_internal.h`.
- **Paused gate.** Both require the CPU paused — new bridge
  `bool DEBUG_AgentIsPaused(void)` (`src/debug/debug.cpp`, declared in
  `include/debug.h`) returns the same `debugging` flag `DEBUG_AgentStep` checks.
  The agent dispatch while paused runs inside `DEBUG_Loop` (CPU halted between
  instructions — a safe point to replace whole-machine state). Running →
  `bad_state`, exactly like `cpu.step`.
- **Headless suppression — the crux.** `SaveState::save/load` are UI-coupled: a
  `tinyfd_inputBox` remark prompt on save, `loadstateconfirm` (GUI_Shortcut)
  version/program/memory/machine confirms on load, and `notifyError`
  (`systemmessagebox`) modals — all of which would block the single-threaded
  agent forever. The handlers temporarily set `noremark_save_state=true` (save)
  / `force_load_state=true` (restore), and pin `use_save_file=false` so the
  `slot` argument is always honoured regardless of any user `savefile=` config.
  These three are plain globals (`use_save_file` in sdlmain.cpp; the other two in
  savestates.cpp) — **declared `extern` at *global* scope** at the top of
  agent_cpu.cpp, not inside `namespace agent` (a block-scope `extern` there mints
  `agent::`-mangled symbols → LNK2001; learned the hard way this session).
- **Pre-validation closes the remaining modal paths** the suppression flags
  don't cover: out-of-range slot → `bad_args` (checked *first*, so it is
  unit-testable headless); empty slot on restore → `not_found` (before load()'s
  empty-slot `notifyError`); >1 GB guest memory → `unsupported` (before
  save/load's own modal+return). Save returns void, so success is inferred from
  `!isEmpty(slot)` afterwards (→ `io_error` if the write didn't land).
- **Restore reply reuses `buildStepResult()`** → `{regs, cs_ip, insn}` of the
  restored CPU, plus `slot`/`name`, so the client sees where the machine resumes
  with no follow-up `regs.get`. Save reply is just `{slot, name}` (state
  unchanged). `name` = `getName(slot)` (read-only zip peek; no modal).

### Reply shape

```json
state.save    → {"slot":7,"name":"[Program: OBSTEST] (2026-06-01 14:02)"}
state.restore → {"slot":7,"name":"…","regs":{…},"cs_ip":"0814:0000017D",
                 "insn":{"cs_ip":"0814:017D","bytes":"B8 AA AA","text":"mov ax,0xaaaa"}}
```

Errors: `bad_args` (slot missing / outside [0,99]), `bad_state` (not paused),
`not_found` (restore: empty slot), `unsupported` (>1 GB memory), `io_error`
(save: write didn't land).

### Tests

- **`tests/agent_observability_tests.cpp`** — 4 new gTests. The save/load drive
  the whole subsystem (zip I/O, every device component, MemBase) and need a
  paused CPU, none of which exist in `-tests` mode, so these exercise the two
  pre-paused gates: `bad_args` for an out-of-range/missing slot (validated first)
  and `bad_state` for a valid slot while not paused (never touches SaveState).
  **111 Agent gTests total, all pass.** The real round-trip is covered live.
- **`tests/agent_live/test_observability.py`** — **Phase 8** reuses the existing
  `trace_a` landmark (no OBSTEST.COM rebuild): pause at `trace_a` (chain head,
  `mov ax,AAAA` not yet run), `BPDEL` (breakpoints aren't part of a snapshot),
  `regs.get` snapshot, `state.save 7`, `cpu.step`×3 (ax/bx/dx ← AAAA/BBBB/DDDD,
  CS:IP advances), `state.restore 7`, then assert the restore reply's `cs_ip` ==
  `trace_a`, **all 16 registers == the saved snapshot**, `insn.bytes` == the
  guest memory there, and an independent `regs.get` confirms the live state
  snapped back. **All 10 phases pass.**

### Notes for a future refinement

- **Slot-based, on-disk** (the existing subsystem). Fine for the iteration
  use-case; a future in-memory / named-snapshot variant would be faster and
  isolate from the user's slots, but the proposal asked for "entry points into
  the *existing* subsystem" and this delivers that.
- **The residual modal risk** (genuine disk-I/O failure on save; loading an
  externally-corrupted slot, where a *component's* `setBytes` can call
  `savestatecorrupt()`) is documented in USAGE.md. It cannot occur in the normal
  save→restore cycle. Add a global "suppress savestate modals" flag only if a
  real consumer trips it.

## Previously shipped (4.9.7 — `cpu.step` / `cpu.step_over`)

Structured single-step: `cpu.step` (trace into) and `cpu.step_over` (treat
`CALL`/`INT`/`LOOP`/`REP` as one unit). Each returns the post-step register
snapshot, the new `CS:IP`, and the disassembly of the now-current instruction —
so you can walk a suspect dispatch one instruction at a time and *see* where a
stale pointer sends control, instead of arming a watch and hoping it fires.
Built, **107 unit tests pass**, and verified end-to-end (a new OBSTEST phase 7).
4.9.7 complete.

| # | Item | Proves | Status |
|---|------|--------|--------|
| 4.9.7 | `cpu.step` / `cpu.step_over` → `{regs, cs_ip, insn}` | "where does control go from here, one instruction at a time?" deterministically | ✅ |

### Design / code map

- **The step itself is a debug.cpp bridge** — `int DEBUG_AgentStep(bool over)`
  (`src/debug/debug.cpp`, placed right after `DEBUG_Run`; declared in
  `include/debug.h`). It mirrors the curses **F11** (trace into) and **F10**
  (step over) key handlers exactly, just driven from the agent dispatch instead
  of a keypress. The agent dispatch runs from `AGENT_Poll(true)` inside
  `DEBUG_Loop`'s paused branch — the *same* call-stack depth `DEBUG_CheckKeys`
  runs at — so a direct `DEBUG_Run(1,…)` here nests precisely as the key
  handlers do. Return codes: `0` not paused, `1` stepped one instruction (still
  paused — read regs now), `2` launched an async step-over.
- **Why step-over is asynchronous (and why that's correct).** DOSBox's loop
  model re-reads the global `loop` pointer every `DOSBOX_RunMachine` do-while
  iteration, so curses step-over (and the agent's) doesn't *block*: `StepOver()`
  sets a temporary breakpoint at the return address, `DEBUG_Run(1,false)` runs
  the `CALL` itself and `DOSBOX_SetNormalLoop()`s, and the next loop iteration
  runs the body at full speed until the temp BP fires and re-enters the
  debugger. There is no clean way to make that synchronous without
  re-implementing the machine loop, so the reply is **deferred** — the same
  pattern `screen.capture` already uses.
- **Deferred reply plumbing.** `handleCpuStepOver` (`agent_cpu.cpp`) returns
  `""` (rc==2) so the protocol layer queues no immediate reply, after stashing
  `g_stepOverPending`/`g_stepOverId` (defined in `agent.cpp`). When the temp BP
  fires, `DEBUG_EnableDebugger` (after emitting `debugger.entered`/`state.paused`)
  calls the new `AGENT_OnDebuggerPaused()` (`agent.cpp`), which sends the
  deferred `{regs, cs_ip, insn}` reply on the stashed id and clears the slot.
  If a *different* breakpoint (or a manual `cpu.pause`) settles the pause first,
  the reply is sent there — same as the curses `P` behaviour — so the slot never
  sticks. A second `cpu.step_over` while one is pending returns `busy`.
- **Result builder shared with `regs.get`.** `buildRegs()` (the 16-register
  object, refactored out of `handleRegsGet`) and `buildStepResult()` (`{regs,
  cs_ip, insn}`) live in `agent_cpu.cpp`; the deferred path in `agent.cpp` calls
  `buildStepResult()` too, so the synchronous and deferred replies are
  byte-identical in shape. `insn` reuses the `cpu.disasm` row shape
  (`DEBUG_AgentDisasmOne` + a local `phys_readb` hex dump).
- **The trace-into callback case.** If a single step lands on a DOSBox callback
  trampoline opcode, `DEBUG_Run` returns the callback index; `DEBUG_AgentStep`
  dispatches `CallBack_Handlers[ret]` exactly as `DEBUG_CheckKeys` does, so the
  BIOS/DOS service actually runs. `skipFirstInstruction` (set by `DEBUG_Run`)
  means stepping off a breakpoint doesn't immediately re-trigger it.
- **Dispatch.** Two cases in `agent.cpp`: `cpu.step` → `jsonEncode(handleCpuStep)`
  (always synchronous), `cpu.step_over` → `handleCpuStepOver` directly (may
  return `""`, like `screen.capture`). `include/agent.h` gained
  `AGENT_OnDebuggerPaused` (+ the `#else` no-op).

### Reply shape

```json
{"regs":{"eax":…, "ebx":…, …, "eflags":…},
 "cs_ip":"0814:0000017D",
 "insn":{"cs_ip":"0814:017D","bytes":"BB BB BB","text":"mov bx,0xbbbb"}}
```

`regs` is the exact `regs.get` shape; `cs_ip` is the post-step `%04X:%08X`;
`insn` is the instruction *now at* `CS:IP` (about to execute). Errors:
`bad_state` (not paused), `busy` (a step-over is already in flight).

### Tests

- **`tests/agent_observability_tests.cpp`** — 3 new gTests. The step itself runs
  guest instructions through `DEBUG_Run`, which needs a paused CPU + initialised
  core (MemBase), neither of which exist in `-tests` mode; so these exercise the
  *gate*: `cpu.step` / `cpu.step_over` are routed and cleanly refuse with
  `bad_state` (the CPU is never paused in test mode), and a `cpu.step_over` while
  `g_stepOverPending` is set returns `busy` without clobbering the in-flight id.
  **107 Agent gTests total, all pass.** The real stepping is covered live.
- **`tests/agent_live/obstest.asm` + `OBSTEST.COM` (rebuilt)** — Phase 7: a
  single known NEAR `call near ptr landmark_sub`. Landmark table grew to 14
  entries (`+22` call, `+24` call_ret, `+26` sub). **`test_observability.py`**
  Phase 7 has two checks: **7a** parks a BP at the phase-3 trace chain and
  single-steps the six `mov`/`xchg`/`inc`/`dec` instructions, asserting each
  register transition (eax→AAAA, ebx→BBBB, … after xchg eax↔ebx, inc, dec) and
  that every step's `insn.bytes` equals `mem.read`; **7b** at the known `CALL`
  asserts `cpu.step` lands at `landmark_sub` (descends) while `cpu.step_over`
  lands at `landmark_call_ret` (steps over) — the latter exercising the deferred
  async reply. **All 9 phases pass.**

## Previously shipped (4.9.5 — multi-sentinel watches)

`farcall.watch`, `cpu.watch_target`, and `cpu.watch_range` each hold a **set**
of sentinels instead of a single one, and every emitted event carries a `which`
index naming the sentinel that fired.

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

### Build note (read this — it bit a prior iteration)

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

- **All nine proposal-4.9 items (4.9.1–4.9.9) are now complete.** The most
  valuable next thing is **not** a new proposal item — it is the **savestate
  restore→resume crash** documented in its own section above. Fix that first: it
  is a real, reproducible defect that breaks the 4.9.8+4.9.9 loop the proposal is
  built around.
- After that, the remaining Phase-2 surface from `TASKS.md` § Iteration 7+:
  typed `bp.add`/`bp.list`/`bp.del` (stable handles), `regs.set`, `mem.write`
  (typed store), and mouse input (`mouse.move`/`mouse.click`).

### Notes for whoever does a future step / savestate refinement

- **`cpu.step_over` reason on the temp-BP `bp.hit`.** A step-over of a CALL
  emits a `bp.hit` for its *internal* temporary breakpoint (whatever `bp_index`
  it happens to occupy). Accurate but a touch noisy; a future iteration could
  tag it or suppress it. Not worth it until a consumer complains.
- **Step-over of a routine that never returns** (infinite loop / program exit)
  leaves `g_stepOverPending` set with the CPU running; `cpu.pause` recovers it
  (the pause delivers the deferred reply). Documented in USAGE.md. There's no
  watchdog timeout on the step-over the way `screen.capture` has one — add one
  only if a real consumer hits this.
- **`cpu.step` callback dispatch** mirrors `DEBUG_CheckKeys` (dispatch
  `CallBack_Handlers[ret]` when the stepped opcode is a callback trampoline). If
  a future change makes single-stepping into protected-mode callbacks misbehave,
  that's the line to look at (`DEBUG_AgentStep`, the `ret > 0` branch).

### Smaller follow-ups

- **`Makefile.am`** not updated for `agent_observe.cpp` — only the VS project
  was. A Linux/macOS build needs the agent TU list updated; verify on a Unix
  host.
- **`contrib/agent-client/dbxagent.py`** now has typed wrappers for the 4.9.7
  commands (`cpu_step()`, `cpu_step_over()`), the 4.9.8 commands (`state_save()`,
  `state_restore()`), and the 4.9.9 commands (`bp_set()`, `bp_clear()`), but
  still not for the earlier 4.9 commands — clients use `call("debug.status")`
  etc. Add `debug_status()`, `cpu_probe()`, `cpu_trace_ring()`,
  `cpu_traceback()`, `cpu_disasm()` when convenient.
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
- 4.9.7 "walk a dispatch one instruction at a time" → phase7a (six `cpu.step`s
  through the trace chain with verified per-instruction register transitions)
  and phase7b (`cpu.step` descends into a `CALL` → `landmark_sub`;
  `cpu.step_over` treats it as one unit → `landmark_call_ret`).
- 4.9.8 "snapshot here, re-run from here" → phase8 (`state.save` at `trace_a`,
  step three instructions, `state.restore` — restore reply `cs_ip` == `trace_a`
  and **all 16 registers** == the pre-step snapshot; live `regs.get` confirms).
- 4.9.9 "halt only on the interesting case + atomic on-hit macro" → phase9a
  (`bp.set if=hits==150 do=regs.get` halts on the 150th loop hit; `bp.cond`
  `hits==150, halted==true`; macro `regs.get` captured `cx==151`) and phase9b
  (`bp.set if=cx==0x97 continue=true` fires once without halting;
  `hits==iters*fires` in `debug.status`).

## Environment note

This session hit repeated harness instability (tool results duplicating/
truncating, an occasional PowerShell-classifier outage, large parallel
batches getting cancelled). Process exit codes and *file* contents stayed
authoritative even when stdout echo garbled. If it recurs: keep tool calls
small and sequential, and when in doubt write results to a file and read it
back rather than trusting inline stdout.
