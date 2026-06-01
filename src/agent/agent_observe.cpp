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

/* Agent control channel — observability & trust commands (proposal 4.9).
 *
 *   debug.status     (4.9.1) typed snapshot: cpu state, instr_count, every
 *                    breakpoint with its live hit counter + bytes_now, every
 *                    watch with its hit counter, the probe table, trace ring.
 *   cpu.probe        (4.9.2) non-halting execution counter — arm a set of
 *                    (seg:off) points; each maintains a hit count visible in
 *                    debug.status, with no per-hit event and no CPU halt.
 *   cpu.trace_ring   (4.9.3) configure a fixed ring of retired instructions.
 *   cpu.traceback    (4.9.3) dump the last N retired instructions, disassembled.
 *   cpu.disasm       (4.9.6) structured disassembly via the in-tree DasmI386.
 *
 * The probe and trace ring are checked once per instruction from the heavy-
 * debug per-instruction hook (DEBUG_HeavyIsBreakpoint); each is gated on a
 * fast bool so a disarmed observer costs a single load on the hot path. */

#include "config.h"

#if C_DEBUG

#include "dosbox.h"
#include "mem.h"
#include "regs.h"

#include "agent.h"
#include "agent_internal.h"
#include "debug.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* Forward-declared rather than pulled from a header (mirrors agent.cpp's
 * convention of not including the full debugger surface). */
extern bool IsDebuggerActive(void);

