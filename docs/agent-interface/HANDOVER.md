# Handover — agent-interface

**Read this first.** Then read the current iteration in `TASKS.md`. Only consult `PLAN.md` if a design question isn't answered by those two.

## State

- **Branch:** `agent-interface`.
- **Last commit:** the Iteration 1 scaffold commit. Run `git log --oneline -5` to confirm.
- **Build status:** *Not verified in this session.* No Linux/macOS/MinGW toolchain was available in the previous session. The Iteration-1 changes were prepared with care but have not been compiled.
- **Test status:** Unchanged from `master` (no test files added in Iteration 1).

## What was just done — Iteration 1

Wired the empty agent skeleton in. Behaviour of the emulator is unchanged: the CLI flags parse and store values, the `[agent]` config section registers under `#if C_DEBUG`, but no socket is opened, no thread is created, no log message printed.

Files added:
- `include/agent.h` — public surface, inline-noop in non-debug builds.
- `src/agent/agent.cpp` — empty `AGENT_StartIfRequested / AGENT_Stop / AGENT_Poll / AGENT_OnLoopChange / AGENT_IsHeadless`.
- `src/agent/agent_server.cpp`, `agent_json.cpp`, `agent_keyboard.cpp` — empty TUs (compile but contain nothing yet under `#if C_DEBUG`).
- `src/agent/agent_events.cpp` — empty `AGENT_EmitBpHit / AGENT_EmitLog`.
- `src/agent/Makefile.am` — builds `libagent.a` from the five `.cpp` files.

Files modified:
- `include/control.h` — three new string fields `opt_agent_listen / opt_agent_portfile / opt_agent_token`.
- `src/gui/sdlmain.cpp` — parse `-agent-listen / -agent-portfile / -agent-token`; help text in `-helpdebug`.
- `src/dosbox.cpp` — `[agent]` config section, gated on `#if C_DEBUG`.
- `src/Makefile.am` — added `agent` to `SUBDIRS` and `agent/libagent.a` as the first entry in `dosbox_x_LDADD`.
- `vs/dosbox-x.vcxproj` and `.vcxproj.filters` — five `ClCompile` entries and one `ClInclude` for `agent.h`, plus a new `Sources\agent` filter.

## What to do next

Start **Iteration 2** in `TASKS.md`: TCP server, JSON framing, `vm.version`. The agent now goes from no-op to listening-on-demand.

**Verify the Iteration-1 build first** before touching new code:
1. Pick a build script that matches your environment (`./build-debug` on Linux/macOS, `./build-mingw` for MinGW, or VS Debug-SDL2 x64). The build must include `C_DEBUG` for libagent to contain any code at all.
2. Run `./dosbox-x --helpdebug` and confirm the three `-agent-*` lines appear.
3. Boot a normal session without `-agent-*` flags — should be byte-identical behaviour. Verify with `netstat`/`ss` that no new listening socket appears (it shouldn't — `AGENT_StartIfRequested` is empty).
4. **If the build fails**, the most likely cause is a typo in the C_DEBUG guards or a header include. The agent code is intentionally trivial; treat any build error as easy to fix and don't proceed to Iteration 2 until clean.
5. Once a build is in hand, run `./update-dosbox-x-reference-conf` to confirm the reference conf is unchanged (it should be, since `[agent]` is `#if C_DEBUG`-gated and the existing conf is generated from a non-debug build).

## Open decisions / gotchas

- **VS project wiring deviates from the plan:** TASKS.md Iteration 1 said "four debug configurations only", but `src/debug/*` is wired unconditionally in `vs/dosbox-x.vcxproj`, and `vs/config.h` hard-codes `C_DEBUG 1` for all VS builds. So the agent files were added unconditionally too. The `#if C_DEBUG` guards in source are the real gate. If we later want a "release without C_DEBUG" VS configuration, both `src/debug/*` and `src/agent/*` need the same per-config exclusion treatment — they're a pair.
- **Reference conf not regenerated** — see Iteration-1 task notes in `TASKS.md`. Next session with a built binary should run `./update-dosbox-x-reference-conf` and commit any unexpected diff (none expected).
- **`Property::Changeable::OnlyAtStart` used for all four agent settings.** Intentional: starting/stopping the listener on a live config change isn't supported and isn't worth supporting in Phase 1.

## End-of-session checklist (for whoever closes the next session)

1. Are all `[~]` items in the current iteration either `[x]` or backed out?
2. Has the iteration pointer at the top of `TASKS.md` been advanced (if the iteration is done)?
3. Is this `HANDOVER.md` rewritten (not appended) to reflect what the next agent walks into?
4. Has the commit landed on `agent-interface`?
