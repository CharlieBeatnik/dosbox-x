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

/* Agent control channel — observability & trust tests (proposal 4.9).
 *
 * What is and isn't covered here, and why:
 *
 *  - The probe (4.9.2) and trace ring (4.9.3) are pure agent-side state with
 *    hot-path hooks (AGENT_ProbeCheck / AGENT_TraceRecord) that touch no
 *    guest memory. So we can drive them directly the way the CPU core would,
 *    then read the result back through debug.status — a genuine end-to-end
 *    check of the counter/ring logic without a running guest.
 *
 *  - The watch hit counters (4.9.1) live on AGENT_*WatchMatches, also pure
 *    agent state, so they too are exercised directly.
 *
 *  - cpu.disasm (4.9.6) and a non-empty cpu.traceback dump call
 *    DEBUG_AgentDisasmOne -> DasmI386, which reads guest memory via MemBase.
 *    In -tests mode the memory subsystem is not initialised (MemBase==NULL),
 *    exactly as documented in agent_cpu_tests.cpp, so here we only exercise
 *    their argument validation and empty-ring shapes. The real byte-level
 *    decode is covered by tests/agent_live/test_observability.py against a
 *    booted guest.
 *
 *  - debug.status's per-exec-breakpoint bytes_now field also reads guest
 *    memory; every debug.status test below therefore clears the BP list
 *    first so that path is never taken in-process. */

#include <string>

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "../include/agent.h"
#include "../src/agent/agent_internal.h"
#include "regs.h"

namespace {

using namespace agent;

class AgentObservabilityTest : public DOSBoxTestFixture {
public:
    void SetUp() override {
        /* Disarm everything so each test starts from a known-clean state. */
        AGENT_FarWatchClear();
        AGENT_TargetWatchClear();
        AGENT_RangeWatchClear();
        dispatchLine("{\"id\":99,\"cmd\":\"cpu.probe\",\"args\":{\"points\":[]}}");
        /* Reset the trace ring to *empty*: re-arming with a depth clears the
         * recorded count, then disarm. A bare enabled:false would deliberately
         * RETAIN a prior test's recorded entries (that retention is a feature —
         * it lets a post-pause traceback work — so it must be cleared here, not
         * relied upon to be empty). */
        dispatchLine("{\"id\":99,\"cmd\":\"cpu.trace_ring\",\"args\":{\"depth\":256}}");
        dispatchLine("{\"id\":99,\"cmd\":\"cpu.trace_ring\",\"args\":{\"enabled\":false}}");
        dispatchLine("{\"id\":99,\"cmd\":\"mem.unwatch\"}");
        /* Clear the BP list so debug.status never reads bytes_now (no MemBase). */
        dispatchLine("{\"id\":99,\"cmd\":\"debugger.command\",\"args\":{\"text\":\"BPDEL 0 *\"}}");
    }
    void TearDown() override {
        AGENT_FarWatchClear();
        AGENT_TargetWatchClear();
        AGENT_RangeWatchClear();
        dispatchLine("{\"id\":99,\"cmd\":\"cpu.probe\",\"args\":{\"points\":[]}}");
        dispatchLine("{\"id\":99,\"cmd\":\"cpu.trace_ring\",\"args\":{\"enabled\":false}}");
        dispatchLine("{\"id\":99,\"cmd\":\"mem.unwatch\"}");
    }

