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
#include "paging.h"        /* MEM_GetPageHandler, PFLAG_READABLE, PageHandler */
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

/* ---- mem.watch write-intercept state (4.9.4) ------------------------- */
/* The armed flag itself is the global AGENT_memWatchArmed (defined at the
 * bottom of this file) so the paging.h write hook can read it without going
 * through the agent namespace. Everything else is file-scope here. The
 * linear range [linLo, linHi] is precomputed from seg:lo..hi on arm so the
 * hot path compares the incoming linear write address directly. */
enum MemWatchPred {
    MW_PRED_NONE = 0,
    MW_PRED_NEW_EQ,           /* new == val (masked to size)              */
    MW_PRED_NEW_NE_OLD,       /* new != old (masked to size)              */
    MW_PRED_NEW_AND_MASK_EQ   /* (new & mask) == (val & mask)             */
};
static uint16_t g_memWatchSeg     = 0;
static uint16_t g_memWatchLo      = 0;
static uint16_t g_memWatchHi      = 0;
static uint32_t g_memWatchBase    = 0;   /* seg << 4, precomputed              */
static uint32_t g_memWatchLinLo   = 0;   /* base + lo                          */
static uint32_t g_memWatchLinHi   = 0;   /* base + hi                          */
static int      g_memWatchSize    = 0;   /* 0 = any; else exact 1 / 2 / 4      */
static int      g_memWatchPred    = MW_PRED_NONE;
static uint32_t g_memWatchPredVal = 0;
static uint32_t g_memWatchPredMask= 0;
static uint64_t g_memWatchHits    = 0;

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

bool parseU32Any(const JsonValue &v, uint32_t &out) {
    if (v.isNumber()) {
        if (v.n < 0 || v.n > 4294967295.0) return false;
        out = uint32_t(v.n);
        return true;
    }
    if (v.isString())
        return parseHexU32(v.s, out);
    return false;
}

/* ---- mem.watch helpers (4.9.4) --------------------------------------- */

void memWatchClear() {
    AGENT_memWatchArmed = false;
    g_memWatchSeg = g_memWatchLo = g_memWatchHi = 0;
    g_memWatchBase = g_memWatchLinLo = g_memWatchLinHi = 0;
    g_memWatchSize = 0;
    g_memWatchPred = MW_PRED_NONE;
    g_memWatchPredVal = g_memWatchPredMask = 0;
    g_memWatchHits = 0;
}

/* The cheap range+size pre-filter (no memory read). Shared by the hot-path
 * hook (which calls it before reading the old value) and AGENT_MemWatchMatch
 * (which adds the value predicate). Matches on the write's *starting* address
 * lying in [linLo, linHi]. */
bool memWatchInScope(uint32_t lin_addr, int size) {
    if (g_memWatchSize != 0 && size != g_memWatchSize) return false;
    if (lin_addr < g_memWatchLinLo) return false;
    if (lin_addr > g_memWatchLinHi) return false;
    return true;
}

/* Mask covering `size` low bytes (1->0xFF, 2->0xFFFF, 4->0xFFFFFFFF). */
uint32_t sizeMask(int size) {
    return (size >= 4) ? 0xFFFFFFFFu : ((1u << (unsigned(size) * 8u)) - 1u);
}

/* True when reading `lin_addr` back is side-effect-free, i.e. it resolves to
 * plain host RAM/ROM whose page handler exposes a direct host read pointer
 * (PFLAG_READABLE). The VGA framebuffer and MMIO handlers do NOT set
 * PFLAG_READABLE: a read of VGA memory loads all four plane latches, which
 * would corrupt a latched copy the guest's very next instruction relies on, so
 * the write hook must not read those back. mem.watch is a real-mode tool (its
 * range is the real-mode linear (seg<<4)+off), so `lin_addr` is treated as the
 * physical page for the handler lookup — exactly how emitMemWrite and the
 * mem.read "physical" path already treat it. */
