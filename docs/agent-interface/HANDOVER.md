# Handover — agent-interface

**Read this first.** Then the current iteration in `TASKS.md`. Only consult `PLAN.md` if a design question isn't answered by those two.

## State

- **Branch:** `agent-interface`.
- **Last commit on branch:** about to land — `agent: events, state transitions, headless debugger control`. Confirm with `git log --oneline -5`.
- **Build status:** **Verified.** VS Debug x64 (v143 toolset) compiles cleanly: 0 errors.
- **Test status:** **Verified.** All 34 agent tests pass: 17 protocol, 7 dispatch, 8 keymap, 2 events.

## What was just done — Iteration 5

The agent can now drive the debugger entirely from an external client. With `-agent-listen` in effect:

- The curses window is suppressed (`DBGUI_StartUp` / `DEBUG_SetupConsole` early-return on `AGENT_IsHeadless()`).
- `cpu.pause` and `cpu.run` commands route through the existing debugger entry points.
- Five new events arrive at any connected client: `bp.hit`, `debugger.entered`, `state.paused`, `state.running`, plus the existing `log.line`.
- `AGENT_Poll(true)` fires inside `DEBUG_Loop`'s paused branch, so commands sent while the CPU is halted get dispatched immediately rather than waiting for the next tick.

Files added:
- `tests/agent_events_tests.cpp` — 2 gTest cases: the four emitter helpers are no-op-safe without a client; `AGENT_IsHeadless()` returns false in test mode.

Files modified:
- `include/agent.h` — three new public emitters (`AGENT_EmitDebuggerEntered`, `AGENT_EmitStateRunning`, `AGENT_EmitStatePaused`) with inline-noop stubs for `!C_DEBUG`.
- `src/agent/agent_events.cpp` — populated `AGENT_EmitBpHit` (was an empty stub since iteration 1) and the three new emitters. All format compact JSON inline and call `serverBroadcastLine`.
- `src/agent/agent.cpp` — `AGENT_IsHeadless()` now returns `g_started` (the old `g_headless` field is gone). Two new dispatch handlers (`handleCpuPause`, `handleCpuRun`) and routes for `cpu.pause` / `cpu.run`.
- `src/debug/debug.cpp` — `#include "agent.h"`. Event emit calls in: `CBreakpoint::CheckBreakpoint` (both `return true` paths), `DEBUG_EnableDebugger` (on the running→paused transition), `RUN` and `RUNWATCH` ParseCommand handlers, and the auto-resume branch in `DEBUG_Loop`. `AGENT_Poll(true)` added to the paused branch of `DEBUG_Loop`. Null guards added/strengthened: `DEBUG_CheckKeys` early-returns if `dbg.win_main == NULL`; `DrawVariables` guards `dbg.win_var`; `DEBUG_SetupConsole` early-returns if `AGENT_IsHeadless()`.
- `src/debug/debug_gui.cpp` — `DBGUI_StartUp` early-returns if `AGENT_IsHeadless()`; `DEBUG_BeginPagedContent` handles `dbg.win_out == NULL` by setting `debugPageStopAt = 0` and returning.
- `tests/tests.h` — `#include "agent_events_tests.cpp"`.

## What to do next

Start **Iteration 6** in `TASKS.md`: Python reference client + smoke doc + the first opening PR.

The iteration plan from `TASKS.md`:
- `contrib/agent-client/dbxagent.py` — ~200 lines: connect, sync request/response with id correlation, async event listener.
- `docs/agent-interface/SMOKE.md` — the 6-step manual smoke sequence from `PLAN.md` § Verification.
- Open PR `agent-interface → master`.

The Python client is the right vehicle to **finally** run the manual smoke checks that have been carried forward since iteration 2. The order should be:

1. Write `dbxagent.py` minimally — enough to do request/reply + event subscription.
2. Use it to run the four pending manual checks (see below) on a real DOSBox-X build.
3. If anything breaks, that's the bug-fix budget for iteration 6.
4. Write `SMOKE.md` as a reproducible recipe.
5. Open the PR.

## Outstanding manual checks (still pending)

These haven't been validated against a live boot in any session yet:

1. **Boot with `-agent-listen 127.0.0.1:0 -agent-portfile dbxport.txt`.** Confirm: log line `agent: listening on 127.0.0.1:NNNNN`, portfile contains NNNNN, Python client gets a reply to `vm.version`.
2. **Boot with no agent flags.** `netstat -ano | grep LISTENING` must show no new socket; no `agent:` line in the log. The byte-identical guarantee.
3. **Linux/macOS build** (`./build-debug` or `./build-debug-sdl2`). Only VS x64 is exercised today.
4. **The full smoke flow from iteration 5's last task:**
   - Connect, `log.subscribe`.
   - Set a code breakpoint via `debugger.command BP 0:7C00`.
   - Reboot the machine (`debugger.command BOOT A:` or similar).
   - Expect the connected client to receive `{event:"bp.hit",...}` then `{event:"debugger.entered","reason":"breakpoint"}` then `{event:"state.paused"}`.
   - Send `debugger.command BPLIST` → see one entry in `output`.
   - Send `cpu.run` (or `debugger.command RUN`) → receive `{event:"state.running"}`.

The headless-curses changes have not been exercised against `DEBUG_Run(1,false)` (called by ParseCommand("RUN")). The first session that does the manual smoke should poke `cpu.run` hard — that's where curses-missing surprises most likely lurk.

## Open decisions / gotchas

- **`debugger.entered` always reports `reason:"breakpoint"`.** Plan distinguishes breakpoint / manual / int3 / sysenter; we only emit one signal. Differentiate when a real consumer needs it.
- **`AGENT_IsHeadless()` is true whenever the agent is started.** No way to run agent + curses concurrently today. Acceptable for Phase 1; trade-off documented.
- **The `cpu.pause` then `RUN` race** when `debugrunmode==1` ("Run Normal"): the user sees `state.paused` then immediately `state.running`. Accurate but possibly confusing. Document in the smoke doc if users hit it.
- **`AGENT_Poll(true)` runs once per `DEBUG_Loop` iteration** which is paced by `SDL_Delay(1)` — same 1 ms cadence as the tick handler. Two pollers, but `serverPoll` is idempotent.
- **`bp_index` is iteration order, not a stable handle.** If breakpoints get added/removed between hits, the index a client receives for the same breakpoint changes. Iteration 7+ should switch to a stable ID when it adds `bp.add` / `bp.list` / `bp.del`.

## End-of-session checklist (for whoever closes the next session)

1. Are all `[~]` items in the current iteration either `[x]` or backed out?
2. Has the iteration pointer at the top of `TASKS.md` been advanced (if the iteration is done)?
3. Is this `HANDOVER.md` rewritten (not appended) to reflect what the next agent walks into?
4. Has the commit landed on `agent-interface` and been pushed?