    static JsonValue parse(const std::string &reply) {
        JsonValue v;
        EXPECT_TRUE(jsonParse(reply, v)) << reply;
        return v;
    }
    static JsonValue status() {
        JsonValue v = parse(dispatchLine("{\"id\":1,\"cmd\":\"debug.status\"}"));
        EXPECT_TRUE(v.get("ok") && v.get("ok")->b);
        return v;
    }
};

/* ---- debug.status reply shape (4.9.1) -------------------------------- */

TEST_F(AgentObservabilityTest, StatusHasAllTopLevelFields)
{
    JsonValue v = status();
    const JsonValue *r = v.get("result");
    ASSERT_TRUE(r && r->isObject());
    EXPECT_TRUE(r->get("cpu") && r->get("cpu")->isString());
    EXPECT_TRUE(r->get("cs_ip") && r->get("cs_ip")->isString());
    EXPECT_TRUE(r->get("instr_count") && r->get("instr_count")->isNumber());
    EXPECT_TRUE(r->get("breakpoints") && r->get("breakpoints")->isArray());
    EXPECT_TRUE(r->get("watches") && r->get("watches")->isObject());
    EXPECT_TRUE(r->get("probe") && r->get("probe")->isArray());
    EXPECT_TRUE(r->get("trace") && r->get("trace")->isObject());

    const JsonValue *w = r->get("watches");
    EXPECT_TRUE(w->get("far") && w->get("far")->isObject());
    EXPECT_TRUE(w->get("target") && w->get("target")->isObject());
    EXPECT_TRUE(w->get("range") && w->get("range")->isObject());
    EXPECT_TRUE(w->get("mem") && w->get("mem")->isObject());
}

TEST_F(AgentObservabilityTest, StatusReportsCsIpFromRegisters)
{
    Segs.val[cs] = 0x0824;
    reg_eip      = 0x99E2;
    JsonValue v = status();
    EXPECT_EQ(v.get("result")->get("cs_ip")->s, "0824:000099E2");
    EXPECT_EQ(uint16_t(v.get("result")->get("cs")->n), 0x0824u);
    EXPECT_EQ(uint32_t(v.get("result")->get("eip")->n), 0x99E2u);
}

/* ---- watch hit counters (4.9.1) ------------------------------------- */

TEST_F(AgentObservabilityTest, TargetWatchHitsCountAndSurfaceInStatus)
{
    AGENT_TargetWatchSet(0x0824, 0xC01E);

    /* Fresh arm: armed true, zero hits. */
    {
        JsonValue v = status();
        const JsonValue *t = v.get("result")->get("watches")->get("target");
        EXPECT_TRUE(t->get("armed")->b);
        EXPECT_EQ(uint64_t(t->get("hits")->n), 0u);
        EXPECT_EQ(uint16_t(t->get("seg")->n), 0x0824u);
        EXPECT_EQ(uint16_t(t->get("off")->n), 0xC01Eu);
    }

    /* Three matches + two non-matches: the matcher is what the CPU core
     * calls, and it both returns true and bumps the counter. */
    EXPECT_TRUE (AGENT_TargetWatchMatches(0x0824, 0xC01E));
    EXPECT_FALSE(AGENT_TargetWatchMatches(0x0824, 0xC01F));
    EXPECT_TRUE (AGENT_TargetWatchMatches(0x0824, 0xC01E));
    EXPECT_FALSE(AGENT_TargetWatchMatches(0x0000, 0xC01E));
    EXPECT_TRUE (AGENT_TargetWatchMatches(0x0824, 0xC01E));

    JsonValue v = status();
    EXPECT_EQ(uint64_t(v.get("result")->get("watches")->get("target")->get("hits")->n), 3u);
}

TEST_F(AgentObservabilityTest, FarWatchHitsCount)
{
    AGENT_FarWatchSet(0x483C);
    EXPECT_TRUE (AGENT_FarWatchMatches(0x483C));
    EXPECT_TRUE (AGENT_FarWatchMatches(0x483C));
    EXPECT_FALSE(AGENT_FarWatchMatches(0x0001));
    JsonValue v = status();
    const JsonValue *f = v.get("result")->get("watches")->get("far");
    EXPECT_TRUE(f->get("armed")->b);
    EXPECT_EQ(uint64_t(f->get("hits")->n), 2u);
}

TEST_F(AgentObservabilityTest, RangeWatchHitsCountOnlyFromOutside)
{
    AGENT_RangeWatchSet(0x0824, 0x9600, 0x99FF);
    /* Entry from outside the window: counts. */
    EXPECT_TRUE(AGENT_RangeWatchEntry(0x0824, 0x9700, 0x0824, 0x6000));
    /* Intra-range step (from inside): must NOT count. */
    EXPECT_FALSE(AGENT_RangeWatchEntry(0x0824, 0x9701, 0x0824, 0x9700));
    /* FAR landing from another seg: counts. */
    EXPECT_TRUE(AGENT_RangeWatchEntry(0x0824, 0x9700, 0x1000, 0x0010));
    JsonValue v = status();
    EXPECT_EQ(uint64_t(v.get("result")->get("watches")->get("range")->get("hits")->n), 2u);
}

TEST_F(AgentObservabilityTest, ClearResetsHitCounter)
{
    AGENT_TargetWatchSet(0x1000, 0x2000);
    AGENT_TargetWatchMatches(0x1000, 0x2000);
    AGENT_TargetWatchClear();
    JsonValue v = status();
    const JsonValue *t = v.get("result")->get("watches")->get("target");
    EXPECT_FALSE(t->get("armed")->b);
    EXPECT_EQ(uint64_t(t->get("hits")->n), 0u);
}

/* ---- cpu.probe (4.9.2) ----------------------------------------------- */

TEST_F(AgentObservabilityTest, ProbeArmsAndDisarms)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.probe\",\"args\":{\"points\":[\"0824:6F10\",\"0824:99E2\"]}}"));
    ASSERT_TRUE(v.get("ok")->b);
    EXPECT_EQ(int(v.get("result")->get("armed")->n), 2);
    EXPECT_TRUE(AGENT_ProbeActive());

    /* Empty list disarms. */
    v = parse(dispatchLine("{\"id\":2,\"cmd\":\"cpu.probe\",\"args\":{\"points\":[]}}"));
    ASSERT_TRUE(v.get("ok")->b);
    EXPECT_EQ(int(v.get("result")->get("armed")->n), 0);
    EXPECT_FALSE(AGENT_ProbeActive());
}

