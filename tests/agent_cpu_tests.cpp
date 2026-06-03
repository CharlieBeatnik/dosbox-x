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

/* Agent control channel — regs.get and mem.read tests.
 *
 * regs.get reads CPU globals (cpu_regs / Segs); those exist in the test
 * binary so we can write known values, dispatch, and verify the round-trip.
 *
 * mem.read needs MemBase initialized to actually fetch bytes; in -tests
 * mode the memory subsystem is not brought up, so MemBase is NULL. We
 * therefore only exercise input validation here. End-to-end byte fetches
 * are covered by SMOKE.md against a live boot. */

#include <string>

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "../src/agent/agent_internal.h"
#include "regs.h"

namespace {

using namespace agent;

class AgentCpuTest : public DOSBoxTestFixture {};

/* ---- regs.get -------------------------------------------------------- */

TEST_F(AgentCpuTest, RegsGetReturnsAllFields)
{
    /* Plant recognisable values into the CPU register globals. The macros
     * in regs.h expand to lvalue member accesses on cpu_regs / Segs, so we
     * can simply assign. */
    reg_eax = 0x11111111; reg_ebx = 0x22222222;
    reg_ecx = 0x33333333; reg_edx = 0x44444444;
    reg_esi = 0x55555555; reg_edi = 0x66666666;
    reg_ebp = 0x77777777; reg_esp = 0x88888888;
    reg_eip = 0x99999999;
    Segs.val[cs] = 0xCAFE; Segs.val[ds] = 0xBABE;
    Segs.val[es] = 0xF00D; Segs.val[fs] = 0x0001;
    Segs.val[gs] = 0x0002; Segs.val[ss] = 0xBEEF;
    reg_flags = 0x00000246;  /* IF=1, PF=1 — arbitrary realistic flag value */

    std::string reply = dispatchLine("{\"id\":1,\"cmd\":\"regs.get\"}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    ASSERT_TRUE(v.get("ok") && v.get("ok")->b);
    const JsonValue *r = v.get("result");
    ASSERT_TRUE(r && r->isObject());

    EXPECT_EQ(uint32_t(r->get("eax")->n), 0x11111111u);
    EXPECT_EQ(uint32_t(r->get("ebx")->n), 0x22222222u);
    EXPECT_EQ(uint32_t(r->get("ecx")->n), 0x33333333u);
    EXPECT_EQ(uint32_t(r->get("edx")->n), 0x44444444u);
    EXPECT_EQ(uint32_t(r->get("esi")->n), 0x55555555u);
    EXPECT_EQ(uint32_t(r->get("edi")->n), 0x66666666u);
    EXPECT_EQ(uint32_t(r->get("ebp")->n), 0x77777777u);
    EXPECT_EQ(uint32_t(r->get("esp")->n), 0x88888888u);
    EXPECT_EQ(uint32_t(r->get("eip")->n), 0x99999999u);

    EXPECT_EQ(uint16_t(r->get("cs")->n), 0xCAFEu);
    EXPECT_EQ(uint16_t(r->get("ds")->n), 0xBABEu);
    EXPECT_EQ(uint16_t(r->get("es")->n), 0xF00Du);
    EXPECT_EQ(uint16_t(r->get("fs")->n), 0x0001u);
    EXPECT_EQ(uint16_t(r->get("gs")->n), 0x0002u);
    EXPECT_EQ(uint16_t(r->get("ss")->n), 0xBEEFu);

    EXPECT_EQ(uint32_t(r->get("eflags")->n), 0x00000246u);
}

/* ---- mem.read input validation --------------------------------------- */

TEST_F(AgentCpuTest, MemReadRejectsMissingKind)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.read\",\"args\":{\"addr\":\"1000:0\",\"len\":16}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemReadRejectsMissingAddr)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.read\",\"args\":{\"kind\":\"linear\",\"len\":16}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemReadRejectsNegativeLen)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.read\",\"args\":{\"kind\":\"linear\",\"addr\":\"0\",\"len\":-1}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemReadRejectsOversizeLen)
{
    /* Cap is 64 KB. */
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.read\",\"args\":{\"kind\":\"linear\",\"addr\":\"0\",\"len\":131072}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemReadRejectsUnknownKind)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.read\",\"args\":{\"kind\":\"banana\",\"addr\":\"0\",\"len\":4}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemReadRejectsSegOffWithoutColon)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.read\",\"args\":{\"kind\":\"seg:off\",\"addr\":\"DEADBEEF\",\"len\":4}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemReadRejectsNonHexAddr)
{
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.read\",\"args\":{\"kind\":\"linear\",\"addr\":\"gobbledegook\",\"len\":4}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemReadZeroLengthIsOk)
{
    /* len=0 short-circuits MEM_BlockRead and so is safe even with no
     * memory subsystem initialised. Verify the reply shape. */
    std::string reply = dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.read\",\"args\":{\"kind\":\"linear\",\"addr\":\"0\",\"len\":0}}");
    JsonValue v;
    ASSERT_TRUE(jsonParse(reply, v));
    ASSERT_TRUE(v.get("ok")->b);
    const JsonValue *r = v.get("result");
    EXPECT_EQ(int(r->get("len")->n), 0);
    EXPECT_EQ(r->get("bytes")->s, "");
}

/* ---- regs.set -------------------------------------------------------- *
 *
 * regs.set validates its arguments *before* the "must be paused" gate (the
 * same ordering state.save uses for its slot), so every bad_args path is
 * reachable headless. The successful write needs a paused CPU (the agent
 * dispatch runs between instructions inside DEBUG_Loop), which does not exist
 * in -tests mode, so a well-formed request gets as far as bad_state here; the
 * real register round-trip is covered live. */

static std::string regsSetError(const std::string &argsJson)
{
    return std::string("{\"id\":1,\"cmd\":\"regs.set\",\"args\":") + argsJson + "}";
}

TEST_F(AgentCpuTest, RegsSetRejectsEmpty)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(regsSetError("{}")), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, RegsSetRejectsUnknownRegister)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(regsSetError("{\"rax\":1}")), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, RegsSetRejectsBadValue)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(regsSetError("{\"eax\":\"nothex\"}")), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, RegsSetRejectsOversizeSegment)
{
    /* A segment register must fit in 16 bits. */
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(regsSetError("{\"cs\":\"0x10000\"}")), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, RegsSetValidButNotPausedIsBadState)
{
    /* Well-formed (one GPR, one seg, hex and decimal forms) — passes
     * validation, then trips the paused gate because -tests never pauses. */
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(
        regsSetError("{\"eax\":\"0xDEADBEEF\",\"cs\":2084,\"eflags\":\"0x202\"}")), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_state");
}

