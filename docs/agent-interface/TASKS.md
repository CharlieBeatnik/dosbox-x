# Agent interface — task list

**Current iteration:** Iteration 6
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

- [x] `agent_server.cpp` opens SDL_net listener on `listen` address. Reject non-127.0.0.1.
- [x] Write the port to `portfile` after `SDLNet_TCP_Open` returns.
- [x] `agent_json.cpp` — minimal hand-rolled JSON parse/encode (subset: numbers, strings with escapes including `\uXXXX` + surrogate pairs, bools, null, objects, arrays). One-line-per-message framing (encoder guarantees no embedded `\n`/`\r`).
- [x] `AGENT_Poll(false)` registered via `TIMER_AddTickHandler` from `AGENT_StartIfRequested` in `agent.cpp`. Tick fires every 1 ms; drains accept/recv/send and dispatches complete lines.
- [x] `AGENT_StartIfRequested()` called from `sdlmain.cpp` immediately after `IPX_Init` in the section-init block. Earlier placements (just-after-CLI-parse) were ruled out — `control->GetSection("agent")` is not safe until config files have been parsed, and `TIMER_AddTickHandler` requires the PIC timer system to be live. **Deviation from plan:** TASKS Iteration 2 said "same place `-tests` triggers test setup (`:~7512`)". That line is inside CLI parsing, before config is loaded; the actual init-list region is `sdlmain.cpp:~9751`.
- [x] One implemented command: `vm.version` → `{ "version": "...", "machine": "...", "build": "..." }`.
- [x] Per-client outbox `std::deque<std::string>` with 1 MB cap. Overflow drops the line and emits `agent.overflow` once room frees.
- [x] `tests/agent_protocol_tests.cpp` — JSON encode/decode round-trip + dispatch sanity. Registered in `tests/tests.h`.
- [x] `./dosbox-x -tests --gtest_filter=AgentProtocol*` passes. Verified on VS Debug x64 build: 17/17 tests pass (0.005s, gtest XML output).
- [~] Manual: connect from a Python one-liner, get a reply. **Not exercised in this session** — only the unit tests were run, not a full boot with `-agent-listen`.
- [x] Commit `agent: TCP listener, JSON framing, vm.version` — `7878e1d24`.

### Iteration 2 — open issues for the next session

- **Auth token is parsed but not enforced.** `serverStart` stashes it; nothing checks it on the first message. The check belongs in `dispatchLine` (gate non-`auth.hello` commands until the token has been presented) — fold into iteration 3 or its own pre-iteration-3 task.
- **Bind-port readback assumes `SDLNet_TCP_GetPeerAddress` works on a listening socket.** SDL_net's docs say it returns NULL for server sockets but in practice all platforms return the bound address. If port 0 + portfile reads as 0 on some platform, fall back to platform-specific `getsockname` via the `_TCPsocketX` struct trick used in `misc_util.cpp:~590`.
- **No log subscription yet** — `LOG_MSG` is not yet teed to the agent. That's iteration 3.
- **`AGENT_Poll(true)` is wired but its DEBUG_Loop caller doesn't exist yet** — iteration 5.

## Iteration 3 — debugger pass-through + log tee

Goal: agent can issue any of the existing 88 debugger commands and read their output.

- [x] `debugger.command` command in `agent.cpp` → installs a per-request capture buffer via `agent::captureBegin/End`, calls `ParseCommand` on a writable buffer, returns `{output, recognized}`.
- [x] Tap `DEBUG_ShowMsg` in `src/debug/debug_gui.cpp` (just after the newline strip) calls `AGENT_EmitLog(buf)`.
- [x] `log.subscribe` / `log.unsubscribe` commands; per-client `logSubscribed` flag in the Client struct. `serverEmitLogLine` enqueues `{event:"log.line",text:"..."}` only for the subscribed client.
- [x] `tests/agent_dispatch_tests.cpp` — 7 cases covering the capture state machine (begin/append/end, idempotent end, overwrite semantics) + dispatch routing (debugger.command missing-text, log.subscribe/unsubscribe without a client). **Deviation from plan:** the plan called for fixture-driven `BPLIST` / `D 0:0 16` / `R` ParseCommand round-trips, but ParseCommand depends on the curses debugger being initialized — `dbg.win_out` is NULL inside `-tests` mode, so most commands would crash. Headless ParseCommand support lands in iteration 5; covering ParseCommand end-to-end in unit tests waits for that.
- [~] Manual: `debugger.command BPLIST` returns empty list; `debugger.command D 0:0 16` returns a memory hexdump. **Not exercised in this session** — needs a live boot with `-agent-listen` *and* curses up (Alt-Pause / Alt-F12). Verify once iteration 5 lands the headless path; until then this only works after the user has opened the debugger.
- [x] Verify build (VS Debug x64): 0 errors. Tests: 24/24 pass (17 protocol + 7 dispatch).
- [ ] Commit `agent: ParseCommand pass-through and log tee`.

