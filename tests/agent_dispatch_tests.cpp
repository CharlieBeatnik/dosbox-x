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

/* Agent control channel — capture + dispatch tests.
 *
 * Note: ParseCommand from src/debug/debug.cpp depends on curses being
 * initialized for most commands; that doesn't happen in -tests mode. So
 * these tests exercise the capture state machine + dispatch routing,
 * without actually invoking a debugger command. The `debugger.command`
 * path itself gets a smoke test with an empty `text`, which ParseCommand
 * tolerates (trims to empty → returns false). */

#include <string>

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "../include/agent.h"
#include "../src/agent/agent_internal.h"

namespace {

using namespace agent;

class AgentDispatchTest : public DOSBoxTestFixture {};

/* ---- Capture state machine -------------------------------------------- */

TEST_F(AgentDispatchTest, EmitLogAppendsToCapture)
{
    std::string buf;
    captureBegin(&buf);
    AGENT_EmitLog("first");
    AGENT_EmitLog("second");
    captureEnd();
    EXPECT_EQ(buf, "first\nsecond");
}

TEST_F(AgentDispatchTest, EmitLogIgnoredOutsideCapture)
{
    /* Without a server or capture active, AGENT_EmitLog must be a no-op,
     * not crash. */
    AGENT_EmitLog("dangling");
    AGENT_EmitLog(nullptr);
    SUCCEED();
}

TEST_F(AgentDispatchTest, CaptureEndIsIdempotent)
{
    std::string buf;
    captureBegin(&buf);
    captureEnd();
    captureEnd();
    AGENT_EmitLog("after-end");
    EXPECT_TRUE(buf.empty());
}

TEST_F(AgentDispatchTest, CaptureCanBeNested)
{
    /* The current impl is single-level — captureBegin overwrites the
     * pointer. Document and lock in the behaviour. */
    std::string outer, inner;
    captureBegin(&outer);
    AGENT_EmitLog("outer-1");
    captureBegin(&inner);
    AGENT_EmitLog("inner-1");
    captureEnd();
    /* After captureEnd, the inner capture is cleared — subsequent writes
     * vanish until another captureBegin. The outer buffer is NOT
     * automatically restored. Callers must save/restore explicitly. */
    AGENT_EmitLog("dropped");
    EXPECT_EQ(outer, "outer-1");
    EXPECT_EQ(inner, "inner-1");
}

/* ---- Dispatch routing ------------------------------------------------- */

TEST_F(AgentDispatchTest, DispatchDebuggerCommandRejectsMissingText)
{
    std::string reply = dispatchLine("{\"id\":1,\"cmd\":\"debugger.command\"}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentDispatchTest, DispatchLogSubscribeWithoutClientReturnsError)
{
    /* In test mode no agent client is connected, so subscribe must fail
     * cleanly. */
    std::string reply = dispatchLine("{\"id\":2,\"cmd\":\"log.subscribe\"}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "no_client");
}

TEST_F(AgentDispatchTest, DispatchLogUnsubscribeWithoutClientReturnsError)
{
    std::string reply = dispatchLine("{\"id\":3,\"cmd\":\"log.unsubscribe\"}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "no_client");
}

}  /* anonymous namespace */