TEST_F(AgentObservabilityTest, ProbeCountsOnlyMatchingAddresses)
{
    dispatchLine("{\"id\":1,\"cmd\":\"cpu.probe\",\"args\":{\"points\":[\"0824:6F10\",\"0824:99E2\"]}}");
    ASSERT_TRUE(AGENT_ProbeActive());

    /* Simulate the per-instruction hook the heavy core would call. */
    for (int i = 0; i < 5; ++i) AGENT_ProbeCheck(0x0824, 0x6F10);
    for (int i = 0; i < 2; ++i) AGENT_ProbeCheck(0x0824, 0x99E2);
    AGENT_ProbeCheck(0x0824, 0x1234);   /* not a probe point */

    JsonValue v = status();
    const JsonValue *pr = v.get("result")->get("probe");
    ASSERT_TRUE(pr->isArray() && pr->a && pr->a->size() == 2);

    /* Points are reported in insertion order. */
    const JsonValue &p0 = (*pr->a)[0];
    const JsonValue &p1 = (*pr->a)[1];
    EXPECT_EQ(p0.get("addr")->s, "0824:6F10");
    EXPECT_EQ(uint64_t(p0.get("hits")->n), 5u);
    EXPECT_EQ(p1.get("addr")->s, "0824:99E2");
    EXPECT_EQ(uint64_t(p1.get("hits")->n), 2u);
}

TEST_F(AgentObservabilityTest, ProbeInactiveDoesNothing)
{
    /* No probe armed: ProbeActive false; a stray check is harmless. */
    EXPECT_FALSE(AGENT_ProbeActive());
    AGENT_ProbeCheck(0x0824, 0x6F10);   /* must not crash */
    JsonValue v = status();
    EXPECT_EQ(v.get("result")->get("probe")->a->size(), 0u);
}

TEST_F(AgentObservabilityTest, ProbeRejectsNonArrayPoints)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.probe\",\"args\":{\"points\":\"0824:6F10\"}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentObservabilityTest, ProbeRejectsMalformedPoint)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.probe\",\"args\":{\"points\":[\"deadbeef\"]}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
    /* A rejected arm must not partially arm. */
    EXPECT_FALSE(AGENT_ProbeActive());
}

/* ---- cpu.trace_ring + cpu.traceback (4.9.3) -------------------------- */

