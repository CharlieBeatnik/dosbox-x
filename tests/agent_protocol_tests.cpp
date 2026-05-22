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

/* Agent control channel — JSON parser/encoder round-trip tests and
 * command-dispatch sanity. This file is included from tests/tests.h. */

#include <string>

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "../src/agent/agent_internal.h"

namespace {

using namespace agent;

class AgentProtocolTest : public DOSBoxTestFixture {};

/* ---- Parser ------------------------------------------------------------ */

TEST_F(AgentProtocolTest, ParseSimpleObject)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse("{\"id\":1,\"cmd\":\"vm.version\"}", v));
    ASSERT_TRUE(v.isObject());
    const JsonValue *id = v.get("id");
    ASSERT_TRUE(id && id->isNumber());
    EXPECT_DOUBLE_EQ(id->n, 1.0);
    const JsonValue *cmd = v.get("cmd");
    ASSERT_TRUE(cmd && cmd->isString());
    EXPECT_EQ(cmd->s, "vm.version");
}

TEST_F(AgentProtocolTest, ParseStringEscapes)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse("\"a\\\"b\\nc\\td\\u00e9\"", v));
    ASSERT_TRUE(v.isString());
    EXPECT_EQ(v.s, std::string("a\"b\nc\td\xc3\xa9"));
}

TEST_F(AgentProtocolTest, ParseStringSurrogatePair)
{
    /* U+1F600 GRINNING FACE encoded as a UTF-16 surrogate pair. */
    JsonValue v;
    ASSERT_TRUE(jsonParse("\"\\uD83D\\uDE00\"", v));
    ASSERT_TRUE(v.isString());
    EXPECT_EQ(v.s, std::string("\xf0\x9f\x98\x80"));
}

TEST_F(AgentProtocolTest, ParseNumbers)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse("[0, 1, -1, 3.14, 1e3, -2.5e-1]", v));
    ASSERT_TRUE(v.isArray());
    ASSERT_EQ(v.a->size(), 6u);
    EXPECT_DOUBLE_EQ((*v.a)[0].n,  0.0);
    EXPECT_DOUBLE_EQ((*v.a)[1].n,  1.0);
    EXPECT_DOUBLE_EQ((*v.a)[2].n, -1.0);
    EXPECT_DOUBLE_EQ((*v.a)[3].n,  3.14);
    EXPECT_DOUBLE_EQ((*v.a)[4].n,  1000.0);
    EXPECT_DOUBLE_EQ((*v.a)[5].n, -0.25);
}

TEST_F(AgentProtocolTest, ParseBoolsAndNull)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse("[true,false,null]", v));
    ASSERT_TRUE(v.isArray());
    ASSERT_EQ(v.a->size(), 3u);
    EXPECT_TRUE((*v.a)[0].isBool() && (*v.a)[0].b);
    EXPECT_TRUE((*v.a)[1].isBool() && !(*v.a)[1].b);
    EXPECT_TRUE((*v.a)[2].isNull());
}

TEST_F(AgentProtocolTest, ParseEmptyContainers)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse("{}", v));
    EXPECT_TRUE(v.isObject());
    EXPECT_TRUE(v.o->empty());
    ASSERT_TRUE(jsonParse("[]", v));
    EXPECT_TRUE(v.isArray());
    EXPECT_TRUE(v.a->empty());
}

TEST_F(AgentProtocolTest, RejectsTrailingGarbage)
{
    JsonValue v;
    std::string err;
    EXPECT_FALSE(jsonParse("1 2", v, &err));
    EXPECT_FALSE(err.empty());
}

TEST_F(AgentProtocolTest, RejectsUnterminatedString)
{
    JsonValue v;
    EXPECT_FALSE(jsonParse("\"oops", v));
}

TEST_F(AgentProtocolTest, RejectsControlCharInString)
{
    /* Raw newline inside a string literal is illegal per RFC 8259. */
    JsonValue v;
    EXPECT_FALSE(jsonParse("\"a\nb\"", v));
}

