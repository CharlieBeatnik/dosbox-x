#!/usr/bin/env python3
"""Live-fire test for the agent breakpoint / Trap_Run / farcall.watch fixes.

Runs `bptest.com` inside a real DOSBox-X instance via the agent control
channel and verifies all five scenarios observed end-to-end.  It exercises
the CPU-core code paths that the GTest suite cannot reach because the
in-process test harness bypasses the CPU core entirely.

Scenarios (each preceded by `BPDEL 0 *` to start from a clean BP list):

  1. BP at landmark2 (NOP inside a tight LOOP).  Pre-fix the BP added
     mid-run stayed inactive and the bp.hit never arrived.  Post-fix the
     event must arrive within a generous timeout.

  2. BPINT 06 on the illegal-opcode-triggered INT 6.  Same activation
     story; also confirms CheckIntBreakpoint sees the exception-dispatched
     interrupt the same way it sees a software `INT 6` instruction.

  3. BPM at DS:0500h (memory write watch).  Same activation story.

  4. BP at landmark_tf_next AFTER setting TF=1 with PUSHF/OR/POPF.
     Pre-fix the BP fired but Trap_Run unconditionally injected an extra
     DBINT_STEP that re-vectored CS:IP into the INT 1 BIOS stub before
     the agent reported state.paused.  Post-fix `regs.get` must report
     CS:IP equal to landmark_tf_next, not the IVT[1] handler.

  5. `farcall.watch target_seg=0x9000` then CALL FAR DWORD PTR ds:[farptr].
     The first matching `farcall.transfer` event must carry the
     CALL-site CS:IP, kind=`call_far_indirect`, target 9000:0010.

Usage:
    python tests/agent_live/test_bps.py [--dosbox PATH] [--keep-tmp]

Exit code 0 if every scenario passes; 1 otherwise.
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
from typing import Optional


# --- helpers --------------------------------------------------------------

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_DOSBOX = REPO_ROOT / "bin" / "x64" / "Debug" / "dosbox-x.exe"
BPTEST_COM = pathlib.Path(__file__).resolve().with_name("BPTEST.COM")

# The DOSBox-X agent ships a reference Python client; reuse it rather than
# rolling our own JSON framing.
sys.path.insert(0, str(REPO_ROOT / "contrib" / "agent-client"))
from dbxagent import DbxAgent, AgentError  # noqa: E402


SIGNATURE_ADDR = 0x4F0
SIGNATURE_MAGIC = 0xBEEF


def _alloc_port() -> int:
    """Return a TCP port that was free at the moment of the call.

    DOSBox-X picks an ephemeral port itself when given `-agent-listen
    127.0.0.1:0`, but we still need to know which one to instruct the
    Python client to connect to.  The agent writes that port to a file
    we pass via -agent-portfile; we just poll it.  This helper is only
    used to find a free port for the alternative explicit-port mode (not
    needed in this driver but kept for symmetry with other harnesses).
    """
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _wait_for_portfile(path: pathlib.Path, timeout: float = 30.0) -> int:
    """Block until DOSBox-X writes the agent port number and return it."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            text = path.read_text(encoding="ascii", errors="replace").strip()
            if text.isdigit():
                return int(text)
        time.sleep(0.1)
    raise TimeoutError(f"DOSBox-X never wrote {path}")


def _read_signature(agent: DbxAgent, timeout: float = 20.0) -> int:
    """Poll the IBM-documented 'intra-application communication area' at
    physical 0x4F0..0x4F3 until BPTEST.COM's setup stamps the magic word
    and its CS there.  Return the CS we found."""
    deadline = time.monotonic() + timeout
    last_err: Optional[Exception] = None
    while time.monotonic() < deadline:
        try:
            buf = agent.mem_read("physical", f"{SIGNATURE_ADDR:X}", 4)
        except AgentError as exc:
            last_err = exc
            time.sleep(0.1)
            continue
        magic, cs = struct.unpack("<HH", buf)
        if magic == SIGNATURE_MAGIC:
            return cs
        time.sleep(0.1)
    if last_err:
        raise last_err
    raise TimeoutError(
        f"BPTEST.COM never stamped signature 0x{SIGNATURE_MAGIC:04X} "
        f"at physical 0x{SIGNATURE_ADDR:X}"
    )


def _drain_events(agent: DbxAgent) -> None:
    while agent.next_event(timeout=0.05) is not None:
        pass


