/*
 *  Copyright (C) 2002-2025  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* Agent control channel — public surface.
 *
 * See docs/agent-interface/PLAN.md for the design. The entire subsystem is
 * gated on C_DEBUG; when C_DEBUG is off the externs below resolve to no-ops
 * via inline definitions, so callers do not need #if guards at every call
 * site. */

#ifndef DOSBOX_AGENT_H
#define DOSBOX_AGENT_H

#include <stdint.h>

#include "config.h"

#if C_DEBUG

/* Start the agent server if requested via -agent-listen / [agent] enabled.
 * Called once during startup, after the config is parsed. Safe to call when
 * the agent is disabled — it returns immediately. */
void AGENT_StartIfRequested(void);

/* Stop the agent server, close sockets, remove the port file. Idempotent. */
void AGENT_Stop(void);

/* Service the agent: accept new connections, drain recv buffers, dispatch
 * any complete JSON lines, flush outbound queue. Called periodically from
 * a TIMER_AddTickHandler and additionally from DEBUG_Loop's paused branch
 * (with paused=true) so the debugger can be driven while the CPU is halted. */
void AGENT_Poll(bool paused);

/* Event emitters. All are cheap (string-format + push to a deque); they
 * never touch sockets directly, so they are safe to call from BP-check
 * context. */
/* `from_cs` / `from_ip` are the (CS, IP) of the *previous* instruction
 * that just finished executing — i.e., the source of the transfer that
 * landed the CPU at (seg, off). Heavy-debug builds populate them via
 * DEBUG_HeavyIsBreakpoint's per-instruction tracker. Non-heavy callers
 * pass 0/0 (not meaningful). */
void AGENT_EmitBpHit(uint16_t seg, uint32_t off, int bp_index,
                     uint16_t from_cs = 0, uint16_t from_ip = 0);
void AGENT_EmitLog(const char *line);
void AGENT_EmitDebuggerEntered(const char *reason);
void AGENT_EmitStateRunning(void);
void AGENT_EmitStatePaused(void);
void AGENT_EmitFarTransfer(uint16_t target_seg, uint16_t target_ip,
                           uint16_t from_cs,     uint16_t from_ip,
                           const char *kind);

/* Near-transfer event (proposal 4.4). Same shape as the FAR variant but
 * always within a single CS; emitted as `cpu.transfer`. `kind` is a short
 * literal naming the source opcode group ("jmp_near_indirect",
 * "call_near_indirect", "retn", ...). */
void AGENT_EmitTransfer(const char *kind,
                        uint16_t target_seg, uint16_t target_off,
                        uint16_t from_cs,    uint16_t from_ip);

/* Far-transfer watch. CPU core hot-path queries AGENT_FarWatchMatches()
 * for every CALL FAR / JMP FAR / RETF; when it returns true, the core
 * calls AGENT_EmitFarTransfer(). Single-segment sentinel today; the
 * predicate stays as a one-u16-compare-plus-flag-test so it's safe to
 * call unconditionally from the dispatch loop. */
bool AGENT_FarWatchMatches(uint16_t seg);
void AGENT_FarWatchSet(uint16_t seg);
void AGENT_FarWatchClear(void);

/* Near-transfer watch (proposal 4.4). Sentinel is a (seg, off) pair so
 * the hot path can filter aggressively — NEAR transfers are far more
 * frequent than FAR. CPU core queries AGENT_TargetWatchMatches() at
 * every CALL NEAR / JMP NEAR (direct + indirect), every taken Jcc /
 * LOOP / JCXZ, and every RETN; on a match it calls AGENT_EmitTransfer()
 * with a `cpu.transfer` event. */
bool AGENT_TargetWatchMatches(uint16_t seg, uint16_t off);
void AGENT_TargetWatchSet(uint16_t seg, uint16_t off);
void AGENT_TargetWatchClear(void);

/* Range-entry watch (proposal 4.8). Fires when a control transfer lands
 * at `seg:target_off` with `lo<=target_off<=hi`, AND the previous
 * instruction was NOT in the same `[seg, lo..hi]` window. The "from
 * outside" gate suppresses the intra-range fall-through / LOOP flood —
 * the entire point is to identify the single transfer that first
 * crossed into the range. Same seg only (NEAR semantics by default; FAR
 * landings inherit the gate since from_cs != seg trivially counts as
 * "from outside"). */