bool memReadIsSideEffectFree(uint32_t lin_addr) {
    PageHandler *h = MEM_GetPageHandler(lin_addr >> 12);
    return h != nullptr && (h->getFlags() & PFLAG_READABLE) != 0;
}

std::string memWatchPredDesc() {
    char buf[80];
    switch (g_memWatchPred) {
        case MW_PRED_NEW_EQ:
            snprintf(buf, sizeof(buf), "new_eq=0x%X", unsigned(g_memWatchPredVal));
            return buf;
        case MW_PRED_NEW_NE_OLD:
            return "new_ne_old";
        case MW_PRED_NEW_AND_MASK_EQ:
            snprintf(buf, sizeof(buf), "new_and_mask_eq mask=0x%X value=0x%X",
                     unsigned(g_memWatchPredMask), unsigned(g_memWatchPredVal));
            return buf;
        default:
            return "none";
    }
}

/* Build and broadcast the mem.write event. from_cs/from_ip come from the
 * heavy-debug previous-instruction tracker — i.e. the instruction that is
 * *currently executing the store* (DEBUG_HeavyIsBreakpoint saves the start of
 * each instruction before it runs). Built through jsonEncode so from_text
 * (disassembly of the storing instruction) is escaped safely. `oldKnown` is
 * false for side-effecting destinations (VGA/MMIO) where the old value was not
 * sampled; `old` is then reported as JSON null. */
