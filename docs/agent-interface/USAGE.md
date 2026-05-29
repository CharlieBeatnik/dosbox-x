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
newline-delimited JSON. You can:

- Pause / resume the CPU.
- Type ASCII into the guest, or send raw key make/break events.
- Read all CPU registers in one structured reply (`regs.get`).
- Read up to 64 KB of guest memory at a time, base64-encoded (`mem.read`).
- Invoke any of the existing debugger commands whose output goes through
  `DEBUG_ShowMsg` (`BPLIST`, `BP`, `BPM`, `BPINT`, `BPDEL`, `EV`, `INT`,
  `IDT`, `GDT`, `KERN`, `CALLBACKS`, `DOS MCBS/DEVS/XMS/EMS`, `BIOS MEM`,
  `SELINFO`, …) via `debugger.command`.
- Subscribe to log lines and to events (`bp.hit`, `debugger.entered`,
  `state.paused`, `state.running`, `log.line`).

Not available — see "What is **not** capturable" below for the full list,
but the headlines are: anything the curses debugger draws into a pane
(register pane, disassembly pane, hex/ASCII data pane) is invisible to
the agent. `MEMDUMP` writes a file on disk rather than returning bytes
inline. Mouse input, screenshots, and typed breakpoint/step commands are
planned for Phase 2.

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
    print(a.vm_version())                       # {'version':'2026.05.02','machine':'vga','build':'heavy-debug'}
    a.keyboard_type("DIR\r")
    a.cpu_pause()
    regs = a.call("regs.get")                   # {'eax': 0, 'ebx': 0, ... 'eflags': 0x246}
    print(f"CS:EIP = {regs['cs']:04x}:{regs['eip']:08x}")
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

| Command             | Args                          | Result                              |
|---------------------|-------------------------------|-------------------------------------|
| `vm.version`        | none                          | `{version, machine, build}`         |
| `regs.get`          | none                          | All GPRs + segregs + EIP + EFLAGS   |
| `mem.read`          | `{kind, addr, len}`           | `{bytes, len}` — bytes base64       |
| `debugger.command`  | `{text}`                      | `{output, recognized}`              |
| `cpu.pause`         | none                          | `{}`                                |
| `cpu.run`           | none                          | `{}`                                |
| `keyboard.type`     | `{text}`                      | `{queued}` (byte count)             |
| `keyboard.press`    | `{key}`                       | `{}`                                |
| `keyboard.release`  | `{key}`                       | `{}`                                |
| `keyboard.tap`      | `{key}`                       | `{}`                                |
| `log.subscribe`     | none                          | `{subscribed: true}`                |
| `log.unsubscribe`   | none                          | `{subscribed: false}`               |
| `farcall.watch`     | `{target_seg}`                | `{watching, target_seg}`            |
| `farcall.unwatch`   | none                          | `{watching: false}`                 |
| `cpu.watch_target`  | `{target_seg, target_off}`    | `{watching, target_seg, target_off}`|
| `cpu.unwatch_target`| none                          | `{watching: false}`                 |
| `screen.capture`    | `{raw?: bool}`                | `{path, raw}` (deferred — see below) |

### `vm.version`

- `version` — string, e.g. `"2026.05.02"`.
- `machine` — lowercased `[machine]` enum: `hercules`, `cga`, `tandy`,
  `pcjr`, `ega`, `vga`, `amstrad`, `pc98`, `fmtowns`, `mcga`, `mda`,
  or `unknown`.
- `build` — `"debug"` or `"heavy-debug"`. (`"release"` is unreachable; the
  agent code is compiled out in release builds.)

Cheap. Useful as a connection sanity-check after the portfile appears.

### `regs.get`

Returns all general-purpose registers, segment registers, EIP, and EFLAGS
as one structured reply. No arguments.

```json
{
  "eax": 4275878552, "ebx": 0, "ecx": 0, "edx": 0,
  "esi": 0, "edi": 0, "ebp": 0, "esp": 65520,
  "eip": 256,
  "cs": 4096, "ds": 4096, "es": 4096,
  "fs": 0, "gs": 0, "ss": 4096,
  "eflags": 582
}
```

All values are unsigned integers (JSON numbers). 32-bit fields for the
GPRs and EIP, 16-bit for the segment registers. Use `f"{regs['cs']:04x}"`
to print as hex.