bool AGENT_RangeWatchEntry(uint16_t target_seg, uint16_t target_off,
                           uint16_t from_seg,   uint16_t from_ip);
void AGENT_RangeWatchSet(uint16_t seg, uint16_t lo, uint16_t hi);
void AGENT_RangeWatchClear(void);
void AGENT_EmitRangeEnter(const char *kind,
                          uint16_t target_seg, uint16_t target_off,
                          uint16_t from_cs,    uint16_t from_ip);

/* Notified when DOSBOX_SetNormalLoop / DOSBOX_SetLoop changes the main
 * loop. Used so we know when to flip state.paused <-> state.running. */
void AGENT_OnLoopChange(void);

/* Notified by the capture subsystem after a screenshot PNG has been fully
 * written (fclose returned). `path` is the just-written file (the global
 * `pathscr` while it's still set); `raw` distinguishes the raw VGA
 * scan-line capture from the post-scaler render. The agent uses this to
 * (a) emit a `screen.captured` event for any subscriber, and (b) send the
 * deferred reply for a `screen.capture` request whose handler returned
 * empty pending the file write. */
void AGENT_OnScreenCaptured(const char *path, bool raw);

/* True when the agent is running in "headless debugger" mode, i.e. the
 * caller should skip curses init in DEBUG_EnableDebugger. */
bool AGENT_IsHeadless(void);

#else /* !C_DEBUG */

static inline void AGENT_StartIfRequested(void) {}
static inline void AGENT_Stop(void) {}
static inline void AGENT_Poll(bool /*paused*/) {}
static inline void AGENT_EmitBpHit(uint16_t /*seg*/, uint32_t /*off*/, int /*bp_index*/,
                                   uint16_t /*from_cs*/ = 0, uint16_t /*from_ip*/ = 0) {}
static inline void AGENT_EmitLog(const char * /*line*/) {}
static inline void AGENT_EmitDebuggerEntered(const char * /*reason*/) {}
static inline void AGENT_EmitStateRunning(void) {}
static inline void AGENT_EmitStatePaused(void) {}
static inline void AGENT_EmitFarTransfer(uint16_t /*target_seg*/, uint16_t /*target_ip*/,
                                         uint16_t /*from_cs*/,    uint16_t /*from_ip*/,
                                         const char * /*kind*/) {}
static inline void AGENT_EmitTransfer(const char * /*kind*/,
                                      uint16_t /*target_seg*/, uint16_t /*target_off*/,
                                      uint16_t /*from_cs*/,    uint16_t /*from_ip*/) {}
static inline bool AGENT_FarWatchMatches(uint16_t /*seg*/) { return false; }
static inline void AGENT_FarWatchSet(uint16_t /*seg*/) {}
static inline void AGENT_FarWatchClear(void) {}
static inline bool AGENT_TargetWatchMatches(uint16_t /*seg*/, uint16_t /*off*/) { return false; }
static inline void AGENT_TargetWatchSet(uint16_t /*seg*/, uint16_t /*off*/) {}
static inline void AGENT_TargetWatchClear(void) {}
static inline bool AGENT_RangeWatchEntry(uint16_t /*target_seg*/, uint16_t /*target_off*/,
                                         uint16_t /*from_seg*/,   uint16_t /*from_ip*/) { return false; }
static inline void AGENT_RangeWatchSet(uint16_t /*seg*/, uint16_t /*lo*/, uint16_t /*hi*/) {}
static inline void AGENT_RangeWatchClear(void) {}
static inline void AGENT_EmitRangeEnter(const char * /*kind*/,
                                        uint16_t /*target_seg*/, uint16_t /*target_off*/,
                                        uint16_t /*from_cs*/,    uint16_t /*from_ip*/) {}
static inline void AGENT_OnLoopChange(void) {}
static inline void AGENT_OnScreenCaptured(const char * /*path*/, bool /*raw*/) {}
static inline bool AGENT_IsHeadless(void) { return false; }

#endif /* C_DEBUG */

#endif /* DOSBOX_AGENT_H */
