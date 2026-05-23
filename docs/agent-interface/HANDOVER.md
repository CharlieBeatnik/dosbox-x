# Handover — agent-interface

**Phase 1 is done.** The PR opens this branch against `master`.

> **If you are here to *use* the agent channel, not extend it, read
> [`USAGE.md`](USAGE.md) instead.** This file is the development handover.

## State

- **Branch:** `agent-interface`.
- **Last commit on branch:** about to land — `agent: reference Python client, smoke doc, and Phase-1 fixes`. Confirm with `git log --oneline -10`.
- **Build status:** **Verified.** VS Debug x64 (v143 toolset) compiles cleanly: 0 errors.
- **Test status:** **Verified.** 34/34 agent tests pass (17 protocol, 7 dispatch, 8 keymap, 2 events).
- **Manual smoke:** **Verified.** All six checks from `docs/agent-interface/SMOKE.md` pass on VS Debug x64. See "What was just done" below.

## What was just done — Iteration 6 + Phase-1 cleanup

Wrote the reference client (`contrib/agent-client/dbxagent.py`), wrote the smoke doc (`docs/agent-interface/SMOKE.md`), and **found and fixed three bugs** while running the smoke against a real build:

### Bug 1: listener bound nowhere

`SDLNet_TCP_Open(IPaddress)` treats *any* address that isn't `INADDR_ANY` or `INADDR_NONE` as a client connect target, not a bind. Our code passed `127.0.0.1` and SDL_net dutifully tried to *connect* to it — failing with "Couldn't connect to remote host".

Fix in `src/agent/agent_server.cpp`: validate the configured listen host resolves to loopback first, then re-resolve with `NULL` host (INADDR_ANY) for the actual `SDLNet_TCP_Open`. Loopback enforcement moved to per-accept via `SDLNet_TCP_GetPeerAddress` in `acceptIfReady`. The listener now binds to `0.0.0.0:PORT` but actively rejects any non-loopback peer.

### Bug 2: ephemeral port read back as garbage

SDL_net stores a server socket's bound address in its internal `_TCPsocket.localAddress` field — except SDLnetTCP.c declares the field but **never assigns it**. So our struct-layout mirror was reading whatever was on the heap (consistently 52685 in our test, while the actual listener was on 55511, then 63495, etc.).

Fix in `src/agent/agent_server.cpp`: read the OS-assigned port via `getsockname()` on the underlying socket (via the `channel` field of the same struct-layout mirror). Needs `#include <winsock2.h>` on Windows, `<sys/socket.h>` etc. elsewhere.

### Bug 3: interrupt breakpoints didn't fire `bp.hit`

The plan said "emit `bp.hit` from `CBreakpoint::CheckBreakpoint`" — but interrupt breakpoints (`BPINT`) go through a separate function, `CheckIntBreakpoint`. So `BPINT 08` correctly stopped the CPU but never told the agent.

Fix in `src/debug/debug.cpp`: added the same `AGENT_EmitBpHit(seg, off, bp_index)` call inside the matching branch of `CBreakpoint::CheckIntBreakpoint`.

### Iteration 6 deliverables

- `contrib/agent-client/dbxagent.py` — 273 lines. Thread-safe reader demuxes responses by id, events queue. CLI `--portfile/--port` + convenience methods + a demo `main`.
- `docs/agent-interface/SMOKE.md` — 6-step verifiable recipe with command lines, expected outputs, and what each step proves.

### What works end-to-end now (verified on this host)

```
{'event': 'state.running'}
{'event': 'bp.hit', 'seg': 61440, 'off': 53638, 'bp_index': 0}
{'event': 'debugger.entered', 'reason': 'breakpoint'}
{'event': 'state.paused'}
```

`cpu.pause` → headless debugger entry. `debugger.command BPLIST` round-trips through `ParseCommand` with captured `DEBUG_ShowMsg` output. `cpu.run` resumes. `BPINT 08` trips → events arrive in the documented order.

## What to do next

**Phase 1 is shippable.** Open the PR (`agent-interface → master`) with a short summary; the design and rationale already live in `docs/agent-interface/PLAN.md`. After it lands, the natural follow-ups are the items in TASKS.md § Iteration 7+:

- `bp.add` / `bp.list` / `bp.del` as typed commands (today the client uses `debugger.command BP ...` strings).
- `mem.read` / `mem.write` with binary base64.
- Typed `regs.get` / `regs.set`.
- `cpu.step` / `cpu.step_over` with structured output.
- `disasm` via `DasmI386`.
- Mouse input.
- `vm.screenshot`.

## Carry-overs that didn't make it into Phase 1

- **Linux/macOS build verification.** Only VS Debug x64 has been exercised. The struct-layout trick in `agent_server.cpp` and the `#include` block for sockets need a smoke test on a Unix host before tagging anything stable.
- **Multi-client.** Second concurrent connection still gets `{"event":"busy"}` and is closed. PLAN.md § Out of scope says this is intentional for Phase 1.
- **`debugger.entered` reason granularity.** Always `"breakpoint"` today; plan calls for distinguishing manual / int3 / sysenter. Trivial follow-up when a consumer needs it.
- **`bp_index` is iteration order, not a stable handle.** Iteration 7's `bp.add/list/del` should switch to a stable ID.

## Open decisions to flag in the PR description

- **Loopback enforcement is per-accept, not per-bind**, because SDL_net's binding API can only bind to INADDR_ANY. The listener appears in `netstat` as `0.0.0.0:PORT` but rejects non-127/8 connections immediately on accept. Behaviour is loopback-only in effect, but the bind address may surprise a paranoid sysadmin.
- **The struct-layout trick on `_TCPsocket` is fragile.** It works against the vendored `vs/sdlnet/` source and any SDL_net build sharing the upstream layout. If a future SDL_net rev changes the struct order, ephemeral-port readback breaks silently. Document in SMOKE.md or wrap in a runtime sanity check.
- **`AGENT_IsHeadless()` is binary on agent-started.** No way today to mix agent + curses concurrently. Trade-off documented; first complaint is the trigger to revisit.

## End-of-session checklist (post-PR-open)

1. PR opened on GitHub against `master`.
2. PR description references PLAN.md / SMOKE.md / this HANDOVER.
3. Carry-over items list is preserved in TASKS.md so iteration 7 doesn't start from scratch.