### Iteration 3 — open issues for the next session

- **Most ParseCommand commands need curses.** Calling `debugger.command BPLIST` against a freshly-booted DOSBox-X (no debugger window) will crash inside `DEBUG_BeginPagedContent` (dereferences `dbg.win_out`). Iteration 5 fixes this with `AGENT_IsHeadless()` guards. Until then, the agent client should only send `debugger.command` after a `debugger.entered` event, OR after the user has opened the debugger interactively.
- **Capture nesting is overwrite, not stack.** `captureBegin` while a capture is active loses the outer pointer (documented in `CaptureCanBeNested` test). Not a problem today — `debugger.command` is the only caller — but worth knowing if iteration 7+ adds more capture sites.
- **Log subscription has no replay / backlog.** A client that subscribes mid-session sees only future log lines. The first iteration consumer (Claude Code) starts subscribed at connect time, so this is fine.

## Iteration 4 — keyboard injection

Goal: agent can type into the running guest.

- [x] Key-name → `KBD_KEYS` table in `agent_keyboard.cpp`. Covers every value in the enum (`KBD_LAST - 1` entries; KBD_NONE excluded). Unit test guards against accidental drift if `KBD_KEYS` gains a value.
- [x] `keyboard.type` `{text}` → append to `strPasteBuffer`. Reply `{queued: N}`. The existing 1-char-per-`paste_speed`-tick pump in `sdlmain.cpp:6785` delivers them. **Deviation from plan:** `delay_ms` is not implemented — `paste_speed` is a per-session config setting, not a per-call override; honouring `delay_ms` would mean either temporarily mutating `paste_speed` (racy) or building a parallel paste pump (out-of-scope for this iteration). Document in HANDOVER for a future revisit.
- [x] `keyboard.press` / `keyboard.release` / `keyboard.tap` `{key}` → `KEYBOARD_AddKey`. Each returns `{ok:true, result:{}}` on success or `{ok:false, error:{code:"bad_args",...}}` on missing/unknown key.
- [x] `tests/agent_keymap_tests.cpp` — 8 tests: table-size enum coverage, common-key resolution, unknown-name rejection, type round-trip into `strPasteBuffer`, type-rejects-non-string, press/release/tap arg validation, valid-key press dispatch.
- [~] Manual: boot DOSBox-X, type `DIR\r`, observe DOS run `DIR`. **Not exercised in this session** — needs a live boot with `-agent-listen` and a connected client.
- [x] Verify build (VS Debug x64): 0 errors. Tests: 32/32 pass (17 protocol + 7 dispatch + 8 keymap).
- [ ] Commit `agent: keyboard input (type / press / release / tap)`.

### Iteration 4 — open issues for the next session

- **`delay_ms` arg deferred.** The paste driver is paced by `[sdl] pastespeed=` (default 30). A per-call override would need a parallel pump or temporary config mutation — neither feels worth it for Phase 1. Re-evaluate when a real consumer asks.
- **`keyboard.type` accepts arbitrary text but the paste driver only handles ASCII.** Non-ASCII chars in `strPasteBuffer` will likely produce wrong scancodes or be silently dropped, depending on platform. The agent honestly returns `queued: <byte-count>`; consumers needing reliable unicode input should fall back to per-key `tap` events.
- **No way to query the current paste-buffer length** so a client can wait until DOSBox-X has drained queued input before sending more. Could add `keyboard.status` later if needed.