This is the supported replacement for the `R` and `R EAX` commands many
clients reach for — there is no `R` handler in `ParseCommand`, and the
curses register pane is drawn directly to ncurses windows (not capturable
through the agent). Prefer `regs.get` over the legacy `EV <register>`
expression-reader, which only returns one register per call.

### `mem.read`

Read up to 64 KB of guest memory at a time. Returns base64-encoded bytes.

```json
{"id": 1, "cmd": "mem.read",
 "args": {"kind": "seg:off", "addr": "1000:0100", "len": 256}}
```

- `kind` (required, string) — one of:
  - `"seg:off"` — real-mode address. `addr` must be `"SEG:OFF"` in hex.
    Translated to linear = `seg<<4 + off`, then read through the paging
    layer (so it works in both real and protected mode).
  - `"linear"` — paged/virtual linear address. `addr` is hex. Goes through
    the CPU's TLB; respects the current page tables when paging is on.
  - `"physical"` — bypasses paging entirely. `addr` is hex, treated as a
    direct physical RAM offset. Reads past end-of-memory return `0xFF`.
- `addr` (required, string) — hex, with or without `0x` prefix.
- `len` (required, integer) — 0 to 65536. Larger reads must be chunked.

Reply: `{"bytes": "<base64>", "len": <N>}` — `len` echoes the requested
length; `bytes` is `base64.b64decode(...)` away from raw bytes.

```python
import base64
res = a.call("mem.read", kind="seg:off", addr="0040:0017", len=2)
keyboard_flags = base64.b64decode(res["bytes"])  # BIOS data area
```

