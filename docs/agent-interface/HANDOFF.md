# HANDOFF — Proposal 4.9 (Observability & Trust)

Leave-behind for the next agent continuing the **proposal-4.9** work on the
`agent-interface` branch. For the original Phase-1 development handover see
[`HANDOVER.md`](HANDOVER.md); to *use* the channel see [`USAGE.md`](USAGE.md).
Source proposal:
`X2RE/.claude/notes/dosbox-x-fixes/PROPOSAL_4.9_observability_and_trust.md`.

---

## Proposal 4.10 — value-landed `mem.watch` + register-indexed BP conditions (shipped)

Source: `X2RE/.claude/notes/dosbox-x-fixes/PROPOSAL_4.10_value_landed_watch_and_indexed_conditions.md`.
Two small generalizations of existing machinery, each turning an N-run /
static-analysis detour into a one-run measurement. **Built (0 errors); 150 Agent
gTests pass (+9); live `test_observability.py` 18/18 (3 new scenarios).**

| Feature | What | Proves |
|---------|------|--------|
| **A** `mem.watch when:{becomes_eq:V}` + `value_size` | value-landed predicate: fire once per rising edge when the `value_size`-byte unit at `seg:lo` *becomes* V by ANY overlapping store (byte-wise / partial / `rep` / block start-below-`lo`) | names the writer a byte-at-a-time populate hides from `new_eq` (the iter-20 false zero) |
| **B** `bp.set if "[reg±disp]"` + optional DS seg | the `[seg:off]` operand offset may be `reg ['+'/'-' disp]`, segment optional (default DS) | halt at `call word [si+04]` only when the node about to be called has handler X |

### Code map (4.10)

- **Feature A — `src/agent/agent_observe.cpp` only.** New enum `MW_PRED_BECOMES_EQ`;
  state `g_memWatchValueSize` + `g_memWatchLastUnit` (seeded with the 33-bit
  sentinel `MEMWATCH_UNIT_SENTINEL` so the first landing always edges).
  `memWatchInScope` gains a **interval-overlap** branch for becomes_eq (store
  `[lin,lin+size)` vs unit `[linLo,linLo+value_size)`), ignoring the access-size
  filter. The hot path `AGENT_MemWatchNote` runs *before* the store, so it reads
  the pre-store unit and **merges** the store bytes over it (`mergeStoreIntoUnit`,
  pure) to get the settled unit — no after-store re-read needed; fires iff
  `post==V && lastUnit!=V`. `emitMemWrite` gained an explicit `valueMask` arg so
  becomes_eq reports old/new at the *unit* width while `size` stays the store's
  width; `addr` is the unit (seg:lo). becomes_eq is a RAM tool (gated on
  `memReadIsSideEffectFree` — no VGA/MMIO units). Pure test hooks
  `AGENT_MemWatchUnitOverlap` / `AGENT_MemWatchMergeUnit` in `include/agent.h`.
- **Feature B — `src/agent/agent_cpu.cpp` + `agent_internal.h`.** `BpOperand`
  gains `int32_t memDisp`. Tokenizer gains `BPT_PLUS`/`BPT_MINUS`. `bpParseOperand`
  memref branch rewritten: optional `segreg:` (default DS via a `BpValSrc{REG,
  BPREG_DS}`), offset base (reg or literal), optional `±` bare-hex disp.
  `bpEvalOperand` computes `off = (base + disp) & 0xFFFF`. Registers in both
  halves were *already* accepted (4.9.9) — the proposal's "regs not accepted"
  claim was stale; the genuinely-new bits are the disp and the default segment.
  Note: `[ss]` (no colon) now parses as `[ds:ss]` — the old reject test for it
  was updated.

### Live coverage (4.10)

`tests/agent_live/obstest.asm` rebuilt (MASM 6.11 + TLINK; `build_masm.py`) with
2 new keystrokes and 5 new landmark-table entries (table now 19 u16 at CS:0103):

- key **'8'** scenario 8: settle `bw_field` to 0x0853 a byte at a time →
  scenario12a (becomes_eq names `landmark_bw_hi`, old=0x0053→new=0x0853 size=1)
  and scenario12b (the old `new_eq size=2` sees **0 events**, the false zero).
- key **'9'** scenario 9: `call word ptr [si+04]` walker over a 3-node table →
  scenario13 (`bp.set word [si+04]==<hdl_bad>` halts at node1, SI=node1).

