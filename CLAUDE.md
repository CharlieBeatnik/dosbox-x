# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DOSBox-X is a C++ DOS / Windows 9x emulator, forked from DOSBox in 2011. Compared to upstream DOSBox, it focuses on emulation accuracy across the full IBM PC/XT/AT, AX, Tandy, PCjr, NEC PC-98, and DOS/V hardware spectrum (8086 through Pentium III), and on running Windows 1.0 through Windows ME guests. Compilation requires C++11 (new code should also build as C++14 per `CONTRIBUTING.md`). The project officially targets pre-2001 hardware only — Pentium 4 and later, Vista+ guests, and cycle-accurate timing are explicit non-goals.

## Build entry points

There is no single build command — each `build-*` script wraps autogen + configure + make for a specific target. Each script invokes `./autogen.sh` (regenerates configure via aclocal/autoheader/automake/autoconf), then builds the in-tree SDL/SDLnet/libpng/freetype copies under `vs/`, then configures and runs `make -j3`. Pick the one that matches the host and SDL flavor:

- Linux/macOS SDL1 debug: `./build-debug` (add `32` first arg for 32-bit on x86_64). For SDL2: `./build-debug-sdl2`.
- Linux/macOS release: `./build-macos`, `./build-macos-sdl2`, `./build-macosold[-sdl2]`. Append `universal` on Apple Silicon for fat binaries.
- MinGW (Windows): `./build-mingw`, `./build-mingw-sdl2`, `./build-mingw-lowend[-sdl2]` (XP), `./build-mingw-lowend9x` (9x/NT4), `./build-mingw-hx-dos` (real DOS via HX extender). Lowend / HX-DOS variants MUST use original 32-bit MinGW, not MinGW-w64.
- Visual Studio (Windows): open `vs/dosbox-x.sln`. Default platform toolset is v142; switch to v141 for VS2017/XP-compatible builds and run `contrib/windows/installer/PatchPE.exe` on the resulting exe. Requires the DirectX 2010 SDK for Direct3D9. SDL2 builds are separate solution configurations in the same `.sln`.
- OS/2: `build-os2-sdl2.cmd` / `build-debug-os2-sdl2.cmd`. Emscripten: `build-emscripten-sdl2`. RISC OS: `build-riscos`.

All scripts forward extra args to `./configure`; see `BUILD.md` for the full flag list (`--enable-debug`, `--enable-debug=heavy`, `--disable-dynamic-core`, `--disable-printer`, `--disable-mt32`, etc.). `--enable-debug` is needed for the ncurses debugger (Alt+Pause / Alt+F12 on Mac).

## Tests

Unit tests live in `tests/` (gTest/gMock, registered via `dosbox_test_fixture.h`). They are not a separate binary — the test runner is the emulator itself:

```
./dosbox-x -tests              # run all tests
./dosbox-x -tests --gtest_filter=ShellRedirection.*
./dosbox-x -gtest_list_tests   # list tests; aliases: -test, -tests
```

The `-tests` option sets `SDL_VIDEODRIVER=dummy` and runs in a non-interactive fast-launch mode (see `src/gui/sdlmain.cpp` around line 7512). Coverage today is intentionally narrow (shell commands, drive code, DOS file I/O) — the emulator's real test suite is running real DOS programs and Windows guests.

## Architecture cheatsheet

Use `README.source-code-description` as the source of truth — it documents every major file. The shape worth knowing before opening files:

