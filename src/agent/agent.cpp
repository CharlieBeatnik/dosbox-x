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

/* Agent control channel — lifecycle and JSON dispatch loop.
 *
 * Iteration 2: the listener runs on demand; one command is implemented
 * (`vm.version`). The rest of the protocol surface still no-ops. */

#include "config.h"

#if C_DEBUG

#include "agent.h"
#include "agent_internal.h"

#include "control.h"
#include "dosbox.h"
#include "logging.h"
#include "setup.h"
#include "timer.h"

#include <cstdio>
#include <string>
#include <vector>

namespace agent {

/* Far-transfer watch state — file-scope so the public AGENT_FarWatch*
 * helpers below can mutate it without going through dispatch. Read on the
 * CPU dispatch hot path; written only from the main thread. */
bool     g_farWatchEnabled = false;
uint16_t g_farWatchSeg     = 0;

/* Near-transfer watch state (proposal 4.4). Same single-thread invariants
 * as the FAR watch above; the (seg, off) pair filters the much higher
 * NEAR-transfer rate down to one event per matching arrival. */
bool     g_targetWatchEnabled = false;
uint16_t g_targetWatchSeg     = 0;
uint16_t g_targetWatchOff     = 0;

namespace {

bool g_started        = false;     /* did we register the tick handler / start the server? */
bool g_tickInstalled  = false;
bool g_exitInstalled  = false;

void agentShutdown(Section * /*sec*/) {
    /* Exit callback. AGENT_Stop is declared in agent.h above. */
    ::AGENT_Stop();
}

void tickPoll(void) {
    serverPoll();
}

/* Map the MachineType enum to a stable lowercase string suitable for
 * protocol consumers. Mirrors the table in src/debug/debug.cpp:205. */
const char *machineName() {
    switch (machine) {
        case MCH_HERC:     return "hercules";
        case MCH_CGA:      return "cga";
        case MCH_TANDY:    return "tandy";
        case MCH_PCJR:     return "pcjr";
        case MCH_EGA:      return "ega";
        case MCH_VGA:      return "vga";
        case MCH_AMSTRAD:  return "amstrad";
        case MCH_PC98:     return "pc98";
        case MCH_FM_TOWNS: return "fmtowns";
        case MCH_MCGA:     return "mcga";
        case MCH_MDA:      return "mda";
    }
    return "unknown";
}

/* Pull the effective listen/portfile/auth_token values, with CLI flags
 * taking precedence over the [agent] section. Returns true if the agent
 * should be started. */
bool resolveConfig(std::string &listen, std::string &portfile,
                   std::string &authToken)
{
    Section_prop *sec = static_cast<Section_prop *>(control->GetSection("agent"));
    bool enabled = sec && sec->Get_bool("enabled");

    /* CLI overrides — any of them being set implies enabled=true. */
    bool cliRequested = !control->opt_agent_listen.empty()
                      || !control->opt_agent_portfile.empty()
                      || !control->opt_agent_token.empty();

    if (!enabled && !cliRequested) return false;

    listen    = !control->opt_agent_listen.empty()   ? control->opt_agent_listen
              : (sec ? sec->Get_string("listen")    : "127.0.0.1:0");
    portfile  = !control->opt_agent_portfile.empty() ? control->opt_agent_portfile
              : (sec ? sec->Get_string("portfile")  : "");
    authToken = !control->opt_agent_token.empty()    ? control->opt_agent_token
              : (sec ? sec->Get_string("auth_token"): "");
    return true;
}

/* ---- Command dispatch -------------------------------------------------- */

/* Build VM_VERSION at compile time so the agent can report exactly which
 * build (SDL1 vs SDL2, debug vs heavy debug) the client is talking to. */
const char *buildTag() {
#if defined(C_HEAVY_DEBUG)
    return "heavy-debug";
#elif defined(C_DEBUG)
    return "debug";
#else
    return "release";
#endif
}

JsonValue handleVmVersion(double id, const JsonValue & /*args*/) {
    JsonObject r;
    r.emplace("version", JsonValue::makeString(VERSION));
    r.emplace("machine", JsonValue::makeString(machineName()));
    r.emplace("build",   JsonValue::makeString(buildTag()));
    return makeReplyOk(id, std::move(r));
}

/* `debugger.command` — invoke ParseCommand and return whatever
 * DEBUG_ShowMsg writes during the call. ParseCommand modifies its char*
 * argument, so we hand it a writable buffer. */
JsonValue handleDebuggerCommand(double id, const JsonValue &args) {
    const JsonValue *text = args.get("text");
    if (!text || !text->isString()) {
        return makeReplyError(id, "bad_args", "expected {\"text\":\"...\"}");
    }

    std::string captured;
    std::vector<char> buf(text->s.begin(), text->s.end());
    buf.push_back('\0');

    bool ParseCommand(char *);

    captureBegin(&captured);
    bool ok = ParseCommand(buf.data());
    captureEnd();

    JsonObject r;
    r.emplace("output",    JsonValue::makeString(std::move(captured)));
    r.emplace("recognized", JsonValue::makeBool(ok));
    return makeReplyOk(id, std::move(r));
}

JsonValue handleLogSubscribe(double id, const JsonValue & /*args*/) {
    if (!serverSetLogSubscribed(true))
        return makeReplyError(id, "no_client", "no connected client to subscribe");
    JsonObject r;
    r.emplace("subscribed", JsonValue::makeBool(true));
    return makeReplyOk(id, std::move(r));
}

JsonValue handleLogUnsubscribe(double id, const JsonValue & /*args*/) {
    if (!serverSetLogSubscribed(false))
        return makeReplyError(id, "no_client", "no connected client");
    JsonObject r;
    r.emplace("subscribed", JsonValue::makeBool(false));
    return makeReplyOk(id, std::move(r));
}

/* cpu.pause / cpu.run — route through the existing debugger entry points.
 * Both are forward-declared here because including debug.h would pull a
 * lot more than we need (and that header isn't exposed via the include
 * path the agent subsystem uses). */
JsonValue handleCpuPause(double id, const JsonValue & /*args*/) {
    Bitu DEBUG_EnableDebugger(void);
    DEBUG_EnableDebugger();
    return makeReplyOk(id, JsonObject{});
}

JsonValue handleCpuRun(double id, const JsonValue & /*args*/) {
    bool ParseCommand(char *);
    char cmd[] = "RUN";
    ParseCommand(cmd);
    return makeReplyOk(id, JsonObject{});
}

/* Accept target_seg as a JSON number (e.g. 18492) or a hex string
 * ("0x483C" / "483C") to match mem.read's accept-either convention. */
bool parseSegArg(const JsonValue &v, uint32_t &out)
{
    if (v.isNumber()) {
        if (v.n < 0 || v.n > 0xFFFF) return false;
        out = (uint32_t)v.n;
        return true;
    }
    if (v.isString()) {
        const std::string &s = v.s;
        size_t skip = 0;
        if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) skip = 2;
        if (s.size() == skip) return false;
        char *end = nullptr;
        unsigned long val = strtoul(s.c_str() + skip, &end, 16);
        if (!end || *end != '\0') return false;
        if (val > 0xFFFF) return false;
        out = (uint32_t)val;
        return true;
    }
    return false;
}

JsonValue handleFarcallWatch(double id, const JsonValue &args) {
    const JsonValue *seg = args.get("target_seg");
    if (!seg) {
        return makeReplyError(id, "bad_args",
            "expected {\"target_seg\":<u16 or hex string>} (null to clear)");
    }
    if (seg->isNull()) {
        AGENT_FarWatchClear();
        JsonObject r;
        r.emplace("watching", JsonValue::makeBool(false));
        return makeReplyOk(id, std::move(r));
    }
    uint32_t segValue = 0;
    if (!parseSegArg(*seg, segValue)) {
        return makeReplyError(id, "bad_args",
            "target_seg must be u16 number or hex string");
    }
    AGENT_FarWatchSet((uint16_t)segValue);
    JsonObject r;
    r.emplace("watching",   JsonValue::makeBool(true));
    r.emplace("target_seg", JsonValue::makeNumber(segValue));
    return makeReplyOk(id, std::move(r));
}

JsonValue handleFarcallUnwatch(double id, const JsonValue & /*args*/) {
    AGENT_FarWatchClear();
    JsonObject r;
    r.emplace("watching", JsonValue::makeBool(false));
    return makeReplyOk(id, std::move(r));
}

/* cpu.watch_target — set (or clear, via target_seg=null) the NEAR-transfer
 * sentinel. Mirrors farcall.watch's accept-either-number-or-hex-string
 * convention; target_off is required only when target_seg is non-null. */
JsonValue handleCpuWatchTarget(double id, const JsonValue &args) {
    const JsonValue *seg = args.get("target_seg");
    if (!seg) {
        return makeReplyError(id, "bad_args",
            "expected {\"target_seg\":<u16 or hex string>, \"target_off\":<u16 or hex string>}"
            " (target_seg=null to clear)");
    }
    if (seg->isNull()) {
        AGENT_TargetWatchClear();
        JsonObject r;
        r.emplace("watching", JsonValue::makeBool(false));
        return makeReplyOk(id, std::move(r));
    }
    const JsonValue *off = args.get("target_off");
    if (!off) {
        return makeReplyError(id, "bad_args",
            "target_off required when target_seg is set");
    }
    uint32_t segValue = 0, offValue = 0;
    if (!parseSegArg(*seg, segValue))
        return makeReplyError(id, "bad_args",
            "target_seg must be u16 number or hex string");
    if (!parseSegArg(*off, offValue))
        return makeReplyError(id, "bad_args",
            "target_off must be u16 number or hex string");
    AGENT_TargetWatchSet((uint16_t)segValue, (uint16_t)offValue);
    JsonObject r;
    r.emplace("watching",   JsonValue::makeBool(true));
    r.emplace("target_seg", JsonValue::makeNumber(segValue));
    r.emplace("target_off", JsonValue::makeNumber(offValue));
    return makeReplyOk(id, std::move(r));
}

JsonValue handleCpuUnwatchTarget(double id, const JsonValue & /*args*/) {
    AGENT_TargetWatchClear();
    JsonObject r;
    r.emplace("watching", JsonValue::makeBool(false));
    return makeReplyOk(id, std::move(r));
}

}  /* anonymous namespace */