The driver unpacks `<19H>`; rebuild OBSTEST.COM via `build_masm.py` (needs the
`MASM`/`TASM` env roots) before running the live test if the .asm changes.

---

## What shipped this iteration (typed breakpoints: `bp.add` / `bp.list` / `bp.del`)

The typed replacement for the `debugger.command "BP …"` / "BPINT …" / "BPDEL"
strings — real CPU-halting breakpoints addressed by a **stable handle**, closing
the longest-standing rough edge (`bp_index` was an iteration position, not a
handle). Built (0 errors), **142 Agent gTests pass** (+8), and verified
end-to-end (a new live **phase 11**, no OBSTEST rebuild). All Phase-2 breakpoint
surface is now typed.

| Item | Proves | Status |
|------|--------|--------|
| `bp.add {addr}` / `{kind:"int",int,ah?,al?}` → `{bp_id, kind, …}` | "set a breakpoint and get a handle that survives other edits" | ✅ |
| `bp.list` → `{count, breakpoints:[{bp_id, index, kind, …}]}` | "what is armed, by stable handle" | ✅ |
| `bp.del {bp_id}` / `{all:true}` → `{deleted, remaining}` | "remove exactly this one by handle, or all" | ✅ |
| `bp.hit` event now carries `bp_id` (alongside `bp_index`) | "correlate a hit with the handle I created" | ✅ |

### Design / code map

- **Stable handle = a monotonic id on `CBreakpoint`.** `src/debug/debug.cpp`:
  the class gains a `uint32_t bpId` assigned at construction from a process-wide
  `static uint32_t nextBpId` (`GetBpId()`), so *every* breakpoint — including
  internal temp/step-over ones — has a unique id that is independent of its
  position in `BPoints`. This is the crux: deleting BP #2 no longer renumbers
  the rest. New `CBreakpoint::DeleteByBpId(id)` (mirrors `DeleteByIndex`) and
  `Count()`. `DEBUG_AgentForEachBreakpoint` now fills `info.id`
  (`AgentBreakpointInfo` gained a leading `uint32_t id`).
- **Thin debug.cpp bridges** keep the agent off the file-local `CBreakpoint` /
  `BPINT_ALL` internals: `DEBUG_AgentAddExecBreakpoint(seg,off)`,
  `DEBUG_AgentAddIntBreakpoint(intnr, ah, al)` (ah/al `< 0` ⇒ the BPINT "any"
  wildcard), `DEBUG_AgentDeleteBreakpointById(id)`,
  `DEBUG_AgentDeleteAllBreakpoints()` (returns count). They wrap the existing
  `AddBreakpoint`/`AddIntBreakpoint`, which already `Activate()` immediately
  (fix 4.1), so a BP added mid-run fires without a subsequent RUN.
- **Handlers in `src/agent/agent_cpu.cpp`** (`handleBpAdd`/`handleBpList`/
  `handleBpDel`), next to `bp.set`/`bp.clear` — but operating on the *real*
  CBreakpoint table, not the agent-side conditional table those manage. `bp.list`
  reuses a local collector (same per-BP shape as `debug.status`, leading with
  `bp_id`). Three dispatch cases in `agent.cpp`, three prototypes in
  `agent_internal.h`. No new TU, no `Makefile.am` / vcxproj change.
- **`bp.hit` carries the handle.** `AGENT_EmitBpHit` gained a `uint32_t bp_id`
  parameter (`agent.h` decl + `#else` no-op, `agent_events.cpp` impl emits both
  `bp_index` and `bp_id`); the three call sites in `CheckBreakpoint` /
  `CheckIntBreakpoint` pass `bp->GetBpId()`. So a consumer that `bp.add`s a BP
  and later sees `bp.hit` can match on `bp_id` without re-listing.
- **`bytesHex` (agent_cpu.cpp) now returns `""` when `MemBase==NULL`** so
  `bp.list`'s `bytes_now` is safe headlessly (it reads guest memory, which the
  `-tests` binary doesn't bring up); harmless in production where MemBase is set.
- **`bp.del` requires an explicit `{all:true}`** to clear everything (an omitted
  `bp_id` is `bad_args`, not clear-all) — stricter than `bp.clear`, deliberately,
  because these are the real CPU-halting breakpoints (including the debugger's
  default INT3 trap).

### Reply / event shapes

