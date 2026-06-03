#!/usr/bin/env python3
"""Diagnostic: confirm the CPU_Interrupt hook actually fires for INT 8.

Reads IVT[8] to find where the timer ISR currently lives, arms
farcall.watch on that segment, runs for ~3 seconds, and reports how many
int_hw events the agent saw.  If the count is 0, the hook is broken.

Usage:
    python tests/agent_live/test_int_hooks.py [--dosbox PATH]
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

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="inthook_"))
    portfile = tmp / "dbxport.txt"
    conf = _write_conf(tmp)
    proc: Optional[subprocess.Popen] = None
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

            # Wait for the guest to reach the DOS prompt so IVT is populated.
            time.sleep(2.0)

            # Read IVT[8] (linear 0x20..0x23 = IP lo, IP hi, CS lo, CS hi).
            ivt8 = agent.mem_read("physical", "20", 4)
            ip8, cs8 = struct.unpack("<HH", ivt8)
            print(f"IVT[8]  = {cs8:04X}:{ip8:04X}", flush=True)
            ivt21 = agent.mem_read("physical", "84", 4)
            ip21, cs21 = struct.unpack("<HH", ivt21)
            print(f"IVT[21] = {cs21:04X}:{ip21:04X}", flush=True)

            # Scenario A: seg-only farcall.watch on IVT[8]'s segment. This
            # exercises the CPU_Interrupt choke-point hook in the wrapper
            # at src/cpu/cpu.cpp:1213.
            agent.call("farcall.watch", target_seg=cs8)
            print(f"\nScenario A: farcall.watch armed on seg=0x{cs8:04X}", flush=True)

            t_start = time.monotonic()
            kinds_a: dict = {}
            total_a = 0
            samples_a = []
            while time.monotonic() - t_start < 3.0:
                ev = agent.next_event(timeout=0.5)
                if ev is None:
                    continue
                if ev.get("event") == "farcall.transfer":
                    total_a += 1
                    k = ev.get("kind", "?")
                    kinds_a[k] = kinds_a.get(k, 0) + 1
                    if len(samples_a) < 3:
                        samples_a.append(ev)
            agent.call("farcall.unwatch")
            print(f"  total events in 3s: {total_a}  kinds: {kinds_a}")
            for s in samples_a:
                print(f"  {s}")

            # Scenario B: cpu.watch_target on IVT[8] entry. Exercises the
            # (seg,off)-matching NEAR-watch fire from the same hook.
            agent.call("cpu.watch_target", target_seg=cs8, target_off=ip8)
            print(f"\nScenario B: cpu.watch_target armed on {cs8:04X}:{ip8:04X}", flush=True)

            t_start = time.monotonic()
            kinds_b: dict = {}
            total_b = 0
            samples_b = []
            while time.monotonic() - t_start < 3.0:
                ev = agent.next_event(timeout=0.5)
                if ev is None:
                    continue
                if ev.get("event") == "cpu.transfer":
                    total_b += 1
                    k = ev.get("kind", "?")
                    kinds_b[k] = kinds_b.get(k, 0) + 1
                    if len(samples_b) < 3:
                        samples_b.append(ev)
            agent.call("cpu.unwatch_target")
            print(f"  total events in 3s: {total_b}  kinds: {kinds_b}")
            for s in samples_b:
                print(f"  {s}")

            ok_a = any(k.startswith("int_") for k in kinds_a)
            ok_b = "int_hw" in kinds_b
            if ok_a and ok_b:
                print("\nPASS: CPU_Interrupt choke-point fires for both FAR seg-watch "
                      "and NEAR (seg,off)-watch.")
                return 0
            if ok_a and not ok_b:
                print("\nPARTIAL: FAR seg-watch fires but NEAR (seg,off)-watch did not."
                      " AGENT_TargetWatchMatches may be broken.")
                return 1
            if not ok_a and ok_b:
                print("\nPARTIAL: NEAR watch fires but FAR seg-watch did not."
                      " AGENT_FarWatchMatches may be broken.")
                return 1
            print("\nFAIL: hook produced zero int_* events on either watch.")
            return 1
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(run())
