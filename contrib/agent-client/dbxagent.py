#!/usr/bin/env python3
"""Reference client for the DOSBox-X agent control channel.

The wire protocol is one JSON object per line over a loopback TCP socket.
A request is ``{"id": N, "cmd": "...", "args": {...}}``; a response carries
the same ``id`` plus ``ok`` and either ``result`` or ``error``. Messages
without an ``id`` are server-initiated events.

Typical use::

    from dbxagent import DbxAgent

    with DbxAgent(portfile="dbxport.txt") as a:
        print(a.vm_version())
        a.log_subscribe()
        a.keyboard_type("DIR\\r")
        a.cpu_pause()
        print(a.debugger_command("BPLIST"))
        a.cpu_run()
        for ev in a.events(timeout=2.0):
            print(ev)

The module is deliberately small (no third-party deps, no asyncio) so it
can serve both as a hands-on driver and as a worked example for clients
written in other languages.
"""

from __future__ import annotations

import argparse
import json
import queue
import socket
import sys
import threading
import time
from typing import Any, Dict, Iterator, Optional


class AgentError(RuntimeError):
    """Raised when the server returns ``ok:false`` for a request."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class DbxAgent:
    """Synchronous request/response client with an async event queue.

    A background reader thread demultiplexes server messages: replies are
    routed back to the caller blocked in :meth:`call`, unsolicited events
    land in a thread-safe queue accessible via :meth:`events` /
    :meth:`next_event`.
    """

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: Optional[int] = None,
        portfile: Optional[str] = None,
        connect_timeout: float = 5.0,
    ) -> None:
        if port is None:
            if portfile is None:
                raise ValueError("must provide either port or portfile")
            with open(portfile, "r") as f:
                port = int(f.read().strip())
        self._sock = socket.create_connection((host, port), timeout=connect_timeout)
        self._sock.settimeout(None)
        self._reader = self._sock.makefile("rb")
        self._writer = self._sock.makefile("wb")

        self._lock = threading.Lock()
        self._next_id = 1
        self._pending: Dict[int, "threading.Event"] = {}
        self._replies: Dict[int, dict] = {}
        self._events: "queue.Queue[dict]" = queue.Queue()
        self._closed = False
        self._read_err: Optional[Exception] = None

        self._reader_thread = threading.Thread(
            target=self._read_loop, name="dbxagent-reader", daemon=True
        )
        self._reader_thread.start()

    # ---- Connection lifecycle ----------------------------------------

    def close(self) -> None:
        self._closed = True
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()

    def __enter__(self) -> "DbxAgent":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # ---- Request / response ------------------------------------------

    def call(self, cmd: str, *, timeout: float = 5.0, **args: Any) -> dict:
        """Send ``cmd`` with keyword ``args`` and block for the reply.

        Returns the ``result`` object from a successful reply. Raises
        :class:`AgentError` on ``ok:false``, :class:`TimeoutError` if no
        reply arrives within ``timeout``.
        """
        ev = threading.Event()
        with self._lock:
            mid = self._next_id
            self._next_id += 1
            self._pending[mid] = ev

        msg = {"id": mid, "cmd": cmd}
        if args:
            msg["args"] = args
        encoded = (json.dumps(msg, separators=(",", ":")) + "\n").encode("utf-8")
        try:
            self._writer.write(encoded)
            self._writer.flush()
        except OSError as e:
            with self._lock:
                self._pending.pop(mid, None)
            raise ConnectionError(f"send failed: {e}") from e

        if not ev.wait(timeout):
            with self._lock:
                self._pending.pop(mid, None)
            raise TimeoutError(f"no reply within {timeout}s for cmd={cmd!r}")

        with self._lock:
            reply = self._replies.pop(mid)
        if reply.get("ok"):
            return reply.get("result", {})
        err = reply.get("error", {})
        raise AgentError(err.get("code", "unknown"), err.get("message", ""))

    # ---- Events ------------------------------------------------------

    def next_event(self, timeout: Optional[float] = None) -> Optional[dict]:
        """Return the next queued event, or ``None`` on timeout."""
        try:
            return self._events.get(timeout=timeout)
        except queue.Empty:
            return None

    def events(self, timeout: Optional[float] = None) -> Iterator[dict]:
        """Yield events until ``timeout`` elapses with no further events.

        ``timeout=None`` blocks forever waiting for each event; ``0`` drains
        whatever has already arrived.
        """
        while True:
            ev = self.next_event(timeout=timeout)
            if ev is None:
                return
            yield ev

    # ---- Convenience wrappers ----------------------------------------

    def vm_version(self) -> dict:
        return self.call("vm.version")

    def debug_status(self) -> dict:
        """One-shot snapshot of the agent's observability state.

        Returns ``{cpu, cs_ip, cs, eip, instr_count, breakpoints, watches,
        probe, trace, cond_breakpoints}``:

        - ``cpu`` is ``"paused"`` or ``"running"``; ``instr_count`` is the
          cumulative cycle count.
        - ``breakpoints`` lists the debugger's breakpoints, each with a ``hits``
          counter and (when paused at it) ``bytes_now`` — so you can tell
          "fired" from "never reached" in one run.
        - ``watches`` has ``far`` / ``target`` / ``range`` (each a sentinel set
          with per-sentinel ``hits``; the array index is the ``which`` reported
          in events) and ``mem`` (the write-intercept watch).
        - ``probe`` is the ``cpu_probe`` point table with per-point ``hits``;
          ``trace`` is the ``cpu_trace_ring`` status; ``cond_breakpoints`` lists
          ``bp_set`` entries with their ``hits`` (reaches) and ``fires``.

        Counters and disassembly bytes are accurate only in a heavy-debug build;
        see the per-command notes.
        """
        return self.call("debug.status")

    def cpu_pause(self) -> dict:
        return self.call("cpu.pause")

    def cpu_run(self) -> dict:
        return self.call("cpu.run")

    def cpu_step(self) -> dict:
        """Single-step one instruction (trace into). The CPU must be paused.

        Returns ``{regs, cs_ip, insn}``: ``regs`` is the post-step register
        snapshot (same shape as ``regs_get``), ``cs_ip`` the new CS:EIP, and
        ``insn`` the disassembly (``{cs_ip, bytes, text}``) of the instruction
        now at CS:IP. Errors ``bad_state`` if the CPU is not paused.
        """
        return self.call("cpu.step")

    def cpu_step_over(self, timeout: float = 10.0) -> dict:
        """Step over one instruction, treating CALL/INT/LOOP/REP as one unit.

        Same ``{regs, cs_ip, insn}`` result shape as ``cpu_step``. For a
        CALL/INT/LOOP/REP the CPU resumes to a temporary breakpoint at the
        return address (you will see bp.hit / debugger.entered / state.paused
        events meanwhile); the reply still arrives on this request id, so the
        call blocks transparently. A larger default timeout covers the body
        run. Errors ``bad_state`` (not paused) or ``busy`` (one already in
        flight — wait for it, or cpu.pause to bail out).
        """
        return self.call("cpu.step_over", timeout=timeout)

    def cpu_probe(self, points: Optional[list] = None) -> dict:
        """Arm non-halting execution counters at a set of addresses.

        ``points`` is a list of ``"SEG:OFF"`` hex strings; each is counted every
        time the CPU executes there, at full speed and without halting — the
        answer to "is X on the path, and how often?". Pass ``[]`` or ``None`` to
        disarm. Returns ``{armed}`` (the number of points now armed); read the
        per-point ``hits`` back from ``debug_status``' ``probe`` array. Counts
        only in a heavy-debug build. Errors ``bad_args`` (too many points or a
        malformed ``SEG:OFF``).
        """
        return self.call("cpu.probe", points=list(points) if points else [])

    def cpu_trace_ring(self, enabled: bool = True, *, depth: Optional[int] = None,
                       seg: Optional[int] = None) -> dict:
        """Arm/disarm the rolling CS:IP trace ring.

        With ``enabled=True`` (the default) records the last ``depth``
        instructions (server default if omitted) into a ring buffer, optionally
        filtered to a single ``seg``. ``enabled=False`` stops recording but
        *retains* the ring so a later ``cpu_traceback`` still works. Returns
        ``{enabled, depth[, seg]}``. Records only in a heavy-debug build. Pair
        with ``cpu_traceback`` to answer "how did the CPU get here?".
        """
        args: dict = {"enabled": enabled}
        if depth is not None:
            args["depth"] = depth
        if seg is not None:
            args["seg"] = seg
        return self.call("cpu.trace_ring", **args)

    def cpu_traceback(self, count: Optional[int] = None) -> dict:
        """Dump the trace ring captured by ``cpu_trace_ring``.

        Returns ``{entries}`` where each entry is ``{cs_ip, bytes, text}``
        (disassembled), ordered oldest-first so the array reads most-recent-last.
        ``count`` caps how many of the most-recent entries to return (default:
        all currently held).
        """
        if count is None:
            return self.call("cpu.traceback")
        return self.call("cpu.traceback", count=count)

    def cpu_disasm(self, addr: str, count: int = 1) -> dict:
        """Disassemble ``count`` instructions starting at ``addr``.

        ``addr`` is ``"SEG:OFF"`` (hex); ``count`` is clamped to 64 server-side.
        Returns ``{insns}`` where each instruction is ``{cs_ip, bytes, text}`` —
        ``bytes`` is the exact opcode bytes (verifiable against ``mem_read``), so
        you never hand-decode. Errors ``bad_args`` on a malformed ``addr`` /
        non-positive ``count``.
        """
        return self.call("cpu.disasm", addr=addr, count=count)

    def state_save(self, slot: int) -> dict:
        """Snapshot the whole machine to save ``slot`` (0-99). CPU must be paused.

        Headless wrapper over DOSBox-X's savestate subsystem with the remark /
        confirmation dialogs suppressed and the slot argument always honoured
        (independent of any ``savefile=`` config). Returns ``{slot, name}``
        where ``name`` is the human-readable slot label. Errors ``bad_args``
        (slot out of range), ``bad_state`` (CPU not paused), ``unsupported``
        (guest memory over the 1 GB savestate limit), or ``io_error``.
        """
        return self.call("state.save", slot=slot)

    def state_restore(self, slot: int) -> dict:
        """Restore the whole machine from save ``slot`` (0-99). CPU must be paused.

        The companion to ``state_save``: restores in place and replies with the
        restored ``{regs, cs_ip, insn}`` (same shape as ``cpu_step``) plus
        ``slot`` and ``name``, so you immediately see where the machine will
        resume. Errors ``bad_args`` (slot out of range), ``bad_state`` (CPU not
        paused), ``not_found`` (slot empty), or ``unsupported``.
        """
        return self.call("state.restore", slot=slot)

    def bp_set(self, addr: str, *, if_: Optional[str] = None,
               do: Optional[list] = None, cont: bool = False) -> dict:
        """Arm a conditional / Nth-hit breakpoint with an optional on-hit macro.

        ``addr`` is "SEG:OFF" (hex). ``if_`` is an optional condition string —
        a single comparison ``operand [& mask] op operand`` where an operand is
        a register (ax/eax/al/cs/flags/...), ``hits`` (the BP's own reach
        count), a number (decimal, or 0x-hex; bare hex inside ``[seg:off]``), or
        a memory reference ``[byte|word|dword] [seg:off]``. Examples:
        ``"hits==150"``, ``"cx==0x151"``, ``"byte [es:di]==0x5A"``,
        ``"flags&0x40!=0"``. ``do`` is an optional list of read-only macro
        commands run atomically when the condition holds — each a dict
        ``{"cmd": ..., "args": {...}}`` with cmd in {regs.get, mem.read,
        cpu.disasm, cpu.traceback, debug.status}; their output is delivered in
        the ``bp.cond`` event's ``results``. ``cont=True`` runs the macro then
        auto-resumes instead of halting.

        Returns ``{bp_id, addr, seg, off, condition, macro_len, continue}``.
        When the BP fires you receive a ``bp.cond`` event (and, if it halts,
        the usual debugger.entered / state.paused). Heavy-debug build only —
        errors ``unsupported`` otherwise, or ``bad_args`` on a malformed addr /
        condition / macro entry. (`if` is a Python keyword, hence ``if_``.)
        """
        args: dict = {"addr": addr, "continue": cont}
        if if_ is not None:
            args["if"] = if_
        if do is not None:
            args["do"] = do
        return self.call("bp.set", **args)

    def bp_clear(self, bp_id: Optional[int] = None) -> dict:
        """Clear conditional breakpoints. With ``bp_id``, removes just that one
        (``not_found`` if it is gone); without, removes them all. Returns
        ``{cleared, remaining}``."""
        if bp_id is None:
            return self.call("bp.clear")
        return self.call("bp.clear", bp_id=bp_id)

    def bp_add(self, addr: Optional[str] = None, *, kind: str = "exec",
               int_: Optional[int] = None, ah: Optional[int] = None,
               al: Optional[int] = None) -> dict:
        """Add a real (CPU-halting) breakpoint and get back a stable handle.

        The typed replacement for ``debugger.command("BP …")`` / ``"BPINT …"``.
        Two kinds:

          * ``kind="exec"`` (default): execution breakpoint at ``addr`` =
            "SEG:OFF" (hex), e.g. ``bp_add("0824:6F8E")``.
          * ``kind="int"``: interrupt breakpoint on ``int_`` (0..255), optionally
            narrowed to a specific ``ah`` and ``al`` (each 0..255; ``al``
            requires ``ah``), e.g. ``bp_add(kind="int", int_=0x21, ah=0x09)``.

        Returns ``{bp_id, kind, ...}``. ``bp_id`` is a *stable* handle: it stays
        valid as other breakpoints are added/removed (unlike the BPoints
        iteration index the ``bp.hit`` event's ``bp_index`` reports). When this
        breakpoint trips you get a ``bp.hit`` event carrying the same ``bp_id``,
        then the usual ``debugger.entered`` / ``state.paused``. Pass the
        ``bp_id`` to :meth:`bp_del` to remove just this one.
        """
        if kind == "exec":
            if not addr:
                raise ValueError("bp_add(kind='exec') requires addr='SEG:OFF'")
            return self.call("bp.add", kind="exec", addr=addr)
        if kind == "int":
            if int_ is None:
                raise ValueError("bp_add(kind='int') requires int_=<0..255>")
            args: dict = {"kind": "int", "int": int_}
            if ah is not None:
                args["ah"] = ah
            if al is not None:
                args["al"] = al
            return self.call("bp.add", **args)
        raise ValueError("kind must be 'exec' or 'int'")

    def bp_list(self) -> dict:
        """List the real breakpoints. Returns ``{count, breakpoints}`` where each
        entry is ``{bp_id, index, kind, enabled, hits, …}`` — ``addr/seg/off``
        (plus ``bytes_now`` for exec BPs) for exec/mem kinds, or ``int`` for an
        interrupt BP. ``bp_id`` is the stable handle; ``index`` is the (shifting)
        iteration position that the legacy ``bp_index`` / ``BPDEL`` key on."""
        return self.call("bp.list")

    def bp_del(self, bp_id: Optional[int] = None, *, all: bool = False) -> dict:
        """Delete a real breakpoint by stable handle, or all of them.

        ``bp_del(7)`` removes the breakpoint with ``bp_id`` 7 (``not_found`` if
        it is gone). ``bp_del(all=True)`` removes *every* breakpoint — including
        the debugger's default INT3 trap — and is required to be explicit so an
        omitted id can never wipe the list by accident. Returns
        ``{deleted, remaining[, bp_id]}``."""
        if all:
            return self.call("bp.del", all=True)
        if bp_id is None:
            raise ValueError("bp_del requires a bp_id, or all=True")
        return self.call("bp.del", bp_id=bp_id)

    def regs_get(self) -> dict:
        """Snapshot all CPU registers in one structured reply.

        Keys: eax/ebx/ecx/edx/esi/edi/ebp/esp/eip (32-bit unsigned ints),
        cs/ds/es/fs/gs/ss (16-bit), and eflags. All values are decoded as
        Python ints — format as hex client-side if needed.
        """
        return self.call("regs.get")

    def regs_set(self, regs: Optional[dict] = None, **kwargs: Any) -> dict:
        """Write one or more CPU registers. The CPU must be paused.

        The write counterpart to ``regs_get`` — pass registers by the *same*
        names it returns (``eax``..``esp``, ``eip``, ``cs``/``ds``/``es``/``fs``/
        ``gs``/``ss``, ``eflags``), as keyword args or a dict, e.g.
        ``regs_set(eax=0xDEAD, cs=0x0824)`` or ``regs_set({"eip": 0x100})``.
        Values are ints (or hex strings). Only the registers you name change;
        the rest are left alone. Validation is all-or-nothing — a bad name /
        value / oversize segment rejects the whole request with ``bad_args`` and
        writes nothing. Segment writes use the real-mode form (they do not
        reload protected-mode descriptor limits, same as the debugger's ``SR``).

        Returns ``{set, regs}`` — the names applied plus the full post-set
        register snapshot (``regs_get`` shape), so you confirm the write with no
        follow-up. Errors ``bad_state`` (CPU not paused) or ``bad_args``.
        """
        fields: dict = dict(regs or {})
        fields.update(kwargs)
        if not fields:
            raise ValueError("regs_set requires at least one register")
        return self.call("regs.set", **fields)

    def mem_read(self, kind: str, addr: str, length: int) -> bytes:
        """Read up to 64 KB of guest memory.

        ``kind`` is ``"seg:off"``, ``"linear"``, or ``"physical"``; ``addr``
        is the hex address string (``"1000:0100"`` for seg:off, otherwise
        plain hex with or without ``0x`` prefix). Returns the raw bytes
        directly — the wire format's base64 is decoded for you.
        """
        import base64
        res = self.call("mem.read", kind=kind, addr=addr, len=length)
        return base64.b64decode(res["bytes"])

    def mem_write(self, kind: str, addr: str, data: bytes) -> dict:
        """Write raw bytes into guest memory (the store counterpart to ``mem_read``).

        ``kind`` is ``"seg:off"``, ``"linear"``, or ``"physical"``; ``addr`` is
        the hex address string (``"1000:0100"`` for seg:off, otherwise plain hex
        with or without ``0x``). ``data`` is a ``bytes``-like object — it is
        base64-encoded for you on the wire. Capped at 64 KB per call (chunk
        larger writes). ``physical`` bypasses paging; ``seg:off`` / ``linear``
        go through the paged path. Returns ``{"written": <n>}``.

        Does not require the CPU paused, but writing while the guest runs races
        with the guest's own stores — pause first when patching a live value.
        """
        import base64
        enc = base64.b64encode(bytes(data)).decode("ascii")
        return self.call("mem.write", kind=kind, addr=addr, bytes=enc)

    def keyboard_type(self, text: str) -> dict:
        return self.call("keyboard.type", text=text)

    def keyboard_tap(self, key: str) -> dict:
        return self.call("keyboard.tap", key=key)

    def keyboard_press(self, key: str) -> dict:
        return self.call("keyboard.press", key=key)

    def keyboard_release(self, key: str) -> dict:
        return self.call("keyboard.release", key=key)

    def debugger_command(self, text: str) -> dict:
        return self.call("debugger.command", text=text)

    def log_subscribe(self) -> dict:
        return self.call("log.subscribe")

    def log_unsubscribe(self) -> dict:
        return self.call("log.unsubscribe")

    def farcall_watch(self, target_seg: Optional[int] = None, *,
                      target_segs: Optional[list] = None) -> dict:
        """Watch FAR calls/jumps into one or more target segments.

        Single form: ``farcall_watch(0x1234)`` arms a sentinel on segment
        ``0x1234``. Set form: ``farcall_watch(target_segs=[0x1234, 0x5678])``
        arms a *set* (replaces any prior set; ``[]`` clears). Each
        ``farcall.transfer`` event carries a ``which`` index into that set, and
        per-sentinel ``hits`` show up in ``debug_status``' ``watches.far``.
        Called with no argument (or ``target_seg=None``) it clears the watch.
        """
        if target_segs is not None:
            return self.call("farcall.watch", target_segs=list(target_segs))
        return self.call("farcall.watch", target_seg=target_seg)

    def farcall_unwatch(self) -> dict:
        return self.call("farcall.unwatch")

    def cpu_watch_target(self, target_seg: Optional[int] = None,
                        target_off: Optional[int] = None, *,
                        targets: Optional[list] = None) -> dict:
        """Watch NEAR transfers to one or more exact CS:IP targets.

        Single form: ``cpu_watch_target(0x1234, 0x5678)``. Set form:
        ``cpu_watch_target(targets=["1234:5678", "1234:9ABC"])`` arms a *set* of
        ``"SEG:OFF"`` sentinels (replaces any prior set; ``[]`` clears). Each
        ``cpu.transfer`` event carries a ``which`` index into the set, with
        per-sentinel ``hits`` in ``debug_status``' ``watches.target``. Called
        with no argument (or ``target_seg=None``) it clears the watch.
        """
        if targets is not None:
            return self.call("cpu.watch_target", targets=list(targets))
        if target_seg is None:
            return self.call("cpu.watch_target", target_seg=None)
        return self.call("cpu.watch_target", target_seg=target_seg, target_off=target_off)

    def cpu_unwatch_target(self) -> dict:
        return self.call("cpu.unwatch_target")

    def cpu_watch_range(self, seg: Optional[int] = None, lo: Optional[int] = None,
                       hi: Optional[int] = None, *, ranges: Optional[list] = None) -> dict:
        """Watch entry into one or more [seg, lo..hi] ranges from outside.

        Single form: ``cpu_watch_range(seg, lo, hi)`` fires on the boundary
        crossing *into* ``[seg, lo..hi]`` (use this over ``cpu_watch_target``
        when the region has heavy intra-range traffic). Set form:
        ``cpu_watch_range(ranges=[{"seg":…,"lo":…,"hi":…}, …])`` arms a *set*
        (replaces any prior set; ``[]`` clears). Each ``cpu.range_enter`` event
        carries a ``which`` index into the set, with per-sentinel ``hits`` in
        ``debug_status``' ``watches.range``. Called with no argument (or
        ``seg=None``) it clears the watch.
        """
        if ranges is not None:
            return self.call("cpu.watch_range", ranges=list(ranges))
        if seg is None:
            return self.call("cpu.watch_range", seg=None)
        return self.call("cpu.watch_range", seg=seg, lo=lo, hi=hi)

    def cpu_unwatch_range(self) -> dict:
        return self.call("cpu.unwatch_range")

    def mem_watch(self, seg: int, lo: int, hi: int, *, size: Optional[int] = None,
                  when: Optional[dict] = None) -> dict:
        """Intercept guest writes into [seg, lo..hi] and report the storing
        instruction.

        A real write-intercept (not a value-change poll): on a matching store
        you get a ``mem.write`` event naming the instruction
        (``from_cs:from_ip`` + ``from_text``), the old/new value, and the access
        size. ``lo``/``hi`` are offsets within ``seg`` (matched against the
        write's starting linear address). ``size`` (1, 2, or 4) optionally
        filters by access width. ``when`` optionally filters by value — pass one
        of ``{"new_eq": v}``, ``{"new_ne_old": True}``, or
        ``{"new_and_mask_eq": {"mask": m, "value": v}}``. Returns
        ``{armed, seg, lo, hi, size, predicate}``. VGA-framebuffer writes report
        ``old: null`` (the read-back is skipped to avoid latch corruption). Use
        ``mem_unwatch`` to clear. Heavy-debug build records ``from_*``; the watch
        itself fires in any C_DEBUG build.
        """
        args: dict = {"seg": seg, "lo": lo, "hi": hi}
        if size is not None:
            args["size"] = size
        if when is not None:
            args["when"] = when
        return self.call("mem.watch", **args)

    def mem_unwatch(self) -> dict:
        return self.call("mem.unwatch")

    def screen_capture(self, raw: bool = True, timeout: float = 10.0) -> dict:
        """Trigger a PNG screenshot and block until written.

        Returns ``{"path": <abs PNG path>, "raw": <bool>}``. ``raw=True``
        (the default) captures the native VGA scan-line image; ``raw=False``
        captures the post-scaler render output. Requires ``[dosbox]
        captures=`` to be set in the configuration and the CPU to be
        running so the VGA render path can fire ``CAPTURE_AddImage`` /
        ``WriteRawImage``. Raises ``AgentError("bad_state")`` if the
        captures directory isn't configured, ``AgentError("busy")`` if a
        previous ``screen_capture`` call is still pending.
        """
        return self.call("screen.capture", timeout=timeout, raw=raw)

    # ---- Internals ---------------------------------------------------

    def _read_loop(self) -> None:
        try:
            for raw in self._reader:
                line = raw.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    # Malformed framing: log to stderr and keep going. The
                    # server is supposed to emit only valid lines, so this
                    # is purely defensive.
                    print(f"dbxagent: malformed line: {line!r}", file=sys.stderr)
                    continue
                if isinstance(msg, dict) and "id" in msg:
                    mid = int(msg["id"])
                    with self._lock:
                        ev = self._pending.pop(mid, None)
                        if ev is not None:
                            self._replies[mid] = msg
                    if ev is not None:
                        ev.set()
                else:
                    self._events.put(msg)
        except Exception as e:  # pragma: no cover — best-effort cleanup
            if not self._closed:
                self._read_err = e
        finally:
            # Unblock any waiters still pending so close() doesn't deadlock.
            with self._lock:
                for ev in list(self._pending.values()):
                    ev.set()
                self._pending.clear()


# ---- CLI demo --------------------------------------------------------


def _demo(args: argparse.Namespace) -> int:
    with DbxAgent(host=args.host, port=args.port, portfile=args.portfile) as a:
        ver = a.vm_version()
        print(f"connected: version={ver['version']} machine={ver['machine']} build={ver['build']}")
        if args.subscribe:
            a.log_subscribe()
        if args.type:
            res = a.keyboard_type(args.type)
            print(f"queued {res.get('queued')} bytes for paste")
        if args.cmd:
            res = a.debugger_command(args.cmd)
            print("--- debugger.command output ---")
            print(res.get("output", ""))
            print("--- end ---")
        if args.events:
            deadline = time.time() + args.events
            while time.time() < deadline:
                ev = a.next_event(timeout=max(0.1, deadline - time.time()))
                if ev is not None:
                    print(f"event: {ev}")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="DOSBox-X agent reference client")
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", type=int, help="connect to this TCP port directly")
    src.add_argument("--portfile", help="read port number from this file")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--type", help="paste this text into the guest")
    parser.add_argument("--cmd", help="run this debugger command and print the captured output")
    parser.add_argument("--subscribe", action="store_true", help="subscribe to log lines before doing anything else")
    parser.add_argument("--events", type=float, default=0.0, help="after the commands, listen for events for this many seconds")
    return _demo(parser.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