```json
bp.add  → {"bp_id":7,"kind":"exec","addr":"0824:6F8E","seg":2084,"off":28558}
bp.add  → {"bp_id":8,"kind":"int","int":33,"ah":9}
bp.list → {"count":1,"breakpoints":[{"bp_id":7,"index":0,"kind":"exec",
            "enabled":true,"hits":0,"addr":"0824:6F8E","seg":2084,"off":28558,
            "bytes_now":"8B 46 FE 50"}]}
bp.del  → {"deleted":1,"bp_id":7,"remaining":0}     // or {"deleted":N,"remaining":0} for all
bp.hit event → {"event":"bp.hit","seg":2084,"off":28558,"bp_index":0,"bp_id":7,
                "from_cs":2084,"from_ip":28432}
```

Errors: `bp.add` — `bad_args` (unknown kind / missing or malformed addr / int
out of range / `al` without `ah`), `io_error` (add failed). `bp.del` —
`bad_args` (neither `bp_id` nor `all:true`), `not_found` (`bp_id` is gone).

### Tests

- **`tests/agent_breakpoint_tests.cpp`** — **8 new gTests** (a heavy-debug build
  makes `Activate()` a flag set and BP matching a seg:off compare, so the whole
  add/list/del path runs headless without MemBase, the same reason the existing
  fix-4.1 tests there do): exec add returns a working handle (`DEBUG_Breakpoint()`
  fires), int add, `bp.list` reflection, the headline **stable-id-survives-reorder**
  (add 3, delete the middle by id, the survivors keep their handles while their
  indices shift), `bp.del` not_found / requires-id-or-all / del-all-clears, and
  add arg-validation. **142 Agent gTests total, all pass.** (Gotcha re-learned:
  `GetAddress` special-cases `seg==SegValue(cs)` to use the hidden descriptor
  base, so a unit test must set CS:IP *before* adding a BP at that CS, exactly as
  the existing tests do; the live guest's descriptor cache is coherent so this is
  test-harness-only.)
- **`tests/agent_live/test_observability.py` phase 11** (no OBSTEST rebuild) —
  **11a**: add two exec + one int BP, `bp.list` shows all three by stable id,
  delete the middle by `bp_id`, confirm the survivors keep their handles (indices
  shifted), re-delete is `not_found`, `bp.del all` clears. **11b**: `bp.add` at
  the phase-1 loopbody, run, and assert the resulting `bp.hit` event's `bp_id`
  equals the one `bp.add` returned — the handle correlates a hit. **All 15 phases
  pass** (11b retries the `'1'` keytap, tolerating the known paste-pump drop —
  see the env note; phases 1b/9b still tap once and remain intermittently flaky
  on that same quirk, pre-existing).

## What shipped (previous iteration: Phase-2 mutation — `regs.set` + `mem.write`)

The first post-4.9 Phase-2 items from `TASKS.md` § Iteration 7+ — the *write*
counterparts to the long-shipped `regs.get` / `mem.read`. Built (0 errors),
**134 Agent gTests pass** (+13), and verified end-to-end (a new live **phase
10**, no OBSTEST.COM rebuild). With these the agent can now *change* CPU + guest
state, not only observe it.

| Item | Proves | Status |
|------|--------|--------|
| `regs.set {<reg>:<val>, …}` → `{set, regs}` | "poke a register and re-run from there" | ✅ |
| `mem.write {kind, addr, bytes(base64)}` → `{written}` | "patch a byte / word / blob in place" | ✅ |

### Design / code map

- **Both handlers live in `src/agent/agent_cpu.cpp`** next to their read twins
  (`handleRegsGet` / `handleMemRead`); two dispatch cases in `agent.cpp`, two
  prototypes in `agent_internal.h`. No new TU, no `Makefile.am` / vcxproj change.