/* ---- Encoder ----------------------------------------------------------- */

TEST_F(AgentProtocolTest, EncodeIntegerNoDecimal)
{
    EXPECT_EQ(jsonEncode(JsonValue::makeNumber(42)), "42");
    EXPECT_EQ(jsonEncode(JsonValue::makeNumber(-7)), "-7");
}

TEST_F(AgentProtocolTest, EncodeStringEscapes)
{
    EXPECT_EQ(jsonEncode(JsonValue::makeString("a\"b\\c\n")),
              "\"a\\\"b\\\\c\\n\"");
}

TEST_F(AgentProtocolTest, EncodeNoLiteralNewline)
{
    /* Server framing relies on the encoded form never containing a
     * literal newline. */
    JsonObject o;
    o.emplace("a", JsonValue::makeString("line1\nline2\r\nline3"));
    std::string s = jsonEncode(JsonValue::makeObject(std::move(o)));
    EXPECT_EQ(s.find('\n'), std::string::npos);
    EXPECT_EQ(s.find('\r'), std::string::npos);
}

TEST_F(AgentProtocolTest, RoundTripObject)
{
    JsonObject result;
    result.emplace("version", JsonValue::makeString("2026.05.02"));
    result.emplace("port",    JsonValue::makeNumber(54321));
    result.emplace("ready",   JsonValue::makeBool(true));

    JsonObject reply;
    reply.emplace("id",     JsonValue::makeNumber(7));
    reply.emplace("ok",     JsonValue::makeBool(true));
    reply.emplace("result", JsonValue::makeObject(std::move(result)));

    std::string s = jsonEncode(JsonValue::makeObject(std::move(reply)));
    JsonValue back;
    ASSERT_TRUE(jsonParse(s, back));
    EXPECT_DOUBLE_EQ(back.get("id")->n, 7.0);
    EXPECT_TRUE(back.get("ok")->b);
    const JsonValue *r = back.get("result");
    ASSERT_TRUE(r && r->isObject());
    EXPECT_EQ(r->get("version")->s, "2026.05.02");
    EXPECT_DOUBLE_EQ(r->get("port")->n, 54321.0);
    EXPECT_TRUE(r->get("ready")->b);
}

/* ---- Dispatch --------------------------------------------------------- */

TEST_F(AgentProtocolTest, DispatchUnknownCommandReportsError)
{
    std::string reply = dispatchLine("{\"id\":42,\"cmd\":\"no.such.command\"}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_DOUBLE_EQ(v.get("id")->n, 42.0);
    EXPECT_FALSE(v.get("ok")->b);
    const JsonValue *err = v.get("error");
    ASSERT_TRUE(err && err->isObject());
    EXPECT_EQ(err->get("code")->s, "unknown_cmd");
}

TEST_F(AgentProtocolTest, DispatchMissingCmd)
{
    std::string reply = dispatchLine("{\"id\":1}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "missing_cmd");
}

TEST_F(AgentProtocolTest, DispatchParseErrorEmitsEvent)
{
    std::string reply = dispatchLine("not json");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    /* Without a parsed `id`, we send an unsolicited event rather than a
     * reply. */
    const JsonValue *ev = v.get("event");
    ASSERT_TRUE(ev && ev->isString());
    EXPECT_EQ(ev->s, "agent.error");
    EXPECT_EQ(v.get("code")->s, "parse_error");
}

TEST_F(AgentProtocolTest, DispatchVmVersionShape)
{
    std::string reply = dispatchLine("{\"id\":3,\"cmd\":\"vm.version\"}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_DOUBLE_EQ(v.get("id")->n, 3.0);
    EXPECT_TRUE(v.get("ok")->b);
    const JsonValue *r = v.get("result");
    ASSERT_TRUE(r && r->isObject());
    ASSERT_TRUE(r->get("version") && r->get("version")->isString());
    ASSERT_TRUE(r->get("machine") && r->get("machine")->isString());
    ASSERT_TRUE(r->get("build")   && r->get("build")->isString());
}

}  /* anonymous namespace */
