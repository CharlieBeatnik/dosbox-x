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

/* Agent control channel — observability & trust tests.
 *
 * What is and isn't covered here, and why:
 *
 *  - The probe and trace ring are pure agent-side state with
 *    hot-path hooks (AGENT_ProbeCheck / AGENT_TraceRecord) that touch no
 *    guest memory. So we can drive them directly the way the CPU core would,
 *    then read the result back through debug.status — a genuine end-to-end
 *    check of the counter/ring logic without a running guest.
 *
 *  - The watch hit counters live on AGENT_*WatchMatches, also pure
 *    agent state, so they too are exercised directly.
 *
 *  - cpu.disasm and a non-empty cpu.traceback dump call
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
        dispatchLine("{\"id\":99,\"cmd\":\"bp.clear\"}");   /* conditional BPs */
        g_stepOverPending = false;   /* no cpu.step_over in flight */
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
        dispatchLine("{\"id\":99,\"cmd\":\"bp.clear\"}");
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

/* ---- debug.status reply shape -------------------------------- */

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

/* ---- watch hit counters ------------------------------------- */

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

/* ---- multi-sentinel watches ---------------------------------- */

/* Arming a set of NEAR targets; the matcher reports *which* sentinel fired
 * (g_targetWatchWhich — the value the cpu.transfer emitter stamps) and bumps
 * only that sentinel's counter. debug.status surfaces per-sentinel hits and
 * a total. */
TEST_F(AgentObservabilityTest, MultiTargetWatchWhichAndPerSentinelHits)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.watch_target\",\"args\":{\"targets\":"
        "[\"0824:C01E\",\"0824:C020\",\"1000:0040\"]}}"));
    ASSERT_TRUE(v.get("ok")->b) << jsonEncode(v);
    EXPECT_TRUE(v.get("result")->get("watching")->b);
    ASSERT_EQ(v.get("result")->get("targets")->a->size(), 3u);
    ASSERT_EQ(g_targetWatch.size(), 3u);

    /* Drive the matcher at sentinels 1, 2, 1, 0 (and a non-match). Each call
     * is what the CPU core makes; check the recorded `which` each time. */
    EXPECT_TRUE (AGENT_TargetWatchMatches(0x0824, 0xC020));  EXPECT_EQ(g_targetWatchWhich, 1);
    EXPECT_TRUE (AGENT_TargetWatchMatches(0x1000, 0x0040));  EXPECT_EQ(g_targetWatchWhich, 2);
    EXPECT_TRUE (AGENT_TargetWatchMatches(0x0824, 0xC020));  EXPECT_EQ(g_targetWatchWhich, 1);
    EXPECT_TRUE (AGENT_TargetWatchMatches(0x0824, 0xC01E));  EXPECT_EQ(g_targetWatchWhich, 0);
    EXPECT_FALSE(AGENT_TargetWatchMatches(0x0824, 0xFFFF));

    JsonValue sv = status();
    const JsonValue *t = sv.get("result")->get("watches")->get("target");
    EXPECT_TRUE(t->get("armed")->b);
    EXPECT_EQ(uint64_t(t->get("hits")->n), 4u);                 /* total */
    /* Back-compat: top-level seg/off mirror sentinel[0]. */
    EXPECT_EQ(uint16_t(t->get("seg")->n), 0x0824u);
    EXPECT_EQ(uint16_t(t->get("off")->n), 0xC01Eu);
    const JsonArray &s = *t->get("sentinels")->a;
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(uint64_t(s[0].get("hits")->n), 1u);
    EXPECT_EQ(uint64_t(s[1].get("hits")->n), 2u);
    EXPECT_EQ(uint64_t(s[2].get("hits")->n), 1u);
    EXPECT_EQ(uint16_t(s[2].get("seg")->n), 0x1000u);
    EXPECT_EQ(uint16_t(s[2].get("off")->n), 0x0040u);
}

/* An empty targets array disarms the whole set. */
TEST_F(AgentObservabilityTest, MultiTargetWatchEmptyArrayClears)
{
    dispatchLine("{\"id\":1,\"cmd\":\"cpu.watch_target\",\"args\":{\"targets\":[\"0824:C01E\"]}}");
    ASSERT_EQ(g_targetWatch.size(), 1u);
    JsonValue v = parse(dispatchLine(
        "{\"id\":2,\"cmd\":\"cpu.watch_target\",\"args\":{\"targets\":[]}}"));
    ASSERT_TRUE(v.get("ok")->b);
    EXPECT_FALSE(v.get("result")->get("watching")->b);
    EXPECT_TRUE(g_targetWatch.empty());
    JsonValue sv = status();
    EXPECT_FALSE(sv.get("result")->get("watches")->get("target")->get("armed")->b);
}

