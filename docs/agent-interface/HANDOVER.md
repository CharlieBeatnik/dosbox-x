# Handover — agent-interface

**Read this first.** Then the current iteration in `TASKS.md`. Only consult `PLAN.md` if a design question isn't answered by those two.

## State

- **Branch:** `agent-interface`.
- **Last commit on branch:** about to land — `agent: ParseCommand pass-through and log tee`. Confirm with `git log --oneline -5`.
- **Build status:** **Verified.** VS Debug x64 (v143 toolset) compiles cleanly: 0 errors. See `~/.claude/projects/D--data-Git-dosbox-x/memory/reference_windows_build.md` for the exact PowerShell invocation that imports the VS env and runs `msbuild`.
- **Test status:** **Verified.** All 24 agent tests pass (17 `AgentProtocolTest`, 7 `AgentDispatchTest`).

## What was just done — Iteration 3

The agent can now invoke any debugger command via `ParseCommand` and capture its output. Three new commands are dispatched:

- `debugger.command {text}` → runs `ParseCommand`, returns `{output, recognized}`.
- `log.subscribe` → flips the client's `logSubscribed` flag.
- `log.unsubscribe` → clears it.

Every `DEBUG_ShowMsg` call (which is what `LOG_MSG` resolves to) is now teed to the agent: if a capture is active, the line goes into the capture buffer; if a client is subscribed, the line is also emitted as `{event:"log.line",text:"..."}`.

Files added:
- `tests/agent_dispatch_tests.cpp` — 7 gTest cases. Pure capture-state-machine tests + dispatch routing checks that don't require `ParseCommand` to actually execute (it needs curses, which isn't up in `-tests` mode).

Files modified:
- `src/agent/agent_internal.h` — adds `serverSetLogSubscribed`, `serverEmitLogLine`, `captureBegin/captureEnd`, `emitLogLine` to the agent-private API.
- `src/agent/agent_events.cpp` — implements the capture pointer (single-threaded plain static, no `thread_local`) and `AGENT_EmitLog` → `emitLogLine` plumbing.
- `src/agent/agent_server.cpp` — adds `logSubscribed` flag to `Client`, `serverSetLogSubscribed`, and `serverEmitLogLine` (inline JSON encoder so this TU stays independent of agent_json.cpp).
- `src/agent/agent.cpp` — three new command handlers + `<vector>` include; routes `debugger.command` / `log.subscribe` / `log.unsubscribe`.
- `src/debug/debug_gui.cpp` — `#include "agent.h"`; one call to `AGENT_EmitLog(buf)` immediately after the newline-strip loop in `DEBUG_ShowMsg`.
- `tests/tests.h` — `#include "agent_dispatch_tests.cpp"`.

## What to do next

Start **Iteration 4** in `TASKS.md`: keyboard injection. The plan is a key-name → `KBD_KEYS` table in `agent_keyboard.cpp` plus three commands: `keyboard.type`, `keyboard.press`/`release`/`tap`.

Helpful starting points:
- The `KBD_KEYS` enum is in `include/keyboard.h:22-76`.
- The paste driver lives in `src/misc/clipboard.cpp` — append into `strPasteBuffer` rather than reinventing modifier juggling.
- `KEYBOARD_AddKey(KBD_KEYS, bool)` is the raw make/break entry point at `include/keyboard.h:78`.

## Verifying iteration 3 manually (before iteration 4)

The unit tests cover routing and capture, but not the live end-to-end. To exercise `debugger.command` against a real DOSBox-X you need curses up; for now that means:

1. Start with `-agent-listen 127.0.0.1:0 -agent-portfile dbxport.txt`.
2. Open the debugger interactively (Alt-Pause on Win/Linux, Alt-F12 on Mac).
3. Connect a client and send `{"id":1,"cmd":"debugger.command","args":{"text":"BPLIST"}}\n`.
4. Expect `{"id":1,"ok":true,"result":{"output":"Breakpoint list:\n...","recognized":true}}`.

`debugger.command` against a process without curses initialized will crash inside `DEBUG_BeginPagedContent` (NULL `dbg.win_out`). Iteration 5 adds the `AGENT_IsHeadless()` guards needed to fix that.

Also still outstanding from iteration 2:
1. Confirm that, with **no** agent flags, no listening socket appears and no `agent:` line is emitted — the "byte-identical to upstream" guarantee.
2. SDL2 / Linux / macOS build (`./build-debug` or `./build-debug-sdl2`).

## Open decisions / gotchas

- **`ParseCommand` is not headless-safe.** Documented above; iteration 5 fixes it. The agent will happily crash a non-debugger DOSBox-X if it dispatches a command like `BPLIST` at the wrong time.
- **Capture is single-level overwrite.** `captureBegin(B)` while another capture is active silently switches to `B`. Today this is fine — only `debugger.command` captures — but document if any new caller is added.
- **`serverEmitLogLine` has its own inline JSON encoder** rather than going through `agent_json.cpp`. Intentional: the log tap is in the DEBUG_ShowMsg hot path, and dragging in `JsonValue` allocations per log line is wasted work. The encoder there is a stripped-down copy of `encodeString` from `agent_json.cpp` — keep them in sync if escape semantics ever change.
- **Test mode disables most of the debugger.** `dbg.win_out` is NULL during `-tests`. Any future test that wants to drive `ParseCommand` needs to either build a curses stub or wait until iteration 5's headless path makes the commands safe.

## End-of-session checklist (for whoever closes the next session)

1. Are all `[~]` items in the current iteration either `[x]` or backed out?
2. Has the iteration pointer at the top of `TASKS.md` been advanced (if the iteration is done)?
3. Is this `HANDOVER.md` rewritten (not appended) to reflect what the next agent walks into?
4. Has the commit landed on `agent-interface` and been pushed?