void emitMemWrite(uint32_t lin_addr, uint32_t oldval, uint32_t newval, int size, bool oldKnown) {
    uint16_t seg = g_memWatchSeg;
    uint16_t off = uint16_t((lin_addr - g_memWatchBase) & 0xFFFFu);
    uint16_t from_cs = DEBUG_GetPrevCS();
    uint16_t from_ip = DEBUG_GetPrevIP();
    uint32_t mask = sizeMask(size);

    JsonObject o;
    o.emplace("event", JsonValue::makeString("mem.write"));
    o.emplace("seg",   JsonValue::makeNumber(double(seg)));
    o.emplace("off",   JsonValue::makeNumber(double(off)));
    char addr[24];
    snprintf(addr, sizeof(addr), "%04X:%04X", unsigned(seg), unsigned(off));
    o.emplace("addr",  JsonValue::makeString(addr));
    o.emplace("size",  JsonValue::makeNumber(double(size)));
    o.emplace("old",   oldKnown ? JsonValue::makeNumber(double(oldval & mask))
                                : JsonValue::makeNull());
    o.emplace("new",   JsonValue::makeNumber(double(newval & mask)));
    o.emplace("from_cs", JsonValue::makeNumber(double(from_cs)));
    o.emplace("from_ip", JsonValue::makeNumber(double(from_ip)));
    char from[24];
    snprintf(from, sizeof(from), "%04X:%04X", unsigned(from_cs), unsigned(from_ip));
    o.emplace("from",  JsonValue::makeString(from));
    char text[200];
    int len = DEBUG_AgentDisasmOne(from_cs, from_ip, text, sizeof(text));
    if (len >= 1)
        o.emplace("from_text", JsonValue::makeString(text));

    serverBroadcastLine(jsonEncode(JsonValue::makeObject(std::move(o))));
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

    /* Watches. Each watch is a *set* of sentinels (proposal 4.9.5). `hits` is
     * the total across the set; `sentinels[]` carries the per-sentinel count
     * (the `which` index in emitted events is the position in this array). For
     * back-compat the single-sentinel case also exposes the first sentinel's
     * seg/off/lo/hi at the top level. */
    JsonObject w;
    {
        JsonObject f;
        uint64_t total = 0;
        JsonArray sent;
        for (const AgentFarSentinel &s : g_farWatch) {
            total += s.hits;
            JsonObject so;
            so.emplace("seg",  JsonValue::makeNumber(double(s.seg)));
            so.emplace("hits", JsonValue::makeNumber(double(s.hits)));
            sent.push_back(JsonValue::makeObject(std::move(so)));
        }
        f.emplace("armed", JsonValue::makeBool(!g_farWatch.empty()));
        f.emplace("hits",  JsonValue::makeNumber(double(total)));
        if (!g_farWatch.empty())
            f.emplace("seg", JsonValue::makeNumber(double(g_farWatch[0].seg)));
        f.emplace("sentinels", JsonValue::makeArray(std::move(sent)));
        w.emplace("far", JsonValue::makeObject(std::move(f)));
    }
    {
        JsonObject t;
        uint64_t total = 0;
        JsonArray sent;
        for (const AgentTargetSentinel &s : g_targetWatch) {
            total += s.hits;
            JsonObject so;
            so.emplace("seg",  JsonValue::makeNumber(double(s.seg)));
            so.emplace("off",  JsonValue::makeNumber(double(s.off)));
            so.emplace("hits", JsonValue::makeNumber(double(s.hits)));
            sent.push_back(JsonValue::makeObject(std::move(so)));
        }
        t.emplace("armed", JsonValue::makeBool(!g_targetWatch.empty()));
        t.emplace("hits",  JsonValue::makeNumber(double(total)));
        if (!g_targetWatch.empty()) {
            t.emplace("seg", JsonValue::makeNumber(double(g_targetWatch[0].seg)));
            t.emplace("off", JsonValue::makeNumber(double(g_targetWatch[0].off)));
        }
        t.emplace("sentinels", JsonValue::makeArray(std::move(sent)));
        w.emplace("target", JsonValue::makeObject(std::move(t)));
    }
    {
        JsonObject rg;
        uint64_t total = 0;
        JsonArray sent;
        for (const AgentRangeSentinel &s : g_rangeWatch) {
            total += s.hits;
            JsonObject so;
            so.emplace("seg",  JsonValue::makeNumber(double(s.seg)));
            so.emplace("lo",   JsonValue::makeNumber(double(s.lo)));
            so.emplace("hi",   JsonValue::makeNumber(double(s.hi)));
            so.emplace("hits", JsonValue::makeNumber(double(s.hits)));
            sent.push_back(JsonValue::makeObject(std::move(so)));
        }
        rg.emplace("armed", JsonValue::makeBool(!g_rangeWatch.empty()));
        rg.emplace("hits",  JsonValue::makeNumber(double(total)));
        if (!g_rangeWatch.empty()) {
            rg.emplace("seg", JsonValue::makeNumber(double(g_rangeWatch[0].seg)));
            rg.emplace("lo",  JsonValue::makeNumber(double(g_rangeWatch[0].lo)));
            rg.emplace("hi",  JsonValue::makeNumber(double(g_rangeWatch[0].hi)));
        }
        rg.emplace("sentinels", JsonValue::makeArray(std::move(sent)));
        w.emplace("range", JsonValue::makeObject(std::move(rg)));
    }
    {
        /* mem.watch (4.9.4): the write-intercept watch. size=0 means "any
         * access width"; predicate is a short human-readable description. */
        JsonObject m;
        m.emplace("armed", JsonValue::makeBool(AGENT_memWatchArmed));
        m.emplace("hits",  JsonValue::makeNumber(double(g_memWatchHits)));
        if (AGENT_memWatchArmed) {
            m.emplace("seg",  JsonValue::makeNumber(double(g_memWatchSeg)));
            m.emplace("lo",   JsonValue::makeNumber(double(g_memWatchLo)));
            m.emplace("hi",   JsonValue::makeNumber(double(g_memWatchHi)));
            m.emplace("size", JsonValue::makeNumber(double(g_memWatchSize)));
            m.emplace("predicate", JsonValue::makeString(memWatchPredDesc()));
        }
        w.emplace("mem", JsonValue::makeObject(std::move(m)));
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

    /* Conditional / Nth-hit breakpoints (4.9.9). Each carries the reach
     * counter `hits` (the value the `hits` operand sees) and `fires` (the
     * subset where the condition held and the macro ran) so an agent can tell
     * "reached but never matched" from "matched N times" without consuming the
     * bp.cond event stream — the 4.9.1 "fired or not?" principle applied to
     * conditional BPs too. */
    {
        JsonArray cbs;
        for (const CondBp &bp : g_condBps) {
            JsonObject o;
            o.emplace("bp_id", JsonValue::makeNumber(double(bp.id)));
            char addr[24];
            snprintf(addr, sizeof(addr), "%04X:%04X", unsigned(bp.seg), unsigned(bp.off));
            o.emplace("addr", JsonValue::makeString(addr));
            o.emplace("seg",  JsonValue::makeNumber(double(bp.seg)));
            o.emplace("off",  JsonValue::makeNumber(double(bp.off)));
            o.emplace("condition", bp.condStr.empty()
                      ? JsonValue::makeNull() : JsonValue::makeString(bp.condStr));
            o.emplace("macro_len", JsonValue::makeNumber(double(bp.macro.size())));
            o.emplace("continue",  JsonValue::makeBool(bp.cont));
            o.emplace("hits",  JsonValue::makeNumber(double(bp.hits)));
            o.emplace("fires", JsonValue::makeNumber(double(bp.fires)));
            cbs.push_back(JsonValue::makeObject(std::move(o)));
        }
        r.emplace("cond_breakpoints", JsonValue::makeArray(std::move(cbs)));
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

/* ---- mem.watch (4.9.4) ----------------------------------------------- */

JsonValue handleMemWatch(double id, const JsonValue &args) {
    const JsonValue *seg = args.get("seg");
    if (!seg) {
        return makeReplyError(id, "bad_args",
            "expected {\"seg\":<u16 or hex>, \"lo\":<u16>, \"hi\":<u16>"
            "[, \"size\":1|2|4, \"when\":{...}]} (seg=null to clear)");
    }
    if (seg->isNull()) {
        memWatchClear();
        JsonObject r;
        r.emplace("armed", JsonValue::makeBool(false));
        return makeReplyOk(id, std::move(r));
    }

    uint16_t segV = 0, loV = 0, hiV = 0;
    if (!parseU16Any(*seg, segV))
        return makeReplyError(id, "bad_args", "seg must be u16 number or hex string");
    const JsonValue *lo = args.get("lo");
    const JsonValue *hi = args.get("hi");
    if (!lo || !hi)
        return makeReplyError(id, "bad_args", "lo and hi required when seg is set");
    if (!parseU16Any(*lo, loV))
        return makeReplyError(id, "bad_args", "lo must be u16 number or hex string");
    if (!parseU16Any(*hi, hiV))
        return makeReplyError(id, "bad_args", "hi must be u16 number or hex string");
    if (loV > hiV)
        return makeReplyError(id, "bad_args", "lo must be <= hi");

    /* Optional access-size filter. */
    int sizeV = 0;
    if (const JsonValue *sz = args.get("size")) {
        if (!sz->isNull()) {
            if (!sz->isNumber())
                return makeReplyError(id, "bad_args", "size must be 1, 2, or 4");
            int s = int(sz->n);
            if (s != 1 && s != 2 && s != 4)
                return makeReplyError(id, "bad_args", "size must be 1, 2, or 4");
            sizeV = s;
        }
    }

    /* Optional value predicate — exactly one of the three forms. */
    int      predV     = MW_PRED_NONE;
    uint32_t predValV  = 0;
    uint32_t predMaskV = 0;
    if (const JsonValue *when = args.get("when")) {
        if (!when->isNull()) {
            if (!when->isObject())
                return makeReplyError(id, "bad_args", "'when' must be an object");
            const JsonValue *neq   = when->get("new_eq");
            const JsonValue *nne   = when->get("new_ne_old");
            const JsonValue *nmask = when->get("new_and_mask_eq");
            int count = (neq ? 1 : 0) + (nne ? 1 : 0) + (nmask ? 1 : 0);
            if (count == 0)
                return makeReplyError(id, "bad_args",
                    "'when' needs one of new_eq / new_ne_old / new_and_mask_eq");
            if (count > 1)
                return makeReplyError(id, "bad_args",
                    "'when' accepts only one predicate at a time");
            if (neq) {
                if (!parseU32Any(*neq, predValV))
                    return makeReplyError(id, "bad_args",
                        "new_eq must be a u32 number or hex string");
                predV = MW_PRED_NEW_EQ;
            } else if (nne) {
                if (!nne->isBool() || !nne->b)
                    return makeReplyError(id, "bad_args", "new_ne_old must be true");
                predV = MW_PRED_NEW_NE_OLD;
            } else {
                if (!nmask->isObject())
                    return makeReplyError(id, "bad_args",
                        "new_and_mask_eq must be {\"mask\":<u32>, \"value\":<u32>}");
                const JsonValue *m   = nmask->get("mask");
                const JsonValue *val = nmask->get("value");
                if (!m || !val)
                    return makeReplyError(id, "bad_args",
                        "new_and_mask_eq needs both mask and value");
                if (!parseU32Any(*m, predMaskV))
                    return makeReplyError(id, "bad_args",
                        "mask must be a u32 number or hex string");
                if (!parseU32Any(*val, predValV))
                    return makeReplyError(id, "bad_args",
                        "value must be a u32 number or hex string");
                predV = MW_PRED_NEW_AND_MASK_EQ;
            }
        }
    }

    /* Commit. */
    g_memWatchSeg      = segV;
    g_memWatchLo       = loV;
    g_memWatchHi       = hiV;
    g_memWatchBase     = uint32_t(segV) << 4;
    g_memWatchLinLo    = g_memWatchBase + loV;
    g_memWatchLinHi    = g_memWatchBase + hiV;
    g_memWatchSize     = sizeV;
    g_memWatchPred     = predV;
    g_memWatchPredVal  = predValV;
    g_memWatchPredMask = predMaskV;
    g_memWatchHits     = 0;
    AGENT_memWatchArmed = true;

    JsonObject r;
    r.emplace("armed",     JsonValue::makeBool(true));
    r.emplace("seg",       JsonValue::makeNumber(double(segV)));
    r.emplace("lo",        JsonValue::makeNumber(double(loV)));
    r.emplace("hi",        JsonValue::makeNumber(double(hiV)));
    r.emplace("size",      JsonValue::makeNumber(double(sizeV)));
    r.emplace("predicate", JsonValue::makeString(memWatchPredDesc()));
    return makeReplyOk(id, std::move(r));
}

JsonValue handleMemUnwatch(double id, const JsonValue & /*args*/) {
    memWatchClear();
    JsonObject r;
    r.emplace("armed", JsonValue::makeBool(false));
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

/* ---- mem.watch hot-path hook (4.9.4) --------------------------------- */

/* The fast gate read by mem_write{b,w,d}_inline (paging.h). Global scope so
 * that very hot path needn't reach into the agent namespace. */
bool AGENT_memWatchArmed = false;

/* Pure predicate: does this write match the armed watch? No memory read, no
 * event, no counter bump — so the unit tests can drive it directly with a
 * synthesised old/new pair and no MemBase. */
bool AGENT_MemWatchMatch(uint32_t lin_addr, uint32_t newval, uint32_t oldval, int size) {
    if (!AGENT_memWatchArmed) return false;
    if (!agent::memWatchInScope(lin_addr, size)) return false;

    const uint32_t mask = agent::sizeMask(size);
    const uint32_t nv = newval & mask;   /* the new value, at the access width */
    const uint32_t ov = oldval & mask;
    switch (agent::g_memWatchPred) {
        case agent::MW_PRED_NEW_EQ:
            /* Compare against the full predicate value: a new_eq wider than the
             * access size (e.g. new_eq=0x0853 against a 1-byte write) can never
             * match, rather than matching on a coincidental low byte. */
            return nv == agent::g_memWatchPredVal;
        case agent::MW_PRED_NEW_NE_OLD:
            return nv != ov;
        case agent::MW_PRED_NEW_AND_MASK_EQ:
            return (nv & agent::g_memWatchPredMask)
                 == (agent::g_memWatchPredVal & agent::g_memWatchPredMask);
        case agent::MW_PRED_NONE:
        default:
            return true;
    }
}

/* Predicate variant for writes whose destination cannot be read back without
 * side effects (the VGA framebuffer, MMIO). The old value is unknown, so this
 * evaluates only the new-value predicates: new_eq / new_and_mask_eq test the
 * stored value directly; new_ne_old conservatively matches (we cannot prove an
 * idempotent store without the old value); none matches every in-scope write.
 * Like AGENT_MemWatchMatch it self-gates on armed + range/size scope. */
bool AGENT_MemWatchMatchNoOld(uint32_t lin_addr, uint32_t newval, int size) {
    if (!AGENT_memWatchArmed) return false;
    if (!agent::memWatchInScope(lin_addr, size)) return false;

    const uint32_t mask = agent::sizeMask(size);
    const uint32_t nv = newval & mask;
    switch (agent::g_memWatchPred) {
        case agent::MW_PRED_NEW_EQ:
            return nv == agent::g_memWatchPredVal;
        case agent::MW_PRED_NEW_AND_MASK_EQ:
            return (nv & agent::g_memWatchPredMask)
                 == (agent::g_memWatchPredVal & agent::g_memWatchPredMask);
        case agent::MW_PRED_NEW_NE_OLD:   /* old unknown -> cannot filter      */
            return true;
        case agent::MW_PRED_NONE:
        default:
            return true;
    }
}

/* Called from the guest memory-write path for every write while armed (the
 * armed flag is checked at the call site so the disarmed cost is one bool
 * load). Runs BEFORE the store, so a plain read returns the old value. On a
 * match, bumps the hit counter and emits the mem.write event.
 *
 * The destination governs whether the old value can be sampled: plain host
 * RAM/ROM (PFLAG_READABLE) is read back as today; the VGA framebuffer and MMIO
 * are NOT read back — a VGA read loads the plane latches, which would corrupt a
 * latched copy the guest's next instruction relies on — so for those old is
 * left unknown (reported null) and only the new-value predicate is evaluated.
 * This is what extends mem.watch to the VGA framebuffer (proposal 4.9.4)
 * without disturbing emulation. */
void AGENT_MemWatchNote(uint32_t lin_addr, uint32_t newval, int size) {
    /* Cheap range/size pre-filter before touching anything. */
    if (!agent::memWatchInScope(lin_addr, size)) return;
    if (size != 1 && size != 2 && size != 4) return;

    const bool oldKnown = agent::memReadIsSideEffectFree(lin_addr);

    uint32_t oldval = 0;
    bool matched;
    if (oldKnown) {
        switch (size) {
            case 1: oldval = mem_readb(lin_addr); break;
            case 2: oldval = mem_readw(lin_addr); break;
            case 4: oldval = mem_readd(lin_addr); break;
            default: return;   /* unreachable: size validated above */
        }
        matched = AGENT_MemWatchMatch(lin_addr, newval, oldval, size);
    } else {
        matched = AGENT_MemWatchMatchNoOld(lin_addr, newval, size);
    }
    if (!matched) return;

    agent::g_memWatchHits++;
    agent::emitMemWrite(lin_addr, oldval, newval, size, oldKnown);
}

#endif /* C_DEBUG */