JsonValue makeReplyOk(double id, JsonObject result) {
    JsonObject reply;
    reply.emplace("id", JsonValue::makeNumber(id));
    reply.emplace("ok", JsonValue::makeBool(true));
    reply.emplace("result", JsonValue::makeObject(std::move(result)));
    return JsonValue::makeObject(std::move(reply));
}

JsonValue makeReplyError(double id, const std::string &code, const std::string &message) {
    JsonObject err;
    err.emplace("code",    JsonValue::makeString(code));
    err.emplace("message", JsonValue::makeString(message));

    JsonObject reply;
    reply.emplace("id", JsonValue::makeNumber(id));
    reply.emplace("ok", JsonValue::makeBool(false));
    reply.emplace("error", JsonValue::makeObject(std::move(err)));
    return JsonValue::makeObject(std::move(reply));
}

std::string dispatchLine(const std::string &line) {
    JsonValue req;
    std::string err;
    if (!jsonParse(line, req, &err)) {
        /* No id available — emit an unsolicited error event. */
        JsonObject ev;
        ev.emplace("event",   JsonValue::makeString("agent.error"));
        ev.emplace("code",    JsonValue::makeString("parse_error"));
        ev.emplace("message", JsonValue::makeString(err));
        return jsonEncode(JsonValue::makeObject(std::move(ev)));
    }

    double id = 0;
    if (const JsonValue *vid = req.get("id"))
        if (vid->isNumber()) id = vid->n;

    const JsonValue *cmd = req.get("cmd");
    if (!cmd || !cmd->isString()) {
        return jsonEncode(makeReplyError(id, "missing_cmd", "request lacks string 'cmd' field"));
    }

    const JsonValue *args = req.get("args");
    JsonValue empty = JsonValue::makeObject();
    const JsonValue &a = (args && args->isObject()) ? *args : empty;

    if (cmd->s == "vm.version")        return jsonEncode(handleVmVersion(id, a));
    if (cmd->s == "debugger.command")  return jsonEncode(handleDebuggerCommand(id, a));
    if (cmd->s == "log.subscribe")     return jsonEncode(handleLogSubscribe(id, a));
    if (cmd->s == "log.unsubscribe")   return jsonEncode(handleLogUnsubscribe(id, a));
    if (cmd->s == "keyboard.type")     return jsonEncode(handleKeyboardType(id, a));
    if (cmd->s == "keyboard.press")    return jsonEncode(handleKeyboardPress(id, a));
    if (cmd->s == "keyboard.release")  return jsonEncode(handleKeyboardRelease(id, a));
    if (cmd->s == "keyboard.tap")      return jsonEncode(handleKeyboardTap(id, a));
    if (cmd->s == "cpu.pause")         return jsonEncode(handleCpuPause(id, a));
    if (cmd->s == "cpu.run")           return jsonEncode(handleCpuRun(id, a));
    if (cmd->s == "regs.get")          return jsonEncode(handleRegsGet(id, a));
    if (cmd->s == "mem.read")          return jsonEncode(handleMemRead(id, a));
    if (cmd->s == "farcall.watch")     return jsonEncode(handleFarcallWatch(id, a));
    if (cmd->s == "farcall.unwatch")   return jsonEncode(handleFarcallUnwatch(id, a));
    if (cmd->s == "cpu.watch_target")  return jsonEncode(handleCpuWatchTarget(id, a));
    if (cmd->s == "cpu.unwatch_target")return jsonEncode(handleCpuUnwatchTarget(id, a));

    return jsonEncode(makeReplyError(id, "unknown_cmd",
        std::string("unknown command: ") + cmd->s));
}

}  /* namespace agent */

