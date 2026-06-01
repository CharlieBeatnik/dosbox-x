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

/* Agent control channel — private interface between the agent translation
 * units (src/agent/*.cpp). Not exported to the rest of DOSBox-X. */

#ifndef DOSBOX_AGENT_INTERNAL_H
#define DOSBOX_AGENT_INTERNAL_H

#include "config.h"

#if C_DEBUG

#include <stdint.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "keyboard.h"      /* for the KBD_KEYS enum used below */

/* ---- JSON ---------------------------------------------------------------
 * A minimal hand-rolled JSON value. Numbers are stored as doubles; the
 * agent protocol uses integers and never relies on fractional precision
 * beyond what double can represent exactly (53 bits). */

namespace agent {

struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray  = std::vector<JsonValue>;

enum class JsonType { Null, Bool, Number, String, Array, Object };

struct JsonValue {
    JsonType type = JsonType::Null;
    bool        b = false;
    double      n = 0.0;
    std::string s;
    std::shared_ptr<JsonArray>  a;
    std::shared_ptr<JsonObject> o;

    JsonValue() = default;

    static JsonValue makeNull()                              { JsonValue v; v.type = JsonType::Null;   return v; }
    static JsonValue makeBool(bool x)                        { JsonValue v; v.type = JsonType::Bool;   v.b = x; return v; }
    static JsonValue makeNumber(double x)                    { JsonValue v; v.type = JsonType::Number; v.n = x; return v; }
    static JsonValue makeString(std::string x)               { JsonValue v; v.type = JsonType::String; v.s = std::move(x); return v; }
    static JsonValue makeArray(JsonArray x = {})             { JsonValue v; v.type = JsonType::Array;  v.a = std::make_shared<JsonArray>(std::move(x));  return v; }
    static JsonValue makeObject(JsonObject x = {})           { JsonValue v; v.type = JsonType::Object; v.o = std::make_shared<JsonObject>(std::move(x)); return v; }

    bool isNull()   const { return type == JsonType::Null;   }
    bool isBool()   const { return type == JsonType::Bool;   }
    bool isNumber() const { return type == JsonType::Number; }
    bool isString() const { return type == JsonType::String; }
    bool isArray()  const { return type == JsonType::Array;  }
    bool isObject() const { return type == JsonType::Object; }

