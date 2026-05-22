# Handover — agent-interface

**Read this first.** Then read the current iteration in `TASKS.md`. Only consult `PLAN.md` if a design question isn't answered by those two.

## State

- **Branch:** `agent-interface` (just created off `master`).
- **Last commit:** the scaffold commit on this branch (run `git log --oneline -1 -- docs/agent-interface/` to see the SHA).
- **Build status:** Unchanged from `master`; no source under `src/` or `include/` modified yet.
- **Test status:** Unchanged from `master`.

## What was just done

Iteration 0 only — created `PLAN.md`, `TASKS.md`, and this file on a fresh branch `agent-interface`. No code changes yet, no Makefile or VS project edits. The repo on `master` is byte-identical for everything outside `docs/agent-interface/`.

## What to do next

Start **Iteration 1** in `TASKS.md`: scaffold `src/agent/`, add the CLI flags, add the `[agent]` config section. Behaviour of the emulator stays identical after Iteration 1 — flags are parsed but do nothing.

Key pointers before you start:
- The full file-by-file reuse map is in `PLAN.md` § "Reuse — don't reinvent". Open it once before touching code.
- Iteration 1 is light: ~7 files created, ~3 files lightly modified, no behavior change. Should fit comfortably in one session.
- The Visual Studio project changes (`vs/dosbox-x.vcxproj` and `.filters`) must touch all four Debug configurations (Win32/x64 × SDL1/SDL2). Release configs are intentionally untouched (`C_DEBUG` is off there).

## Open decisions / gotchas

None at scaffold time. Future agents: add anything here that the next agent must know but is not yet captured in `PLAN.md`. Keep the list short — if a decision is permanent, fold it into `PLAN.md` and remove it from here.

## End-of-session checklist (for whoever closes the next session)

1. Are all `[~]` items in the current iteration either `[x]` or backed out?
2. Has the iteration pointer at the top of `TASKS.md` been advanced (if the iteration is done)?
3. Is this `HANDOVER.md` rewritten (not appended) to reflect what the next agent walks into?
4. Has the commit landed on `agent-interface`?