/* ---- Public surface ------------------------------------------------------ */

void AGENT_StartIfRequested(void) {
    if (agent::g_started) return;

    std::string listen, portfile, token;
    if (!agent::resolveConfig(listen, portfile, token)) return;

    agent::serverStart(listen, portfile, token);
    if (!agent::serverActive()) return;     /* logged inside serverStart */

    if (!agent::g_tickInstalled) {
        TIMER_AddTickHandler(agent::tickPoll);
        agent::g_tickInstalled = true;
    }
    if (!agent::g_exitInstalled) {
        AddExitFunction(AddExitFunctionFuncPair(agent::agentShutdown), false);
        agent::g_exitInstalled = true;
    }
    agent::g_started = true;
}

void AGENT_Stop(void) {
    if (!agent::g_started) return;
    if (agent::g_tickInstalled) {
        TIMER_DelTickHandler(agent::tickPoll);
        agent::g_tickInstalled = false;
    }
    agent::serverStop();
    agent::g_started = false;
}

void AGENT_Poll(bool /*paused*/) {
    /* The tick handler already drives serverPoll() every 1 ms. The
     * paused-context call from DEBUG_Loop (added in iteration 5) needs
     * the same drain logic, so route both through the same entry. */
    if (!agent::g_started) return;
    agent::serverPoll();
}

