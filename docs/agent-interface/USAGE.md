# Driving DOSBox-X over the agent control channel

This is the **consumer-facing** guide for the agent interface. If you are an
automated agent (or a human writing one) that wants to launch DOSBox-X, type
into a DOS guest, set breakpoints, and react to events, read this file end to
end before opening any source.

For the design rationale see `PLAN.md`. For build/verification steps see
`SMOKE.md`. For development history see `HANDOVER.md`. None of those are
required reading just to *use* the channel.

## What you get

A loopback TCP socket on the running DOSBox-X process, speaking
newline-delimited JSON. With Phase 1 you can:

- Pause / resume the CPU.
- Type ASCII into the guest, or send raw key make/break events.
- Invoke **any** of the 88 existing debugger commands (`BP`, `BPM`, `BPINT`,
  `BPLIST`, `D`, `MEMDUMP`, `R`, `DV`, `INT`, `IDT`, `GDT`, `KERN`,
  `CALLBACKS`, …) and read whatever the debugger would have printed.
- Subscribe to log lines and to events (`bp.hit`, `debugger.entered`,
  `state.paused`, `state.running`, `log.line`).

What is **not** available yet (planned for Phase 2): typed breakpoint /
memory / register / step / disasm commands; mouse input; screenshots; stable
breakpoint handles; finer `debugger.entered` reasons.

## Requirements

- DOSBox-X **debug build**. The agent is gated on `C_DEBUG` and the
  release build is byte-identical to upstream with no listener and no port
  file. On Windows: VS solution `vs/dosbox-x.sln`, configuration
  `Debug | x64`, toolset `v143`. On Linux/macOS: `./build-debug`.
- A way to launch DOSBox-X with CLI flags.
- A TCP client. The reference is `contrib/agent-client/dbxagent.py` (Python
  3.8+, stdlib only).

## Quickstart

### 1. Launch DOSBox-X with the agent listening

```
dosbox-x -agent-listen 127.0.0.1:0 -agent-portfile dbxport.txt -fastlaunch
```

- `-agent-listen 127.0.0.1:0` — bind to loopback, OS-assigned ephemeral port.
- `-agent-portfile dbxport.txt` — DOSBox-X writes the chosen port number
  (one ASCII integer, no trailing newline guaranteed) to this file as soon
  as the listener is up. Poll for the file's existence; don't try to
  connect before it appears.
- `-fastlaunch` — skip the splash for faster iteration. Optional.
- `-agent-token <hex>` — currently parsed but **not enforced**. Don't rely
  on it for security; loopback-only binding is the real gate.

Equivalent without CLI flags — in `dosbox-x.conf`:

```ini
[agent]
enabled  = true
listen   = 127.0.0.1:0
portfile = dbxport.txt
```

### 2. Connect from Python

```python
from dbxagent import DbxAgent          # contrib/agent-client/dbxagent.py

with DbxAgent(portfile="dbxport.txt") as a:
    print(a.vm_version())               # {'version':'2026.05.02','machine':'vga','build':'heavy-debug'}
    a.keyboard_type("DIR\r")
    a.cpu_pause()
    print(a.debugger_command("BPLIST")["output"])
    a.cpu_run()
```

That's the whole loop: connect, issue commands synchronously, drain events
asynchronously, close.

## Wire protocol

One JSON object per line. UTF-8. `\n`-terminated. No framing prelude, no
length prefix, no handshake.

Request: `{"id": N, "cmd": "...", "args": {...}}`
- `id` — any number; the server echoes it on the matching reply. Use a
  monotonically increasing integer.
- `args` — optional object; omitted when the command takes no arguments.

Reply: same `id`, plus either
- `{"id": N, "ok": true,  "result": {...}}`, or
- `{"id": N, "ok": false, "error": {"code": "...", "message": "..."}}`.

Event (server-initiated): `{"event": "...", ...other fields...}`. No `id`.

Demultiplex by checking for `"id"` on incoming messages — replies have one,
events don't. The reference client does this in a single reader thread.

## Commands