- **`regs.set`** takes the register names `regs.get` *returns* as top-level args
  (`eax`..`esp`, `eip`, `cs`/`ds`/`es`/`fs`/`gs`/`ss`, `eflags`) — get and set
  are symmetric. Each value is a JSON number or hex string. It mirrors the
  debugger's `ChangeRegister`: GPRs/EIP by direct assignment, segment registers
  via `SegSet16` (real-mode form — does **not** reload protected-mode descriptor
  limits, exactly like the debugger's `SR`), `EFLAGS` via `CPU_SetFlags(v,
  FMASK_ALL)`. **Two-pass / all-or-nothing:** every entry is validated (unknown
  name, bad value, segment > 0xFFFF → `bad_args`) *before* any write, so a bad
  request mutates nothing. Validation runs **before** the paused gate (the same
  ordering `state.save` uses for its slot) so the `bad_args` paths are
  unit-testable headless; a well-formed request then requires the CPU paused
  (`DEBUG_AgentIsPaused`, same gate as `cpu.step`/`state.save`) → `bad_state`.
  Reply `{set:[names], regs:{…}}` echoes what changed plus the full post-set
  snapshot, so no follow-up `regs.get` is needed.
- **`mem.write`** is `mem.read` in the store direction: same `kind`
  (`seg:off`/`linear`/`physical`), same 64 KB cap, bytes arrive base64 (new
  `base64Decode`, strict RFC-4648 — length %4, alphabet-checked, `=`-padded).
  `seg:off`/`linear` go through the paged `MEM_BlockWrite`; `physical` is a
  `phys_writeb` loop (bypasses paging), mirroring `mem.read`'s physical path. A
  zero-length write is a guarded no-op. **No paused gate** — like `mem.read`, the
  agent dispatch runs on the emulator thread between instructions so the store is
  race-free; writing while the guest runs naturally races with the guest's own
  stores, documented as "pause first when patching a live value."

### Reply shapes

```json
regs.set  → {"set":["cs","eax"],"regs":{"eax":3735928559,…,"eflags":518}}
mem.write → {"written":6}
```

Errors: `regs.set` — `bad_args` (no registers / unknown name / bad value /
segment > 0xFFFF), `bad_state` (CPU not paused). `mem.write` — `bad_args`
(missing kind/addr/bytes, bad base64, oversize, bad addr, unknown kind).

### Tests

- **`tests/agent_cpu_tests.cpp`** — **13 new gTests** (AgentCpuTest 9→22). The
  successful write needs a paused CPU / `MemBase`, neither present in `-tests`,
  so these cover every headless-reachable path: `regs.set` empty / unknown reg /
  bad value / oversize segment → `bad_args`, and a well-formed set → `bad_state`
  (validation-before-gate makes all of these reachable); `mem.write` missing
  field / invalid + unpadded base64 / oversize / unknown kind / `seg:off`
  without colon → `bad_args`, and the zero-length no-op → `{written:0}`. The real
  apply is covered live. **134 Agent gTests total, all pass.**
- **`tests/agent_live/test_observability.py` phase 10** (no OBSTEST rebuild):
  pause, `regs.set eax/ebx/esi`, confirm the reply + an independent `regs.get`
  took the new values while an un-named register (`edi`) stayed put; a
  `regs.set` with a bogus register is rejected `bad_args` and leaves `eax`
  unchanged (atomicity); then `mem.write` a 6-byte blob via `seg:off`, read it
  back, and a `physical` write at the aliased real-mode linear address is
  observed through `seg:off`. The phase restores the clobbered registers before
  resuming. **All 13 phases pass.**

## What shipped (previous iteration: typed wrappers for the earlier 4.9 commands)

`contrib/agent-client/dbxagent.py` gained typed wrappers for every remaining
command (`debug_status`, `cpu_probe`, `cpu_trace_ring`, `cpu_traceback`,
`cpu_disasm`, the watch family, multi-range `cpu_watch_range`). See the
*Smaller follow-ups* note. (`regs_set` / `mem_write` wrappers were added this
iteration alongside the handlers.)

## What shipped (savestate restore→resume crash — FIXED)

Root-caused and fixed the `state.restore` → `cpu.run` → ~crash defect the
previous iteration discovered (it was documented as a "pre-existing savestate
bug, suspected mixer handler"). The real cause is the **agent's own per-tick
poll handler**. Built, **121 unit tests pass** (no change), and the live suite
now passes **12/12 with phase 8 and phase 9 in natural proposal order**, with a
new decisive check that resumes the restored machine and confirms it survives.

### Root cause

The agent installs `agent::tickPoll` via `TIMER_AddTickHandler` (`agent.cpp`,
`AGENT_StartIfRequested`) so the server is serviced every emulator millisecond.
The savestate **PIC** component (`SerializePic` in `src/hardware/pic.cpp`)
serializes the per-tick handler list by mapping each handler pointer through a
*fixed table*, `pic_state_timer_table`, which lists only the core handlers
(keyboard, mixer). `PIC_State_FindTimer` returns `0xffff` for any handler not in
the table; on load `PIC_State_IndexTimer(0xffff)` maps that back to **NULL**.
So `SaveState::load()` rebuilds the ticker list with the agent's slot as a NULL
handler. `TIMER_AddTick()` (`pic.cpp`) then calls **every** ticker handler
unconditionally each millisecond — so on the first tick after `cpu.run` it calls
NULL and the emulator dies. (The previous iteration's "WER `BEX64`, near-NULL
call from `ntdll`, suspected mixer channel handler" was the symptom of exactly
this NULL call; the table is shared with the genuinely-handled `MIXER_Mix` /
`KEYBOARD_TickHandler`, which is why mixer looked suspicious. The agent ticker
was the only un-tabled one active in the test config.)

This is fundamentally a consequence of the agent registering a tick handler the
core savestate table doesn't know about — i.e. an agent-introduced interaction,
not a mixer defect.

### The fix (one chokepoint, repair owned by the agent)

New public hook `AGENT_OnStateRestored()` (`src/agent/agent.cpp`, declared in
`include/agent.h` with the usual `#else` no-op) is called from the **end of
`SaveState::load()`** (`src/misc/savestates.cpp`, right after `flagged_restore`).
It:

