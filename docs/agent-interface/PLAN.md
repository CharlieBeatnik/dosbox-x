# Agent control channel — design

## Why

The DOSBox-X debugger (`src/debug/`) is powerful but human-only — ncurses TUI, typed commands. This adds a programmatic channel so an external agent (a Claude agent in particular) can launch a DOS program and drive both the guest (keystrokes, mouse) and the debugger (run/pause/step, every breakpoint type, memory/register inspection) with full fidelity to what a human can do today.

## Shape

| | |
|---|---|
| Transport | TCP on 127.0.0.1, ephemeral port written to a port-file. SDL_net (already linked for IPX/modem). |
| Protocol | Newline-delimited JSON. `{"id":N,"cmd":"...","args":{...}}` → `{"id":N,"ok":true,"result":{...}}`. Server pushes events as `{"event":"...","...":...}`. |
| Build gate | `#if C_DEBUG` only. Release builds are byte-identical to upstream. |
| Branch | `agent-interface` off `master`. |
| Phasing | Phase 1 = thin pass-through over `ParseCommand`. Phase 2 = structured typed ops. |

## Reuse — don't reinvent

| Need | Existing API (file:line) |
|---|---|
| Run any debugger command from text | `ParseCommand(char*)` — `src/debug/debug.cpp:1915`. Curses-independent. Covers all 88 commands (BP, BPM, BPLM, BPINT, BPPM, BPLIST, BPDEL, MEMDUMP, D/DP/DV, SR/SM, RUN, RUNWATCH, INT, IDT, GDT, KERN, CALLBACKS, …). |
| Capture debugger + `LOG_MSG` output | `DEBUG_ShowMsg()` — `src/debug/debug_gui.cpp:675`. `LOG_MSG` is `#define LOG_MSG DEBUG_ShowMsg` (`include/logging.h:78`). Existing reentrancy guard `in_debug_showmsg` (`:671`); formatted `buf[]` at `:710`; tap after the newline strip at `:714`. |
| Type ASCII into guest | `PasteClipboardNext()` + `strPasteBuffer` — `src/misc/clipboard.cpp:354`. Handles modifier juggling, paced by `paste_speed`. |
| Raw key make/break | `KEYBOARD_AddKey(KBD_KEYS, bool)` — `include/keyboard.h:78`. `KBD_KEYS` enum at `:22-76`. PS/2 and PC-98 share the dispatcher. |
| Breakpoint state | `CBreakpoint` + `BPoints` — `src/debug/debug.cpp:559+`. `AddBreakpoint`, `AddIntBreakpoint`, `AddMemBreakpoint`, `DeleteByIndex`, `ShowList`, `CheckBreakpoint(:731)`. |
| Periodic main-thread service | `TIMER_AddTickHandler` — `src/hardware/pic.cpp:841`. Fires every 1 ms regardless of pause state. |
| Cross-platform sockets | SDL_net — see `src/hardware/ipx.cpp:43-362`. Mandatory: zero-timeout `SDLNet_CheckSockets` before each `Recv`. |
| Subsystem init pattern | `AddVMEventFunction(VM_EVENT_*, AddVMEventFunctionFuncPair(...))` — `include/setup.h:293-343`. Canonical example `src/hardware/sblaster.cpp:4612-4616`. Use `VM_EVENT_DOS_INIT_AT_PROMPT` to start, `VM_EVENT_DOS_EXIT_BEGIN` to tear down. |
| Config section pattern | Printer section, `src/dosbox.cpp:4533-4572`. New `[agent]` section. |
| CLI option pattern | `src/gui/sdlmain.cpp:7473-7549` (option chain) + help text `:7450-7465`. Field on `Config` struct at `include/control.h:66-116`. |

## Control flow — the critical bit

Headless debugger control is delivered by **two surgical points**:

1. **Agent poll inside `DEBUG_Loop`'s paused branch.** `DEBUG_Loop` (`src/debug/debug.cpp:4770`) already polls `getch()` non-blockingly — `nodelay(dbg.win_main,true)` at `src/debug/debug_gui.cpp:625` — and yields `SDL_Delay(1)`. Add `AGENT_Poll(/*paused=*/true)` next to `DEBUG_CheckKeys()`. Drained JSON lines dispatch through `ParseCommand`. `RUN`/`RUNWATCH` (`:2621`/`:2647`) already call `DOSBOX_SetNormalLoop()` — no agent-specific resume logic.
2. **Headless guard on curses init.** Guard `DBGUI_StartUp()` in `DEBUG_EnableDebugger` (`:5652`) when agent-headless. `DEBUG_ShowMsg` already copes with `dbg.win_out == NULL`. Add null-window early-returns in `DEBUG_CheckKeys` and any `Draw*` paths that don't already have them; predicate: extend `DBGUI_IsActive()` at `:581`.

For the running-CPU case, `AGENT_Poll(false)` is registered via `TIMER_AddTickHandler` and runs every 1 ms. Both sources (curses keys + agent socket) can coexist; `ParseCommand` is the choke point.

## Event flow

- **Breakpoint hit:** emit `bp.hit` from inside `CBreakpoint::CheckBreakpoint` (`src/debug/debug.cpp:731`) immediately before the return that triggers `DEBUG_EnableDebugger`.
- **Other debugger entries** (F12, sysenter, INT3): emit a single `debugger.entered` from inside `DEBUG_EnableDebugger` after `DEBUG_Enable_Handler(true)`.
- **Log lines:** tap the formatted `buf[]` in `DEBUG_ShowMsg` (`debug_gui.cpp:714`, right after newline strip). The existing `in_debug_showmsg` reentrancy guard protects the emit path.
- **State transitions:** emit `state.paused`/`state.running` from the three known transition sites: `DEBUG_EnableDebugger` (entered), `RUN`/`RUNWATCH` (resumed), and the auto-resume inside `DEBUG_Loop` (`:~4812`).