TEST_F(AgentObservabilityTest, TraceRingArmsWithDepthAndDisarmsKeepingContents)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.trace_ring\",\"args\":{\"depth\":8}}"));
    ASSERT_TRUE(v.get("ok")->b);
    EXPECT_TRUE(v.get("result")->get("enabled")->b);
    EXPECT_EQ(int(v.get("result")->get("depth")->n), 8);
    EXPECT_TRUE(AGENT_TraceActive());

    /* Disarm keeps the allocated ring (so a post-pause traceback still works). */
    v = parse(dispatchLine("{\"id\":2,\"cmd\":\"cpu.trace_ring\",\"args\":{\"enabled\":false}}"));
    ASSERT_TRUE(v.get("ok")->b);
    EXPECT_FALSE(v.get("result")->get("enabled")->b);
    EXPECT_FALSE(AGENT_TraceActive());
    EXPECT_EQ(int(v.get("result")->get("depth")->n), 8);
}

TEST_F(AgentObservabilityTest, TraceRingRecordsAndStatusCounts)
{
    dispatchLine("{\"id\":1,\"cmd\":\"cpu.trace_ring\",\"args\":{\"depth\":4}}");
    ASSERT_TRUE(AGENT_TraceActive());

    /* Record 6 entries into a depth-4 ring: count saturates at 4. */
    for (uint16_t i = 0; i < 6; ++i) AGENT_TraceRecord(0x0824, uint16_t(0x1000 + i));

    JsonValue v = status();
    const JsonValue *t = v.get("result")->get("trace");
    EXPECT_TRUE(t->get("enabled")->b);
    EXPECT_EQ(int(t->get("depth")->n), 4);
    EXPECT_EQ(int(t->get("count")->n), 4);
}

TEST_F(AgentObservabilityTest, TraceRingSegFilterDropsOtherSegments)
{
    dispatchLine("{\"id\":1,\"cmd\":\"cpu.trace_ring\",\"args\":{\"depth\":16,\"seg\":\"0824\"}}");
    ASSERT_TRUE(AGENT_TraceActive());

    AGENT_TraceRecord(0x0824, 0x1000);   /* kept   */
    AGENT_TraceRecord(0x1000, 0x2000);   /* dropped (wrong seg) */
    AGENT_TraceRecord(0x0824, 0x1002);   /* kept   */

    JsonValue v = status();
    EXPECT_EQ(int(v.get("result")->get("trace")->get("count")->n), 2);
    EXPECT_EQ(uint16_t(v.get("result")->get("trace")->get("seg")->n), 0x0824u);
}

TEST_F(AgentObservabilityTest, TraceRingCollapsesConsecutiveDuplicates)
{
    /* The hook is re-invoked for the same CS:IP when a halting breakpoint
     * sits there; without dedup the ring would fill with N copies of that
     * address and bury the slide that led into it. Consecutive duplicates
     * must collapse to one; a non-adjacent repeat is recorded again. */
    dispatchLine("{\"id\":1,\"cmd\":\"cpu.trace_ring\",\"args\":{\"depth\":16}}");
    ASSERT_TRUE(AGENT_TraceActive());

    AGENT_TraceRecord(0x0824, 0x1000);
    AGENT_TraceRecord(0x0824, 0x1003);
    for (int i = 0; i < 50; ++i)            /* BP re-entry flood at one addr */
        AGENT_TraceRecord(0x0824, 0x1008);
    AGENT_TraceRecord(0x0824, 0x1003);      /* same addr, not adjacent → kept */

    JsonValue v = status();
    /* 1000, 1003, 1008 (collapsed from 50), 1003 == 4 distinct retired steps. */
    EXPECT_EQ(int(v.get("result")->get("trace")->get("count")->n), 4);
}

TEST_F(AgentObservabilityTest, TracebackEmptyOnCleanRing)
{
    /* SetUp left a freshly-allocated, disarmed, *empty* ring. A traceback on
     * it must return zero entries (nothing recorded yet) without touching
     * guest memory / disassembly. */
    JsonValue v = parse(dispatchLine("{\"id\":1,\"cmd\":\"cpu.traceback\",\"args\":{\"count\":16}}"));
    ASSERT_TRUE(v.get("ok")->b);
    const JsonValue *e = v.get("result")->get("entries");
    ASSERT_TRUE(e->isArray());
    EXPECT_EQ(e->a->size(), 0u);
}

