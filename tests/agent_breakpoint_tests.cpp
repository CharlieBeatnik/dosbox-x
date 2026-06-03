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

/* Regression test for fix 4.1 ("BPs added via the agent activate
 * immediately").
 *
 * The bug we are pinning down: prior to fix 4.1, CBreakpoint::AddBreakpoint
 * /AddInt/AddMem returned a breakpoint with `active=false`. The breakpoint
 * was only flipped active by CBreakpoint::ActivateBreakpoints(), which the
 * curses path triggers from F5/RUN. Agents that add a BP while the CPU is
 * running (e.g. dispatch a `debugger.command BP CS:IP` mid-run) never hit
 * that path, so the BP was silently dead.
 *
 * We exercise the BP add path indirectly via the agent dispatcher (the same
 * route the bug took) and confirm CheckBreakpoint / DEBUG_Breakpoint see
 * the BP as active. */

#include <string>

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "../src/agent/agent_internal.h"
#include "regs.h"
#include "debug.h"

namespace {

using namespace agent;

class AgentBreakpointTest : public DOSBoxTestFixture {
public:
    void SetUp() override {
        /* Always start from an empty BP list. The previous test in the run
         * may have left BPs behind. */
        dispatchLine("{\"id\":99,\"cmd\":\"debugger.command\",\"args\":{\"text\":\"BPDEL 0 *\"}}");
    }
    void TearDown() override {
        dispatchLine("{\"id\":99,\"cmd\":\"debugger.command\",\"args\":{\"text\":\"BPDEL 0 *\"}}");
    }
};

/* DEBUG_Breakpoint() (declared in debug.h) checks CheckBreakpoint(CS,EIP).
 * Plant matching state in the CPU globals, dispatch the BP add, then call
 * it; it must report a hit. */
TEST_F(AgentBreakpointTest, AddBpViaDispatchActivatesImmediately)
{
    Segs.val[cs] = 0x1000;
    reg_eip      = 0x0100;

    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"debugger.command\","
        "\"args\":{\"text\":\"BP 1000:0100\"}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << reply;

    EXPECT_TRUE(DEBUG_Breakpoint())
        << "BP added via debugger.command must fire on CheckBreakpoint match — "
           "regression of fix 4.1 (agent BPs activated only after RUN)";
}

/* Twice-add to the same address: prior behaviour was that the second
 * AddBreakpoint also started inactive. After fix 4.1 both are active; the
 * Activate() implementation's heavy-debug path is just a flag set, so the
 * idempotence is trivial. Sanity-check via DEBUG_Breakpoint. */
TEST_F(AgentBreakpointTest, DuplicateAddStillActive)
{
    Segs.val[cs] = 0x3000;
    reg_eip      = 0x0300;

    dispatchLine("{\"id\":1,\"cmd\":\"debugger.command\","
                 "\"args\":{\"text\":\"BP 3000:0300\"}}");
    dispatchLine("{\"id\":2,\"cmd\":\"debugger.command\","
                 "\"args\":{\"text\":\"BP 3000:0300\"}}");

    EXPECT_TRUE(DEBUG_Breakpoint());
}

/* ---- bp.add / bp.list / bp.del : typed real BPs with stable handles ----
 *
 * These create real CBreakpoints headlessly. In a heavy-debug build (the test
 * config) Activate() is a flag set and breakpoint matching is a seg:off
 * compare, so the whole add/list/del path runs without MemBase — the same
 * reason AgentBreakpointTest above can exercise it. bp.list's bytes_now reads
 * guest memory but is guarded to "" when MemBase==NULL. */

class AgentBpTypedTest : public DOSBoxTestFixture {
public:
    void SetUp() override    { clearAll(); }
    void TearDown() override  { clearAll(); }
    static void clearAll() {
        dispatchLine("{\"id\":99,\"cmd\":\"bp.del\",\"args\":{\"all\":true}}");
    }
    /* Dispatch and return the parsed reply; ASSERTs ok==true. */
    static JsonValue okReply(const std::string &line) {
        JsonValue v;
        EXPECT_TRUE(jsonParse(dispatchLine(line), v)) << line;
        return v;
    }
    /* Find the breakpoint object with the given bp_id in a bp.list result, or
     * nullptr. Binds the array to a stable JsonValue the caller owns. */
    static const JsonValue *findById(const JsonValue &list, uint32_t bpId) {
        const JsonValue *bps = list.get("result");
        bps = bps ? bps->get("breakpoints") : nullptr;
        if (!bps || !bps->isArray() || !bps->a) return nullptr;
        for (const JsonValue &e : *bps->a) {
            const JsonValue *idv = e.get("bp_id");
            if (idv && idv->isNumber() && uint32_t(idv->n) == bpId) return &e;
        }
        return nullptr;
    }
};

TEST_F(AgentBpTypedTest, AddExecReturnsStableIdAndFires)
{
    /* Set CS:IP first: GetAddress() special-cases seg==SegValue(cs) to use the
     * hidden descriptor base (SegPhys(cs)), so the address the breakpoint
     * records at add time and the one CheckBreakpoint computes must be taken
     * with the same CS — exactly as AgentBreakpointTest above does it. */
    Segs.val[cs] = 0x1000;
    reg_eip      = 0x0100;

    JsonValue v = okReply(
        "{\"id\":1,\"cmd\":\"bp.add\",\"args\":{\"addr\":\"1000:0100\"}}");
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << jsonEncode(v);
    const JsonValue *r = v.get("result");
    ASSERT_TRUE(r && r->isObject());
    EXPECT_EQ(r->get("kind")->s, "exec");
    EXPECT_GT(uint32_t(r->get("bp_id")->n), 0u);
    EXPECT_EQ(r->get("addr")->s, "1000:0100");
    EXPECT_EQ(uint16_t(r->get("seg")->n), 0x1000u);
    EXPECT_EQ(uint16_t(r->get("off")->n), 0x0100u);

    /* The real breakpoint must be live (fix 4.1) — CheckBreakpoint sees it. */
    EXPECT_TRUE(DEBUG_Breakpoint());
}

TEST_F(AgentBpTypedTest, AddIntReturnsStableId)
{
    JsonValue v = okReply(
        "{\"id\":1,\"cmd\":\"bp.add\",\"args\":{\"kind\":\"int\",\"int\":33,\"ah\":9}}");
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << jsonEncode(v);
    const JsonValue *r = v.get("result");
    EXPECT_EQ(r->get("kind")->s, "int");
    EXPECT_GT(uint32_t(r->get("bp_id")->n), 0u);
    EXPECT_EQ(int(r->get("int")->n), 33);
    EXPECT_EQ(int(r->get("ah")->n), 9);
    EXPECT_FALSE(r->get("al"));   /* al omitted -> "any", not echoed */
}

TEST_F(AgentBpTypedTest, ListReflectsAddedBreakpoints)
{
    uint32_t idExec = uint32_t(okReply(
        "{\"id\":1,\"cmd\":\"bp.add\",\"args\":{\"addr\":\"0824:6F8E\"}}")
        .get("result")->get("bp_id")->n);
    uint32_t idInt = uint32_t(okReply(
        "{\"id\":2,\"cmd\":\"bp.add\",\"args\":{\"kind\":\"int\",\"int\":33}}")
        .get("result")->get("bp_id")->n);

    JsonValue list = okReply("{\"id\":3,\"cmd\":\"bp.list\"}");
    ASSERT_TRUE(list.get("ok")->b);
    EXPECT_EQ(int(list.get("result")->get("count")->n), 2);

    const JsonValue *be = findById(list, idExec);
    ASSERT_TRUE(be) << jsonEncode(list);
    EXPECT_EQ(be->get("kind")->s, "exec");
    EXPECT_EQ(be->get("addr")->s, "0824:6F8E");
    EXPECT_TRUE(be->get("bytes_now"));   /* present (empty in -tests, no MemBase) */

    const JsonValue *bi = findById(list, idInt);
    ASSERT_TRUE(bi);
    EXPECT_EQ(bi->get("kind")->s, "int");
    EXPECT_EQ(int(bi->get("int")->n), 33);
}

/* The headline property: a handle survives the add/remove of *other*
 * breakpoints, even though the BPoints iteration index does not. */
TEST_F(AgentBpTypedTest, StableIdSurvivesReorder)
{
    uint32_t a = uint32_t(okReply(
        "{\"id\":1,\"cmd\":\"bp.add\",\"args\":{\"addr\":\"1000:0001\"}}")
        .get("result")->get("bp_id")->n);
    uint32_t b = uint32_t(okReply(
        "{\"id\":2,\"cmd\":\"bp.add\",\"args\":{\"addr\":\"2000:0002\"}}")
        .get("result")->get("bp_id")->n);
    uint32_t c = uint32_t(okReply(
        "{\"id\":3,\"cmd\":\"bp.add\",\"args\":{\"addr\":\"3000:0003\"}}")
        .get("result")->get("bp_id")->n);
    EXPECT_LT(a, b);
    EXPECT_LT(b, c);

    /* Delete the middle one by handle. */
    JsonValue del = okReply(
        std::string("{\"id\":4,\"cmd\":\"bp.del\",\"args\":{\"bp_id\":") +
        std::to_string(b) + "}}");
    ASSERT_TRUE(del.get("ok")->b);
    EXPECT_EQ(int(del.get("result")->get("deleted")->n), 1);
    EXPECT_EQ(int(del.get("result")->get("remaining")->n), 2);

    /* a and c keep their handles; b is gone. Their iteration indices have
     * shifted (push_front + the erase), but the bp_ids are unchanged. */
    JsonValue list = okReply("{\"id\":5,\"cmd\":\"bp.list\"}");
    EXPECT_TRUE(findById(list, a));
    EXPECT_TRUE(findById(list, c));
    EXPECT_FALSE(findById(list, b));
}

TEST_F(AgentBpTypedTest, DelByIdNotFound)
{
    /* No breakpoint with this id (clearAll ran in SetUp). */
    JsonValue v = okReply(
        "{\"id\":1,\"cmd\":\"bp.del\",\"args\":{\"bp_id\":4294967295}}");
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "not_found");
}