/* A malformed targets entry is rejected and leaves the set untouched. */
TEST_F(AgentObservabilityTest, MultiTargetWatchRejectsBadEntry)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.watch_target\",\"args\":{\"targets\":[\"nocolon\"]}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
    EXPECT_TRUE(g_targetWatch.empty());
}

/* The same for a set of FAR-destination segments. */
TEST_F(AgentObservabilityTest, MultiFarWatchWhichAndPerSentinelHits)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"farcall.watch\",\"args\":{\"target_segs\":[\"483C\",4096,\"0x9000\"]}}"));
    ASSERT_TRUE(v.get("ok")->b) << jsonEncode(v);
    ASSERT_EQ(g_farWatch.size(), 3u);

    EXPECT_TRUE (AGENT_FarWatchMatches(0x9000)); EXPECT_EQ(g_farWatchWhich, 2);
    EXPECT_TRUE (AGENT_FarWatchMatches(0x483C)); EXPECT_EQ(g_farWatchWhich, 0);
    EXPECT_TRUE (AGENT_FarWatchMatches(0x9000)); EXPECT_EQ(g_farWatchWhich, 2);
    EXPECT_FALSE(AGENT_FarWatchMatches(0x0001));

    JsonValue sv = status();
    const JsonValue *f = sv.get("result")->get("watches")->get("far");
    EXPECT_EQ(uint64_t(f->get("hits")->n), 3u);
    const JsonArray &s = *f->get("sentinels")->a;
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(uint16_t(s[0].get("seg")->n), 0x483Cu);
    EXPECT_EQ(uint64_t(s[0].get("hits")->n), 1u);
    EXPECT_EQ(uint64_t(s[2].get("hits")->n), 2u);
}

/* A set of ranges, each with its own from-outside gate and counter. */
TEST_F(AgentObservabilityTest, MultiRangeWatchWhichAndPerSentinelHits)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"cpu.watch_range\",\"args\":{\"ranges\":["
        "{\"seg\":\"0824\",\"lo\":\"9600\",\"hi\":\"99FF\"},"
        "{\"seg\":\"1000\",\"lo\":\"0000\",\"hi\":\"00FF\"}]}}"));
    ASSERT_TRUE(v.get("ok")->b) << jsonEncode(v);
    ASSERT_EQ(g_rangeWatch.size(), 2u);

    /* Range 0 entry from outside: which==0. */
    EXPECT_TRUE (AGENT_RangeWatchEntry(0x0824, 0x9700, 0x0824, 0x6000)); EXPECT_EQ(g_rangeWatchWhich, 0);
    /* Intra-range step in range 0: no match. */
    EXPECT_FALSE(AGENT_RangeWatchEntry(0x0824, 0x9701, 0x0824, 0x9700));
    /* Range 1 entry (different seg): which==1. */
    EXPECT_TRUE (AGENT_RangeWatchEntry(0x1000, 0x0010, 0x0824, 0x9700)); EXPECT_EQ(g_rangeWatchWhich, 1);

    JsonValue sv = status();
    const JsonValue *rg = sv.get("result")->get("watches")->get("range");
    EXPECT_EQ(uint64_t(rg->get("hits")->n), 2u);
    const JsonArray &s = *rg->get("sentinels")->a;
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(uint64_t(s[0].get("hits")->n), 1u);
    EXPECT_EQ(uint64_t(s[1].get("hits")->n), 1u);
    EXPECT_EQ(uint16_t(s[1].get("seg")->n), 0x1000u);
    EXPECT_EQ(uint16_t(s[1].get("hi")->n), 0x00FFu);
}

/* The single-sentinel scalar form still works and is reported as a
 * one-element set (back-compat with the original single-sentinel callers). */
TEST_F(AgentObservabilityTest, ScalarFormIsAOneElementSet)
{
    dispatchLine("{\"id\":1,\"cmd\":\"farcall.watch\",\"args\":{\"target_seg\":\"483C\"}}");
    ASSERT_EQ(g_farWatch.size(), 1u);
    EXPECT_TRUE(AGENT_FarWatchMatches(0x483C));
    EXPECT_EQ(g_farWatchWhich, 0);
    JsonValue sv = status();
    const JsonValue *f = sv.get("result")->get("watches")->get("far");
    EXPECT_EQ(uint16_t(f->get("seg")->n), 0x483Cu);
    EXPECT_EQ(f->get("sentinels")->a->size(), 1u);
}

/* ---- cpu.probe ----------------------------------------------- */

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

/* ---- cpu.trace_ring + cpu.traceback -------------------------- */

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

/* ---- cpu.disasm argument validation -------------------------- */
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

