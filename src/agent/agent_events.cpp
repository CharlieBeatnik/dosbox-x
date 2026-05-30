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
#include <string>

#if defined(WIN32)
# include <windows.h>
#elif defined(HAVE_REALPATH) || defined(__unix__) || defined(__APPLE__)
# include <limits.h>
# include <stdlib.h>
#endif

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

void AGENT_EmitBpHit(uint16_t seg, uint32_t off, int bp_index,
                     uint16_t from_cs, uint16_t from_ip)
{
    char buf[192];
    if (from_cs == 0 && from_ip == 0) {
        snprintf(buf, sizeof(buf),
            "{\"event\":\"bp.hit\",\"seg\":%u,\"off\":%u,\"bp_index\":%d}",
            static_cast<unsigned>(seg), static_cast<unsigned>(off), bp_index);
    } else {
        snprintf(buf, sizeof(buf),
            "{\"event\":\"bp.hit\",\"seg\":%u,\"off\":%u,\"bp_index\":%d,"
            "\"from_cs\":%u,\"from_ip\":%u}",
            static_cast<unsigned>(seg), static_cast<unsigned>(off), bp_index,
            static_cast<unsigned>(from_cs), static_cast<unsigned>(from_ip));
    }
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

void AGENT_EmitFarTransfer(uint16_t target_seg, uint16_t target_ip,
                           uint16_t from_cs,    uint16_t from_ip,
                           const char *kind)
{
    /* `kind` is one of a fixed set of literals from the CPU core hooks
     * ("call_far_direct", "call_far_indirect", "jmp_far_direct",
     * "jmp_far_indirect", "retf"). No escaping needed; buffer sized for
     * the longest. */
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"farcall.transfer\","
        "\"target_seg\":%u,\"target_off\":%u,"
        "\"from_cs\":%u,\"from_ip\":%u,"
        "\"kind\":\"%s\"}",
        static_cast<unsigned>(target_seg), static_cast<unsigned>(target_ip),
        static_cast<unsigned>(from_cs),    static_cast<unsigned>(from_ip),
        kind ? kind : "unknown");
    agent::serverBroadcastLine(buf);
}

/* Inline JSON-string-escape into `out`. The capture path passes absolute
 * file paths which on Windows contain backslashes — those MUST be escaped
 * or the consumer's JSON parser rejects the line. We don't worry about
 * non-ASCII here; PNG paths under the configured captures directory are
 * almost always ASCII. */
static void appendJsonEscaped(std::string &out, const char *s)
{
    if (!s) return;
    for (; *s; ++s) {
        unsigned char c = static_cast<unsigned char>(*s);
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out.append(esc);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
}

void AGENT_EmitTransfer(const char *kind,
                        uint16_t target_seg, uint16_t target_off,
                        uint16_t from_cs,    uint16_t from_ip)
{
    /* `kind` is a fixed-set literal from the CPU core hooks
     * ("jmp_near_indirect", "call_near_indirect", "retn", "retn_imm",
     * "call_near_direct", "jmp_near_direct", "jmp_short", "jcc_short",
     * "jcc_near"). No escaping needed. */
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"cpu.transfer\","
        "\"target_seg\":%u,\"target_off\":%u,"
        "\"from_cs\":%u,\"from_ip\":%u,"
        "\"kind\":\"%s\"}",
        static_cast<unsigned>(target_seg), static_cast<unsigned>(target_off),
        static_cast<unsigned>(from_cs),    static_cast<unsigned>(from_ip),
        kind ? kind : "unknown");
    agent::serverBroadcastLine(buf);
}

void AGENT_EmitRangeEnter(const char *kind,
                          uint16_t target_seg, uint16_t target_off,
                          uint16_t from_cs,    uint16_t from_ip)
{
    /* `seg` matches the watched segment (NEAR semantics). `target_off`
     * is the landing offset inside [lo, hi]. `from_cs`/`from_ip` point
     * at the source instruction that performed the boundary-crossing
     * transfer — the one to disassemble to find the divert. */
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"cpu.range_enter\","
        "\"seg\":%u,\"target_off\":%u,"
        "\"from_cs\":%u,\"from_ip\":%u,"
        "\"kind\":\"%s\"}",
        static_cast<unsigned>(target_seg), static_cast<unsigned>(target_off),
        static_cast<unsigned>(from_cs),    static_cast<unsigned>(from_ip),
        kind ? kind : "unknown");
    agent::serverBroadcastLine(buf);
}

namespace agent {
/* Defined in agent.cpp — single-slot pending screen.capture request state.
 * Touched only on the main (CPU) thread, so no locking required. */
extern double g_screenCaptureId;
extern bool   g_screenCaptureRaw;
extern bool   g_screenCapturePending;
}  /* namespace agent */

void AGENT_OnScreenCaptured(const char *path, bool raw)
{
    /* Translate the file path to an absolute path so consumers don't have
     * to know the agent's cwd. On Windows: GetFullPathName. On POSIX:
     * realpath (the captures dir is usually already absolute, but this
     * collapses any relative segments). */
    std::string abs = path ? path : "";
#if defined(WIN32)
    if (!abs.empty()) {
        char fullpath[260];  /* MAX_PATH */
        if (GetFullPathNameA(abs.c_str(), sizeof(fullpath), fullpath, NULL))
            abs = fullpath;
    }
#elif defined(HAVE_REALPATH) || defined(__unix__) || defined(__APPLE__)
    if (!abs.empty()) {
        char fullpath[PATH_MAX];
        if (realpath(abs.c_str(), fullpath) != NULL)
            abs = fullpath;
    }
#endif

    /* Always emit the screen.captured event for anyone subscribing. */
    {
        std::string ev;
        ev.reserve(64 + abs.size());
        ev.append("{\"event\":\"screen.captured\",\"path\":\"");
        appendJsonEscaped(ev, abs.c_str());
        ev.append("\",\"raw\":");
        ev.append(raw ? "true" : "false");
        ev.append("}");
        agent::serverBroadcastLine(ev);
    }

    /* If a screen.capture request is pending, send its deferred reply now.
     * The raw flag must match (otherwise this is a different capture, e.g.
     * a user pressing the screenshot hotkey while no request is pending). */
    if (agent::g_screenCapturePending && agent::g_screenCaptureRaw == raw) {
        std::string reply;
        reply.reserve(96 + abs.size());
        char idBuf[32];
        /* The protocol stores id as a number; reproduce the same encoding
         * the json encoder uses (integer when whole, otherwise %g). */
        if (agent::g_screenCaptureId == static_cast<int64_t>(agent::g_screenCaptureId))
            snprintf(idBuf, sizeof(idBuf), "%lld", static_cast<long long>(agent::g_screenCaptureId));
        else
            snprintf(idBuf, sizeof(idBuf), "%g", agent::g_screenCaptureId);
        reply.append("{\"id\":");
        reply.append(idBuf);
        reply.append(",\"ok\":true,\"result\":{\"path\":\"");
        appendJsonEscaped(reply, abs.c_str());
        reply.append("\",\"raw\":");
        reply.append(raw ? "true" : "false");
        reply.append("}}");
        agent::serverBroadcastLine(reply);
        agent::g_screenCapturePending = false;
    }
}

#endif /* C_DEBUG */
