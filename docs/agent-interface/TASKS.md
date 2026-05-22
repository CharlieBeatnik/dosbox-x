# Agent interface — task list

**Current iteration:** Iteration 2
**Branch:** `agent-interface`

`[ ]` not started · `[~]` in progress (note who/when) · `[x]` done (note commit SHA)

Each iteration is sized for roughly one Claude session and one commit. An iteration is "done" when every task inside it is `[x]`, tests pass, the branch builds, and `HANDOVER.md` has been rewritten.

## Iteration 0 — scaffold (DONE by definition of the doc existing)

- [x] Branch `agent-interface` created off `master`.
- [x] `docs/agent-interface/PLAN.md` written.
- [x] `docs/agent-interface/TASKS.md` written.
- [x] `docs/agent-interface/HANDOVER.md` written.

## Iteration 1 — config / CLI / empty skeleton

Goal: the build is unchanged in behaviour, but the new files exist, the CLI flags parse, and the `[agent]` section appears in the conf reference.

- [x] Create `src/agent/{agent.cpp,agent_server.cpp,agent_json.cpp,agent_keyboard.cpp,agent_events.cpp,Makefile.am}` with `#if C_DEBUG` guards and empty function bodies for the externs in `include/agent.h`.
- [x] Create `include/agent.h` with the public surface (`AGENT_StartIfRequested`, `AGENT_Stop`, `AGENT_Poll`, `AGENT_EmitBpHit`, `AGENT_EmitLog`, `AGENT_OnLoopChange`, `AGENT_IsHeadless`). All inline-noop in this iteration.
- [x] Add `opt_agent_listen`, `opt_agent_portfile`, `opt_agent_token` to `Config` in `include/control.h`.
- [x] Parse `-agent-listen`, `-agent-portfile`, `-agent-token` in `src/gui/sdlmain.cpp` (option chain near `:7473`). Add help text near `:7462`.
- [x] Add `[agent]` section in `DOSBox_SetupConfigSections()` (`src/dosbox.cpp`): `enabled`, `listen`, `portfile`, `auth_token`. Model on the printer section at `:4533-4572`.
- [x] Wire `src/agent` into `src/Makefile.am`.
- [x] Wire `src/agent/*` into `vs/dosbox-x.vcxproj` and `.vcxproj.filters`. **Deviation from plan:** added unconditionally rather than to "debug configurations only" — matches how `src/debug/*` is already wired in the VS project (no per-config exclusions), and `vs/config.h` hard-codes `C_DEBUG 1`. The `#if C_DEBUG` guards in the source files are the actual gate.
- [~] Regenerate `dosbox-x.reference.conf` via `./update-dosbox-x-reference-conf`. **Skipped:** no build environment in this session. Verified the existing reference conf is generated from a non-`C_DEBUG` build (no other debug-only sections appear in it), so the [agent] section would not appear in a regenerated conf anyway — no diff expected. Next maintainer running the script should confirm this.
- [~] `./build-debug` succeeds. **Not run in this session** — Windows without compiler toolchain.
- [~] `./dosbox-x --help` shows the new flags. **Not run in this session** (depends on build).
- [x] Commit `agent: scaffold subsystem, CLI flags, [agent] section (no behaviour)`.

## Iteration 2 — TCP server + JSON framing

Goal: external clients can connect, exchange JSON, and call one trivial command.

- [ ] `agent_server.cpp` opens SDL_net listener on `listen` address. Reject non-127.0.0.1.
- [ ] Write the port to `portfile` after `SDLNet_TCP_Open` returns.
- [ ] `agent_json.cpp` — minimal hand-rolled JSON parse/encode (subset: numbers, strings with escapes, bools, null, objects, arrays). One-line-per-message framing.
- [ ] `AGENT_Poll(false)` registered via `TIMER_AddTickHandler` from a small init function in `agent.cpp`.
- [ ] `AGENT_StartIfRequested()` called from `sdlmain.cpp` at the same place `-tests` triggers test setup (`:~7512`).
- [ ] One implemented command: `vm.version` → `{ "version": "...", "machine": "...", "build": "..." }`.
- [ ] Per-client outbox `std::deque<std::string>` with 1 MB cap and overflow event.
- [ ] `tests/agent_protocol_tests.cpp` — JSON encode/decode round-trip + framing edge cases (partial line, embedded escaped newline, oversize).
- [ ] `./dosbox-x -tests --gtest_filter=AgentProtocol*` passes.
- [ ] Manual: connect from a Python one-liner, get a reply.
- [ ] Commit `agent: TCP listener, JSON framing, vm.version`.