## Threading

Everything runs on the main emulator thread — tick handler, `DEBUG_Loop`, CPU cores, BP check. `AGENT_EmitBpHit` and friends are string-format + `std::deque<std::string>::push_back`; no socket I/O in CPU-core context. The tick handler flushes the deque. Per-client outbound buffer caps at 1 MB and drops with an overflow event rather than blocking the emulator.

## Source tree additions

```
src/agent/agent.cpp            // lifecycle, JSON loop, ParseCommand dispatch
src/agent/agent_server.cpp     // SDL_net accept/recv/send, per-client outbox
src/agent/agent_json.cpp       // minimal JSON parser/encoder
src/agent/agent_keyboard.cpp   // keyboard.type / press / release / tap
src/agent/agent_events.cpp     // event emitters, format helpers
src/agent/Makefile.am
include/agent.h                // AGENT_Poll, AGENT_EmitBpHit, AGENT_EmitLog, AGENT_OnLoopChange, AGENT_IsHeadless
```

## Modified files

- `src/debug/debug.cpp` — call `AGENT_Poll(true)` inside `DEBUG_Loop`; emit `bp.hit` from `CBreakpoint::CheckBreakpoint`; emit `debugger.entered` from `DEBUG_EnableDebugger`; emit `state.running` from RUN/RUNWATCH.
- `src/debug/debug_gui.cpp` — `AGENT_EmitLog(buf)` after the newline strip at `:714`; conditional curses init via `AGENT_IsHeadless()`.
- `src/dosbox.cpp` — register `[agent]` section; install tick handler from an init function.
- `src/gui/sdlmain.cpp` — parse `-agent-*` flags; help text; call `AGENT_StartIfRequested()` near where `-tests` is handled.
- `include/control.h` — `bool opt_agent_*` fields on `Config`.
- `src/Makefile.am` — add `agent` subdir.
- `vs/dosbox-x.vcxproj` + `.filters` — add `src/agent/*` to all four debug-build configurations (Win32/x64 × SDL1/SDL2). Release configs **not** added (`C_DEBUG` is off there).

## CLI / config

- CLI: `-agent-listen <addr:port>` (port `0` = ephemeral), `-agent-portfile <path>`, `-agent-token <hex>`. Help text near the existing `-tests` entry.
- Config: `[agent]` section: `enabled` (bool), `listen` (string), `portfile` (string), `auth_token` (string), `paste_speed_override` (int). `Property::Changeable::OnlyAtStart`.
- Lifecycle: `VM_EVENT_DOS_INIT_AT_PROMPT → AGENT_Start`, `VM_EVENT_DOS_EXIT_BEGIN → AGENT_Stop`. Server can also start earlier if the agent wants to break-on-boot.

## Phase 1 surface

Commands:
- `debugger.command` `{text}` → passes to `ParseCommand`, returns captured `DEBUG_ShowMsg` output.
- `keyboard.type` `{text, [delay_ms]}` → appends to `strPasteBuffer`.
- `keyboard.press` / `keyboard.release` / `keyboard.tap` `{key}` → maps JSON key name → `KBD_KEYS`, calls `KEYBOARD_AddKey`.
- `cpu.pause` → existing `DEBUG_Enable_Handler(true)` path.
- `cpu.run` → `ParseCommand("RUN")`.
- `vm.version` / `vm.info` → build flags, current `machine=`.
- `log.subscribe` `{levels}` / `log.unsubscribe`.

Events:
- `bp.hit` `{seg, off, bp_index}`
- `debugger.entered` `{reason}` — `breakpoint` | `manual` | `int3` | `sysenter`
- `state.paused` / `state.running`
- `log.line` `{level, text}` — only after `log.subscribe`

## Phase 2 surface (each item is its own iteration)

- `bp.add` `{kind, ...}` / `bp.list` / `bp.del` `{index}` — direct `CBreakpoint::*`.
- `mem.read` `{addr_kind, addr, len}` — base64 bytes. Addr kinds: `seg:off`, `linear`, `physical`.
- `mem.write` — symmetric.
- `regs.get` — typed JSON for every register.
- `regs.set` `{reg, value}`.
- `cpu.step` / `cpu.step_over`.
- `disasm` `{addr, count}` — call `DasmI386` (`src/debug/debug_disasm.cpp:1317`) directly.
- `mouse.move` / `mouse.click` — `Mouse_CursorMoved` / `Mouse_ButtonPressed` (`include/mouse.h:37-44`).
- `vm.screenshot` — base64 PNG via existing capture path.

## Out of scope

- Release-build code paths.
- Multi-client (second connection rejected `busy`).
- TLS or remote access (loopback-only; reject non-127.0.0.1 binds with a config error).
- Driving the host SDL window (window move/resize/focus).
- Save-state plumbing beyond what `ParseCommand` exposes.
- Anything Claude-Code-side (MCP server, tool defs) — that's a follow-on.

## Verification

Phase-1 done means:

1. `./build-debug` (Linux) and the VS Debug-SDL2 x64 configuration both compile cleanly.
2. `./dosbox-x -tests --gtest_filter=Agent*` passes.
3. The Phase-1 smoke sequence in `docs/agent-interface/SMOKE.md` (added at end of Phase 1) completes end-to-end including a `bp.hit` event.
4. `./dosbox-x` started **without** `-agent-listen` is byte-identical to master — no listening sockets, no port file, no new threads, no new log noise. Verified with `ss -tlnp` / `netstat -ano` and a startup-log diff against `master`.
5. The existing curses debugger still works interactively with no agent attached.
6. No code paths outside `#if C_DEBUG`.