This is the supported replacement for `MEMDUMP` (which writes
`MEMDUMP.TXT` to DOSBox-X's cwd) and the curses `D`/`DV`/`DP` data panes
(which render into invisible ncurses windows). Use `mem.read` for any
inline byte access.

### `debugger.command`

Pass any text the curses debugger would accept (`BPLIST`, `BP CS:IP`,
`BPINT 21 4C`, `BPM RW 1000:0`, `BPDEL 3`, `EV EAX`, `INT 21 4C00`,
`IDT`, `GDT`, `CALLBACKS`, `KERN`, `DOS MCBS`, `BIOS MEM`, …) and read
back whatever `DEBUG_ShowMsg` produces.

- `text` (required, string) — the debugger command line.
- `output` (string) — captured `DEBUG_ShowMsg` output. **Will be empty
  for commands that don't emit through `DEBUG_ShowMsg`** — see "What is
  not capturable" below.
- `recognized` (bool) — `ParseCommand`'s return value. `false` means
  `ParseCommand` rejected the command name; the agent still replies OK.
  Note: many recognised commands also return `false` from `ParseCommand`
  (it's not a strict success indicator) — trust `output` over this flag.

Use this for breakpoint management (`BPLIST`, `BP*`, `BPDEL`), interrupt
inspection (`INT`), descriptor tables (`IDT`, `GDT`), DOS internals
(`DOS MCBS/DEVS/XMS/EMS/FNKEY`, `BIOS MEM`, `CALLBACKS`, `KERN`), and
the `EV` expression evaluator. **Do not** use it for register dumps
(no `R` command exists) or memory dumps (`MEMDUMP` writes a file). Use
`regs.get` / `mem.read` instead.

### `cpu.pause` / `cpu.run`

`cpu.pause` flips the emulator into the debugger loop (headless — no curses
window pops). `cpu.run` resumes. Both reply immediately with `{}`; watch
`state.paused` / `state.running` events for the actual transitions.

`regs.get` and `mem.read` are safe whether the CPU is running or paused
(they snapshot in-place). `debugger.command` is safe for inspection
commands (`BPLIST`, `EV`, `IDT`, `GDT`, …) at any time; commands that
*mutate* loop state (`RUN`, `RUNWATCH`, `T`/`P` step) should only be
issued while paused.

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

### `farcall.watch` / `farcall.unwatch`

Watch for CPU far transfers (`CALL FAR`, `JMP FAR`, `RETF`) whose
destination CS equals a sentinel segment. Use this to identify the
*caller* when something lands at an unexpected `CS:IP` — set the
sentinel, run, wait for the first `farcall.transfer` event, and the
event tells you which instruction in which source location did the
transfer.

- `target_seg` (required) — `u16` decimal number, or hex string with or
  without `0x` prefix (`"483C"`, `"0x483C"`, `18492` all mean the same
  segment).
- Pass `target_seg: null` to clear the watch (equivalent to
  `farcall.unwatch`).
- Single-sentinel for now; calling `farcall.watch` again replaces the
  previous sentinel.

Reply: `{watching: true|false, target_seg: <u16>}`. Idempotent — calling
again with the same segment is a no-op.

Emits a `farcall.transfer` event per matching transfer:

```json
{"event": "farcall.transfer",
 "target_seg": 18492, "target_off": 4068,
 "from_cs": 2084, "from_ip": 24576,
 "kind": "call_far_direct"}
```

`kind` is one of: `"call_far_direct"` (opcode `0x9A`),
`"call_far_indirect"` (`0xFF /3`), `"jmp_far_direct"` (`0xEA`),
`"jmp_far_indirect"` (`0xFF /5`), `"retf"` (`0xCA` / `0xCB`),
`"iret"` (`0xCF`), or one of the interrupt-source labels listed in the
**Interrupt-sourced transfers** section below (`"int3"`, `"int_sw"`,
`"into"`, `"int_hw"`, `"int_exception"`, `"int_step_trap"`,
`"int_nmi"`, `"int_icebp"`).

`from_cs` / `from_ip` are the source instruction's CS:IP (before the
transfer); `target_seg` / `target_off` are the destination. For RETF
the target is read from the stack frame the RETF pops, which is the
authoritative answer to "who's about to return here?"

The watch is a single u16 compare per far transfer (only on the handful
of opcodes listed above, plus every interrupt dispatch), so it has
negligible cost; you can leave it set across long-running guest code.
The intended workflow is "set before the suspect window, drain events
while reproducing, unwatch afterwards".

### `screen.capture`

Trigger DOSBox-X's existing screenshot path and return the absolute
path of the written PNG. Reuses the same encoder the keyboard-mapper
hotkeys (`Host+P`, `Host+Ctrl+P`) drive, so the captured pixels are
identical to what those produce.

- `raw` (optional, bool, default **true**) — `true` writes the raw VGA
  scan-line capture (native resolution, true palette, no output scaler,
  filename suffix `.raw1.png`); `false` writes the cooked render-output
  capture (post-scaler, `.png`). Default raw keeps pixel offsets honest
  for RE / shift analysis. Both end up as standard PNGs.
- The `[dosbox] captures=` config setting must be non-empty (the
  command returns `bad_state` otherwise). Files are auto-numbered:
  `<lowercased running program>_NNN[.raw1].png` under that directory.

**The reply is deferred.** `screen.capture` returns no immediate reply
from the dispatcher. The screenshot encoder runs on the next VGA frame;
once `fclose` returns the agent sends both:

1. The reply for your `id`: `{"path": "<abs>", "raw": <bool>}`.
2. A `screen.captured` event with the same fields.

This means **the CPU must be running** for the capture to land — the
render path doesn't fire while the CPU is paused in the debugger. The
default `dbxagent.call("screen.capture", ...)` blocks transparently
until the reply arrives, so callers don't need to handle the timing.
Typical round-trip is ~30ms on a clean repro.

```python
res = a.call("screen.capture")              # raw=True by default
img_path = pathlib.Path(res["path"])         # absolute path on disk
data = img_path.read_bytes()                 # PNG bytes
```

**`raw=true` requires an animating screen.** The raw VGA capture is
driven by `VGA_DrawRawLine`, which only fires while the VGA emulator
is actively drawing scanlines. On a static screen (e.g. a paused
title screen or a level-load splash with no animation) DOSBox-X's
on-demand renderer skips redrawing unchanged frames, so the raw
pipeline waits indefinitely for scanlines that never arrive. The
agent enforces a 2-second wall-clock deadline (measured with
`steady_clock`, so it fires even while the CPU is paused) — after
that the pending request gets a `timeout` error and the slot is
freed. For static screens **use `raw=false`** (the cooked render
path runs every frame regardless of changes, so it captures static
screens fine; the trade-off is that the output may have the scaler
/ aspect correction applied).

Errors:
- `bad_args` — `raw` is set to a non-bool value.
- `bad_state` — `[dosbox] captures=` is empty/unset.
- `busy` — another `screen.capture` is still pending; wait for its
  reply or `screen.captured` event before issuing another.
- `timeout` — the capture didn't complete within 2s (the static-screen
  failure mode above, or the CPU was paused so no rendering happened).
  Retry with `raw=false` for static screens.