## Iteration 3 — debugger pass-through + log tee

Goal: agent can issue any of the existing 88 debugger commands and read their output.

- [ ] `debugger.command` command in `agent.cpp` → installs a per-request `DEBUG_ShowMsg` capture buffer (using a thread-local-ish static guarded by the existing `in_debug_showmsg`), calls `ParseCommand`, returns captured output as `{output: "..."}`.
- [ ] Tap `DEBUG_ShowMsg` in `src/debug/debug_gui.cpp:714` to forward formatted `buf[]` to `AGENT_EmitLog`.
- [ ] `log.subscribe` / `log.unsubscribe` commands; `log.line` events only sent to subscribers.
- [ ] `tests/agent_dispatch_tests.cpp` — fixture invokes `AGENT_DispatchJson` with `BPLIST`, `D 0:0 16`, `R`, asserts captured output looks right.
- [ ] Manual: `debugger.command BPLIST` returns empty list; `debugger.command D 0:0 16` returns a memory hexdump.
- [ ] Commit `agent: ParseCommand pass-through and log tee`.

## Iteration 4 — keyboard injection

Goal: agent can type into the running guest.

- [ ] Key-name → `KBD_KEYS` table in `agent_keyboard.cpp`. Cover every value in the enum.
- [ ] `keyboard.type` `{text, delay_ms?}` → append to `strPasteBuffer`. Reuse the existing paste driver.
- [ ] `keyboard.press` / `keyboard.release` / `keyboard.tap` `{key}` → `KEYBOARD_AddKey`.
- [ ] `tests/agent_keymap_tests.cpp` — every JSON key name resolves to a `KBD_KEYS` value; round-trip for a sample.
- [ ] Manual: boot DOSBox-X, type `DIR\r`, observe DOS run `DIR`.
- [ ] Commit `agent: keyboard input (type / press / release / tap)`.

## Iteration 5 — events, state, headless debugger

Goal: agent can pause/resume and is notified when breakpoints fire — without curses being present.

- [ ] `cpu.pause` command → existing `DEBUG_Enable_Handler(true)` path.
- [ ] `cpu.run` command → `ParseCommand("RUN")`.
- [ ] `AGENT_EmitBpHit` emit from `CBreakpoint::CheckBreakpoint` (`src/debug/debug.cpp:731`) before the return.
- [ ] `debugger.entered` emit from `DEBUG_EnableDebugger` after `DEBUG_Enable_Handler(true)`.
- [ ] `state.paused` / `state.running` emits from RUN/RUNWATCH (`:2621`/`:2647`) and the auto-resume inside `DEBUG_Loop` (`:~4812`).
- [ ] Call `AGENT_Poll(true)` inside `DEBUG_Loop`'s paused branch alongside `DEBUG_CheckKeys`.
- [ ] `AGENT_IsHeadless()` predicate; gate `DBGUI_StartUp()` in `DEBUG_EnableDebugger`. Add null-window guards in `DEBUG_CheckKeys` and any `Draw*` paths missing them.
- [ ] Manual: set a code breakpoint, reboot, observe `bp.hit` then `debugger.entered`, run `BPLIST`, then `RUN`, observe `state.running`.
- [ ] Commit `agent: events, state transitions, headless debugger control`.

## Iteration 6 — Python reference client + smoke doc

Goal: a reproducible end-to-end test.

- [ ] `contrib/agent-client/dbxagent.py` — ~200 lines: connect, sync request/response with id correlation, async event listener.
- [ ] `docs/agent-interface/SMOKE.md` — the 6-step manual smoke sequence from `PLAN.md` § Verification.
- [ ] Commit `agent: reference Python client and smoke test`.
- [ ] Open PR `agent-interface → master`.

## Iteration 7+ — Phase 2 (each bullet is its own iteration)

- [ ] `bp.add` / `bp.list` / `bp.del` typed (direct `CBreakpoint::*`).
- [ ] `mem.read` (binary, base64).
- [ ] `mem.write`.
- [ ] `regs.get` / `regs.set` typed.
- [ ] `cpu.step` / `cpu.step_over` with structured output.
- [ ] `disasm` `{addr, count}` via `DasmI386`.
- [ ] `mouse.move` / `mouse.click`.
- [ ] `vm.screenshot`.