/* ---- mem.watch ----------------------------------------------- */
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

/* ---- mem.watch on side-effecting memory (VGA) ------------------ */
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

/* ---- mem.watch becomes_eq (value-landed) --------------------- */
/* The store-value predicates (new_eq/...) miss a byte-wise / partial / block
 * store that only *lands* V at the watched location. becomes_eq closes that:
 * it fires once per rising edge for any store that overlaps the value_size-byte
 * unit, regardless of width or value. The rising-edge + guest read-back happen
 * in AGENT_MemWatchNote (live-tested), but the two ingredients are pure and
 * exercised here: the overlap decision (AGENT_MemWatchUnitOverlap) and the
 * byte-overlay that derives the settled unit (AGENT_MemWatchMergeUnit), plus
 * the arm/parse/status surface. seg=0x1000 -> base 0x10000. */

TEST_F(AgentObservabilityTest, MemWatchBecomesEqArmsWithExplicitValueSize)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\","
        "\"lo\":\"0004\",\"hi\":\"0005\",\"value_size\":2,"
        "\"when\":{\"becomes_eq\":\"0853\"}}}"));
    ASSERT_TRUE(v.get("ok")->b) << jsonEncode(v);
    const JsonValue *res = v.get("result");
    EXPECT_TRUE(res->get("armed")->b);
    EXPECT_EQ(int(res->get("value_size")->n), 2);
    EXPECT_EQ(res->get("predicate")->s, "becomes_eq=0x853");
    EXPECT_TRUE(AGENT_memWatchArmed);

    JsonValue s = status();
    const JsonValue *m = s.get("result")->get("watches")->get("mem");
    EXPECT_TRUE(m->get("armed")->b);
    EXPECT_EQ(int(m->get("value_size")->n), 2);
    EXPECT_EQ(m->get("predicate")->s, "becomes_eq=0x853");
}

TEST_F(AgentObservabilityTest, MemWatchBecomesEqDefaultsValueSizeFromSpan)
{
    /* span = hi-lo+1, clamped down to {1,2,4}. */
    auto vsForSpan = [&](const char *lo, const char *hi) -> int {
        JsonValue v = parse(dispatchLine(
            std::string("{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":\"")
            + lo + "\",\"hi\":\"" + hi + "\",\"when\":{\"becomes_eq\":1}}}"));
        EXPECT_TRUE(v.get("ok")->b) << jsonEncode(v);
        return int(v.get("result")->get("value_size")->n);
    };
    EXPECT_EQ(vsForSpan("0004", "0004"), 1);   /* span 1 -> 1 */
    EXPECT_EQ(vsForSpan("0004", "0005"), 2);   /* span 2 -> 2 */
    EXPECT_EQ(vsForSpan("0004", "0006"), 2);   /* span 3 -> 2 */
    EXPECT_EQ(vsForSpan("0004", "0007"), 4);   /* span 4 -> 4 */
    EXPECT_EQ(vsForSpan("0004", "0013"), 4);   /* span 16 -> 4 */
}

TEST_F(AgentObservabilityTest, MemWatchBecomesEqRejectsBadValueSize)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":1,"
        "\"value_size\":3,\"when\":{\"becomes_eq\":1}}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
    EXPECT_FALSE(AGENT_memWatchArmed);
}

TEST_F(AgentObservabilityTest, MemWatchBecomesEqRejectsCombinedPredicate)
{
    /* becomes_eq is mutually exclusive with the store-value predicates. */
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\",\"lo\":0,\"hi\":1,"
        "\"when\":{\"becomes_eq\":1,\"new_eq\":2}}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentObservabilityTest, MemWatchBecomesEqOverlapTest)
{
    /* unit [0x10004, 0x10006): a 2-byte unit at seg:lo. The overlap gate fires
     * on ANY store that crosses the unit — including a block store that STARTS
     * below lo but writes a byte inside (the exact false-zero case that the
     * start-in-range store-value gate misses). */
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\","
                "\"lo\":\"0004\",\"hi\":\"0005\",\"value_size\":2,"
                "\"when\":{\"becomes_eq\":\"0853\"}}}");

    EXPECT_FALSE(AGENT_MemWatchUnitOverlap(0x10003, 1));  /* byte just below   */
    EXPECT_TRUE (AGENT_MemWatchUnitOverlap(0x10004, 1));  /* low byte          */
    EXPECT_TRUE (AGENT_MemWatchUnitOverlap(0x10005, 1));  /* high byte         */
    EXPECT_FALSE(AGENT_MemWatchUnitOverlap(0x10006, 1));  /* byte just above   */
    EXPECT_TRUE (AGENT_MemWatchUnitOverlap(0x10003, 2));  /* block crossing lo */
    EXPECT_FALSE(AGENT_MemWatchUnitOverlap(0x10002, 2));  /* block ends at lo  */
    EXPECT_TRUE (AGENT_MemWatchUnitOverlap(0x10000, 8));  /* wide block covers */
}

