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

}  /* anonymous namespace */
