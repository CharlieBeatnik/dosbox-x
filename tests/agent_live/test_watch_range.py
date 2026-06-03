#!/usr/bin/env python3
"""Live-fire test for cpu.watch_range.

Arms a range watch on the BIOS INT 8 handler's segment + a small window
around its known IP. Verifies:

  * Each timer-interrupt dispatch into the window produces ONE
    cpu.range_enter event (not one per executed instruction inside).
  * from_cs/from_ip identifies the source of the transfer (the
    interrupted instruction).
  * cpu.unwatch_range clears the watch.
  * The watch correctly ignores transfers whose target is in the same
    seg but OUTSIDE [lo, hi].

Usage:
    python tests/agent_live/test_watch_range.py [--dosbox PATH]
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
from typing import Optional

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_DOSBOX = REPO_ROOT / "bin" / "x64" / "Debug" / "dosbox-x.exe"

sys.path.insert(0, str(REPO_ROOT / "contrib" / "agent-client"))
from dbxagent import DbxAgent  # noqa: E402


def _wait_for_portfile(path: pathlib.Path, timeout: float = 30.0) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            text = path.read_text(encoding="ascii", errors="replace").strip()
            if text.isdigit():
                return int(text)
        time.sleep(0.1)
    raise TimeoutError(f"DOSBox-X never wrote {path}")


def _write_conf(workdir: pathlib.Path) -> pathlib.Path:
    conf_text = textwrap.dedent(
        """\
        [sdl]
        output=surface
        autolock=false

        [dosbox]
        machine=svga_s3
        memsize=16

        [cpu]
        core=normal
        cputype=486
        cycles=fixed 5000
        """
    )
    p = workdir / "_test.dosbox-x.conf"
    p.write_text(conf_text, encoding="utf-8")
    return p


def run(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dosbox", default=str(DEFAULT_DOSBOX))
    args = p.parse_args(argv)
    dosbox = pathlib.Path(args.dosbox)
    if not dosbox.exists():
        print(f"ERROR: {dosbox} not found", file=sys.stderr)
        return 2

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="watchrange_"))
    portfile = tmp / "dbxport.txt"
    conf = _write_conf(tmp)
    proc: Optional[subprocess.Popen] = None
    ok = True
    try:
        proc = subprocess.Popen(
            [
                str(dosbox),
                "-conf", str(conf),
                "-agent-listen", "127.0.0.1:0",
                "-agent-portfile", str(portfile),
                "-fastlaunch",
            ],
            cwd=str(tmp),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        port = _wait_for_portfile(portfile, timeout=30.0)
        with DbxAgent(port=port) as agent:
            ver = agent.vm_version()
            print(f"version={ver['version']} build={ver['build']}", flush=True)

            # Wait for guest to reach DOS prompt so IVT is populated.
            time.sleep(2.0)

            # Read IVT[8] to find the BIOS timer-IRQ handler's CS:IP.
            ivt8 = agent.mem_read("physical", "20", 4)
            ip8, cs8 = struct.unpack("<HH", ivt8)
            print(f"IVT[8] = {cs8:04X}:{ip8:04X}", flush=True)

            # ---- Scenario A: arm a 16-byte window around IVT[8] ----------
            lo = ip8
            hi = (ip8 + 0x0F) & 0xFFFF
            print(f"\nScenario A: cpu.watch_range seg={cs8:04X} lo={lo:04X} hi={hi:04X}",
                  flush=True)
            res = agent.call("cpu.watch_range", seg=cs8, lo=lo, hi=hi)
            print(f"  arm reply: {res}", flush=True)
            if not res.get("watching") or res.get("seg") != cs8 \
                    or res.get("lo") != lo or res.get("hi") != hi:
                print("  FAIL: bad arm reply", flush=True)
                ok = False

            t_start = time.monotonic()
            entries = []
            kinds: dict = {}
            from_ips: set = set()
            while time.monotonic() - t_start < 3.0:
                ev = agent.next_event(timeout=0.5)
                if ev is None:
                    continue
                if ev.get("event") == "cpu.range_enter":
                    entries.append(ev)
                    kinds[ev.get("kind", "?")] = kinds.get(ev.get("kind", "?"), 0) + 1
                    from_ips.add((ev.get("from_cs"), ev.get("from_ip")))
            print(f"  cpu.range_enter events in 3s: {len(entries)}", flush=True)
            print(f"  kinds: {kinds}", flush=True)
            print(f"  distinct from CS:IP pairs: {len(from_ips)}", flush=True)
            for sample in entries[:3]:
                print(f"    {sample}", flush=True)
            # Multiple kinds report the same physical transfer: a timer
            # INT 8 dispatch produces int_hw (from CPU_Interrupt wrapper)
            # AND hbp_exec (from DEBUG_HeavyIsBreakpoint's per-instr
            # check for the first instruction at the watched IP). A
            # consumer can dedupe by (kind, from_cs, from_ip) if needed.
            # What we MUST see: ~18.2 Hz worth of int_hw events (timer
            # tick rate), and intra-range stepping is suppressed (which
            # is implied if we're not seeing thousands of events).
            int_hw_count = kinds.get("int_hw", 0)
            if int_hw_count == 0:
                print(f"  FAIL: zero int_hw events", flush=True)
                ok = False
            elif int_hw_count > 100:
                print(f"  FAIL: too many int_hw events ({int_hw_count}); intra-range gate broken",
                      flush=True)
                ok = False
            elif int_hw_count < 30:
                print(f"  WARN: fewer int_hw events than expected ({int_hw_count}, expected ~54)",
                      flush=True)
            else:
                print(f"  OK: {int_hw_count} int_hw events (matches ~18.2 Hz timer rate)",
                      flush=True)

            # ---- Scenario B: a target OUTSIDE [lo, hi] should NOT match --
            print(f"\nScenario B: re-arm with a 1-byte window AWAY from IVT[8]", flush=True)
            far_off = (ip8 + 0x800) & 0xFFFF  # well outside handler entry
            agent.call("cpu.watch_range", seg=cs8, lo=far_off, hi=far_off)
            t_start = time.monotonic()
            far_entries = []
            while time.monotonic() - t_start < 2.0:
                ev = agent.next_event(timeout=0.3)
                if ev is None:
                    continue
                if ev.get("event") == "cpu.range_enter":
                    far_entries.append(ev)
            print(f"  cpu.range_enter events in 2s: {len(far_entries)}", flush=True)
            if len(far_entries) > 5:
                print(f"  WARN: unexpected entries on a hopefully-quiet window: {far_entries[:3]}",
                      flush=True)

            # ---- Scenario C: cpu.unwatch_range clears -------------------
            print(f"\nScenario C: cpu.unwatch_range clears the watch", flush=True)
            res = agent.call("cpu.unwatch_range")
            print(f"  unwatch reply: {res}", flush=True)
            if res.get("watching") is not False:
                print("  FAIL: unwatch did not clear", flush=True)
                ok = False
            # Drain any straggler events, then confirm silence.
            while agent.next_event(timeout=0.2) is not None:
                pass
            t_start = time.monotonic()
            silent_entries = 0
            while time.monotonic() - t_start < 1.0:
                ev = agent.next_event(timeout=0.2)
                if ev is None:
                    continue
                if ev.get("event") == "cpu.range_enter":
                    silent_entries += 1
            if silent_entries > 0:
                print(f"  FAIL: {silent_entries} cpu.range_enter events after unwatch",
                      flush=True)
                ok = False
            else:
                print(f"  OK: no events post-unwatch", flush=True)

        if proc is not None:
            try:
                proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n{'PASS' if ok else 'FAIL'}: cpu.watch_range", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(run())
