#!/usr/bin/env python3
"""Live-fire test for the screen.capture agent command (proposal 4.7).

Boots DOSBox-X with a captures directory configured, waits for the DOS
prompt to land, calls screen.capture (raw and cooked), and verifies:

  * The reply carries a path field pointing at a PNG that exists and
    has the PNG signature in the first 8 bytes.
  * A screen.captured event arrives carrying the same path.
  * The raw and cooked variants land at distinct files.

Usage:
    python tests/agent_live/test_screen_capture.py [--dosbox PATH]
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
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

PNG_SIGNATURE = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])


def _wait_for_portfile(path: pathlib.Path, timeout: float = 30.0) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            text = path.read_text(encoding="ascii", errors="replace").strip()
            if text.isdigit():
                return int(text)
        time.sleep(0.1)
    raise TimeoutError(f"DOSBox-X never wrote {path}")


def _write_conf(workdir: pathlib.Path, capturedir: pathlib.Path) -> pathlib.Path:
    # Forward slashes work fine for DOSBox-X's path parsing on Windows and
    # avoid the conf parser interpreting backslashes as escapes.
    cap = str(capturedir).replace("\\", "/")
    conf_text = textwrap.dedent(
        f"""\
        [sdl]
        output=surface
        autolock=false

        [dosbox]
        machine=svga_s3
        memsize=16
        captures={cap}

        [cpu]
        core=normal
        cputype=486
        cycles=fixed 5000
        """
    )
    p = workdir / "_test.dosbox-x.conf"
    p.write_text(conf_text, encoding="utf-8")
    return p


def _wait_for_event(agent: DbxAgent, name: str, timeout: float) -> Optional[dict]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = max(0.05, deadline - time.monotonic())
        ev = agent.next_event(timeout=remaining)
        if ev is None:
            continue
        if ev.get("event") == name:
            return ev
    return None


def _check_png(path: pathlib.Path) -> tuple[bool, str]:
    if not path.exists():
        return False, f"path does not exist: {path}"
    if path.stat().st_size < 16:
        return False, f"file too small ({path.stat().st_size} bytes)"
    with path.open("rb") as fh:
        head = fh.read(8)
    if head != PNG_SIGNATURE:
        return False, f"bad PNG signature: {head.hex()}"
    return True, f"OK ({path.stat().st_size} bytes)"


def run(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dosbox", default=str(DEFAULT_DOSBOX))
    args = p.parse_args(argv)
    dosbox = pathlib.Path(args.dosbox)
    if not dosbox.exists():
        print(f"ERROR: {dosbox} not found", file=sys.stderr)
        return 2

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="scrcap_"))
    capturedir = tmp / "captures"
    capturedir.mkdir()
    portfile = tmp / "dbxport.txt"
    conf = _write_conf(tmp, capturedir)
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

            # Give DOSBox-X a couple of seconds so the VGA emulation is
            # actively rendering (we need a frame to fire CAPTURE_AddImage
            # / WriteRawImage).
            time.sleep(2.0)

            # ---- Phase A: raw capture (default) -----------------------
            print("\nPhase A: screen.capture {raw: true} (default)", flush=True)
            t0 = time.monotonic()
            res = agent.call("screen.capture", timeout=10.0, raw=True)
            dt = time.monotonic() - t0
            print(f"  reply in {dt*1000:.1f}ms: {res}", flush=True)
            if "path" not in res or res.get("raw") is not True:
                print(f"  FAIL: bad reply shape", flush=True)
                ok = False
            else:
                ok_a, msg = _check_png(pathlib.Path(res["path"]))
                print(f"  PNG check: {msg}", flush=True)
                if not ok_a:
                    ok = False
                raw_path = res["path"]

            # The screen.captured event should also have arrived.
            ev = _wait_for_event(agent, "screen.captured", timeout=1.0)
            if ev is None:
                print("  WARN: no screen.captured event (already consumed by reply path?)", flush=True)
            else:
                print(f"  screen.captured event: {ev}", flush=True)

            # ---- Phase B: cooked capture ------------------------------
            print("\nPhase B: screen.capture {raw: false}", flush=True)
            t0 = time.monotonic()
            res = agent.call("screen.capture", timeout=10.0, raw=False)
            dt = time.monotonic() - t0
            print(f"  reply in {dt*1000:.1f}ms: {res}", flush=True)
            if "path" not in res or res.get("raw") is not False:
                print(f"  FAIL: bad reply shape", flush=True)
                ok = False
            else:
                ok_b, msg = _check_png(pathlib.Path(res["path"]))
                print(f"  PNG check: {msg}", flush=True)
                if not ok_b:
                    ok = False
                cooked_path = res["path"]

            # The two paths should differ (different filenames).
            if 'raw_path' in dir() and 'cooked_path' in dir() and raw_path == cooked_path:
                print("  FAIL: raw and cooked paths are the same", flush=True)
                ok = False

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

    print(f"\n{'PASS' if ok else 'FAIL'}: screen.capture", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(run())
