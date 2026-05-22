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
static inline void AGENT_OnLoopChange(void) {}
static inline bool AGENT_IsHeadless(void) { return false; }

#endif /* C_DEBUG */

#endif /* DOSBOX_AGENT_H */
