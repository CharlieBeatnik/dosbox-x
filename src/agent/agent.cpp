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

namespace agent {
namespace {

bool g_started        = false;     /* did we register the tick handler / start the server? */
bool g_tickInstalled  = false;
bool g_exitInstalled  = false;
bool g_headless       = false;     /* not driven yet — set in iteration 5 */

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

}  /* anonymous namespace */

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

    if (cmd->s == "vm.version") return jsonEncode(handleVmVersion(id, a));

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
    return agent::g_headless;
}

#endif /* C_DEBUG */
