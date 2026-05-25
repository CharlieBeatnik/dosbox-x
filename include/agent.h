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
void AGENT_EmitBpHit(uint16_t seg, uint32_t off, int bp_index);
void AGENT_EmitLog(const char *line);
void AGENT_EmitDebuggerEntered(const char *reason);
void AGENT_EmitStateRunning(void);
void AGENT_EmitStatePaused(void);
void AGENT_EmitFarTransfer(uint16_t target_seg, uint16_t target_ip,
                           uint16_t from_cs,     uint16_t from_ip,
                           const char *kind);

/* Far-transfer watch. CPU core hot-path queries AGENT_FarWatchMatches()
 * for every CALL FAR / JMP FAR / RETF; when it returns true, the core
 * calls AGENT_EmitFarTransfer(). Single-segment sentinel today; the
 * predicate stays as a one-u16-compare-plus-flag-test so it's safe to
 * call unconditionally from the dispatch loop. */
bool AGENT_FarWatchMatches(uint16_t seg);
void AGENT_FarWatchSet(uint16_t seg);
void AGENT_FarWatchClear(void);

/* Notified when DOSBOX_SetNormalLoop / DOSBOX_SetLoop changes the main
 * loop. Used so we know when to flip state.paused <-> state.running. */
void AGENT_OnLoopChange(void);

/* True when the agent is running in "headless debugger" mode, i.e. the
 * caller should skip curses init in DEBUG_EnableDebugger. */
bool AGENT_IsHeadless(void);

#else /* !C_DEBUG */

static inline void AGENT_StartIfRequested(void) {}
static inline void AGENT_Stop(void) {}
static inline void AGENT_Poll(bool /*paused*/) {}
static inline void AGENT_EmitBpHit(uint16_t /*seg*/, uint32_t /*off*/, int /*bp_index*/) {}
static inline void AGENT_EmitLog(const char * /*line*/) {}
static inline void AGENT_EmitDebuggerEntered(const char * /*reason*/) {}
static inline void AGENT_EmitStateRunning(void) {}
static inline void AGENT_EmitStatePaused(void) {}
static inline void AGENT_EmitFarTransfer(uint16_t /*target_seg*/, uint16_t /*target_ip*/,
                                         uint16_t /*from_cs*/,    uint16_t /*from_ip*/,
                                         const char * /*kind*/) {}
static inline bool AGENT_FarWatchMatches(uint16_t /*seg*/) { return false; }
static inline void AGENT_FarWatchSet(uint16_t /*seg*/) {}
static inline void AGENT_FarWatchClear(void) {}
static inline void AGENT_OnLoopChange(void) {}
static inline bool AGENT_IsHeadless(void) { return false; }

#endif /* C_DEBUG */

#endif /* DOSBOX_AGENT_H */
