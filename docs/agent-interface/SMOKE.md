# Agent control channel — Phase 1 smoke test

Reproducible end-to-end check that the agent interface works against a real DOSBox-X build. Each numbered step has a "what to run" line, an "expect" line, and a "what's actually verified" note.

## Prerequisites

- DOSBox-X built with `C_DEBUG` (any Debug configuration). On Windows: VS solution `vs/dosbox-x.sln`, configuration `Debug | x64`, toolset `v143`. On Linux/macOS: `./build-debug`.
- Python 3.8+ on the same machine.
- The repo working tree (the smoke uses `contrib/agent-client/dbxagent.py`).

## The six checks (from `docs/agent-interface/PLAN.md` § Verification)

### 1. Build is clean

**Run:** Your platform's debug build command.
**Expect:** `0 Error(s)`, exe at `bin/x64/Debug/dosbox-x.exe` (Windows) or `src/dosbox-x` (Linux/macOS).
**Verifies:** No platform-specific breakage from the agent's SDL_net + getsockname() use.

### 2. Unit tests pass

**Run:**
```
./dosbox-x -tests "--gtest_filter=Agent*" "--gtest_output=xml:out.xml"
```
On Windows the `.exe` writes the XML and pauses on a console message; kill it once `out.xml` lands.

**Expect:** 34 tests, 0 failures, across `AgentProtocolTest`, `AgentDispatchTest`, `AgentKeymapTest`, `AgentEventsTest`.
**Verifies:** JSON parser/encoder, command-dispatch routing, key-name table covers `KBD_KEYS`, event emitters don't crash without a client.

### 3. Live boot, vm.version round-trip

**Run** (separate shell windows; the second one talks to the first):
```
# shell 1: start DOSBox-X with the agent listening on an ephemeral port
./dosbox-x -agent-listen 127.0.0.1:0 -agent-portfile dbxport.txt -fastlaunch

# shell 2: speak to it via the reference Python client
python contrib/agent-client/dbxagent.py --portfile dbxport.txt
```

**Expect:** the client prints something like
```
connected: version=2026.05.02 machine=vga build=heavy-debug
```
**Verifies:** TCP listener binds, the ephemeral port is read back correctly via `getsockname()`, the portfile is written, the client connects, `vm.version` reply has the documented shape.

### 4. Byte-identical without `-agent-listen`

**Run:**
```
# Boot WITHOUT any -agent-* flags, then in another shell:
netstat -ano | findstr LISTENING       # Windows
ss -tlnp                               # Linux
```
Filter for the DOSBox-X PID.

**Expect:** No new listening TCP sockets owned by the dosbox-x process. The log contains no `agent:` lines.
**Verifies:** `AGENT_StartIfRequested()` short-circuits when neither the CLI flag nor `[agent] enabled=true` is set. The "release build is byte-identical to upstream" guarantee.

### 5. Headless debugger end-to-end

**Run** (in one Python process, after step 3's DOSBox-X is still up):
```python
import sys, time
sys.path.insert(0, "contrib/agent-client")
from dbxagent import DbxAgent

with DbxAgent(portfile="dbxport.txt") as a:
    a.cpu_pause()
    time.sleep(0.3)
    while a.next_event(timeout=0.05): pass          # drain pause events
    a.debugger_command("BPINT 08")                  # break on every timer tick
    a.cpu_run()
    deadline = time.time() + 2.0
    while time.time() < deadline:
        ev = a.next_event(timeout=max(0.05, deadline - time.time()))
        if ev: print(ev)
```

**Expect — in this order:**
```
{'event': 'state.running'}
{'event': 'bp.hit', 'seg': ..., 'off': ..., 'bp_index': 0, 'bp_id': ...}
{'event': 'debugger.entered', 'reason': 'breakpoint'}
{'event': 'state.paused'}
```

**Verifies:** `cpu.pause` enters the debugger headless (no curses window), `debugger.command` round-trips through `ParseCommand` with output captured via the `DEBUG_ShowMsg` tap, `cpu.run` resumes, the breakpoint trips, `AGENT_EmitBpHit` fires from `CBreakpoint::CheckIntBreakpoint`, `DEBUG_EnableDebugger` emits `debugger.entered` + `state.paused` on the running→paused edge.

`BPINT 08` is the recommended trigger because the PIT timer fires roughly 18× per second regardless of guest state. `BPINT 21` is unreliable from an idle `COMMAND.COM` prompt — the shell waits on `INT 16h` and doesn't issue `INT 21h` until a full line is read.

### 6. Curses debugger still works without an agent attached

**Run:** Boot DOSBox-X with **no** `-agent-*` flags. Press the platform's debugger key (Alt-Pause on Windows/Linux, Alt-F12 on macOS, assuming a TTY).

**Expect:** The curses debugger window opens normally. Existing interactive debugging is unchanged.
**Verifies:** `AGENT_IsHeadless()` returns false when the agent isn't started, so `DBGUI_StartUp` and `DEBUG_SetupConsole` take their normal path.

## Known gotchas

- **Windows: `DOSBox_ConsolePauseWait`.** After `-tests` finishes, the process pauses on `"Press any key to continue"`. Either click the console or `taskkill /F /IM dosbox-x.exe` once your XML output is on disk.
- **The agent listener binds to `0.0.0.0`** (INADDR_ANY) because SDL_net's `SDLNet_TCP_Open` treats any non-INADDR_ANY address as a *client* connect target rather than a bind. Loopback enforcement happens per-accept via `SDLNet_TCP_GetPeerAddress`. The practical effect on Windows is that a firewall prompt may appear on first run — `Public network` permission isn't required, only `Private`.
- **`debugger.command` requires the agent's headless mode** to safely run commands that touch the curses windows (`BPLIST`, `D`, …). That's automatic — the agent flips `AGENT_IsHeadless()` on as soon as the listener starts.
- **The Python client's reader thread is a daemon.** If you `^C` the script, the thread dies with it; you don't need to call `close()` explicitly, but using the context manager (`with DbxAgent(...) as a:`) is cleaner.