## Iteration 5 — events, state, headless debugger

Goal: agent can pause/resume and is notified when breakpoints fire — without curses being present.

- [x] `cpu.pause` command → `DEBUG_EnableDebugger()`. The handler does all the real work (sets `debugging=true`, switches loop to `DEBUG_Loop`, etc.).
- [x] `cpu.run` command → `ParseCommand("RUN")`.
- [x] `AGENT_EmitBpHit` populated in `agent_events.cpp`. Emitted from both `return true` paths inside `CBreakpoint::CheckBreakpoint` — the BKPNT_PHYSICAL match and the C_HEAVY_DEBUG memory-watch match. `bp_index` is the iteration position in `BPoints`.
- [x] `debugger.entered` emitted from `DEBUG_EnableDebugger` immediately after `DEBUG_Enable_Handler(true)` — gated on a `wasRunning` snapshot so we only emit on the running→paused edge. Currently always emits `reason:"breakpoint"`; widening to `"manual"` / `"int3"` / `"sysenter"` is iteration 7+.
- [x] `state.paused` paired with `debugger.entered`. `state.running` emitted from three sites: end of `RUN` handler, end of `RUNWATCH` handler, and the auto-resume branch in `DEBUG_Loop` (after `DOSBOX_SetNormalLoop`).
- [x] `AGENT_Poll(true)` called inside `DEBUG_Loop`'s paused branch, immediately before `DEBUG_CheckKeys`. Lets the agent dispatch commands while the CPU is halted.
- [x] `AGENT_IsHeadless()` predicate returns `g_started`. `DBGUI_StartUp` and `DEBUG_SetupConsole` both early-return when headless, so `dbg.win_*` stays NULL. Null-window guards added/verified in `DEBUG_CheckKeys`, `DEBUG_BeginPagedContent`, `DrawVariables`. All other `Draw*` / `DEBUG_RefreshPage` paths already had guards. **Trade-off:** with `-agent-listen` active, pressing the F12/Alt-Pause mapper key no longer pops a curses window — the agent owns the debugger UI. A future iteration could split "agent active" from "curses suppressed" if both-at-once is wanted.
- [~] Manual: set a code breakpoint, reboot, observe `bp.hit` then `debugger.entered`, run `BPLIST`, then `RUN`, observe `state.running`. **Not exercised in this session** — needs a live boot with `-agent-listen` and a connected client.
- [x] Verify build (VS Debug x64): 0 errors. Tests: 34/34 pass (17 protocol + 7 dispatch + 8 keymap + 2 events).
- [ ] Commit `agent: events, state transitions, headless debugger control`.

### Iteration 5 — open issues for the next session

- **`debugger.entered` reason is always `"breakpoint"`.** Plan lists four reasons (breakpoint, manual, int3, sysenter); distinguishing them needs separate emit sites or a thread-local reason variable set by the trigger. Defer until a real consumer needs the distinction.
- **`debugrunmode==1` immediately RUNs after debugger.entered.** When the agent calls `cpu.pause` and the user has configured `debugrun=normal`, the handler emits `state.paused` then immediately a `state.running` from the chained RUN. The agent sees both events — accurate but jarring. The user config is uncommon in agent flows.
- **`AGENT_IsHeadless()` is binary on agent-started.** No way today to mix agent + curses. If a developer wants to drive the agent AND watch the curses window, this iteration won't let them. Add a `[agent] headless=` config in iteration 6+ if asked.
- **`DEBUG_Run` inside cpu.run is not headless-tested.** ParseCommand("RUN") runs in test mode without crashing today (it's not exercised by our 34 tests), but a real `cpu.run` issued by an external client touches `DEBUG_Run(1,false)` which we haven't proven safe with curses uninitialized. The first manual-test session should poke this hard.
- **`AGENT_Poll(true)` inside `DEBUG_Loop` runs every `SDL_Delay(1)` iteration.** Same cadence as the tick handler (1 ms). No double-poll concern because `serverPoll` is idempotent under accept/drain, but if a future iteration adds expensive per-poll work this could double-bill the CPU.

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
