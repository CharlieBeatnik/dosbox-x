#!/usr/bin/env python3
"""Assemble a MASM 6.11 source into a DOS .COM, running the 16-bit toolchain
*inside* DOSBox-X (the host is 64-bit Windows and cannot exec the 1990s DOS
binaries directly).

This is the dosbox-x in-repo analogue of X2RE's scripts/assemble_masm611.py,
trimmed for the single-source tiny-model .COM programs the agent live-fire
tests need. Two stages, both inside one DOSBox-X session:

  1. ML.EXE /c /AT      assemble  src.ASM  -> <stem>.OBJ   (+ <stem>.LST)
  2. TLINK.EXE /t /x    link      <stem>.OBJ -> <stem>.COM

ML's `/AT` selects the tiny memory model (mandatory for a .COM); TLINK's
`/t` produces a .COM image and `/x` suppresses the .MAP.

Tools are located via the same environment variables X2RE uses, so a host
that can already run that build can run this one unchanged:

    MASM  - MASM 6.11 install root (must contain BIN/ML.EXE)
    TASM  - TASM 1.0 install root  (must contain TLINK.EXE)

DOSBox-X is located via, in priority order: --dosbox, the DOSBOXX env var
(directory containing dosbox-x.exe, as X2RE sets it), or the in-repo
bin/x64/Debug/dosbox-x.exe.

Exit codes:
    0  <stem>.COM produced
    1  build ran but produced no .COM (assembler/linker error — see BUILD.LOG)
    2  configuration / environment error
    3  DOSBox-X timed out
    4  DOSBox-X started but the in-DOS batch produced no log
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import time


HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
DOSBOX_TIMEOUT_S = 180

DOSBOX_EXE_CANDIDATES = (
    "dosbox-x.exe",
    "dosbox-x_MinGWx64_SDL2.exe",
    "dosbox-x_MinGWx64_SDL1.exe",
)


def _fail(msg: str, code: int) -> int:
    print(f"ERROR: {msg}", file=sys.stderr)
    return code


def _find_dosbox(explicit: str | None) -> pathlib.Path | None:
    if explicit:
        p = pathlib.Path(explicit)
        return p if p.exists() else None
    env = os.environ.get("DOSBOXX")
    if env:
        d = pathlib.Path(env)
        for name in DOSBOX_EXE_CANDIDATES:
            if (d / name).exists():
                return d / name
    in_repo = REPO_ROOT / "bin" / "x64" / "Debug" / "dosbox-x.exe"
    if in_repo.exists():
        return in_repo
    return None


def build(src: pathlib.Path, dosbox: str | None = None,
          keep_conf: bool = False) -> int:
    if not src.exists():
        return _fail(f"source not found: {src}", 2)
    stem = src.stem.upper()
    workdir = src.resolve().parent

    masm_env = os.environ.get("MASM")
    tasm_env = os.environ.get("TASM")
    if not masm_env:
        return _fail("MASM environment variable is not set "
                     "(MASM 6.11 install root)", 2)
    if not tasm_env:
        return _fail("TASM environment variable is not set "
                     "(TASM 1.0 install root, for TLINK.EXE)", 2)
    masm_dir = pathlib.Path(masm_env)
    tasm_dir = pathlib.Path(tasm_env)
    if not (masm_dir / "BIN" / "ML.EXE").exists():
        return _fail(f"ML.EXE not found at {masm_dir / 'BIN' / 'ML.EXE'}", 2)
    if not (tasm_dir / "TLINK.EXE").exists():
        return _fail(f"TLINK.EXE not found at {tasm_dir / 'TLINK.EXE'}", 2)

    dosbox_exe = _find_dosbox(dosbox)
    if dosbox_exe is None:
        return _fail("dosbox-x.exe not found (pass --dosbox, set DOSBOXX, or "
                     "build bin/x64/Debug/dosbox-x.exe)", 2)

    # Clean stale artefacts so a failed build can't masquerade as success.
    for ext in ("OBJ", "LST", "COM", "MAP"):
        p = workdir / f"{stem}.{ext}"
        if p.exists():
            p.unlink()
    build_log = workdir / "BUILD.LOG"
    if build_log.exists():
        build_log.unlink()

    # The build commands are inlined directly into [autoexec] rather than a
    # separate .bat invoked via `call`. Under the VS-built DOSBox-X here a
    # `call <file>.bat` from autoexec silently did nothing (no log, no OBJ),
    # while the identical commands inlined into autoexec run fine.
    #
    # ML and TLINK are invoked by ABSOLUTE path (M:\BIN\ML.EXE, T:\TLINK.EXE);
    # PATH= resolution was also unreliable under this build. No `/AT`: the
    # sources use full segment definitions (`code segment / org 100h`), not
    # `.MODEL TINY`, and `/AT` then miscomputes label offsets so the landmark
    # table comes out as garbage. TLINK /t turns the plain OBJ into a .COM.
    #
    # autoexec has no goto/labels, so the batch is linear: we write progress
    # markers and a final `dir` into BUILD.LOG, and decide success in Python
    # from the marker lines + the presence of <stem>.COM.
    autoexec = [
        f'mount C "{workdir}"',
        f'mount M "{masm_dir}"',
        f'mount T "{tasm_dir}"',
        "set INCLUDE=M:\\INCLUDE",
        "set LIB=M:\\LIB",
        "C:",
        f"echo === ML /c {stem}.ASM === > BUILD.LOG",
        f"M:\\BIN\\ML.EXE /c /nologo /Fo{stem}.OBJ /Fl{stem}.LST {src.name} >> BUILD.LOG",
        "echo === ML done === >> BUILD.LOG",
        f"echo === TLINK /t /x {stem}.OBJ === >> BUILD.LOG",
        f"T:\\TLINK.EXE /t /x {stem}.OBJ >> BUILD.LOG",
        "echo === TLINK done === >> BUILD.LOG",
        f"dir {stem}.* >> BUILD.LOG",
        "echo === END === >> BUILD.LOG",
        "exit",
    ]
    conf_text = (
        "# Generated by tests/agent_live/build_masm.py — do not hand edit.\n"
        "[sdl]\noutput=surface\nautolock=false\nwindowresolution=640x400\n\n"
        "[dosbox]\nmachine=svga_s3\nmemsize=16\n\n"
        "[autoexec]\n" + "\n".join(autoexec) + "\n"
    )
    conf_path = workdir / "_masmbuild.dosbox-x.conf"
    conf_path.write_text(conf_text, encoding="utf-8")

    print(f"DOSBox-X : {dosbox_exe}")
    print(f"MASM     : {masm_dir}")
    print(f"TASM     : {tasm_dir}")
    print(f"Source   : {src}")
    print(f"Output   : {workdir / (stem + '.COM')}")
    print("Launching DOSBox-X...", flush=True)

    try:
        subprocess.run(
            [str(dosbox_exe), "-conf", str(conf_path), "-fastlaunch"],
            cwd=str(workdir),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=DOSBOX_TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        return _fail(f"DOSBox-X did not exit within {DOSBOX_TIMEOUT_S}s", 3)

    # The conf's `exit` returns control once the batch is done; the log is
    # written synchronously by COMMAND.COM, so it should exist now.
    deadline = time.monotonic() + 5.0
    while not build_log.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    if not build_log.exists():
        return _fail("DOSBox-X ran but produced no BUILD.LOG", 4)

    print("--- BUILD.LOG ---")
    print(build_log.read_text(encoding="ascii", errors="replace").rstrip())
    print("--- end ---")

    if not keep_conf and conf_path.exists():
        conf_path.unlink()

    com = workdir / f"{stem}.COM"
    ok = com.exists()
    print(f"{stem}.COM produced: {ok}")
    return 0 if ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", nargs="?", default=str(HERE / "obstest.asm"),
                    help="MASM .ASM source (default: obstest.asm)")
    ap.add_argument("--dosbox", help="path to dosbox-x.exe")
    ap.add_argument("--keep-conf", action="store_true",
                    help="keep the generated .bat/.conf for triage")
    args = ap.parse_args(argv)
    return build(pathlib.Path(args.source), args.dosbox, args.keep_conf)


if __name__ == "__main__":
    sys.exit(main())