| Command             | Args                | Result                              |
|---------------------|---------------------|-------------------------------------|
| `vm.version`        | none                | `{version, machine, build}`         |
| `debugger.command`  | `{text}`            | `{output, recognized}`              |
| `cpu.pause`         | none                | `{}`                                |
| `cpu.run`           | none                | `{}`                                |
| `keyboard.type`     | `{text}`            | `{queued}` (byte count)             |
| `keyboard.press`    | `{key}`             | `{}`                                |
| `keyboard.release`  | `{key}`             | `{}`                                |
| `keyboard.tap`      | `{key}`             | `{}`                                |
| `log.subscribe`     | none                | `{subscribed: true}`                |
| `log.unsubscribe`   | none                | `{subscribed: false}`               |

### `vm.version`

- `version` — string, e.g. `"2026.05.02"`.
- `machine` — lowercased `[machine]` enum: `hercules`, `cga`, `tandy`,
  `pcjr`, `ega`, `vga`, `amstrad`, `pc98`, `fmtowns`, `mcga`, `mda`,
  or `unknown`.
- `build` — `"debug"` or `"heavy-debug"`. (`"release"` is unreachable; the
  agent code is compiled out in release builds.)

Cheap. Useful as a connection sanity-check after the portfile appears.

### `debugger.command`

Pass any text the curses debugger would accept — `BP CS:IP`,
`BPINT 21 4C`, `BPM RW 1000:0`, `BPLIST`, `BPDEL 3`, `D DS:SI 64`,
`MEMDUMP 1000:0 100`, `R EAX`, `R`, `INT 21 4C00`, `IDT`, `GDT`,
`CALLBACKS`, `KERN`, … — and read back whatever `DEBUG_ShowMsg` would
have printed.

- `text` (required, string) — the debugger command line.
- `output` (string) — captured output, newlines preserved between lines.
- `recognized` (bool) — `ParseCommand`'s return value. `false` means the
  command itself didn't match anything; the agent will still reply OK.

This is the wide door. Until Phase 2 lands, lean on this for breakpoints,
memory dumps, register reads, disassembly, stepping, anything you'd type
into the curses debugger. The `BPLIST` output is the canonical way to
enumerate breakpoints.

### `cpu.pause` / `cpu.run`

`cpu.pause` flips the emulator into the debugger loop (headless — no curses
window pops). `cpu.run` resumes. Both reply immediately with `{}`; watch
`state.paused` / `state.running` events for the actual transitions.

Most `debugger.command` calls that *inspect* state (`BPLIST`, `D`, `R`)
are safe whether the CPU is running or paused. Commands that **mutate**
the loop (`RUN`, `RUNWATCH`, `T`/`P` step) should be issued while paused.

### `keyboard.type`

- `text` (required, string) — appended verbatim to the paste buffer. The
  existing paste pump delivers one character per `[sdl] pastespeed` tick
  (default 30 ms). ASCII only; non-ASCII bytes produce undefined scancodes.
- `queued` — byte count appended (not characters typed yet). There is no
  built-in "wait until drained" — sleep `len(text) * pastespeed_ms` if you
  need to be sure DOS has seen it before sending more.

`\r` is the way to press Enter from `keyboard.type`. `\n` works on most
DOS apps too.

### `keyboard.press` / `release` / `tap`

- `key` (required, string) — lowercase name from the table below.
- `tap` is `press` followed immediately by `release`.

Held modifiers (e.g. Ctrl+C) must be done as explicit press/release pairs:

```python
a.keyboard_press("leftctrl")
a.keyboard_tap("c")
a.keyboard_release("leftctrl")
```

#### Key names

- **Digits:** `0`–`9`
- **Letters:** `a`–`z` (lowercase only)
- **Function:** `f1`–`f24`
- **Control / nav:** `esc`, `tab`, `backspace`, `enter`, `space`,
  `leftalt`, `rightalt`, `leftctrl`, `rightctrl`, `leftshift`,
  `rightshift`, `capslock`, `scrolllock`, `numlock`
- **Punctuation:** `grave`, `minus`, `equals`, `backslash`, `leftbracket`,
  `rightbracket`, `semicolon`, `quote`, `period`, `comma`, `slash`,
  `extra_lt_gt`