TEST_F(AgentObservabilityTest, MemWatchBecomesEqOverlapFalseWhenDisarmedOrWrongPred)
{
    EXPECT_FALSE(AGENT_memWatchArmed);
    EXPECT_FALSE(AGENT_MemWatchUnitOverlap(0x10004, 2));   /* disarmed         */
    /* A non-becomes_eq watch must report no unit overlap. */
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\","
                "\"lo\":\"0004\",\"hi\":\"0005\",\"when\":{\"new_eq\":\"0853\"}}}");
    EXPECT_FALSE(AGENT_MemWatchUnitOverlap(0x10004, 2));
}

TEST_F(AgentObservabilityTest, MemWatchBecomesEqMergeUnit)
{
    /* unit [0x10004, 0x10006), value_size 2. Merge derives the post-store unit
     * from a supplied pre-store unit + the store's bytes — the heart of "what
     * the unit *becomes*", independent of any single store's value/width. */
    armMemWatch("{\"id\":1,\"cmd\":\"mem.watch\",\"args\":{\"seg\":\"1000\","
                "\"lo\":\"0004\",\"hi\":\"0005\",\"value_size\":2,"
                "\"when\":{\"becomes_eq\":\"0853\"}}}");

    /* Byte-wise populate: low byte then high byte settle the word to 0x0853. */
    EXPECT_EQ(AGENT_MemWatchMergeUnit(0x0700, 0x10004, 0x53, 1), 0x0753u);
    EXPECT_EQ(AGENT_MemWatchMergeUnit(0x0753, 0x10005, 0x08, 1), 0x0853u);
    /* One covering word store. */
    EXPECT_EQ(AGENT_MemWatchMergeUnit(0x0000, 0x10004, 0x0853, 2), 0x0853u);
    /* Block store starting below lo, writing 0x53 into the low unit byte. */
    EXPECT_EQ(AGENT_MemWatchMergeUnit(0x0700, 0x10003, 0x5308, 2), 0x0753u);
    /* A store that misses the unit leaves it unchanged (overlap gate would have
     * filtered it; merge is a no-op for non-overlapping bytes). */
    EXPECT_EQ(AGENT_MemWatchMergeUnit(0x0761, 0x10006, 0xFFFF, 2), 0x0761u);
}

/* ---- cpu.step / cpu.step_over -------------------------------- */
/* The step itself runs guest instructions through DEBUG_Run, which needs a
 * paused CPU and an initialised memory/core (MemBase). In -tests mode the
 * debugger is never entered, so the gate (DEBUG_AgentStep returning 0 because
 * `debugging` is false) is what we exercise here: both commands are routed and
 * cleanly refuse with bad_state instead of touching the uninitialised core.
 * The actual single-step / step-over behaviour is covered live by
 * tests/agent_live/test_observability.py scenario 7. */

TEST_F(AgentObservabilityTest, CpuStepRequiresPause)
{
    JsonValue v = parse(dispatchLine("{\"id\":7,\"cmd\":\"cpu.step\"}"));
    ASSERT_TRUE(v.get("ok"));
    EXPECT_FALSE(v.get("ok")->b);
    ASSERT_TRUE(v.get("error") && v.get("error")->get("code"));
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_state");
}