/* ---- mem.write ------------------------------------------------------- *
 *
 * Like mem.read, the real store needs MemBase, so only argument validation
 * and the zero-length no-op are exercised here; live coverage does the rest. */

TEST_F(AgentCpuTest, MemWriteRejectsMissingKind)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.write\",\"args\":{\"addr\":\"1000:0\",\"bytes\":\"AAAA\"}}"), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemWriteRejectsMissingBytes)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.write\",\"args\":{\"kind\":\"linear\",\"addr\":\"0\"}}"), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemWriteRejectsInvalidBase64)
{
    /* '!' is outside the base64 alphabet. */
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.write\",\"args\":{\"kind\":\"linear\",\"addr\":\"0\",\"bytes\":\"!!!!\"}}"), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemWriteRejectsUnpaddedBase64)
{
    /* Length not a multiple of 4 is rejected (strict RFC-4648). */
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.write\",\"args\":{\"kind\":\"linear\",\"addr\":\"0\",\"bytes\":\"AAA\"}}"), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemWriteRejectsOversize)
{
    /* "AAAA"*N decodes to 3*N bytes; exceed the 64 KB cap. */
    std::string big(87384, 'A');   /* %4==0; decodes to 65538 bytes > 65536 */
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.write\",\"args\":{\"kind\":\"linear\",\"addr\":\"0\",\"bytes\":\""
        + big + "\"}}"), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemWriteRejectsUnknownKind)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.write\",\"args\":{\"kind\":\"banana\",\"addr\":\"0\",\"bytes\":\"AAAA\"}}"), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemWriteRejectsSegOffWithoutColon)
{
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.write\",\"args\":{\"kind\":\"seg:off\",\"addr\":\"DEADBEEF\",\"bytes\":\"AAAA\"}}"), v));
    EXPECT_FALSE(v.get("ok")->b);
    EXPECT_EQ(v.get("error")->get("code")->s, "bad_args");
}

TEST_F(AgentCpuTest, MemWriteZeroLengthIsOk)
{
    /* Empty base64 decodes to zero bytes; the len==0 guard skips MEM_BlockWrite
     * so this is safe with no memory subsystem. Verify the reply shape. */
    JsonValue v;
    ASSERT_TRUE(jsonParse(dispatchLine(
        "{\"id\":1,\"cmd\":\"mem.write\",\"args\":{\"kind\":\"linear\",\"addr\":\"0\",\"bytes\":\"\"}}"), v));
    ASSERT_TRUE(v.get("ok")->b);
    EXPECT_EQ(int(v.get("result")->get("written")->n), 0);
}

}  /* anonymous namespace */
