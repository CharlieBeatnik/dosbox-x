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

/* Agent control channel — event emitters and the per-request log-capture
 * buffer used by `debugger.command`. */

#include "config.h"

#if C_DEBUG

#include "agent.h"
#include "agent_internal.h"

#include <cstdio>

namespace agent {

/* Single-threaded: the agent only runs on the emulator main thread. */
static std::string *g_captureBuf = nullptr;

void captureBegin(std::string *into) {
    g_captureBuf = into;
}

void captureEnd() {
    g_captureBuf = nullptr;
}

void emitLogLine(const char *line) {
    if (!line) return;
    if (g_captureBuf) {
        if (!g_captureBuf->empty()) g_captureBuf->push_back('\n');
        g_captureBuf->append(line);
    }
    serverEmitLogLine(line);
}

}  /* namespace agent */

/* ---- Public surface ----------------------------------------------------- */

void AGENT_EmitBpHit(uint16_t seg, uint32_t off, int bp_index)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"bp.hit\",\"seg\":%u,\"off\":%u,\"bp_index\":%d}",
        static_cast<unsigned>(seg), static_cast<unsigned>(off), bp_index);
    agent::serverBroadcastLine(buf);
}

void AGENT_EmitLog(const char *line)
{
    agent::emitLogLine(line);
}

void AGENT_EmitDebuggerEntered(const char *reason)
{
    char buf[128];
    /* `reason` is one of a fixed set of literals ("breakpoint", "manual",
     * "int3", "sysenter") so no escaping needed. */
    snprintf(buf, sizeof(buf),
        "{\"event\":\"debugger.entered\",\"reason\":\"%s\"}",
        reason ? reason : "unknown");
    agent::serverBroadcastLine(buf);
}

void AGENT_EmitStateRunning(void)
{
    agent::serverBroadcastLine("{\"event\":\"state.running\"}");
}

void AGENT_EmitStatePaused(void)
{
    agent::serverBroadcastLine("{\"event\":\"state.paused\"}");
}

#endif /* C_DEBUG */