TEST_F(AgentObservabilityTest, TraceRingRejectsBadDepth)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.trace_ring\",\"args\":{\"depth\":0}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

/* ---- cpu.disasm argument validation (4.9.6) -------------------------- */
/* (Real decode needs MemBase; covered by the live test.) */

TEST_F(AgentObservabilityTest, DisasmRejectsMissingAddr)
{
    JsonValue v = parse(dispatchLine("{\"id\":1,\"cmd\":\"cpu.disasm\",\"args\":{\"count\":4}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentObservabilityTest, DisasmRejectsAddrWithoutColon)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.disasm\",\"args\":{\"addr\":\"DEADBEEF\"}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentObservabilityTest, DisasmRejectsBadCount)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.disasm\",\"args\":{\"addr\":\"0824:0100\",\"count\":0}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

/* ---- mem.watch (4.9.4) ----------------------------------------------- */
/* The hot-path hook AGENT_MemWatchNote reads guest memory (old value) and
 * emits an event, so the end-to-end path (counter bump, mem.write event,
 * from_cs/ip attribution) is covered by the live test. Here we drive the
 * pure predicate AGENT_MemWatchMatch directly — it touches no memory — plus
 * argument validation and debug.status reflection. The armed watch's linear
 * range is (seg<<4)+lo .. (seg<<4)+hi; the tests below use seg=0x1000 so the
 * base is a round 0x10000. */

/* Helper: arm a mem.watch via dispatch and assert it succeeded. */
static void armMemWatch(const char *json) {
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(json), v)) << json;
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << json;
}

TEST_F(AgentObservabilityTest, MemWatchArmsAndSurfacesInStatus)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":\"0000\",\"hi\":\"00FF\"}}"));
    ASSERT_TRUE(v.get("ok")->b);
    const JsonValue *res = v.get("result");
    EXPECT_TRUE(res->get("armed")->b);
    EXPECT_EQ(uint16_t(res->get("seg")->n), 0x1000u);
    EXPECT_EQ(uint16_t(res->get("lo")->n), 0x0000u);
    EXPECT_EQ(uint16_t(res->get("hi")->n), 0x00FFu);
    EXPECT_EQ(int(res->get("size")->n), 0);                 /* any width */
    EXPECT_EQ(res->get("predicate")->s, "none");
    EXPECT_TRUE(AGENT_memWatchArmed);

    JsonValue s = status();
    const JsonValue *m = s.get("result")->get("watches")->get("mem");
    EXPECT_TRUE(m->get("armed")->b);
    EXPECT_EQ(uint64_t(m->get("hits")->n), 0u);
    EXPECT_EQ(uint16_t(m->get("seg")->n), 0x1000u);
    EXPECT_EQ(uint16_t(m->get("lo")->n), 0x0000u);
    EXPECT_EQ(uint16_t(m->get("hi")->n), 0x00FFu);
}

TEST_F(AgentObservabilityTest, MemUnwatchDisarms)
{
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255}}");
    EXPECT_TRUE(AGENT_memWatchArmed);

    JsonValue v = parse(dispatchLine("{\"id\":2,\"cmd\":\"mem.unwatch\"}"));
    ASSERT_TRUE(v.get("ok")->b);
    EXPECT_FALSE(v.get("result")->get("armed")->b);
    EXPECT_FALSE(AGENT_memWatchArmed);

    /* seg=null is the other way to disarm. */
    armMemWatch("{\"id\":3,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255}}");
    EXPECT_TRUE(AGENT_memWatchArmed);
    v = parse(dispatchLine("{\"id\":4,\"cmd\":\"mem.watch\",\"args\":{\"seg\":null}}"));
    ASSERT_TRUE(v.get("ok")->b);
    EXPECT_FALSE(AGENT_memWatchArmed);
}

TEST_F(AgentObservabilityTest, MemWatchMatchRespectsRangeEdges)
{
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255}}");
    /* base 0x10000, range [0x10000, 0x100FF]. */
    EXPECT_FALSE(AGENT_MemWatchMatch(0x0FFFF, 0x12, 0x00, 1));  /* just below */
    EXPECT_TRUE (AGENT_MemWatchMatch(0x10000, 0x12, 0x00, 1));  /* at lo      */
    EXPECT_TRUE (AGENT_MemWatchMatch(0x10080, 0x12, 0x00, 1));  /* inside     */
    EXPECT_TRUE (AGENT_MemWatchMatch(0x100FF, 0x12, 0x00, 1));  /* at hi      */
    EXPECT_FALSE(AGENT_MemWatchMatch(0x10100, 0x12, 0x00, 1));  /* just above */
}