TEST_F(AgentBpTypedTest, DelRequiresIdOrAll)
{
    JsonValue v = okReply("{\"id\":1,\"cmd\":\"bp.del\",\"args\":{}}");
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");

    /* all=false is not a clear-all request either. */
    JsonValue v2 = okReply("{\"id\":2,\"cmd\":\"bp.del\",\"args\":{\"all\":false}}");
    EXPECT_FALSE(v2.get("ok")->b);
    EXPECT_EQ(v2.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentBpTypedTest, DelAllClears)
{
    okReply("{\"id\":1,\"cmd\":\"bp.add\",\"args\":{\"addr\":\"1000:0100\"}}");
    okReply("{\"id\":2,\"cmd\":\"bp.add\",\"args\":{\"addr\":\"2000:0200\"}}");

    JsonValue v = okReply("{\"id\":3,\"cmd\":\"bp.del\",\"args\":{\"all\":true}}");
    ASSERT_TRUE(v.get("ok")->b);
    EXPECT_GE(int(v.get("result")->get("deleted")->n), 2);
    EXPECT_EQ(int(v.get("result")->get("remaining")->n), 0);

    JsonValue list = okReply("{\"id\":4,\"cmd\":\"bp.list\"}");
    EXPECT_EQ(int(list.get("result")->get("count")->n), 0);
}

TEST_F(AgentBpTypedTest, AddRejectsBadArgs)
{
    /* Unknown kind. */
    JsonValue v1 = okReply(
        "{\"id\":1,\"cmd\":\"bp.add\",\"args\":{\"kind\":\"banana\",\"addr\":\"1000:0\"}}");
    EXPECT_EQ(v1.get("error")->get("code")->s, "bad_args");

    /* exec without addr. */
    JsonValue v2 = okReply("{\"id\":2,\"cmd\":\"bp.add\",\"args\":{\"kind\":\"exec\"}}");
    EXPECT_EQ(v2.get("error")->get("code")->s, "bad_args");

    /* exec addr without a colon. */
    JsonValue v3 = okReply(
        "{\"id\":3,\"cmd\":\"bp.add\",\"args\":{\"addr\":\"DEADBEEF\"}}");
    EXPECT_EQ(v3.get("error")->get("code")->s, "bad_args");

    /* int without int number. */
    JsonValue v4 = okReply("{\"id\":4,\"cmd\":\"bp.add\",\"args\":{\"kind\":\"int\"}}");
    EXPECT_EQ(v4.get("error")->get("code")->s, "bad_args");

    /* int number out of range. */
    JsonValue v5 = okReply(
        "{\"id\":5,\"cmd\":\"bp.add\",\"args\":{\"kind\":\"int\",\"int\":256}}");
    EXPECT_EQ(v5.get("error")->get("code")->s, "bad_args");

    /* al without ah. */
    JsonValue v6 = okReply(
        "{\"id\":6,\"cmd\":\"bp.add\",\"args\":{\"kind\":\"int\",\"int\":33,\"al\":0}}");
    EXPECT_EQ(v6.get("error")->get("code")->s, "bad_args");
}

}  /* anonymous namespace */