- **Entry & control.** `main()` lives near the bottom of `src/gui/sdlmain.cpp` (on Windows, called from SDL's `WinMain`). The global `control` pointer (defined in `include/control.h`) holds both `dosbox-x.conf` settings and command-line flags. Sections and settings are declared in `DOSBox_SetupConfigSections()` in `src/dosbox.cpp`. The codebase assumes sections/settings can always be looked up by name without null-checking, so removing a setting silently can crash other code.
- **Init model differs from DOSBox SVN.** Upstream DOSBox attaches init/destructor functions to each `[section]` and depends on section order. DOSBox-X removed those and uses (a) explicit init calls from `main()` plus (b) VM event callbacks declared in `include/setup.h`. Don't add section init hooks; register a VM event callback instead.
- **Time & scheduling.** A "tick" is 1 ms; `cycles=` is "instructions per ms." `Normal_Loop()` in `src/dosbox.cpp` runs the CPU under `PIC_RunQueue()`. Schedule one-shot events with `PIC_AddEvent(callback, delay_ms)` from `src/hardware/timer.cpp`; periodic events re-`PIC_AddEvent` from their own callback (the PIC detects the recursive call and adjusts delta to keep cadence). Per-tick handlers use `TIMER_AddTickHandler()`. Query current emulator time via `PIC_TickIndex()`, `PIC_TickIndexND()`, `PIC_FullIndex()` (`include/pic.h`).
- **Native ↔ guest bridge.** BIOS and DOS interrupts run through the **callback** mechanism (`src/cpu/callback.cpp`). Callbacks use the invalid opcode sequence `0xFE 0x38 <u16 index>`. Allocate one with `CALLBACK_Allocate()`, return `CBRET_NONE`, manipulate guest flags via `CALLBACK_SCF/SZF/SIF`. Memory or I/O performed by a callback in protected mode can recurse the emulator (page-fault frame pushed, nested CPU loop) — this is fine for DOS / Win3.1 but is the root of subtle Win9x bugs.
- **CPU cores.** `src/cpu/core_normal.cpp` (default), `core_normal_286.cpp`, `core_normal_8086.cpp`, `core_simple.cpp` (no paging), `core_prefetch.cpp` (selected via `cputype=*_prefetch` for copy-protection / self-modifying code), `core_full.cpp` (Bochs-derived), `core_dyn_x86.cpp` (32-bit x86 only — recompiles guest to host; faster but loose on paging and cycle counts).
- **VGA.** Two parallel implementations under `src/hardware/`: `vga_*.cpp` for IBM PC family (state in a global `VGA` struct, S3 used as the "extended state" model), and `vga_pc98_*.cpp` for NEC PC-98 (state in master/slave GDC). The active mode is the `M_*` enum in `include/vga.h`. When adding an `M_*` value you must add a matching string in the array near line 2755 of `src/hardware/vga_draw.cpp` or mode change will segfault. DOSBox-X always renders one scanline at a time (DOSBox SVN renders in quarters except `machine=vgaonly`) to support copper / palette-per-line effects. EGA/VGA planar memory is emulated as `uint32_t` per group of 4 planes — assumes little-endian host. S3 linear framebuffer is mapped at `0xE0000000` via the memory callout system (not `MEM_GetPageHandler()` as upstream).
- **Mixer.** `src/hardware/mixer.cpp`. All sources render to 16-bit stereo, rate-matched per 1 ms tick via fractional integer math (audio is bound to emulator time, not the SDL audio device, so AVI captures never drift). Devices call `MIXER_AddChannel(callback,...)` to get a channel, `MIXER_DelChannel` to destroy. Significant state changes should call `MIXER_FillUp()` / channel `FillUp()` *before* applying the change so audio up to that point is rendered with the old state. Sound Blaster (`sblaster.cpp`) covers SB1.0 through SB16 plus ESS688 / SC400; rate caps (23 kHz non-highspeed, 45 kHz highspeed) and goldplay-mode handling intentionally diverge from DOSBox SVN.
- **Menus.** A single framework (`include/menu.h`, `src/gui/menu.cpp`) backs three host backends — Win32 HMENU, macOS NSMenu, and SDL-drawn — selected via `DOSBOXMENU_*` defines. Items are addressed by name (`get_item(name)`); mapper shortcuts auto-register as `mapper_<name>`. **Never** cache the reference returned from `get_item()` — the underlying vector can reallocate, and `get_item()` `E_Exit`s if the name doesn't exist.
- **DOS subsystem.** `src/dos/` is the DOS kernel itself: `dos.cpp` (INT 21h dispatch), `dos_execute.cpp`, `dos_files.cpp`, `dos_memory.cpp`, `dos_mscdex.cpp` (MSCDEX), `dos_keyboard_layout.cpp`. Drive backends: `drive_local.cpp`, `drive_fat.cpp`, `drive_iso.cpp`, `drive_overlay.cpp`, `drive_physfs.cpp`, `drive_virtual.cpp`. BIOS / INT 10h / EMS / XMS live in `src/ints/`. Internal Z: drive utilities (`MEM.COM`, `MIXER.COM`, `EDIT.COM`, `DEBUG.EXE`, `XCOPY.EXE`, etc.) are embedded as byte arrays in `src/builtin/`. Shell (COMMAND.COM emulation, AUTOEXEC.BAT, command parsing) is in `src/shell/`.
- **Output backends.** `src/output/output_{surface,opengl,direct3d,direct3d11,ttf,metal,gamelink}.{cpp,mm}`. Choice is driven by the `[sdl] output=` setting.

## Coding conventions

- Style is governed by `.editorconfig`: 4-space indent, LF, UTF-8, 120-col max, braces-on-next-line for namespaces / types / functions / case blocks (Allman-ish). `trim_trailing_whitespace` is on.
- Integer-width rules from `README.source-code-description` are load-bearing: never assume `sizeof(int)` / `sizeof(long)`. Use `uint16_t` / `uint32_t` / `uintptr_t` / `size_t` / `off_t`. **Do not** use `%llu` / `%llx` in `printf` — Microsoft's runtime (including MinGW which uses it) needs `%I64u` / `%I64d`. Use `O_BINARY` on every `open()` and assume CR/LF translation otherwise. Use `lseek64` / `_lseeki64` for >2 GB files. The emulator assumes a little-endian, 2's-complement host.
- Far pointers are not supported; memory is assumed flat with optional paging.
- `CONTRIBUTING.md` asks for short methods, DRY, low nesting, and Doxygen-friendly comments (much existing code does not follow this — a cleanup is in progress).
- Per-monthly release pattern (see `README.md`): the last 6 days of each month accept bug-fixes only, the last day of the month is build day with no commits.

## Other docs worth reading before non-trivial changes

- `README.source-code-description` — per-file map plus the long-form versions of the time, callback, mixer, VGA, and Sound Blaster sections above.
- `README.debugger` — what the ncurses debugger windows show and the `na` / `pf` markers in the data view.
- `README.development-in-Windows` — Visual Studio / VS Code setup, `SDL1AdditionalOptions` env var, XInput flag.
- `README.keyboard-layout-handling`, `README.video`, `README.video-debug-overlay.md`, `README.joystick` — subsystem-specific.
- `dosbox-x.reference.conf` / `dosbox-x.reference.full.conf` — auto-generated reference of every config setting; regenerate via `./update-dosbox-x-reference-conf`.
- `experiments/` — proving grounds, may or may not land. `patch-integration/` — community patches queued for review.