TEST_F(AgentObservabilityTest, CpuStepOverRequiresPause)
{
    JsonValue v = parse(dispatchLine("{\"id\":7,\"cmd\":\"cpu.step_over\"}"));
    ASSERT_TRUE(v.get("ok"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_state");
    /* A refused step-over must not leave the deferred-reply slot armed. */
    EXPECT_FALSE(g_stepOverPending);
}

TEST_F(AgentObservabilityTest, CpuStepOverBusyWhilePending)
{
    /* Simulate a step-over already in flight (the CPU resuming to a temp BP).
     * A second cpu.step_over must be rejected with `busy`, not clobber the
     * pending id. */
    g_stepOverPending = true;
    g_stepOverId = 11;
    JsonValue v = parse(dispatchLine("{\"id\":12,\"cmd\":\"cpu.step_over\"}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "busy");
    /* The in-flight slot is untouched (still id 11, still pending). */
    EXPECT_TRUE(g_stepOverPending);
    EXPECT_EQ(g_stepOverId, 11.0);
    g_stepOverPending = false;   /* don't leak into the next test */
}

/* ---- state.save / state.restore ----------------------------- */
/* The save/restore themselves drive the whole savestate subsystem (zip I/O,
 * every device component, MemBase) and require a paused CPU + initialised
 * core, none of which exist in -tests mode. So these exercise the two gates
 * that run before any of that: argument validation (a bad/missing slot is
 * rejected with bad_args, checked first so it is testable headless) and the
 * paused gate (a valid slot while the CPU is not paused refuses with bad_state,
 * never touching SaveState). The real round-trip is covered live by
 * tests/agent_live/test_observability.py scenario 8. */

TEST_F(AgentObservabilityTest, StateSaveRejectsBadSlot)
{
    /* Out-of-range slot (>= 100) -> bad_args, before the paused check. */
    JsonValue v = parse(dispatchLine(
        "{\"id\":8,\"cmd\":\"state.save\",\"args\":{\"slot\":100}}"));
    ASSERT_TRUE(v.get("ok"));
    EXPECT_FALSE(v.get("ok")->b);
    ASSERT_TRUE(v.get("error") && v.get("error")->get("code"));
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");

    /* Missing slot -> bad_args too. */
    v = parse(dispatchLine("{\"id\":8,\"cmd\":\"state.save\"}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentObservabilityTest, StateSaveRequiresPause)
{
    /* Valid slot, but the CPU is not paused in -tests mode -> bad_state. */
    JsonValue v = parse(dispatchLine(
        "{\"id\":8,\"cmd\":\"state.save\",\"args\":{\"slot\":7}}"));
    ASSERT_TRUE(v.get("ok"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_state");
}

TEST_F(AgentObservabilityTest, StateRestoreRejectsBadSlot)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":9,\"cmd\":\"state.restore\",\"args\":{\"slot\":-1}}"));
    ASSERT_TRUE(v.get("ok"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentObservabilityTest, StateRestoreRequiresPause)
{
    /* Valid slot, not paused -> bad_state, before the empty-slot / load path. */
    JsonValue v = parse(dispatchLine(
        "{\"id\":9,\"cmd\":\"state.restore\",\"args\":{\"slot\":7}}"));
    ASSERT_TRUE(v.get("ok"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_state");
}

/* ---- conditional / Nth-hit breakpoints ----------------------- */
/* The condition parser and evaluator are pure functions (the evaluator reads
 * register globals, which exist in -tests mode), so they are exercised
 * directly here. The hot-path worker AGENT_CondBpCheck mutates agent-side
 * state and reads registers only (empty / regs.get macros), so it too runs
 * headless. The actual halt-into-the-debugger and mem.read/disasm macros need
 * a running guest and are covered live by test_observability.py scenario 9. */

TEST_F(AgentObservabilityTest, CondParseAcceptsCommonForms)
{
    const char *ok[] = {
        "sp==0x0200", "hits==7", "cx==151", "ax!=0", "eax>=0x10000",
        "[ss:01F8]==0x1234", "byte [es:di]==0x5A", "dword [ds:bx]<=0xFF",
        "flags&0x40!=0", "ip>0x100",
        /* register-indexed operands (proposal 4.10 Feature B). */
        "word [si+04]==0x853", "word [ds:si+02]==0x37DF", "word [bx+04]!=0x863",
        "byte [di-01]==0xFF", "[si+0]==1", "[6A1C]==0x853",
    };
    for (const char *s : ok) {
        BpCondition c; std::string err;
        EXPECT_TRUE(parseBpCondition(s, c, err)) << s << " :: " << err;
        EXPECT_TRUE(c.present) << s;
    }

    /* Spot-check a couple of parsed structures. */
    BpCondition c; std::string err;
    ASSERT_TRUE(parseBpCondition("hits==7", c, err));
    EXPECT_EQ(c.op, BP_EQ);
    EXPECT_FALSE(c.lhs.isMem);
    EXPECT_EQ(c.lhs.direct.kind, BpValSrc::HITS);
    EXPECT_EQ(c.rhs.direct.kind, BpValSrc::IMM);
    EXPECT_EQ(c.rhs.direct.imm, 7u);

    ASSERT_TRUE(parseBpCondition("flags&0x40!=0", c, err));
    EXPECT_TRUE(c.hasMask);
    EXPECT_EQ(c.mask, 0x40u);
    EXPECT_EQ(c.op, BP_NE);

    ASSERT_TRUE(parseBpCondition("byte [es:di]==0x5A", c, err));
    EXPECT_TRUE(c.lhs.isMem);
    EXPECT_EQ(c.lhs.memSize, 1);
    EXPECT_EQ(c.lhs.memOff.kind, BpValSrc::REG);
    EXPECT_EQ(c.lhs.memDisp, 0);

    /* `word [si+04]` — explicit segment absent (default DS), reg base + disp.
     * BPREG_* ids live in agent_cpu.cpp's anonymous namespace, so the default
     * segment is proven by comparing it to an explicit `[ds:...]` parse: both
     * must resolve memSeg to the SAME register. */
    BpCondition cDef, cExpl;
    ASSERT_TRUE(parseBpCondition("word [si+04]==0x853", cDef, err)) << err;
    ASSERT_TRUE(parseBpCondition("word [ds:si+04]==0x853", cExpl, err)) << err;
    EXPECT_TRUE(cDef.lhs.isMem);
    EXPECT_EQ(cDef.lhs.memSize, 2);
    EXPECT_EQ(cDef.lhs.memSeg.kind, BpValSrc::REG);
    EXPECT_EQ(cExpl.lhs.memSeg.kind, BpValSrc::REG);
    EXPECT_EQ(cDef.lhs.memSeg.reg, cExpl.lhs.memSeg.reg);   /* default == DS */
    EXPECT_EQ(cDef.lhs.memOff.kind, BpValSrc::REG);
    EXPECT_EQ(cDef.lhs.memDisp, 0x04);
    EXPECT_EQ(cDef.rhs.direct.imm, 0x853u);

    /* `word [ds:si+02]` — explicit segment + disp. */
    ASSERT_TRUE(parseBpCondition("word [ds:si+02]==0x37DF", c, err));
    EXPECT_EQ(c.lhs.memSeg.kind, BpValSrc::REG);
    EXPECT_EQ(c.lhs.memOff.kind, BpValSrc::REG);
    EXPECT_EQ(c.lhs.memDisp, 0x02);

    /* A negative displacement parses to a signed memDisp. */
    ASSERT_TRUE(parseBpCondition("byte [di-01]==0xFF", c, err));
    EXPECT_EQ(c.lhs.memDisp, -1);

    /* `[6A1C]` — no segment, literal offset -> default DS, no disp. */
    ASSERT_TRUE(parseBpCondition("[6A1C]==0x853", c, err));
    EXPECT_TRUE(c.lhs.isMem);
    EXPECT_EQ(c.lhs.memSeg.kind, BpValSrc::REG);            /* defaulted to DS */
    EXPECT_EQ(c.lhs.memOff.kind, BpValSrc::IMM);
    EXPECT_EQ(c.lhs.memOff.imm, 0x6A1Cu);
    EXPECT_EQ(c.lhs.memDisp, 0);
}

TEST_F(AgentObservabilityTest, CondParseRejectsMalformed)
{
    const char *bad[] = {
        "", "ax", "ax==", "==5", "ax===5", "foo==1",
        "ax & == 5", "ax & bx == 1", "ax == 5 == 1", "ax <> 5", "byte ax==1",
        /* malformed memory references (Feature B parser error paths). */
        "[ds:]==1",      /* segment but no offset            */
        "[si+]==1",      /* '+' with no displacement         */
        "[+04]==1",      /* displacement with no base        */
        "[si+xz]==1",    /* non-hex displacement             */
        "[si:di:bx]==1", /* too many ':' halves              */
    };
    for (const char *s : bad) {
        BpCondition c; std::string err;
        EXPECT_FALSE(parseBpCondition(s, c, err)) << "should reject: " << s;
        EXPECT_FALSE(err.empty()) << s;
    }
}

TEST_F(AgentObservabilityTest, CondEvalImmediatesAndHits)
{
    BpCondition c; std::string err;

    ASSERT_TRUE(parseBpCondition("hits==7", c, err));
    EXPECT_TRUE (evalBpCondition(c, 7));
    EXPECT_FALSE(evalBpCondition(c, 6));

    ASSERT_TRUE(parseBpCondition("hits>=3", c, err));
    EXPECT_FALSE(evalBpCondition(c, 2));
    EXPECT_TRUE (evalBpCondition(c, 3));
    EXPECT_TRUE (evalBpCondition(c, 99));

    /* An unconditional (default) condition always holds. */
    BpCondition none;
    EXPECT_TRUE(evalBpCondition(none, 0));
}

TEST_F(AgentObservabilityTest, CondEvalRegistersAndMask)
{
    BpCondition c; std::string err;

    reg_ecx = 0x00010151;                 /* cx low = 0x0151, ecx full = 0x10151 */
    ASSERT_TRUE(parseBpCondition("cx==0x151", c, err));
    EXPECT_TRUE(evalBpCondition(c, 0));
    ASSERT_TRUE(parseBpCondition("ecx==0x10151", c, err));
    EXPECT_TRUE(evalBpCondition(c, 0));
    ASSERT_TRUE(parseBpCondition("cx<0x151", c, err));
    EXPECT_FALSE(evalBpCondition(c, 0));

    reg_eax = 0x0000005A;
    ASSERT_TRUE(parseBpCondition("al==0x5A", c, err));
    EXPECT_TRUE(evalBpCondition(c, 0));
    ASSERT_TRUE(parseBpCondition("ah==0", c, err));
    EXPECT_TRUE(evalBpCondition(c, 0));

    reg_flags = 0x0246;                   /* bit 0x40 set */
    ASSERT_TRUE(parseBpCondition("flags&0x40!=0", c, err));
    EXPECT_TRUE(evalBpCondition(c, 0));
    reg_flags = 0x0202;                   /* bit 0x40 clear */
    EXPECT_FALSE(evalBpCondition(c, 0));
}

TEST_F(AgentObservabilityTest, BpSetValidatesArgs)
{
    /* Missing addr. */
    JsonValue v = parse(dispatchLine("{\"id\":1,\"cmd\":\"bp.set\"}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");

    /* Bad addr. */
    v = parse(dispatchLine("{\"id\":1,\"cmd\":\"bp.set\",\"args\":{\"addr\":\"nope\"}}"));
    EXPECT_FALSE(v.get("ok")->b);

    /* Bad condition. */
    v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"bp.set\",\"args\":{\"addr\":\"0824:0166\",\"if\":\"foo==1\"}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");

    /* Macro command not on the read-only allowlist. */
    v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"bp.set\",\"args\":{\"addr\":\"0824:0166\","
        "\"do\":[{\"cmd\":\"cpu.run\"}]}}"));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");

    /* 'continue' wrong type. */
    v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"bp.set\",\"args\":{\"addr\":\"0824:0166\",\"continue\":\"yes\"}}"));
    EXPECT_FALSE(v.get("ok")->b);
}

TEST_F(AgentObservabilityTest, BpSetSucceedsAndAppearsInStatus)
{
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"bp.set\",\"args\":{\"addr\":\"0824:0166\","
        "\"if\":\"hits==150\",\"do\":[{\"cmd\":\"regs.get\"}],\"continue\":false}}"));
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << "bp.set should succeed";
    const JsonValue *r = v.get("result");
    ASSERT_TRUE(r && r->isObject());
    EXPECT_EQ(r->get("addr")->s, "0824:0166");
    EXPECT_EQ(uint16_t(r->get("seg")->n), 0x0824u);
    EXPECT_EQ(uint16_t(r->get("off")->n), 0x0166u);
    EXPECT_EQ(r->get("condition")->s, "hits==150");
    EXPECT_EQ(int(r->get("macro_len")->n), 1);
    EXPECT_FALSE(r->get("continue")->b);
    int bp_id = int(r->get("bp_id")->n);

    /* It surfaces in debug.status with zeroed hit/fire counters. */
    JsonValue st = status();
    const JsonValue *cbs = st.get("result")->get("cond_breakpoints");
    ASSERT_TRUE(cbs && cbs->isArray());
    ASSERT_EQ(cbs->a->size(), 1u);
    const JsonValue &cb = (*cbs->a)[0];
    EXPECT_EQ(int(cb.get("bp_id")->n), bp_id);
    EXPECT_EQ(cb.get("addr")->s, "0824:0166");
    EXPECT_EQ(cb.get("condition")->s, "hits==150");
    EXPECT_EQ(uint64_t(cb.get("hits")->n), 0u);
    EXPECT_EQ(uint64_t(cb.get("fires")->n), 0u);
}

TEST_F(AgentObservabilityTest, BpSetAcceptsRegisterIndexedCondition)
{
    /* The proposal-4.10 Feature-B acceptance: a `[reg+disp]` condition that was
     * rejected at bp.set time before now arms cleanly. (The live test then
     * proves it halts on the right dispatch with the field read from memory.) */
    JsonValue v = parse(dispatchLine(
        "{\"id\":1,\"cmd\":\"bp.set\",\"args\":{\"addr\":\"0824:3723\","
        "\"if\":\"word [si+04]==0x853\"}}"));
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << jsonEncode(v);
    EXPECT_EQ(v.get("result")->get("condition")->s, "word [si+04]==0x853");

    JsonValue st = status();
    const JsonValue *cbs = st.get("result")->get("cond_breakpoints");
    ASSERT_EQ(cbs->a->size(), 1u);
    EXPECT_EQ((*cbs->a)[0].get("condition")->s, "word [si+04]==0x853");
}

TEST_F(AgentObservabilityTest, BpClearAllAndById)
{
    dispatchLine("{\"id\":1,\"cmd\":\"bp.set\",\"args\":{\"addr\":\"0824:0100\"}}");
    JsonValue v2 = parse(dispatchLine(
        "{\"id\":2,\"cmd\":\"bp.set\",\"args\":{\"addr\":\"0824:0200\"}}"));
    int id2 = int(v2.get("result")->get("bp_id")->n);

    /* Clear one by id -> one remains. */
    JsonValue c = parse(dispatchLine(
        std::string("{\"id\":3,\"cmd\":\"bp.clear\",\"args\":{\"bp_id\":") +
        std::to_string(id2) + "}}"));
    ASSERT_TRUE(c.get("ok")->b);
    EXPECT_EQ(uint64_t(c.get("result")->get("remaining")->n), 1u);

    /* Clearing a now-missing id -> not_found. */
    JsonValue nf = parse(dispatchLine(
        std::string("{\"id\":4,\"cmd\":\"bp.clear\",\"args\":{\"bp_id\":") +
        std::to_string(id2) + "}}"));
    EXPECT_FALSE(nf.get("ok")->b);
    EXPECT_EQ(nf.get("error")->get("code")->s, "not_found");

    /* Clear all -> none remain. */
    JsonValue all = parse(dispatchLine("{\"id\":5,\"cmd\":\"bp.clear\"}"));
    ASSERT_TRUE(all.get("ok")->b);
    EXPECT_EQ(uint64_t(all.get("result")->get("remaining")->n), 0u);

    JsonValue st = status();
    EXPECT_EQ(st.get("result")->get("cond_breakpoints")->a->size(), 0u);
}

TEST_F(AgentObservabilityTest, CondBpHotPathCountsFiresAndVotesHalt)
{
    /* Unconditional, halting BP at 0824:0166. The hot path must count every
     * reach, fire each time (no condition), and vote to halt. A reach at a
     * different address must do nothing. */
    parse(dispatchLine("{\"id\":1,\"cmd\":\"bp.set\","
                       "\"args\":{\"addr\":\"0824:0166\"}}"));

    EXPECT_TRUE (AGENT_CondBpCheck(0x0824, 0x0166, 0x0824, 0x0164));  /* halt */
    EXPECT_FALSE(AGENT_CondBpCheck(0x0824, 0x9999, 0x0824, 0x0166));  /* miss */
    EXPECT_TRUE (AGENT_CondBpCheck(0x0824, 0x0166, 0x0824, 0x0164));

    JsonValue st = status();
    const JsonValue &cb = (*st.get("result")->get("cond_breakpoints")->a)[0];
    EXPECT_EQ(uint64_t(cb.get("hits")->n),  2u);
    EXPECT_EQ(uint64_t(cb.get("fires")->n), 2u);
}

TEST_F(AgentObservabilityTest, CondBpConditionGatesHalt)
{
    /* Nth-hit: halt only on the 2nd reach. */
    parse(dispatchLine("{\"id\":1,\"cmd\":\"bp.set\","
                       "\"args\":{\"addr\":\"0824:0166\",\"if\":\"hits==2\"}}"));

    EXPECT_FALSE(AGENT_CondBpCheck(0x0824, 0x0166, 0, 0));  /* hits=1, no halt */
    EXPECT_TRUE (AGENT_CondBpCheck(0x0824, 0x0166, 0, 0));  /* hits=2, halt   */
    EXPECT_FALSE(AGENT_CondBpCheck(0x0824, 0x0166, 0, 0));  /* hits=3, no halt */

    JsonValue st = status();
    const JsonValue &cb = (*st.get("result")->get("cond_breakpoints")->a)[0];
    EXPECT_EQ(uint64_t(cb.get("hits")->n),  3u);   /* reached three times      */
    EXPECT_EQ(uint64_t(cb.get("fires")->n), 1u);   /* condition held only once */
}

TEST_F(AgentObservabilityTest, CondBpContinueFiresWithoutHalting)
{
    /* continue=true with a register condition: fires once (run-and-resume),
     * never votes to halt. */
    parse(dispatchLine("{\"id\":1,\"cmd\":\"bp.set\","
                       "\"args\":{\"addr\":\"0824:0166\",\"if\":\"cx==0x151\","
                       "\"continue\":true}}"));

    reg_ecx = 0x151;
    EXPECT_FALSE(AGENT_CondBpCheck(0x0824, 0x0166, 0, 0));  /* matches, but continue */
    reg_ecx = 0x999;
    EXPECT_FALSE(AGENT_CondBpCheck(0x0824, 0x0166, 0, 0));  /* condition false       */

    JsonValue st = status();
    const JsonValue &cb = (*st.get("result")->get("cond_breakpoints")->a)[0];
    EXPECT_EQ(uint64_t(cb.get("hits")->n),  2u);
    EXPECT_EQ(uint64_t(cb.get("fires")->n), 1u);
}

}  /* anonymous namespace */