TEST_F(AgentObservabilityTest, MemWatchMatchSizeFilter)
{
    /* size=2 means only 2-byte writes match. */
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\","
                "\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255,\"size\":2}}");
    EXPECT_FALSE(AGENT_MemWatchMatch(0x10010, 0x1234, 0, 1));   /* byte write  */
    EXPECT_TRUE (AGENT_MemWatchMatch(0x10010, 0x1234, 0, 2));   /* word write  */
    EXPECT_FALSE(AGENT_MemWatchMatch(0x10010, 0x1234, 0, 4));   /* dword write */
}

TEST_F(AgentObservabilityTest, MemWatchMatchNewEqPredicate)
{
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\","
                "\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255,\"when\":{\"new_eq\":\"0853\"}}}");
    {
        JsonValue s = status();
        EXPECT_EQ(s.get("result")->get("watches")->get("mem")->get("predicate")->s,
                  "new_eq=0x853");
    }
    EXPECT_TRUE (AGENT_MemWatchMatch(0x10010, 0x0853, 0x0761, 2));  /* matches  */
    EXPECT_FALSE(AGENT_MemWatchMatch(0x10010, 0x0761, 0x0853, 2));  /* wrong new */
    /* new_eq=0x0853 is wider than a byte, so a 1-byte write can never match,
     * even when its value equals the predicate's low byte (0x53). */
    EXPECT_FALSE(AGENT_MemWatchMatch(0x10010, 0x53, 0, 1));
}

TEST_F(AgentObservabilityTest, MemWatchMatchNewNeOldPredicate)
{
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\","
                "\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255,\"when\":{\"new_ne_old\":true}}}");
    EXPECT_TRUE (AGENT_MemWatchMatch(0x10010, 0x0853, 0x0761, 2));  /* changed     */
    EXPECT_FALSE(AGENT_MemWatchMatch(0x10010, 0x0853, 0x0853, 2));  /* idempotent  */
    /* Only the low `size` bytes are compared: same low byte, differing high. */
    EXPECT_FALSE(AGENT_MemWatchMatch(0x10010, 0xFF53, 0x0053, 1));
}

TEST_F(AgentObservabilityTest, MemWatchMatchAndMaskEqPredicate)
{
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\","
                "\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255,"
                "\"when\":{\"new_and_mask_eq\":{\"mask\":\"FF00\",\"value\":\"0800\"}}}}");
    EXPECT_TRUE (AGENT_MemWatchMatch(0x10010, 0x0853, 0, 2));  /* (0853 & FF00)=0800 */
    EXPECT_TRUE (AGENT_MemWatchMatch(0x10010, 0x08FF, 0, 2));  /* high byte 08       */
    EXPECT_FALSE(AGENT_MemWatchMatch(0x10010, 0x0953, 0, 2));  /* high byte 09       */
}

TEST_F(AgentObservabilityTest, MemWatchMatchFalseWhenDisarmed)
{
    EXPECT_FALSE(AGENT_memWatchArmed);
    EXPECT_FALSE(AGENT_MemWatchMatch(0x10010, 0x12, 0, 1));
}

TEST_F(AgentObservabilityTest, MemWatchRejectsMissingRange)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\"}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
    EXPECT_FALSE(AGENT_memWatchArmed);
}

TEST_F(AgentObservabilityTest, MemWatchRejectsInvertedRange)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":255,\"hi\":0}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentObservabilityTest, MemWatchRejectsBadSize)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255,\"size\":3}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentObservabilityTest, MemWatchRejectsTwoPredicates)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255,"
        "\"when\":{\"new_eq\":1,\"new_ne_old\":true}}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentObservabilityTest, MemWatchRejectsEmptyPredicate)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":255,\"when\":{}}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