1. `TIMER_DelTickHandler(nullptr)` — removes the dead NULL slot the restore left
   behind (a NULL ticker can only ever be such a corruption, so this is always
   safe; in the agent use-case there is exactly one — ours).
2. re-installs `tickPoll` (`TIMER_DelTickHandler(tickPoll)` de-dup +
   `TIMER_AddTickHandler(tickPoll)`), so the agent keeps servicing commands
   while the CPU runs.

It is a cheap no-op when the agent isn't running (`if (!g_started) return;`).

**Why the `SaveState::load()` chokepoint rather than `handleStateRestore`:** the
identical crash also fires if a user triggers a **GUI/menu load-state** (the `L`
mapper key) while the agent is active — that path calls `SaveState::load()`
directly, not the agent's command. Hooking the single load chokepoint covers
*every* restore path with one call; `handleStateRestore` therefore needs no
explicit repair (it reaches `load()` like everyone else). This matches how the
agent already integrates into core files (`AGENT_*` hooks in `sdlmain.cpp`,
`hardware.cpp`, `debug.cpp`); `agent.h`'s no-op keeps `savestates.cpp` building
with `C_DEBUG` off and needs no `#if` at the call site.

Both savestate provenances are handled: an agent-build savestate (the agent
ticker is present as `0xffff` → one NULL after load) and a foreign savestate (no
agent ticker → our handler is simply absent after the list is rebuilt). Either
way the list ends with a single live `tickPoll` and no NULL.

