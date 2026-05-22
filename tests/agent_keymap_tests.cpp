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

/* Agent control channel — keymap + keyboard dispatch tests. */

#include <string>

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "../src/agent/agent_internal.h"

extern std::string strPasteBuffer;

namespace {

using namespace agent;

class AgentKeymapTest : public DOSBoxTestFixture {};

/* The table should cover essentially every KBD_KEYS value. KBD_NONE and
 * KBD_LAST are sentinels and not present. */
TEST_F(AgentKeymapTest, TableCoversEnumRange)
{
    /* KBD_LAST - 1 == KBD_xfer; KBD_NONE == 0. So the number of usable
     * keys is KBD_LAST - 1 (we exclude KBD_NONE). */
    const size_t enumKeys = static_cast<size_t>(KBD_LAST) - 1u;
    EXPECT_EQ(keyboardTableSize(), enumKeys)
        << "Keyboard name table size must match KBD_KEYS enum range "
           "(excluding KBD_NONE). Add or remove an entry in agent_keyboard.cpp.";
}

TEST_F(AgentKeymapTest, CommonKeysResolve)
{
    KBD_KEYS k;
    EXPECT_TRUE(keyboardNameToKey("enter", k));      EXPECT_EQ(k, KBD_enter);
    EXPECT_TRUE(keyboardNameToKey("esc", k));        EXPECT_EQ(k, KBD_esc);
    EXPECT_TRUE(keyboardNameToKey("space", k));      EXPECT_EQ(k, KBD_space);
    EXPECT_TRUE(keyboardNameToKey("leftshift", k));  EXPECT_EQ(k, KBD_leftshift);
    EXPECT_TRUE(keyboardNameToKey("rightctrl", k));  EXPECT_EQ(k, KBD_rightctrl);
    EXPECT_TRUE(keyboardNameToKey("f10", k));        EXPECT_EQ(k, KBD_f10);
    EXPECT_TRUE(keyboardNameToKey("kp7", k));        EXPECT_EQ(k, KBD_kp7);
    EXPECT_TRUE(keyboardNameToKey("a", k));          EXPECT_EQ(k, KBD_a);
    EXPECT_TRUE(keyboardNameToKey("1", k));          EXPECT_EQ(k, KBD_1);
}

TEST_F(AgentKeymapTest, UnknownNameRejected)
{
    KBD_KEYS k;
    EXPECT_FALSE(keyboardNameToKey("nope", k));
    EXPECT_FALSE(keyboardNameToKey("", k));
    EXPECT_FALSE(keyboardNameToKey("Enter", k));  /* case-sensitive */
}

TEST_F(AgentKeymapTest, TypeAppendsToPasteBuffer)
{
    strPasteBuffer.clear();
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"keyboard.type\",\"args\":{\"text\":\"DIR\\r\"}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_TRUE(v.get("ok")->b);
    EXPECT_DOUBLE_EQ(v.get("result")->get("queued")->n, 4.0);
    EXPECT_EQ(strPasteBuffer, std::string("DIR\r"));
    strPasteBuffer.clear();
}

TEST_F(AgentKeymapTest, TypeRequiresStringText)
{
    std::string reply = dispatchLine(
        "{\"id\":2,\"cmd\":\"keyboard.type\",\"args\":{\"text\":123}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentKeymapTest, PressRejectsUnknownKey)
{
    std::string reply = dispatchLine(
        "{\"id\":3,\"cmd\":\"keyboard.press\",\"args\":{\"key\":\"flibbertigibbet\"}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentKeymapTest, TapRejectsMissingKey)
{
    std::string reply = dispatchLine(
        "{\"id\":4,\"cmd\":\"keyboard.tap\",\"args\":{}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

/* press/release/tap with a valid key would call KEYBOARD_AddKey, which in
 * test mode is harmless (it just queues into the PS/2 buffer). Verify
 * dispatch succeeds and the response shape matches. */
TEST_F(AgentKeymapTest, PressValidKeySucceeds)
{
    std::string reply = dispatchLine(
        "{\"id\":5,\"cmd\":\"keyboard.press\",\"args\":{\"key\":\"a\"}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_TRUE(v.get("ok")->b);

    /* Drain the release too so the test doesn't leave 'a' held down. */
    dispatchLine("{\"id\":6,\"cmd\":\"keyboard.release\",\"args\":{\"key\":\"a\"}}");
}

}  /* anonymous namespace */
