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

    def regs_get(self) -> dict:
        """Snapshot all CPU registers in one structured reply.

        Keys: eax/ebx/ecx/edx/esi/edi/ebp/esp/eip (32-bit unsigned ints),
        cs/ds/es/fs/gs/ss (16-bit), and eflags. All values are decoded as
        Python ints — format as hex client-side if needed.
        """
        return self.call("regs.get")

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

    def cpu_watch_range(self, seg: int, lo: int, hi: int) -> dict:
        """Arm cpu.watch_range — fires on entry into [seg, lo..hi] from outside."""
        return self.call("cpu.watch_range", seg=seg, lo=lo, hi=hi)

    def cpu_unwatch_range(self) -> dict:
        return self.call("cpu.unwatch_range")

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