- **Editing block:** `insert`, `home`, `pageup`, `delete`, `end`, `pagedown`
- **Arrows:** `left`, `up`, `down`, `right`
- **Misc:** `printscreen`, `pause`
- **Keypad:** `kp0`–`kp9`, `kpdivide`, `kpmultiply`, `kpminus`, `kpplus`,
  `kpenter`, `kpperiod`, `kpequals`, `kpcomma`
- **Windows:** `lwindows`, `rwindows`, `rwinmenu`
- **Japanese (NEC PC-98 / AX):** `jp_hankaku`, `jp_muhenkan`, `jp_henkan`,
  `jp_hiragana`, `yen`, `underscore`, `ax`, `conv`, `nconv`, `jp_yen`,
  `jp_backslash`, `colon`, `caret`, `atsign`, `jp_ro`, `help`, `kana`,
  `nfer`, `xfer`
- **Korean:** `kor_hancha`, `kor_hanyong`
- **Vendor (PC-98 etc.):** `stop`, `copy`, `vf1`–`vf5`

Unknown name → `bad_args` error with `unknown key: <name>` message.

### `log.subscribe` / `log.unsubscribe`

After subscribing, the server pushes a `log.line` event for every
`LOG_MSG` / `DEBUG_ShowMsg` call until you unsubscribe (or disconnect).
There is **no replay** of lines emitted before subscribe; subscribe at
connect time if you want everything.

## Events

| Event              | Fields                          | When                                                            |
|--------------------|---------------------------------|-----------------------------------------------------------------|
| `state.running`    | none                            | CPU resumed (after `cpu.run`, RUN, RUNWATCH, or auto-resume)    |
| `state.paused`     | none                            | CPU halted (paired with `debugger.entered`)                     |
| `bp.hit`           | `{seg, off, bp_index}`          | Just before the debugger entry that a breakpoint triggered      |
| `debugger.entered` | `{reason}`                      | After `bp.hit`, or any other debugger entry                     |
| `log.line`         | `{text}`                        | While subscribed                                                |
| `agent.error`      | `{code, message}`               | Malformed input from your side (no `id` available to reply on)  |
| `agent.overflow`   | none                            | Outbox hit its 1 MB cap; lines were dropped                     |
| `busy`             | none                            | A second client tried to connect; that connection is closed     |

Notes:

- `bp.hit.seg` and `bp.hit.off` are the linear `CS:EIP` (or memory address
  for memory breakpoints). `bp_index` is the breakpoint's position in
  `BPLIST` **at the moment it fired** — not stable across `BPDEL`. Phase 2
  will add stable handles.
- `debugger.entered.reason` is currently always `"breakpoint"`. The four
  reasons in PLAN.md (`breakpoint`, `manual`, `int3`, `sysenter`) will be
  distinguished in a later iteration.
- `log.line.text` is the formatted line with the trailing newline stripped.
  No `level` field today (despite what PLAN.md says).
- Event order on a breakpoint hit is **always**: `bp.hit` → `debugger.entered`
  → `state.paused`. On manual `cpu.pause` you get `debugger.entered` →
  `state.paused` (no `bp.hit`).
- On `cpu.run` you get exactly one `state.running`.

## Errors

When a request fails the reply is `{"id": N, "ok": false, "error": {code, message}}`.

| `code`        | Cause                                                  |
|---------------|--------------------------------------------------------|
| `bad_args`    | Required arg missing, wrong type, or invalid value (e.g. unknown key name). |
| `missing_cmd` | Request has no string `cmd` field.                     |
| `unknown_cmd` | `cmd` doesn't match any handler.                       |
| `no_client`   | Internal: emitted from `log.subscribe` when the server lost the client mid-call. Rare. |

Malformed JSON gets an unsolicited `agent.error` event (no `id` to reply
to). The connection stays up; just send the next request.

## Recipes

### Run a DOS command and wait for the prompt

```python
a.keyboard_type("DIR\r")
# pastespeed default = 30 ms/char, so worst case ~0.12 s for "DIR\r"
time.sleep(0.5)
```

There is no "wait for prompt" primitive in Phase 1. If you need
synchronization, set a breakpoint on `INT 21 / AH=08` (keyboard input with
echo) or just sleep generously.

### Set a code breakpoint, run, wait for it

