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
JsonValue handleRegsSet(double id, const JsonValue &args);
JsonValue handleMemRead(double id, const JsonValue &args);
JsonValue handleMemWrite(double id, const JsonValue &args);

/* Structured single-step (proposal 4.9.7), implemented in agent_cpu.cpp.
 * cpu.step always replies synchronously. cpu.step_over returns the empty
 * string (like screen.capture) when it stepped over a CALL/INT/LOOP/REP and
 * must defer its reply until the temp-BP pause; AGENT_OnDebuggerPaused sends
 * that deferred reply. buildStepResult reads the *current* CPU state and is
 * shared by the synchronous replies and the deferred one. */
JsonValue   handleCpuStep(double id, const JsonValue &args);
std::string handleCpuStepOver(double id, const JsonValue &args);
JsonObject  buildStepResult(void);

/* state.save / state.restore (proposal 4.9.8), implemented in agent_cpu.cpp.
 * Slot-based, headless wrappers over the savestate subsystem (UI prompts
 * suppressed). Both require the CPU paused; state.restore replies with the
 * restored {regs, cs_ip, insn} so the client sees where the machine resumes. */
JsonValue handleStateSave(double id, const JsonValue &args);
JsonValue handleStateRestore(double id, const JsonValue &args);

/* Pending cpu.step_over reply slot (defined in agent.cpp). Single-slot because
 * Phase 1 is single-client and a step-over can't be issued while one is in
 * flight (the CPU is running until the temp BP fires). */
extern bool   g_stepOverPending;
extern double g_stepOverId;

/* ---- Conditional / Nth-hit breakpoints (proposal 4.9.9) ----------------
 * A conditional breakpoint is checked once per instruction from the heavy-
 * debug hook (AGENT_CondBpCheck), the same path cpu.probe / cpu.trace_ring
 * use, so a disarmed table costs only the g_condBpActive bool load. On a
 * match it can evaluate a small condition expression (register / memory word /
 * Nth-hit), run an allowlisted read-only command macro whose output is
 * captured atomically at the trigger instant (no round-trip through the
 * command poll), and either halt the CPU or auto-resume ("run-and-continue").
 *
 * Heavy-debug only: the per-instruction check is what lets a condition-false
 * reach continue cleanly (just don't halt this instruction). A non-heavy
 * physical BP traps via an injected 0xCC and cannot be "un-halted" from the
 * trap, so bp.set returns `unsupported` in a non-heavy build. This mirrors how
 * cpu.probe / cpu.trace_ring only function under C_HEAVY_DEBUG. */

enum BpCmpOp { BP_EQ, BP_NE, BP_LT, BP_LE, BP_GT, BP_GE };

/* A leaf value source in a condition: an immediate, a register, or the
 * breakpoint's own hit count (`hits`). */
struct BpValSrc {
    enum Kind { IMM, REG, HITS } kind = IMM;
    uint32_t imm = 0;   /* IMM                                   */
    int      reg = 0;   /* REG -> BpReg id (table in agent_cpu.cpp) */
};

/* One operand: either a direct value source, or a real-mode memory
 * dereference [seg:off] read at a given access width (1/2/4 bytes). */
struct BpOperand {
    bool     isMem = false;
    BpValSrc direct;          /* when !isMem */
    BpValSrc memSeg;          /* when isMem  */
    BpValSrc memOff;
    int      memSize = 2;
};

/* A single comparison: (lhs [& mask]) OP rhs. */
struct BpCondition {
    bool      present = false;   /* false -> unconditional (always true)        */
    BpOperand lhs;
    bool      hasMask = false;
    uint32_t  mask = 0;
    int       op = BP_EQ;        /* one of BpCmpOp                              */
    BpOperand rhs;
};

/* One macro step: an allowlisted read-only command name + its args object. */
struct BpMacroCmd {
    std::string cmd;
    JsonValue   args;
};

/* A conditional breakpoint. */
struct CondBp {
    uint32_t                id = 0;
    uint16_t                seg = 0;
    uint16_t                off = 0;
    BpCondition             cond;
    std::string             condStr;   /* echo of the `if` string ("" = none)  */
    std::vector<BpMacroCmd> macro;
    bool                    cont = false;
    uint64_t                hits = 0;   /* every reach (the `hits` operand)     */
    uint64_t                fires = 0;  /* reaches where the condition held     */
};

/* Defined in agent_cpu.cpp; read by debug.status (agent_observe.cpp) and the
 * hot-path hook AGENT_CondBpCheck. g_condBpActive == !g_condBps.empty(). */
extern std::vector<CondBp> g_condBps;
extern bool                g_condBpActive;

/* Parse the `if` condition mini-language into `out`. Returns false and fills
 * `err` with a short message on a malformed expression. Pure (touches no CPU
 * state) so it is unit-testable without MemBase. */
bool parseBpCondition(const std::string &s, BpCondition &out, std::string &err);

/* Evaluate a parsed condition against live CPU registers / memory plus the
 * breakpoint's current hit count. */
bool evalBpCondition(const BpCondition &c, uint64_t hits);

/* Dispatch entry points implemented in agent_cpu.cpp. */
JsonValue handleBpSet(double id, const JsonValue &args);
JsonValue handleBpClear(double id, const JsonValue &args);

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
