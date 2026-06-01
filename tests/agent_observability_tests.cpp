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
        /* Clear the BP list so debug.status never reads bytes_now (no MemBase). */
        dispatchLine("{\"id\":99,\"cmd\":\"debugger.command\",\"args\":{\"text\":\"BPDEL 0 *\"}}");
    }
    void TearDown() override {
        AGENT_FarWatchClear();
        AGENT_TargetWatchClear();
        AGENT_RangeWatchClear();
        dispatchLine("{\"id\":99,\"cmd\":\"cpu.probe\",\"args\":{\"points\":[]}}");
        dispatchLine("{\"id\":99,\"cmd\":\"cpu.trace_ring\",\"args\":{\"enabled\":false}}");
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

}  /* anonymous namespace */
