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

/* Agent control channel — event emitter + headless predicate tests.
 *
 * The emitters route through agent::serverBroadcastLine, which is a no-op
 * without a connected client. The tests below verify the no-op path
 * doesn't crash; live wire coverage waits for an integration test that
 * actually opens a socket. */

#include <string>

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "../include/agent.h"

namespace {

class AgentEventsTest : public DOSBoxTestFixture {};

TEST_F(AgentEventsTest, EmittersSafeWithoutClient)
{
    /* In test mode no agent has been started; every emit must short-
     * circuit cleanly inside serverBroadcastLine. */
    AGENT_EmitBpHit(0x1234, 0x5678, 0);
    AGENT_EmitDebuggerEntered("breakpoint");
    AGENT_EmitDebuggerEntered(nullptr);
    AGENT_EmitStateRunning();
    AGENT_EmitStatePaused();
    SUCCEED();
}

TEST_F(AgentEventsTest, IsHeadlessFalseInTestMode)
{
    /* The -tests path never invokes AGENT_StartIfRequested, so the agent
     * must report itself as not-headless. The curses debugger init in
     * DEBUG_EnableDebugger depends on this — flipping it would silently
     * suppress the GUI in normal builds. */
    EXPECT_FALSE(AGENT_IsHeadless());
}

}  /* anonymous namespace */