### `cpu.watch_target` / `cpu.unwatch_target`

The NEAR-transfer sibling of `farcall.watch`. Use this when you know the
exact `(CS, IP)` the CPU is landing at and want to identify the *NEAR*
instruction that jumped/called/returned there. Covers every direct and
indirect NEAR transfer plus taken `Jcc` / `LOOP` / `JCXZ` and `RETN`.
NEAR transfers are much more common than FAR, so the sentinel is a
`(seg, off)` pair (not just a segment) — only the exact destination
fires an event.

- `target_seg`, `target_off` (both required when setting) — `u16`
  decimal numbers or hex strings, same convention as `farcall.watch`.
- Pass `target_seg: null` to clear (same as `cpu.unwatch_target`).
- Single sentinel — calling again replaces the previous one.

Reply: `{watching: true|false, target_seg, target_off}`.

Emits a `cpu.transfer` event per matching transfer:

```json
{"event": "cpu.transfer",
 "target_seg": 2084, "target_off": 59598,
 "from_cs": 2084, "from_ip": 8512,
 "kind": "jmp_near_indirect"}
```

`kind` is one of:

| `kind`                | Opcode group                                          |
|-----------------------|-------------------------------------------------------|
| `call_near_direct`    | `0xE8` (rel16, plus 32-bit rel32 under `0x66` prefix) |
| `jmp_near_direct`     | `0xE9` (rel16, plus 32-bit rel32 under `0x66` prefix) |
| `jmp_short`           | `0xEB` (rel8)                                         |
| `call_near_indirect`  | `0xFF /2` (16-bit and 32-bit operand size)            |
| `jmp_near_indirect`   | `0xFF /4` (16-bit and 32-bit operand size)            |
| `retn`                | `0xC3` (16-bit and 32-bit operand size)               |
| `retn_imm`            | `0xC2 imm16` (16-bit and 32-bit operand size)         |
| `jcc_short`           | `0x70..0x7F` (taken) — also `LOOP`/`LOOPZ`/`LOOPNZ`/`JCXZ` |
| `jcc_near`            | `0x0F 0x80..0x8F` (taken, 386+, 16-bit and 32-bit displacement) |
| `retf`                | `0xCA` / `0xCB` (popped CS happens to equal current CS) |
| `iret`                | `0xCF` (popped CS happens to equal current CS)        |
| `int3`                | `0xCC` software int 3                                 |
| `int_sw`              | `0xCD ib` software interrupt                          |
| `into`                | `0xCE` overflow trap (when `OF=1`)                    |
| `int_hw`              | PIC-injected hardware interrupt (`CPU_HW_Interrupt`)  |
| `int_exception`       | CPU fault dispatch (`CPU_Exception` — illegal opcode, #GP, #PF, ...) |
| `int_step_trap`       | `TF=1` single-step trap (`CPU_DebugException`)        |
| `int_nmi`             | NMI (`CPU_NMI_Interrupt`)                             |
| `int_icebp`           | `0xF1` ICEBP                                          |
| `callback_far`        | C++-side `CALLBACK_RunRealFar` direct CS:IP dispatch (DOS device strategy/interrupt, XMS callback, shell exec, etc.) |
| `callback_far_int`    | C++-side `CALLBACK_RunRealFarInt` direct CS:IP dispatch (INT 16 wrap, etc.) |
| `hbp_exec`            | Heavy-debug execution fallback: CPU reached the watched (CS, IP) via *some* transfer that the opcode/`CPU_Interrupt`/`CALLBACK_*` hooks didn't catch. `from_cs`/`from_ip` are the previous instruction's address (the source of the unhooked transfer). |

Conditional jumps only emit when the branch is **taken** (same convention
as the FAR Jcc hooks). For all opcode-driven NEAR kinds `from_cs` equals
`target_seg` — a NEAR transfer can't change CS. For the `int_*` / `retf` /
`iret` kinds the target CS *can* differ from the source CS; the entry
arrives via the same NEAR sentinel `(seg, off)` match, because every
interrupt dispatch and every same-CS far return is also a transfer to a
specific `(CS, IP)`. `from_ip` is the post-operand IP (i.e., the return
address) of the instruction that branched, so disassembling backwards
from `from_ip` finds the source instruction. For interrupts `from_ip` is
the IP at which the interrupt was taken (which is also the return address
pushed onto the stack).

Cost is one `(seg, off)` compare per NEAR transfer (and per interrupt
dispatch / same-CS far return) when the watch is set; the test compiles
to a flag check plus two u16 compares, which is cheap enough to leave in
place across long-running guest code.

#### Interrupt-sourced transfers

Both `cpu.watch_target` and `farcall.watch` also fire on every interrupt
dispatch the CPU performs — `INT 3` / `INT N` / `INTO` / `ICEBP`,
PIC-injected hardware interrupts (`INT 8`, `INT 9`, …), CPU exceptions
(illegal opcode, #GP, #PF, …), the single-step trap when `TF=1`, and
NMI. The `kind` field tells you which source raised the interrupt; see
the table above. This means:

- Set `farcall.watch target_seg=<game_cs>` and you will see every timer
  ISR dispatch to that segment (one `kind="int_hw"` event per ~55 ms),
  plus every `INT 21h` / `INT 10h` / etc. whose IVT entry points into
  that segment.
- Set `cpu.watch_target target_seg=<game_cs> target_off=<entry>` and
  you will see exactly the interrupt that landed at that handler entry
  point — useful for pinning down "who's the caller" when the bad
  transfer is a software `INT N` rather than a `CALL`/`JMP`.

If the same dispatch is converted to an exception mid-flight (e.g., the
gate descriptor is missing, so `CPU_Exception(#GP)` runs instead), only
the exception event fires — the suppressed outer event would otherwise
double-count the same landing.

`callback_far` and `callback_far_int` fire when DOSBox-X's C++ code
synthetically sets `CS:IP` to invoke a guest routine without going
through any opcode (DOS device strategy/interrupt entry points, XMS
callbacks, `INT 16` wrap, shell exec, etc.). Examples of paths these
catch: `dos_devices.cpp` dispatching a CHARACTER device's strategy
routine; `bios_keyboard.cpp:INT16_Handler_Wrap`; `shell` running
COMMAND.COM stop callbacks. None of these touch a hooked opcode.

## Events

| Event              | Fields                          | When                                                            |
|--------------------|---------------------------------|-----------------------------------------------------------------|
| `state.running`    | none                            | CPU resumed (after `cpu.run`, RUN, RUNWATCH, or auto-resume)    |
| `state.paused`     | none                            | CPU halted (paired with `debugger.entered`)                     |
| `bp.hit`           | `{seg, off, bp_index[, from_cs, from_ip]}` | Just before the debugger entry that a breakpoint triggered. In heavy-debug builds `from_cs`/`from_ip` carry the previous instruction's CS:IP (the source of the transfer to `seg:off`). |
| `debugger.entered` | `{reason}`                      | After `bp.hit`, or any other debugger entry                     |
| `log.line`         | `{text}`                        | While subscribed                                                |
| `farcall.transfer` | `{target_seg, target_off, from_cs, from_ip, kind}` | A `CALL FAR` / `JMP FAR` / `RETF` / `IRET` / interrupt dispatch whose target CS matched the active `farcall.watch` sentinel |
| `cpu.transfer`     | `{target_seg, target_off, from_cs, from_ip, kind}` | A NEAR `CALL`/`JMP`/taken `Jcc`/`RETN` / same-CS `RETF` / `IRET` / interrupt dispatch whose `(CS, IP)` matched the active `cpu.watch_target` sentinel |
| `screen.captured`  | `{path, raw}`                   | A `screen.capture` PNG was fully written. Same payload as the command's deferred reply. Fires even for screenshots triggered by keyboard mapper (`Host+P` etc.), so subscribe-and-filter on `path` if you only care about your own requests. |
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

## What is *not* capturable

The Phase-1 surface deliberately covers what `ParseCommand` can route
through `DEBUG_ShowMsg` plus the two structured queries `regs.get` /
`mem.read`. Several things the curses debugger does **cannot** be reached
through the agent:

- **Curses pane content.** The register pane (`DrawRegisters`),
  disassembly window, hex/ASCII data view, variables pane, and FPU stack
  pane all draw directly to ncurses windows. Their contents never go
  through `DEBUG_ShowMsg` and so produce empty `output` when their
  setter command is invoked over the agent. Use `regs.get` / `mem.read`
  instead. There is no Phase-1 disassembly command; if you need one,
  Phase 2 adds `disasm` via `DasmI386` directly.
- **Pane-setter commands.** `D`, `DV`, `DP` (data overview),
  `C` (code overview), `SHOWWIN`/`HIDEWIN`, `MOVEWINDN`/`MOVEWINUP`
  only mutate which curses window shows what. They reply with a short
  status string (`"DEBUG: Set data overview to F000:D106"`) — the
  actual hex dump never crosses the agent boundary.
- **File-output commands.** `MEMDUMP` and `MEMDUMPBIN` write
  `MEMDUMP.TXT` / `MEMDUMP.BIN` in DOSBox-X's current working directory.
  `LOG`/`LOGS`/`LOGL`/`LOGC` write CPU trace logs to disk. The agent
  sees only a one-line "success" message. Use `mem.read` for byte
  access.
- **Single-stepping with structured output.** `T`/`P` step commands
  exist in `ParseCommand`, but the resulting register/code-view refresh
  goes to the curses panes. Phase 2's `cpu.step` / `cpu.step_over`
  will return structured state.
- **The `R` register-dump command.** It doesn't exist in
  `ParseCommand` at all (the curses key `R` is a UI handler). Use
  `regs.get`.
- **Anything that needs the curses window to be visible.** Pages of
  scrollable content (`BPLIST` over many entries, long `IDT`/`GDT`
  listings) are still fully captured — the paging is purely a curses
  display concept and `DEBUG_ShowMsg` writes each line.

If you discover something else that emits to a pane rather than
`DEBUG_ShowMsg`, that's a candidate for a Phase-2 typed command. File
an issue.

### BPM on VGA memory (A0000–BFFFF) — partial coverage

`BPM A000:xxxx` (or any `BPM` in the VGA memory window) is a
**single-byte value-change watch**, not a write intercept. It works
fine for normal RAM, but for VGA memory it carries two caveats that
make it unreliable as a way to find "who's writing to the framebuffer":

1. **It only fires when the byte the BPM reads back changes.** The
   per-instruction check calls `mem_readb_checked` to compare the
   current byte against the stored value. In planar VGA modes (mode
   0x0D, 0x0E, 0x10, 0x12) the readback returns the byte from the
   current Read-Map-Select plane — so a write that affects only other
   planes leaves the BPM read unchanged and the BPM never fires.
   Idempotent writes (writing the same value back) also produce no
   change and so produce no event.
2. **Even when it fires, it fires at the *next* instruction boundary
   after the change**, not at the writer's `CS:IP`. For a single
   `mov` write the difference is the next instruction; for a `rep
   stosb` filling the whole framebuffer the BPM only fires once the
   `rep` retires, so the reported `CS:IP` is whatever follows the
   `rep`, not the writer site.

For the "find the code writing to the framebuffer" use case the
robust workaround is to grep the disassembly for `mov ax, 0A000h` /
`mov ax, 0xa0` literals (or whatever segment constant the game's
source uses), set a regular `BP` on each candidate site, and run.
`cpu.watch_target` works too if you only care about one specific
landing IP. A proper write-intercept BPM that hooks the VGA page
handlers' `writeb` would be a Phase-3 addition.

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

### Set a code breakpoint, run, wait for it, inspect state

```python
import base64

a.debugger_command("BP CS:0100")        # breakpoint at current CS:0100
a.cpu_run()
while True:
    ev = a.next_event(timeout=10.0)
    if ev is None: raise TimeoutError("breakpoint never hit")
    if ev.get("event") == "bp.hit":
        print(f"hit bp {ev['bp_index']} at {ev['seg']:04x}:{ev['off']:08x}")
        break
# CPU is now paused. Use the typed snapshot commands to inspect state.
regs = a.call("regs.get")
print(f"EAX={regs['eax']:08x} EBX={regs['ebx']:08x} CS:EIP={regs['cs']:04x}:{regs['eip']:08x}")
mem = a.call("mem.read", kind="seg:off",
             addr=f"{regs['ds']:04x}:{regs['esi']:04x}", len=32)
print(base64.b64decode(mem["bytes"]).hex())
# Or list breakpoints via the text passthrough:
print(a.debugger_command("BPLIST")["output"])
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
print(a.call("regs.get"))
print(a.debugger_command("BPLIST")["output"])
a.cpu_run()
```

### Find the caller of a far transfer to a known segment

When the CPU lands at an unexpected `CS:EIP` (e.g. mid-data execution
after a corrupted `CALL FAR`), set a far-transfer watch on the
destination segment *before* the bad transfer happens, then drain
events while reproducing:

```python
a.call("farcall.watch", target_seg="483C")    # or 0x483C, or 18492
a.keyboard_tap("space")                       # trigger whatever reproduces
while True:
    ev = a.next_event(timeout=10.0)
    if ev is None: raise TimeoutError("no far-transfer to 483C in 10s")
    if ev.get("event") == "farcall.transfer":
        print(f"{ev['kind']} from {ev['from_cs']:04x}:{ev['from_ip']:04x} "
              f"-> {ev['target_seg']:04x}:{ev['target_off']:04x}")
        break
a.call("farcall.unwatch")
# now snapshot regs/stack and dig
```

The first matching event names the *source* of the bad far transfer —
that's the instruction to start working backwards from. The cost is one
u16 compare per far transfer (only on the listed opcodes), so leaving
the watch set across long runs is fine.

### Hold Ctrl while pressing C

```python
a.keyboard_press("leftctrl")
a.keyboard_tap("c")
a.keyboard_release("leftctrl")
```

## Gotchas

- **Wait for the portfile.** The listener is started during DOS init, not
  at process spawn. Poll for the file's existence before connecting.
  The file is written atomically (write+rename) so once it appears,
  reading it returns a complete port number — no need to retry on empty
  reads.
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
  brittle parsers on top of it; for registers and memory use the typed
  `regs.get` / `mem.read` commands, and for everything else wait for
  Phase 2 to add a typed equivalent.
- **`keyboard.type` is ASCII only.** Non-ASCII input is silently mangled
  by the paste driver. For non-ASCII or modifier combos, use `press` /
  `release` / `tap` with named keys.
- **`bp_index` is not stable.** If you `BPDEL 2`, the indices of all
  later breakpoints shift down. Re-read `BPLIST` after any mutation.
- **BPs added via `debugger.command` activate immediately.** The pause-
  set-resume pattern below still works, but is no longer required —
  setting `BP CS:IP`, `BPM`, `BPINT`, `BPPM`, `BPLM`, or `FM` while the
  CPU is running takes effect on the very next instruction fetch / int
  vector / memory access. Earlier builds had a bug where BPs added
  while running stayed inactive until the next explicit `cpu.run` cycle;
  if you wrote driver code that pauses, sets, then runs solely to work
  around that, the pause is now optional.
- **`agent.overflow` means lines were dropped.** Outbox cap is 1 MB
  per client. If you see this, you're producing events faster than you're
  reading them — drain `events()` more aggressively, or unsubscribe from
  `log.line` if you don't need it.
- **Release builds have no agent at all.** No listener, no port file, no
  CLI flags accepted. You need a Debug build.

## Reference client

`contrib/agent-client/dbxagent.py` (stdlib only). Sync `call(cmd, **args)`
plus typed helpers (`vm_version`, `cpu_pause`, `cpu_run`, `regs_get`,
`mem_read`, `keyboard_type`, `keyboard_tap`, `keyboard_press`,
`keyboard_release`, `debugger_command`, `log_subscribe`,
`log_unsubscribe`). `mem_read` decodes the base64 for you and returns
raw `bytes`. Events arrive via `next_event(timeout)` / `events(timeout)`.
Reader is a daemon thread; closing the socket unblocks it; use as a
context manager (`with DbxAgent(...) as a:`).

CLI demo:

```
python contrib/agent-client/dbxagent.py --portfile dbxport.txt \
    --type "DIR\r" --cmd BPLIST --events 5
```

Read the file — it doubles as a worked example for writing a client in
another language.