    /* Convenience accessors with a default if the field is missing/wrong-type. */
    const JsonValue *get(const char *key) const {
        if (!isObject() || !o) return nullptr;
        auto it = o->find(key);
        return it == o->end() ? nullptr : &it->second;
    }
    std::string getString(const char *key, const char *defv = "") const {
        const JsonValue *v = get(key);
        return (v && v->isString()) ? v->s : std::string(defv);
    }
};

/* Parse one JSON value from `text`. Returns true on success. On failure,
 * `error` (if non-null) receives a short human-readable message. */
bool jsonParse(const std::string &text, JsonValue &out, std::string *error = nullptr);

/* Encode a JsonValue to a compact (no whitespace) string. The output never
 * contains a literal newline, so the encoded form is safe as a single line
 * in the agent's newline-delimited framing. */
std::string jsonEncode(const JsonValue &v);

/* ---- Server -------------------------------------------------------------
 * Owns the listening socket and the (currently single) client connection.
 * Phase-1 rejects a second concurrent connection with "busy". */

void serverStart(const std::string &listen, const std::string &portfile, const std::string &auth_token);
void serverStop();
bool serverActive();

/* Called periodically from the tick handler. Accepts new connections,
 * drains recv buffers, hands each complete line to dispatchLine(), and
 * flushes per-client outboxes. */
void serverPoll();

/* Push one outbound line to every connected client. Trailing newline is
 * added by the server; do not include it in `line`. Drops with an
 * `agent.overflow` event if the per-client outbox would exceed its cap. */
void serverBroadcastLine(const std::string &line);

/* Set the current (only) client's log-subscription flag. Returns true if
 * a client is connected. */
bool serverSetLogSubscribed(bool on);

/* Append a single line as a `log.line` event to every subscribed client.
 * The line is sent literally inside `text` (newlines stripped before
 * encoding); never logs via DEBUG_ShowMsg so it is safe from inside the
 * log-tee path. */
void serverEmitLogLine(const char *text);

/* ---- Log capture --------------------------------------------------------
 * Used by `debugger.command` to gather any `DEBUG_ShowMsg` output produced
 * by `ParseCommand` and return it to the caller. Single-threaded — the
 * agent runs on the emulator main thread, so a plain static pointer is
 * sufficient. */
void captureBegin(std::string *into);
void captureEnd();

/* Called by the DEBUG_ShowMsg tap. Appends to the active capture (if any)
 * and emits a log.line event to subscribed clients. Never recurses into
 * the logging system. */
void emitLogLine(const char *line);

/* ---- Keyboard ----------------------------------------------------------
 * JSON key name -> KBD_KEYS lookup. The table is defined in
 * agent_keyboard.cpp and covers every value of KBD_KEYS except
 * KBD_NONE / KBD_LAST. Names are lowercase canonical (e.g. "leftshift",
 * "kp1", "f10"). Returns false if `name` isn't recognized. */
bool keyboardNameToKey(const std::string &name, KBD_KEYS &out);
size_t keyboardTableSize();

/* Dispatch entry points implemented in agent_keyboard.cpp. */
JsonValue handleKeyboardType(double id, const JsonValue &args);
JsonValue handleKeyboardPress(double id, const JsonValue &args);
JsonValue handleKeyboardRelease(double id, const JsonValue &args);
JsonValue handleKeyboardTap(double id, const JsonValue &args);

/* Dispatch entry points implemented in agent_cpu.cpp. */
JsonValue handleRegsGet(double id, const JsonValue &args);
JsonValue handleMemRead(double id, const JsonValue &args);

/* ---- Observability (proposal 4.9) --------------------------------------
 * Watch state + hit counters live in agent.cpp (read on the CPU hot path,
 * written from the main thread). debug.status in agent_observe.cpp reads
 * them via these externs to report each watch's armed/sentinels/hit-count
 * without halting the CPU or consuming the event stream.
 *
 * Multi-sentinel (proposal 4.9.5): each watch is a *set* of sentinels, each
 * carrying its own hit counter. `armed` is just `!empty()`. The matcher that
 * fires records the matched index in the paired g_*WatchWhich so the emitter
 * that runs immediately after (single-threaded, on the same CPU thread) can
 * stamp a `which` field on the event without re-deriving the index. */
struct AgentFarSentinel    { uint16_t seg;                          uint64_t hits; };
struct AgentTargetSentinel { uint16_t seg; uint16_t off;           uint64_t hits; };
struct AgentRangeSentinel  { uint16_t seg; uint16_t lo; uint16_t hi; uint64_t hits; };

extern std::vector<AgentFarSentinel>    g_farWatch;
extern std::vector<AgentTargetSentinel> g_targetWatch;
extern std::vector<AgentRangeSentinel>  g_rangeWatch;
extern int g_farWatchWhich;
extern int g_targetWatchWhich;
extern int g_rangeWatchWhich;

/* Dispatch entry points implemented in agent_observe.cpp. */
JsonValue handleDebugStatus(double id, const JsonValue &args);    /* 4.9.1 */
JsonValue handleCpuProbe(double id, const JsonValue &args);       /* 4.9.2 */
JsonValue handleCpuTraceRing(double id, const JsonValue &args);   /* 4.9.3 */
JsonValue handleCpuTraceback(double id, const JsonValue &args);   /* 4.9.3 */
JsonValue handleCpuDisasm(double id, const JsonValue &args);      /* 4.9.6 */
JsonValue handleMemWatch(double id, const JsonValue &args);       /* 4.9.4 */
JsonValue handleMemUnwatch(double id, const JsonValue &args);     /* 4.9.4 */

/* Reply helpers shared between agent.cpp and agent_keyboard.cpp. */
JsonValue makeReplyOk(double id, JsonObject result);
JsonValue makeReplyError(double id, const std::string &code, const std::string &message);

/* ---- Dispatch -----------------------------------------------------------
 * Receives one complete JSON object from a client and produces a response
 * line. Always returns a string ready to write back (with no trailing
 * newline). */
std::string dispatchLine(const std::string &line);

}  /* namespace agent */

#endif /* C_DEBUG */

#endif /* DOSBOX_AGENT_INTERNAL_H */
