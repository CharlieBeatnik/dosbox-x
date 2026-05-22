# Handover — agent-interface

**Read this first.** Then the current iteration in `TASKS.md`. Only consult `PLAN.md` if a design question isn't answered by those two.

## State

- **Branch:** `agent-interface`.
- **Last commit on branch:** about to land — `agent: keyboard input (type / press / release / tap)`. Confirm with `git log --oneline -5`.
- **Build status:** **Verified.** VS Debug x64 (v143 toolset) compiles cleanly: 0 errors. See `~/.claude/projects/D--data-Git-dosbox-x/memory/reference_windows_build.md` for the exact invocation.
- **Test status:** **Verified.** All 32 agent tests pass: 17 `AgentProtocolTest`, 7 `AgentDispatchTest`, 8 `AgentKeymapTest`.

## What was just done — Iteration 4

The agent can now type into the guest. Four new commands:

- `keyboard.type {text}` → `strPasteBuffer.append(text)`; the existing paste pump in `sdlmain.cpp:6785` delivers chars at the configured `pastespeed`. Returns `{queued: N}` where N is byte-count.
- `keyboard.press {key}` → `KEYBOARD_AddKey(k, true)`.
- `keyboard.release {key}` → `KEYBOARD_AddKey(k, false)`.
- `keyboard.tap {key}` → press then release.

The key-name table in `agent_keyboard.cpp` covers every value of `KBD_KEYS` except `KBD_NONE` / `KBD_LAST`. A test (`TableCoversEnumRange`) catches accidental drift if a new key value lands in `include/keyboard.h` without a matching table entry.

Files added:
- `tests/agent_keymap_tests.cpp` — 8 gTest cases.

Files modified:
- `src/agent/agent_keyboard.cpp` — was an empty stub; now ~190 lines of name table + four dispatch handlers.
- `src/agent/agent_internal.h` — exposes `keyboardNameToKey`, `keyboardTableSize`, the four `handleKeyboardXxx` functions, and the previously-private `makeReplyOk`/`makeReplyError` helpers (moved from `agent.cpp`'s anonymous namespace into the `agent` namespace so `agent_keyboard.cpp` can use them). Pulled in `keyboard.h` for the `KBD_KEYS` enum.
- `src/agent/agent.cpp` — `makeReplyOk`/`makeReplyError` un-anonymised; dispatch routes for the four new commands added.
- `tests/tests.h` — `#include "agent_keymap_tests.cpp"`.

## What to do next

Start **Iteration 5** in `TASKS.md`: events, state, and headless debugger.

This is the largest iteration so far and the one that unlocks fully-automated debugger sessions. The key pieces, from the plan:

- `cpu.pause` → existing `DEBUG_Enable_Handler(true)` path.
- `cpu.run` → `ParseCommand("RUN")`.
- `AGENT_EmitBpHit` populated; call site at `src/debug/debug.cpp:731` (`CBreakpoint::CheckBreakpoint`).
- `debugger.entered` event from `DEBUG_EnableDebugger` after `DEBUG_Enable_Handler(true)`.
- `state.paused` / `state.running` from the three transition sites: `DEBUG_EnableDebugger`, RUN/RUNWATCH (`:2621`/`:2647`), and the auto-resume inside `DEBUG_Loop` (`:~4812`).
- `AGENT_Poll(true)` inside `DEBUG_Loop`'s paused branch.
- `AGENT_IsHeadless()` predicate; gate `DBGUI_StartUp()` in `DEBUG_EnableDebugger`. Null-window guards in `DEBUG_CheckKeys` / `DEBUG_BeginPagedContent` / `DEBUG_EndPagedContent` / `DEBUG_DrawInput` and probably `CBreakpoint::ShowList`.

**Why the headless bit matters:** iteration 3's `debugger.command` only works today when the user has manually opened the curses debugger. Iteration 5 lets the agent open it via `cpu.pause` without flashing a curses window on screen, and lets commands like `BPLIST` / `D 0:0 16` run safely against a NULL `dbg.win_out`.

Suggested order:
1. Null-guard the curses entry points first (cheap, easy, no behaviour change with curses up). This makes existing iteration-3 tests slightly more robust as a side effect.
2. Add the `AGENT_IsHeadless()` predicate and gate `DBGUI_StartUp` on it.
3. Add the event emitters one site at a time, testing each by single-stepping in a connected session.
4. Then `cpu.pause` / `cpu.run` dispatch — those are one-liners that route to existing entry points.

## Outstanding manual checks (carry-overs)

These have been pending since iteration 2 and remain relevant; they're the verification path the docs keep deferring:

1. Boot with `-agent-listen 127.0.0.1:0 -agent-portfile dbxport.txt`. Confirm a log line "agent: listening on 127.0.0.1:NNNNN", that `dbxport.txt` contains the port, and the Python one-liner works (see iteration 2 section of git history for the snippet).
2. Boot with **no** agent flags. `netstat -ano | grep LISTENING` must show no new socket and no `agent:` line in the log. The byte-identical-to-upstream guarantee.
3. Linux/macOS build (`./build-debug`). Only VS x64 has been exercised. The `#if defined(C_SDL2_NET) && C_SDL2_NET` branch in `agent_server.cpp` is only reached on those builds.
4. Once iteration 5 lands: `keyboard.type "DIR\r"` over a live agent connection should make DOS run `DIR`. (Today it'd technically work too — paste doesn't need curses.)

## Open decisions / gotchas

- **`delay_ms` arg on `keyboard.type` is intentionally not implemented.** `paste_speed` is a session-wide config setting, not a per-call override. Adding the arg meaningfully needs either a temp config mutation (racy) or a parallel pump. Revisit if a real consumer asks.
- **`keyboard.type` accepts arbitrary bytes; the paste driver only handles ASCII reliably.** Non-ASCII chars in `strPasteBuffer` produce platform-dependent results — Windows uses `VkKeyScan` per char, other platforms use a scancode table. For reliable unicode input, fall back to `keyboard.tap` per-key with explicit shift/altgr modifiers.
- **`makeReplyOk` / `makeReplyError` were moved** from `agent.cpp`'s anonymous namespace into the `agent` namespace. If you write any new dispatch handler in `agent.cpp` itself, the helpers are still there at file scope — no change to call sites.
- **The keymap test (`TableCoversEnumRange`) will fail loudly** if `KBD_KEYS` ever gains or loses a value and `agent_keyboard.cpp`'s table isn't updated to match. Treat that as a useful trip-wire, not a flake.
- **`ParseCommand` is still not headless-safe.** Iteration 5 fixes it. Until then, any `debugger.command` issued before the curses debugger has been opened can crash inside `DEBUG_BeginPagedContent`.

## End-of-session checklist (for whoever closes the next session)

1. Are all `[~]` items in the current iteration either `[x]` or backed out?
2. Has the iteration pointer at the top of `TASKS.md` been advanced (if the iteration is done)?
3. Is this `HANDOVER.md` rewritten (not appended) to reflect what the next agent walks into?
4. Has the commit landed on `agent-interface` and been pushed?