def _wait_for_event(
    agent: DbxAgent, name: str, timeout: float = 8.0
) -> dict:
    """Drain events until one with `event == name` arrives, or raise."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = max(0.05, deadline - time.monotonic())
        ev = agent.next_event(timeout=remaining)
        if ev is None:
            continue
        if ev.get("event") == name:
            return ev
    raise TimeoutError(f"no {name!r} event within {timeout}s")


def _bp_reset(agent: DbxAgent) -> None:
    """Pause CPU, clear all BPs, drain events.  Caller resumes."""
    agent.cpu_pause()
    _drain_events(agent)
    agent.debugger_command("BPDEL 0 *")
    _drain_events(agent)


# --- DOSBox-X launch ------------------------------------------------------


def _write_conf(workdir: pathlib.Path) -> pathlib.Path:
    """Generate a minimal dosbox-x.conf that mounts the test dir as C: and
    auto-launches BPTEST.COM.  After BPTEST exits, COMMAND.COM hits the
    `exit` line and DOSBox-X shuts down cleanly."""
    conf_text = textwrap.dedent(
        f"""\
        # Generated by tests/agent_live/test_bps.py
        [sdl]
        output=surface
        autolock=false
        windowresolution=640x400

        [dosbox]
        machine=svga_s3
        memsize=16

        [cpu]
        core=normal
        cputype=486
        cycles=fixed 5000

        [autoexec]
        mount C "{workdir}"
        C:
        BPTEST.COM
        exit
        """
    )
    conf_path = workdir / "_test.dosbox-x.conf"
    conf_path.write_text(conf_text, encoding="utf-8")
    return conf_path


def _launch_dosbox(
    dosbox_exe: pathlib.Path,
    workdir: pathlib.Path,
    portfile: pathlib.Path,
) -> subprocess.Popen:
    conf = _write_conf(workdir)
    args = [
        str(dosbox_exe),
        "-conf", str(conf),
        "-agent-listen", "127.0.0.1:0",
        "-agent-portfile", str(portfile),
        "-fastlaunch",
    ]
    return subprocess.Popen(
        args,
        cwd=str(workdir),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


# --- scenario implementations ------------------------------------------------


class Reporter:
    def __init__(self) -> None:
        self.results: list[tuple[str, bool, str]] = []

    def record(self, name: str, ok: bool, detail: str = "") -> None:
        self.results.append((name, ok, detail))
        status = "PASS" if ok else "FAIL"
        line = f"[{status}] {name}"
        if detail:
            line += f" :: {detail}"
        print(line, flush=True)

    @property
    def all_passed(self) -> bool:
        return all(ok for _, ok, _ in self.results)


def scenario1_bp_in_loop(
    agent: DbxAgent, cs: int, landmark2: int, rep: Reporter
) -> None:
    name = f"scenario1: BP CS:{landmark2:04X} (NOP inside LOOP)"
    try:
        _bp_reset(agent)
        agent.debugger_command(f"BP {cs:04X}:{landmark2:04X}")
        agent.cpu_run()
        _drain_events(agent)
        agent.keyboard_tap("1")
        ev = _wait_for_event(agent, "bp.hit", timeout=8.0)
        if ev["seg"] != cs or ev["off"] != landmark2:
            rep.record(name, False, f"got seg=0x{ev['seg']:04X} off=0x{ev['off']:04X}")
            return
        rep.record(name, True, f"hit at {cs:04X}:{landmark2:04X}")
    except Exception as exc:
        rep.record(name, False, repr(exc))
    finally:
        with contextlib.suppress(Exception):
            agent.cpu_pause()
            _drain_events(agent)
            agent.debugger_command("BPDEL 0 *")
            agent.cpu_run()
            _drain_events(agent)


def scenario2_bpint_06(
    agent: DbxAgent, cs: int, landmark4: int, rep: Reporter
) -> None:
    name = "scenario2: BPINT 06 (illegal-opcode INT 6)"
    try:
        _bp_reset(agent)
        agent.debugger_command("BPINT 06")
        agent.cpu_run()
        _drain_events(agent)
        agent.keyboard_tap("2")
        ev = _wait_for_event(agent, "bp.hit", timeout=8.0)
        if ev["seg"] != cs or ev["off"] != landmark4:
            rep.record(
                name,
                False,
                f"got seg=0x{ev['seg']:04X} off=0x{ev['off']:04X} "
                f"(expected {cs:04X}:{landmark4:04X})",
            )
            return
        rep.record(name, True, f"hit at {cs:04X}:{landmark4:04X}")
    except Exception as exc:
        rep.record(name, False, repr(exc))
    finally:
        with contextlib.suppress(Exception):
            agent.cpu_pause()
            _drain_events(agent)
            agent.debugger_command("BPDEL 0 *")
            agent.cpu_run()
            _drain_events(agent)


def scenario3_bpm(agent: DbxAgent, cs: int, rep: Reporter) -> None:
    name = f"scenario3: BPM CS:0500h (memory write)"
    try:
        _bp_reset(agent)
        agent.debugger_command(f"BPM {cs:04X}:0500")
        agent.cpu_run()
        _drain_events(agent)
        agent.keyboard_tap("3")
        ev = _wait_for_event(agent, "bp.hit", timeout=8.0)
        rep.record(
            name,
            True,
            f"hit, event={ev}",
        )
    except Exception as exc:
        rep.record(name, False, repr(exc))
    finally:
        with contextlib.suppress(Exception):
            agent.cpu_pause()
            _drain_events(agent)
            agent.debugger_command("BPDEL 0 *")
            agent.cpu_run()
            _drain_events(agent)


def scenario4_tf_then_bp(
    agent: DbxAgent, cs: int, landmark_tf_next: int, rep: Reporter
) -> None:
    name = (
        f"scenario4: BP CS:{landmark_tf_next:04X} after TF=1 -- regs.get must NOT "
        "be re-vectored into INT 1"
    )
    try:
        _bp_reset(agent)
        agent.debugger_command(f"BP {cs:04X}:{landmark_tf_next:04X}")
        agent.cpu_run()
        _drain_events(agent)
        agent.keyboard_tap("4")
        ev = _wait_for_event(agent, "bp.hit", timeout=8.0)
        regs = agent.regs_get()
        if ev["seg"] != cs or ev["off"] != landmark_tf_next:
            rep.record(
                name,
                False,
                f"bp.hit at unexpected addr seg=0x{ev['seg']:04X} "
                f"off=0x{ev['off']:04X}",
            )
            return
        # The pre-fix behaviour drops here with CS:IP set to IVT[1]'s
        # bare-IRET BIOS stub (typically CS=0x0070, IP small).  The
        # post-fix behaviour leaves CS:IP at the breakpoint location.
        if regs["cs"] != cs or regs["eip"] != landmark_tf_next:
            rep.record(
                name,
                False,
                f"regs.get reports CS:EIP={regs['cs']:04X}:{regs['eip']:08X} "
                f"(expected {cs:04X}:{landmark_tf_next:04X} -- the Trap_Run "
                "DBINT_STEP injection is still re-vectoring after a BP-induced "
                "debugger entry; fix c50d109 is not in effect)",
            )
            return
        rep.record(
            name,
            True,
            f"bp.hit at {cs:04X}:{landmark_tf_next:04X}, regs.get matches "
            "(no INT 1 re-vector)",
        )
    except Exception as exc:
        rep.record(name, False, repr(exc))
    finally:
        with contextlib.suppress(Exception):
            agent.cpu_pause()
            _drain_events(agent)
            agent.debugger_command("BPDEL 0 *")
            agent.cpu_run()
            _drain_events(agent)


def scenario5_farcall(
    agent: DbxAgent, cs: int, landmark_callfar: int, rep: Reporter
) -> None:
    name = "scenario5: farcall.watch 9000h + CALL FAR DWORD PTR"
    try:
        _bp_reset(agent)
        agent.call("farcall.watch", target_seg="9000")
        agent.cpu_run()
        _drain_events(agent)
        agent.keyboard_tap("5")
        ev = _wait_for_event(agent, "farcall.transfer", timeout=8.0)
        expected_from_ip = (landmark_callfar + 4) & 0xFFFF
        problems: list[str] = []
        if ev.get("target_seg") != 0x9000:
            problems.append(f"target_seg=0x{ev.get('target_seg'):04X}")
        if ev.get("target_off") != 0x0010:
            problems.append(f"target_off=0x{ev.get('target_off'):04X}")
        if ev.get("from_cs") != cs:
            problems.append(f"from_cs=0x{ev.get('from_cs'):04X} (expected {cs:04X})")
        if ev.get("from_ip") != expected_from_ip:
            problems.append(
                f"from_ip=0x{ev.get('from_ip'):04X} "
                f"(expected 0x{expected_from_ip:04X})"
            )
        if ev.get("kind") != "call_far_indirect":
            problems.append(f"kind={ev.get('kind')!r}")
        if problems:
            rep.record(name, False, "; ".join(problems))
            return
        rep.record(
            name,
            True,
            f"from {cs:04X}:{expected_from_ip:04X} -> 9000:0010 "
            "kind=call_far_indirect",
        )
    except Exception as exc:
        rep.record(name, False, repr(exc))
    finally:
        with contextlib.suppress(Exception):
            agent.cpu_pause()
            _drain_events(agent)
            with contextlib.suppress(Exception):
                agent.call("farcall.unwatch")
            agent.cpu_run()
            _drain_events(agent)


# --- main ----------------------------------------------------------------


def run(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--dosbox",
        default=str(DEFAULT_DOSBOX),
        help=f"path to dosbox-x.exe (default: {DEFAULT_DOSBOX})",
    )
    p.add_argument(
        "--keep-tmp",
        action="store_true",
        help="don't remove the temp working directory on exit (for triage)",
    )
    args = p.parse_args(argv)

    dosbox_exe = pathlib.Path(args.dosbox)
    if not dosbox_exe.exists():
        print(f"ERROR: dosbox-x.exe not found at {dosbox_exe}", file=sys.stderr)
        return 2
    if not BPTEST_COM.exists():
        print(
            f"ERROR: BPTEST.COM not found at {BPTEST_COM}. "
            "Build it first via tests/agent_live/_build.dosbox-x.conf "
            "(TASM 1.0 + TLINK /t).",
            file=sys.stderr,
        )
        return 2

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="bptest_"))
    shutil.copy2(BPTEST_COM, tmp / "BPTEST.COM")
    portfile = tmp / "dbxport.txt"

    proc: Optional[subprocess.Popen] = None
    rep = Reporter()
    try:
        proc = _launch_dosbox(dosbox_exe, tmp, portfile)
        port = _wait_for_portfile(portfile, timeout=30.0)
        print(f"DOSBox-X agent listening on 127.0.0.1:{port}", flush=True)

        with DbxAgent(port=port) as agent:
            ver = agent.vm_version()
            print(
                f"connected: version={ver['version']} machine={ver['machine']} "
                f"build={ver['build']}",
                flush=True,
            )
            if ver["build"] != "heavy-debug":
                print(
                    "WARNING: BPM watches require a heavy-debug build; "
                    f"this build advertises {ver['build']!r}.  Scenario 3 will "
                    "almost certainly fail.",
                    flush=True,
                )

            cs = _read_signature(agent)
            print(f"BPTEST.COM loaded at CS=0x{cs:04X}", flush=True)

            landmark_bytes = agent.mem_read("seg:off", f"{cs:04X}:0103", 6)
            l2, l_tf, l_far = struct.unpack("<HHH", landmark_bytes)
            print(
                f"landmarks: landmark2=0x{l2:04X} "
                f"landmark_tf_next=0x{l_tf:04X} landmark_callfar=0x{l_far:04X}",
                flush=True,
            )

            scenario1_bp_in_loop(agent, cs, l2, rep)
            scenario2_bpint_06(agent, cs, 0x0175, rep)
            scenario3_bpm(agent, cs, rep)
            scenario4_tf_then_bp(agent, cs, l_tf, rep)
            scenario5_farcall(agent, cs, l_far, rep)

            # Clean exit: tell BPTEST to quit, which returns to COMMAND.COM,
            # which runs `exit` from the conf and shuts DOSBox-X down.
            with contextlib.suppress(Exception):
                _bp_reset(agent)
                agent.cpu_run()
                _drain_events(agent)
                agent.keyboard_tap("q")

        # Give DOSBox-X a moment to exit on its own; fall back to terminate.
        if proc is not None:
            try:
                proc.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
        if not args.keep_tmp:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"(kept tmp dir: {tmp})", flush=True)

    print("---")
    print(f"{sum(1 for _, ok, _ in rep.results if ok)}/{len(rep.results)} scenarios passed")
    return 0 if rep.all_passed else 1


if __name__ == "__main__":
    sys.exit(run())