void AGENT_OnLoopChange(void) {
    /* Iteration 5 will emit state.paused / state.running here. */
}

bool AGENT_IsHeadless(void) {
    /* For Phase 1 the agent owns the debugger UI whenever the listener is
     * up. Granular control (e.g. agent + curses concurrently) can be
     * revisited later; the loss is that pressing Alt-Pause with the agent
     * active no longer pops a curses window. */
    return agent::g_started;
}

bool AGENT_FarWatchMatches(uint16_t seg) {
    return agent::g_farWatchEnabled && seg == agent::g_farWatchSeg;
}

void AGENT_FarWatchSet(uint16_t seg) {
    agent::g_farWatchSeg     = seg;
    agent::g_farWatchEnabled = true;
}

void AGENT_FarWatchClear(void) {
    agent::g_farWatchEnabled = false;
    agent::g_farWatchSeg     = 0;
}

bool AGENT_TargetWatchMatches(uint16_t seg, uint16_t off) {
    return agent::g_targetWatchEnabled
        && seg == agent::g_targetWatchSeg
        && off == agent::g_targetWatchOff;
}

void AGENT_TargetWatchSet(uint16_t seg, uint16_t off) {
    agent::g_targetWatchSeg     = seg;
    agent::g_targetWatchOff     = off;
    agent::g_targetWatchEnabled = true;
}

void AGENT_TargetWatchClear(void) {
    agent::g_targetWatchEnabled = false;
    agent::g_targetWatchSeg     = 0;
    agent::g_targetWatchOff     = 0;
}

#endif /* C_DEBUG */