**Chosen this over** adding the agent handler to `pic_state_timer_table`: that
would couple core `pic.cpp` to an agent symbol behind `#if C_DEBUG` and *still*
need the re-install (the core can't know to re-add `tickPoll`), so the
agent-owned repair is both necessary and sufficient, and keeps the logic in
`src/agent/` (the only core touch is the one-line hook call).

### Tests

- **Negative control performed:** with the repair call commented out and
  rebuilt, the live suite's phase 8 fails with `VM died after restore+run
  (savestate crash)` (the socket is forcibly closed — the process crashed) and
  phase 9 fails behind it (9/12). With the repair restored, **12/12**. So the
  test is load-bearing, not vacuous.
- **`tests/agent_live/test_observability.py`** — phase 8 gained a decisive
  regression check: after the register round-trip it does `cpu.run`, waits 1.5 s
  (well past the documented ~0.5 s crash window), and asserts the VM is still
  responsive (`debug.status` succeeds). phase 8 and phase 9 were restored to
  natural proposal order (8 then 9) on the **same** instance, which exercises
  the 4.9.8+4.9.9 "restore then arm a conditional BP" pairing end-to-end.
- No unit test was added: the repair manipulates the live `firstticker` list via
  the real savestate load, none of which exists in `-tests` mode; it is covered
  live (same split as the rest of state.save/restore). **121 Agent gTests still
  pass.**

### Note for a future core hardening (optional, out of scope here)

The same `pic_state_timer_table` / `pic_state_event_table` mechanism will NULL
*any* tick/PIC-event handler not in its list — `IPX_*`, `NE2000_Poller`, and
several device PIC events (`DSP_BusyComplete`, `GUS_DMA_Event`, the `IDE_*`
events, `ACPI_PMTIMER_Event`, …) are likewise absent. They don't bite the agent
use-case (those devices are inactive in the test config and rarely combined with
savestates), but a principled core fix would have `SerializePic::setBytes` skip
a `0xffff` index instead of installing a NULL. Left alone deliberately: it's a
core-savestate behavior change, and the agent's own handler — the only one that
breaks the proposal workflow — is now handled.

## Previously shipped (4.9.9 — conditional / Nth-hit BP + on-hit macro)

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

#### Phase ordering note

Phase 8 then phase 9, natural proposal order, on the same instance. (An earlier
revision ran 9 before 8 to dodge the restore→resume crash; that crash is fixed
this iteration — see the top section — so the order was restored.)

## ✅ Resolved: savestate restore → resume crash

The previous iteration's "pre-existing savestate defect (suspected mixer
handler)" is **fixed this iteration** — see *What shipped this iteration*
(savestate restore→resume crash) at the top. In short: the savestate's per-tick
handler restore nulled the agent's own `tickPoll` handler (it isn't in
`pic.cpp`'s `pic_state_timer_table`), and `TIMER_AddTick` then called NULL on the
first tick after resume. `handleStateRestore` now repairs the ticker list after
`load()`. Verified with a negative control (crash reproduces with the repair
removed) and the strengthened phase-8 live check.

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

- **All nine proposal-4.9 items (4.9.1–4.9.9) are complete, and the savestate
  restore→resume crash that blocked the 4.9.8+4.9.9 loop is fixed** (top
  section). The restore-and-rerun loop the proposal is built around is now
  trustworthy.
- `bp.add`/`bp.list`/`bp.del` (typed breakpoints with stable handles) shipped
  this iteration (top section), retiring the `bp_index` isn't-a-stable-handle
  carry-over. `regs.set`/`mem.write` shipped the iteration before. The **only**
  remaining Phase-2 surface from `TASKS.md` § Iteration 7+ is **mouse input**
  (`mouse.move`/`mouse.click`) — `Mouse_CursorMoved` / `Mouse_ButtonPressed`
  (`include/mouse.h`). Everything else in that list is done.
- *Optional core hardening:* make `SerializePic::setBytes` skip a `0xffff`
  ticker/event index instead of installing a NULL handler, so the same class of
  bug can't bite `IPX`/`NE2000`/other un-tabled handlers. Deliberately not done
  here (core-behavior change; the agent's own handler is already repaired). See
  the note under the top section.

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
- **`contrib/agent-client/dbxagent.py`** has typed wrappers for **every** agent
  command, including this iteration's `bp_add()` / `bp_list()` / `bp_del()` and
  the prior `regs_set()` / `mem_write()` (each added alongside its handler;
  the live phase-11 test drives them). An earlier iteration filled the 4.9 gap —
  the commands:
  `debug_status()`, `cpu_probe()`, `cpu_trace_ring()`, `cpu_traceback()`,
  `cpu_disasm()`, plus the watch family `farcall_watch()`/`farcall_unwatch()`,
  `cpu_watch_target()`/`cpu_unwatch_target()`, `mem_watch()`/`mem_unwatch()`, and
  `cpu_watch_range()` gained the 4.9.5 multi-range `ranges=[…]` form
  (backward-compatible: the positional `(seg, lo, hi)` single-range call still
  works). The watch wrappers expose both the single-sentinel and the set form,
  and clear by calling with no argument. Verified at the protocol level (each
  wrapper emits the exact `cmd`+`args` its server-side handler parses; the live
  test still drives the server via `call(...)`, which is unchanged).
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
- Phase-2 "poke a register / patch memory and re-run" → phase10 (`regs.set`
  eax/ebx/esi confirmed via an independent `regs.get` with an un-named register
  intact and a bad request left atomic; `mem.write` a blob via `seg:off` plus an
  aliased `physical` write, both read back).
- Phase-2 "typed breakpoints with stable handles" → phase11a (`bp.add` 3 BPs,
  `bp.list` by `bp_id`, `bp.del` the middle by handle — survivors keep their
  ids while indices shift; re-delete is `not_found`; `bp.del all` clears) and
  phase11b (a `bp.hit` event carries the `bp_id` the `bp.add` returned).

## Environment note

This session hit repeated harness instability (tool results duplicating/
truncating, an occasional PowerShell-classifier outage, large parallel
batches getting cancelled). Process exit codes and *file* contents stayed
authoritative even when stdout echo garbled. If it recurs: keep tool calls
small and sequential, and when in doubt write results to a file and read it
back rather than trusting inline stdout.
