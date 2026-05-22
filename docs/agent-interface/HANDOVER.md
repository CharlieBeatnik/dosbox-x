# Handover — agent-interface

**Read this first.** Then the current iteration in `TASKS.md`. Only consult `PLAN.md` if a design question isn't answered by those two.

## State

- **Branch:** `agent-interface`.
- **Last commit on branch (about to land):** the Iteration 2 commit. Verify with `git log --oneline -5` once committed; before the commit, `git status` will show the changes listed below.
- **Build status:** *Not verified in this session.* Same as previous session — Windows host without a compiler toolchain. Code review and protocol logic only.
- **Test status:** New unit tests added (`tests/agent_protocol_tests.cpp`); not run.

## What was just done — Iteration 2

The agent now listens. When `-agent-listen ADDR:PORT` (or `[agent] enabled=true`) is in effect, DOSBox-X opens a loopback TCP socket, accepts one client, drains newline-delimited JSON, and answers `vm.version`. With no flag and `enabled=false`, behaviour is byte-identical to master — no socket, no thread, no tick handler.

Files added:
- `src/agent/agent_internal.h` — private header shared between agent TUs. Declares the `JsonValue` ADT, `jsonParse`/`jsonEncode`, the server lifecycle (`serverStart`/`serverStop`/`serverPoll`/`serverBroadcastLine`/`serverActive`), and `dispatchLine`.
- `tests/agent_protocol_tests.cpp` — gTest coverage of the JSON parser/encoder edge cases (escapes, surrogate pairs, control-char rejection, number forms, round-trip) plus `dispatchLine` sanity (`vm.version`, unknown cmd, missing cmd, malformed JSON). 13 tests.

Files filled in (previously empty under `#if C_DEBUG`):
- `src/agent/agent_json.cpp` — hand-rolled parser + encoder. ~270 LOC. Encoder guarantees no literal `\n`/`\r` in output so framing is trivial.
- `src/agent/agent_server.cpp` — SDL_net listener, single-client accept, per-client recv buffer with newline framing, 1 MB outbox cap with `agent.overflow` event. Loopback-only enforced by checking the resolved IP starts with 127.
- `src/agent/agent.cpp` — `AGENT_StartIfRequested` resolves CLI → `[agent]` section, calls `serverStart`, registers `tickPoll` via `TIMER_AddTickHandler`, and installs an `AddExitFunction` shutdown hook. `dispatchLine` handles `vm.version`; all other commands return `unknown_cmd`.

Files modified:
- `src/gui/sdlmain.cpp` — `#include "agent.h"`; call `AGENT_StartIfRequested()` immediately after `IPX_Init()` in the section-init block (`:~9752`).
- `tests/tests.h` — `#include "agent_protocol_tests.cpp"`.
- `vs/dosbox-x.vcxproj` + `.filters` — added `src/agent/agent_internal.h` as a ClInclude under `Sources\agent`.

## What to do next

Start **Iteration 3** in `TASKS.md`: debugger pass-through (`debugger.command`) and log tee.

**Verify Iteration 2 build first** before adding new code — every commit since Iteration 1 has shipped uncompiled. Highest-leverage check, in order:

1. `./build-debug` on Linux/macOS or VS Debug-SDL2 x64. The agent + tests are under `#if C_DEBUG`; a release build will silently elide them.
2. `./dosbox-x -tests --gtest_filter=AgentProtocol*` — expect 13 tests to pass. If one of the `RejectsControlCharInString` / surrogate-pair tests fails on a particular compiler, the JSON code likely has a portability slip; fix before iteration 3.
3. Boot with `-agent-listen 127.0.0.1:0 -agent-portfile /tmp/dbxport`. Confirm: a log line "agent: listening on 127.0.0.1:NNNNN" appears, the portfile contains that NNNNN, and a Python one-liner can connect & receive a reply:
   ```python
   import socket, json
   s = socket.create_connection(("127.0.0.1", int(open("/tmp/dbxport").read().strip())))
   s.sendall(b'{"id":1,"cmd":"vm.version"}\n')
   print(s.makefile().readline())
   ```
4. Boot with **no** agent flags. Confirm `netstat -ano` / `ss -tlnp` shows no new listening socket and no `agent:` line in the log. This is the crucial "release-build is byte-identical" guarantee.

**If the build fails:** the most likely culprits are (a) the `SDL_net.h` include path differing between SDL1 and SDL2 builds — the `#if defined(C_SDL2_NET) && C_SDL2_NET` block in `agent_server.cpp` mirrors what `misc_util.cpp` does, so any divergence there will catch up to us; (b) MSVC complaining about `snprintf` (handled — we use `<cstdio>`); (c) `Section_prop` / `control` symbols not visible — `agent.cpp` includes `control.h` and `setup.h`.

## Open decisions / gotchas

- **Auth token plumbed but not checked.** `serverStart` stores `auth_token`, no command rejects without it. Phase-1 loopback-only relies on the OS to protect us; the token is for the case where someone later adds remote access. Iteration 3 should add the gate (block all commands until `{"cmd":"auth.hello","args":{"token":"..."}}` is received) if remote access is on the roadmap; otherwise the gate can wait.
- **Single client only.** Second concurrent connection gets `{"event":"busy"}` and is closed. PLAN.md § Out of scope says this is intentional.
- **`SDLNet_TCP_GetPeerAddress` on a server socket** — SDL_net's docstring claims it returns NULL for server sockets, but in practice all implementations DOSBox-X targets return the bound address. If the portfile shows port 0 on some platform, fall back to `getsockname` via the `_TCPsocketX` struct hack used in `src/hardware/serialport/misc_util.cpp:~590`.
- **The plan said `AGENT_StartIfRequested()` should be called at `sdlmain.cpp:~7512`.** That's CLI parsing — before config files load and before the PIC timer is running. Moved to the section-init block (`:~9752`, just after `IPX_Init`). Note this in PLAN.md if you touch it.
- **No log tee yet.** `LOG_MSG` is not forwarded to the agent. Iteration 3 adds the tap at `debug_gui.cpp:714`.

## End-of-session checklist (for whoever closes the next session)

1. Are all `[~]` items in the current iteration either `[x]` or backed out?
2. Has the iteration pointer at the top of `TASKS.md` been advanced (if the iteration is done)?
3. Is this `HANDOVER.md` rewritten (not appended) to reflect what the next agent walks into?
4. Has the commit landed on `agent-interface`?