namespace agent {

namespace {

/* ---- Execution probe state (4.9.2) ----------------------------------- */
struct ProbePoint { uint16_t seg; uint16_t off; uint64_t hits; };
static std::vector<ProbePoint> g_probes;
static bool                    g_probeActive = false;
const size_t                   PROBE_MAX_POINTS = 256;

/* ---- Instruction-trace ring (4.9.3) ---------------------------------- */
struct TraceEntry { uint16_t seg; uint16_t off; };
static std::vector<TraceEntry> g_traceRing;      /* capacity == ring size  */
static size_t                  g_traceHead  = 0;  /* next slot to write     */
static size_t                  g_traceCount = 0;  /* valid entries (<= cap) */
static bool                    g_traceActive   = false;
static bool                    g_traceSegFilter = false;
static uint16_t                g_traceSeg = 0;
const uint32_t                 TRACE_MAX_DEPTH = 4096;
const uint32_t                 TRACE_DEFAULT_DEPTH = 256;

/* Longest legal x86 instruction; clamp DasmI386's reported size so a bad
 * decode can't make us read/echo a huge byte run. */
const int MAX_INSN_LEN = 15;

/* ---- small parse helpers (number or hex string) ---------------------- */

bool parseHexU32(const std::string &s, uint32_t &out) {
    if (s.empty()) return false;
    size_t pos = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) pos = 2;
    if (pos >= s.size()) return false;
    uint64_t v = 0;
    for (; pos < s.size(); ++pos) {
        char c = s[pos];
        uint32_t d;
        if (c >= '0' && c <= '9') d = uint32_t(c - '0');
        else if (c >= 'a' && c <= 'f') d = 10 + uint32_t(c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + uint32_t(c - 'A');
        else return false;
        v = (v << 4) | d;
        if (v > 0xFFFFFFFFu) return false;
    }
    out = uint32_t(v);
    return true;
}

bool parseSegOffStr(const std::string &s, uint16_t &seg, uint16_t &off) {
    size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    uint32_t sg, of;
    if (!parseHexU32(s.substr(0, colon), sg)) return false;
    if (!parseHexU32(s.substr(colon + 1), of)) return false;
    if (sg > 0xFFFF || of > 0xFFFF) return false;
    seg = uint16_t(sg);
    off = uint16_t(of);
    return true;
}

bool parseU16Any(const JsonValue &v, uint16_t &out) {
    if (v.isNumber()) {
        if (v.n < 0 || v.n > 0xFFFF) return false;
        out = uint16_t(v.n);
        return true;
    }
    if (v.isString()) {
        uint32_t x;
        if (!parseHexU32(v.s, x) || x > 0xFFFF) return false;
        out = uint16_t(x);
        return true;
    }
    return false;
}

/* Read `len` real-mode bytes at seg:off and render them space-separated
 * uppercase hex ("2E FF 65 FE"). phys_readb matches mem.read "physical"
 * semantics (no paging, OOB returns 0xFF); for real-mode code this is the
 * physical == linear address. */
std::string hexBytes(uint16_t seg, uint16_t off, int len) {
    std::string out;
    uint32_t base = (uint32_t(seg) << 4);
    for (int k = 0; k < len; ++k) {
        if (k) out.push_back(' ');
        uint8_t b = phys_readb((PhysPt)(base + uint16_t(off + k)));
        char hb[3];
        snprintf(hb, sizeof(hb), "%02X", b);
        out.append(hb);
    }
    return out;
}

/* Collector passed to DEBUG_AgentForEachBreakpoint; appends one JSON object
 * per breakpoint to the JsonArray pointed at by ctx. */
void bpCollector(void *ctx, const AgentBreakpointInfo *info) {
    JsonArray *arr = static_cast<JsonArray *>(ctx);
    JsonObject o;
    o.emplace("index",   JsonValue::makeNumber(double(info->index)));
    const char *kind = "other";
    switch (info->kind) {
        case AGENT_BPKIND_EXEC: kind = "exec"; break;
        case AGENT_BPKIND_INT:  kind = "int";  break;
        case AGENT_BPKIND_MEM:  kind = "mem";  break;
        default: break;
    }
    o.emplace("kind",    JsonValue::makeString(kind));
    o.emplace("enabled", JsonValue::makeBool(info->enabled));
    o.emplace("hits",    JsonValue::makeNumber(double(info->hits)));
    if (info->kind == AGENT_BPKIND_INT) {
        o.emplace("int", JsonValue::makeNumber(double(info->intnr)));
    } else {
        char addr[24];
        snprintf(addr, sizeof(addr), "%04X:%04X",
                 unsigned(info->seg), unsigned(info->off & 0xFFFF));
        o.emplace("addr", JsonValue::makeString(addr));
        o.emplace("seg",  JsonValue::makeNumber(double(info->seg)));
        o.emplace("off",  JsonValue::makeNumber(double(info->off)));
        if (info->kind == AGENT_BPKIND_EXEC) {
            /* bytes_now catches "armed on a wrong/relocated address". */
            o.emplace("bytes_now",
                JsonValue::makeString(hexBytes(info->seg, uint16_t(info->off), 4)));
        }
    }
    arr->push_back(JsonValue::makeObject(std::move(o)));
}

JsonValue watchObj(bool armed, uint64_t hits) {
    JsonObject o;
    o.emplace("armed", JsonValue::makeBool(armed));
    o.emplace("hits",  JsonValue::makeNumber(double(hits)));
    return JsonValue::makeObject(std::move(o));
}

}  /* anonymous namespace */

/* ---- debug.status (4.9.1) -------------------------------------------- */

JsonValue handleDebugStatus(double id, const JsonValue & /*args*/) {
    JsonObject r;

    r.emplace("cpu", JsonValue::makeString(IsDebuggerActive() ? "paused" : "running"));

    char csip[24];
    snprintf(csip, sizeof(csip), "%04X:%08X",
             unsigned(SegValue(cs)), unsigned(reg_eip));
    r.emplace("cs_ip", JsonValue::makeString(csip));
    r.emplace("cs",    JsonValue::makeNumber(double(SegValue(cs))));
    r.emplace("eip",   JsonValue::makeNumber(double(reg_eip)));
    r.emplace("instr_count", JsonValue::makeNumber(double(cycle_count)));

    /* Breakpoints. */
    JsonArray bps;
    DEBUG_AgentForEachBreakpoint(&bpCollector, &bps);
    r.emplace("breakpoints", JsonValue::makeArray(std::move(bps)));

    /* Watches. */
    JsonObject w;
    {
        JsonObject f;
        f.emplace("armed", JsonValue::makeBool(g_farWatchEnabled));
        f.emplace("hits",  JsonValue::makeNumber(double(g_farWatchHits)));
        if (g_farWatchEnabled)
            f.emplace("seg", JsonValue::makeNumber(double(g_farWatchSeg)));
        w.emplace("far", JsonValue::makeObject(std::move(f)));
    }
    {
        JsonObject t;
        t.emplace("armed", JsonValue::makeBool(g_targetWatchEnabled));
        t.emplace("hits",  JsonValue::makeNumber(double(g_targetWatchHits)));
        if (g_targetWatchEnabled) {
            t.emplace("seg", JsonValue::makeNumber(double(g_targetWatchSeg)));
            t.emplace("off", JsonValue::makeNumber(double(g_targetWatchOff)));
        }
        w.emplace("target", JsonValue::makeObject(std::move(t)));
    }
    {
        JsonObject rg;
        rg.emplace("armed", JsonValue::makeBool(g_rangeWatchEnabled));
        rg.emplace("hits",  JsonValue::makeNumber(double(g_rangeWatchHits)));
        if (g_rangeWatchEnabled) {
            rg.emplace("seg", JsonValue::makeNumber(double(g_rangeWatchSeg)));
            rg.emplace("lo",  JsonValue::makeNumber(double(g_rangeWatchLo)));
            rg.emplace("hi",  JsonValue::makeNumber(double(g_rangeWatchHi)));
        }
        w.emplace("range", JsonValue::makeObject(std::move(rg)));
    }
    r.emplace("watches", JsonValue::makeObject(std::move(w)));

    /* Probe table (4.9.2). */
    JsonArray pr;
    for (const ProbePoint &p : g_probes) {
        JsonObject o;
        char addr[24];
        snprintf(addr, sizeof(addr), "%04X:%04X", unsigned(p.seg), unsigned(p.off));
        o.emplace("addr", JsonValue::makeString(addr));
        o.emplace("seg",  JsonValue::makeNumber(double(p.seg)));
        o.emplace("off",  JsonValue::makeNumber(double(p.off)));
        o.emplace("hits", JsonValue::makeNumber(double(p.hits)));
        pr.push_back(JsonValue::makeObject(std::move(o)));
    }
    r.emplace("probe", JsonValue::makeArray(std::move(pr)));

    /* Trace ring (4.9.3). */
    {
        JsonObject t;
        t.emplace("enabled", JsonValue::makeBool(g_traceActive));
        t.emplace("depth",   JsonValue::makeNumber(double(g_traceRing.size())));
        t.emplace("count",   JsonValue::makeNumber(double(g_traceCount)));
        if (g_traceSegFilter)
            t.emplace("seg", JsonValue::makeNumber(double(g_traceSeg)));
        r.emplace("trace", JsonValue::makeObject(std::move(t)));
    }

    return makeReplyOk(id, std::move(r));
}

/* ---- cpu.probe (4.9.2) ----------------------------------------------- */

JsonValue handleCpuProbe(double id, const JsonValue &args) {
    const JsonValue *pts = args.get("points");
    if (!pts || !pts->isArray()) {
        return makeReplyError(id, "bad_args",
            "expected {\"points\":[\"SEG:OFF\", ...]} ([] to disarm)");
    }

    std::vector<ProbePoint> next;
    if (pts->a) {
        if (pts->a->size() > PROBE_MAX_POINTS) {
            return makeReplyError(id, "bad_args",
                std::string("too many points (max ") +
                std::to_string(PROBE_MAX_POINTS) + ")");
        }
        for (const JsonValue &e : *pts->a) {
            if (!e.isString())
                return makeReplyError(id, "bad_args",
                    "each point must be a \"SEG:OFF\" hex string");
            uint16_t s, o;
            if (!parseSegOffStr(e.s, s, o))
                return makeReplyError(id, "bad_args",
                    std::string("bad point: \"") + e.s + "\"");
            next.push_back({s, o, 0});
        }
    }

    g_probes.swap(next);
    g_probeActive = !g_probes.empty();

    JsonObject r;
    r.emplace("armed", JsonValue::makeNumber(double(g_probes.size())));
    return makeReplyOk(id, std::move(r));
}

/* ---- cpu.trace_ring (4.9.3) ------------------------------------------ */

JsonValue handleCpuTraceRing(double id, const JsonValue &args) {
    /* enabled defaults to true (the common "arm it" call); enabled=false
     * disarms but RETAINS the ring contents so a subsequent traceback after
     * a pause still works. */
    bool enabled = true;
    if (const JsonValue *en = args.get("enabled")) {
        if (!en->isBool())
            return makeReplyError(id, "bad_args", "'enabled' must be a bool");
        enabled = en->b;
    }

    if (!enabled) {
        g_traceActive = false;
        JsonObject r;
        r.emplace("enabled", JsonValue::makeBool(false));
        r.emplace("depth",   JsonValue::makeNumber(double(g_traceRing.size())));
        return makeReplyOk(id, std::move(r));
    }

    uint32_t depth = TRACE_DEFAULT_DEPTH;
    if (const JsonValue *d = args.get("depth")) {
        if (!d->isNumber() || d->n < 1)
            return makeReplyError(id, "bad_args", "'depth' must be a positive integer");
        depth = uint32_t(d->n);
        if (depth > TRACE_MAX_DEPTH) depth = TRACE_MAX_DEPTH;
    }

    bool     segFilter = false;
    uint16_t seg = 0;
    if (const JsonValue *s = args.get("seg")) {
        if (!s->isNull()) {
            if (!parseU16Any(*s, seg))
                return makeReplyError(id, "bad_args",
                    "'seg' must be u16 number, hex string, or null");
            segFilter = true;
        }
    }

    /* (Re)allocate and reset the ring. */
    g_traceRing.assign(depth, TraceEntry{0, 0});
    g_traceHead = 0;
    g_traceCount = 0;
    g_traceSegFilter = segFilter;
    g_traceSeg = seg;
    g_traceActive = true;

    JsonObject r;
    r.emplace("enabled", JsonValue::makeBool(true));
    r.emplace("depth",   JsonValue::makeNumber(double(depth)));
    if (segFilter) r.emplace("seg", JsonValue::makeNumber(double(seg)));
    return makeReplyOk(id, std::move(r));
}

/* ---- cpu.traceback (4.9.3) ------------------------------------------- */

JsonValue handleCpuTraceback(double id, const JsonValue &args) {
    size_t want = g_traceCount;
    if (const JsonValue *c = args.get("count")) {
        if (!c->isNumber() || c->n < 0)
            return makeReplyError(id, "bad_args", "'count' must be a non-negative integer");
        size_t req = size_t(c->n);
        if (req < want) want = req;
    }
    const size_t cap = g_traceRing.size();

    JsonArray entries;
    if (cap != 0 && want != 0) {
        /* Oldest-of-the-last-`want` first, so the array reads most-recent-last. */
        for (size_t i = 0; i < want; ++i) {
            size_t pos = (g_traceHead + cap - want + i) % cap;
            const TraceEntry &e = g_traceRing[pos];

            char text[200];
            int len = DEBUG_AgentDisasmOne(e.seg, e.off, text, sizeof(text));
            if (len < 1) len = 1;
            if (len > MAX_INSN_LEN) len = MAX_INSN_LEN;

            char csip[24];
            snprintf(csip, sizeof(csip), "%04X:%04X", unsigned(e.seg), unsigned(e.off));

            JsonObject o;
            o.emplace("cs_ip", JsonValue::makeString(csip));
            o.emplace("bytes", JsonValue::makeString(hexBytes(e.seg, e.off, len)));
            o.emplace("text",  JsonValue::makeString(text));
            entries.push_back(JsonValue::makeObject(std::move(o)));
        }
    }

    JsonObject r;
    r.emplace("entries", JsonValue::makeArray(std::move(entries)));
    return makeReplyOk(id, std::move(r));
}

/* ---- cpu.disasm (4.9.6) ---------------------------------------------- */

JsonValue handleCpuDisasm(double id, const JsonValue &args) {
    const JsonValue *vaddr = args.get("addr");
    if (!vaddr || !vaddr->isString())
        return makeReplyError(id, "bad_args", "expected {\"addr\":\"SEG:OFF\"}");

    uint16_t seg, off;
    if (!parseSegOffStr(vaddr->s, seg, off))
        return makeReplyError(id, "bad_args", "addr must be \"SEG:OFF\" in hex");

    int count = 1;
    if (const JsonValue *c = args.get("count")) {
        if (!c->isNumber() || c->n < 1)
            return makeReplyError(id, "bad_args", "'count' must be a positive integer");
        count = int(c->n);
        if (count > 64) count = 64;
    }

    JsonArray insns;
    uint16_t cur = off;
    for (int i = 0; i < count; ++i) {
        char text[200];
        int len = DEBUG_AgentDisasmOne(seg, cur, text, sizeof(text));
        if (len < 1) len = 1;
        if (len > MAX_INSN_LEN) len = MAX_INSN_LEN;

        char csip[24];
        snprintf(csip, sizeof(csip), "%04X:%04X", unsigned(seg), unsigned(cur));

        JsonObject o;
        o.emplace("cs_ip", JsonValue::makeString(csip));
        o.emplace("bytes", JsonValue::makeString(hexBytes(seg, cur, len)));
        o.emplace("text",  JsonValue::makeString(text));
        insns.push_back(JsonValue::makeObject(std::move(o)));

        cur = uint16_t(cur + len);
    }

    JsonObject r;
    r.emplace("insns", JsonValue::makeArray(std::move(insns)));
    return makeReplyOk(id, std::move(r));
}

}  /* namespace agent */

