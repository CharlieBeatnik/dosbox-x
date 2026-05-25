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

/* Agent control channel — farcall.watch dispatch + state tests, plus a
 * smoke-test for the immediate-activation breakpoint fix (4.1).
 *
 * The CPU-core hooks themselves (in src/cpu/core_normal/prefix_none.h) need
 * a running guest to exercise, which the -tests harness doesn't provide;
 * those are covered by SMOKE.md / the X2RE shift-test reproducer. Here we
 * verify the agent-side surface (state, dispatch, predicate, emitter
 * safety). */

#include <string>

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "../include/agent.h"
#include "../src/agent/agent_internal.h"

namespace {

using namespace agent;

class AgentFarWatchTest : public DOSBoxTestFixture {
public:
    void SetUp() override { AGENT_FarWatchClear(); }
    void TearDown() override { AGENT_FarWatchClear(); }
};

/* ---- Predicate / setters --------------------------------------------- */

TEST_F(AgentFarWatchTest, MatchesIsFalseByDefault)
{
    EXPECT_FALSE(AGENT_FarWatchMatches(0x0000));
    EXPECT_FALSE(AGENT_FarWatchMatches(0x483C));
    EXPECT_FALSE(AGENT_FarWatchMatches(0xFFFF));
}

TEST_F(AgentFarWatchTest, SetThenMatchesOnlyThatSeg)
{
    AGENT_FarWatchSet(0x483C);
    EXPECT_TRUE(AGENT_FarWatchMatches(0x483C));
    EXPECT_FALSE(AGENT_FarWatchMatches(0x483B));
    EXPECT_FALSE(AGENT_FarWatchMatches(0x483D));
    EXPECT_FALSE(AGENT_FarWatchMatches(0x0000));
}

TEST_F(AgentFarWatchTest, SetReplacesPreviousSentinel)
{
    AGENT_FarWatchSet(0x483C);
    AGENT_FarWatchSet(0x1234);
    EXPECT_FALSE(AGENT_FarWatchMatches(0x483C));
    EXPECT_TRUE(AGENT_FarWatchMatches(0x1234));
}

TEST_F(AgentFarWatchTest, ClearStopsMatching)
{
    AGENT_FarWatchSet(0x483C);
    AGENT_FarWatchClear();
    EXPECT_FALSE(AGENT_FarWatchMatches(0x483C));
}

TEST_F(AgentFarWatchTest, SetZeroIsLegalSentinel)
{
    /* Seg 0x0000 is a real address (IVT); a user watching it must work. */
    AGENT_FarWatchSet(0x0000);
    EXPECT_TRUE(AGENT_FarWatchMatches(0x0000));
    EXPECT_FALSE(AGENT_FarWatchMatches(0x0001));
}

/* ---- Dispatch surface ------------------------------------------------ */

TEST_F(AgentFarWatchTest, DispatchSetsViaNumber)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"farcall.watch\",\"args\":{\"target_seg\":18492}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << reply;
    EXPECT_TRUE(v.get("result")->get("watching")->b);
    EXPECT_EQ(uint16_t(v.get("result")->get("target_seg")->n), 0x483Cu);
    EXPECT_TRUE(AGENT_FarWatchMatches(0x483C));
}

TEST_F(AgentFarWatchTest, DispatchSetsViaHexStringWithPrefix)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"farcall.watch\",\"args\":{\"target_seg\":\"0x483C\"}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << reply;
    EXPECT_TRUE(AGENT_FarWatchMatches(0x483C));
}

TEST_F(AgentFarWatchTest, DispatchSetsViaHexStringNoPrefix)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"farcall.watch\",\"args\":{\"target_seg\":\"483c\"}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << reply;
    EXPECT_TRUE(AGENT_FarWatchMatches(0x483C));
}

TEST_F(AgentFarWatchTest, DispatchClearsViaNullArg)
{
    AGENT_FarWatchSet(0x483C);
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"farcall.watch\",\"args\":{\"target_seg\":null}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << reply;
    EXPECT_FALSE(v.get("result")->get("watching")->b);
    EXPECT_FALSE(AGENT_FarWatchMatches(0x483C));
}

TEST_F(AgentFarWatchTest, DispatchUnwatchClears)
{
    AGENT_FarWatchSet(0x483C);
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"farcall.unwatch\"}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b) << reply;
    EXPECT_FALSE(v.get("result")->get("watching")->b);
    EXPECT_FALSE(AGENT_FarWatchMatches(0x483C));
}

TEST_F(AgentFarWatchTest, DispatchRejectsMissingArg)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"farcall.watch\"}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentFarWatchTest, DispatchRejectsOutOfRange)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"farcall.watch\",\"args\":{\"target_seg\":99999}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentFarWatchTest, DispatchRejectsBadHex)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"farcall.watch\",\"args\":{\"target_seg\":\"zzzz\"}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

/* ---- Emitter safety -------------------------------------------------- */

TEST_F(AgentFarWatchTest, EmitterSafeWithoutClient)
{
    /* No connected client; emit must short-circuit cleanly. */
    AGENT_EmitFarTransfer(0x483C, 0x0FE4, 0x0824, 0x6000, "call_far_direct");
    AGENT_EmitFarTransfer(0x0000, 0x0000, 0x0000, 0x0000, nullptr);
    SUCCEED();
}

}  /* anonymous namespace */