```python
a.debugger_command("BP CS:0100")        # breakpoint at current CS:0100
a.cpu_run()
while True:
    ev = a.next_event(timeout=10.0)
    if ev is None: raise TimeoutError("breakpoint never hit")
    if ev.get("event") == "bp.hit":
        print(f"hit bp {ev['bp_index']} at {ev['seg']:04x}:{ev['off']:08x}")
        break
# CPU is now paused; subsequent debugger.command calls inspect state
print(a.debugger_command("R")["output"])
print(a.debugger_command("D DS:SI 32")["output"])
a.cpu_run()                              # resume
```

### Break on every PIT timer tick (handy smoke test)

```python
a.debugger_command("BPINT 08")
a.cpu_run()
ev = a.next_event(timeout=2.0)            # arrives ~55 ms after RUN
```

**Prefer `BPINT 08` over `BPINT 21`** for testing — the PIT fires
~18×/sec regardless of guest state. `INT 21h` is only issued by code that
asks; an idle `COMMAND.COM` prompt is blocked on `INT 16h` and will never
trip a bare `BPINT 21`.

### Stream all log output

```python
a.log_subscribe()
for ev in a.events(timeout=None):         # block forever
    if ev.get("event") == "log.line":
        print(ev["text"])
```

### Pause, inspect, resume

```python
a.cpu_pause()
# drain the debugger.entered / state.paused events
while a.next_event(timeout=0.1) is not None: pass
print(a.debugger_command("R")["output"])
print(a.debugger_command("BPLIST")["output"])
a.cpu_run()
```

### Hold Ctrl while pressing C

```python
a.keyboard_press("leftctrl")
a.keyboard_tap("c")
a.keyboard_release("leftctrl")
```

## Gotchas

- **Wait for the portfile.** The listener is started during DOS init, not
  at process spawn. Poll for the file before connecting.
- **Bind shows as `0.0.0.0` in `netstat`.** SDL_net's bind API can only
  bind to `INADDR_ANY`; loopback enforcement happens per-accept by
  inspecting the peer address. Non-loopback peers are dropped immediately.
  Windows may show a firewall prompt the first time; only `Private`
  permission is required.
- **One client at a time.** A second connection gets `{"event":"busy"}\n`
  and is closed. Don't try to multiplex multiple drivers.
- **Curses is disabled when the agent runs.** With `-agent-listen` active,
  Alt-Pause / Alt-F12 no longer opens a curses debugger window — the agent
  owns the debugger UI. Boot without `-agent-*` flags to use the curses
  debugger interactively.
- **`debugger.command` is text in, text out.** Output formatting matches
  the curses debugger and can change between versions. Don't write
  brittle parsers; for stable structured access, wait for Phase 2 typed
  commands.
- **`keyboard.type` is ASCII only.** Non-ASCII input is silently mangled
  by the paste driver. For non-ASCII or modifier combos, use `press` /
  `release` / `tap` with named keys.
- **`bp_index` is not stable.** If you `BPDEL 2`, the indices of all
  later breakpoints shift down. Re-read `BPLIST` after any mutation.
- **`agent.overflow` means lines were dropped.** Outbox cap is 1 MB
  per client. If you see this, you're producing events faster than you're
  reading them — drain `events()` more aggressively, or unsubscribe from
  `log.line` if you don't need it.
- **Release builds have no agent at all.** No listener, no port file, no
  CLI flags accepted. You need a Debug build.

## Reference client

`contrib/agent-client/dbxagent.py` (273 lines, stdlib only). Sync
`call(cmd, **args)` plus typed helpers (`vm_version`, `cpu_pause`,
`cpu_run`, `keyboard_type`, `keyboard_tap`, `keyboard_press`,
`keyboard_release`, `debugger_command`, `log_subscribe`,
`log_unsubscribe`). Events arrive via `next_event(timeout)` /
`events(timeout)`. Reader is a daemon thread; closing the socket unblocks
it; use as a context manager (`with DbxAgent(...) as a:`).

CLI demo:

```
python contrib/agent-client/dbxagent.py --portfile dbxport.txt \
    --type "DIR\r" --cmd BPLIST --events 5
```

Read the file — it doubles as a worked example for writing a client in
another language.