/* ---- mem.watch on side-effecting memory (4.9.4 VGA) ------------------ */
/* When the write destination cannot be read back without side effects (the
 * VGA framebuffer loads its plane latches on read), AGENT_MemWatchNote skips
 * the old-value read and evaluates AGENT_MemWatchMatchNoOld instead. That pure
 * predicate is exercised directly here (it touches no memory); the hook's
 * destination classification (MEM_GetPageHandler / PFLAG_READABLE) and the
 * old=null event field are MemBase-dependent and covered by the live test. */

TEST_F(AgentObservabilityTest, MemWatchMatchNoOldRespectsScope)
{
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"A000\",\"lo\":0,\"hi\":255}}");
    /* base 0xA0000, range [0xA0000, 0xA00FF]; no predicate -> any in-scope. */
    EXPECT_FALSE(AGENT_MemWatchMatchNoOld(0x9FFFF, 0x5A, 1));  /* just below */
    EXPECT_TRUE (AGENT_MemWatchMatchNoOld(0xA0000, 0x5A, 1));  /* at lo      */
    EXPECT_TRUE (AGENT_MemWatchMatchNoOld(0xA00FF, 0x5A, 1));  /* at hi      */
    EXPECT_FALSE(AGENT_MemWatchMatchNoOld(0xA0100, 0x5A, 1));  /* just above */
}

TEST_F(AgentObservabilityTest, MemWatchMatchNoOldSizeFilter)
{
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\","
                "\"args\":{\"seg\":\"A000\",\"lo\":0,\"hi\":255,\"size\":1}}");
    EXPECT_TRUE (AGENT_MemWatchMatchNoOld(0xA0010, 0x5A, 1));   /* byte write  */
    EXPECT_FALSE(AGENT_MemWatchMatchNoOld(0xA0010, 0x5A, 2));   /* word write  */
}

TEST_F(AgentObservabilityTest, MemWatchMatchNoOldNewEqPredicate)
{
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\","
                "\"args\":{\"seg\":\"A000\",\"lo\":0,\"hi\":255,\"size\":1,"
                "\"when\":{\"new_eq\":\"5A\"}}}");
    EXPECT_TRUE (AGENT_MemWatchMatchNoOld(0xA0010, 0x5A, 1));   /* matches the new value */
    EXPECT_FALSE(AGENT_MemWatchMatchNoOld(0xA0010, 0x5B, 1));   /* wrong value           */
}

TEST_F(AgentObservabilityTest, MemWatchMatchNoOldAndMaskEqPredicate)
{
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\","
                "\"args\":{\"seg\":\"A000\",\"lo\":0,\"hi\":255,\"size\":1,"
                "\"when\":{\"new_and_mask_eq\":{\"mask\":\"F0\",\"value\":\"50\"}}}}");
    EXPECT_TRUE (AGENT_MemWatchMatchNoOld(0xA0010, 0x5A, 1));   /* (5A & F0)=50 */
    EXPECT_FALSE(AGENT_MemWatchMatchNoOld(0xA0010, 0x6A, 1));   /* (6A & F0)=60 */
}

TEST_F(AgentObservabilityTest, MemWatchMatchNoOldNeOldAlwaysMatches)
{
    /* Without the old value we can't prove a store is idempotent, so the
     * new_ne_old predicate conservatively matches every in-scope write. */
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\","
                "\"args\":{\"seg\":\"A000\",\"lo\":0,\"hi\":255,\"when\":{\"new_ne_old\":true}}}");
    EXPECT_TRUE(AGENT_MemWatchMatchNoOld(0xA0010, 0x5A, 1));
    EXPECT_TRUE(AGENT_MemWatchMatchNoOld(0xA0010, 0x00, 1));
    /* still scope-gated */
    EXPECT_FALSE(AGENT_MemWatchMatchNoOld(0xB0000, 0x5A, 1));
}

TEST_F(AgentObservabilityTest, MemWatchMatchNoOldFalseWhenDisarmed)
{
    EXPECT_FALSE(AGENT_memWatchArmed);
    EXPECT_FALSE(AGENT_MemWatchMatchNoOld(0xA0010, 0x5A, 1));
}

}  /* anonymous namespace */