/* ---- Public hot-path hooks (called from DEBUG_HeavyIsBreakpoint) ------ */

bool AGENT_ProbeActive(void) { return agent::g_probeActive; }

void AGENT_ProbeCheck(uint16_t seg, uint16_t off) {
    for (agent::ProbePoint &p : agent::g_probes)
        if (p.seg == seg && p.off == off) p.hits++;
}

bool AGENT_TraceActive(void) { return agent::g_traceActive; }

void AGENT_TraceRecord(uint16_t seg, uint16_t off) {
    if (agent::g_traceSegFilter && seg != agent::g_traceSeg) return;
    if (agent::g_traceRing.empty()) return;
    /* Collapse consecutive duplicates of the same CS:IP. The hook
     * (DEBUG_HeavyIsBreakpoint) runs *before* an instruction retires and is
     * re-invoked for the SAME instruction when the core re-evaluates it
     * without advancing — most importantly when a halting breakpoint sits at
     * that address (the BP re-entry would otherwise flood the ring with N
     * identical copies of the BP location and bury the slide that led into
     * it, which is the exact history a traceback exists to show). A genuine
     * tight self-loop (`jmp $`) also dedupes to one entry, which is a fine
     * representation. The ring therefore approximates the *retired*
     * instruction stream. */
    if (agent::g_traceCount > 0) {
        size_t last = (agent::g_traceHead + agent::g_traceRing.size() - 1)
                      % agent::g_traceRing.size();
        const agent::TraceEntry &prev = agent::g_traceRing[last];
        if (prev.seg == seg && prev.off == off) return;
    }
    agent::g_traceRing[agent::g_traceHead] = agent::TraceEntry{seg, off};
    agent::g_traceHead = (agent::g_traceHead + 1) % agent::g_traceRing.size();
    if (agent::g_traceCount < agent::g_traceRing.size()) agent::g_traceCount++;
}

#endif /* C_DEBUG */
