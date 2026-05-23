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

/* Agent control channel — typed CPU-state queries.
 *
 * regs.get  - all GPRs + segregs + EIP + EFLAGS in one structured reply.
 * mem.read  - read N bytes from guest memory, returned base64-encoded.
 *
 * These exist because the legacy ParseCommand passthrough cannot reach the
 * curses register pane (drawn directly to ncurses windows, never via
 * DEBUG_ShowMsg) and the MEMDUMP commands only produce a file on disk. */

#include "config.h"

#if C_DEBUG

#include "agent_internal.h"

#include "mem.h"
#include "regs.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace agent {

namespace {

/* RFC-4648 base64 with `=` padding. */
const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i+1]) << 8) | uint32_t(data[i+2]);
        out.push_back(b64chars[(v >> 18) & 0x3F]);
        out.push_back(b64chars[(v >> 12) & 0x3F]);
        out.push_back(b64chars[(v >>  6) & 0x3F]);
        out.push_back(b64chars[ v        & 0x3F]);
        i += 3;
    }
    if (i < len) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i+1]) << 8;
        out.push_back(b64chars[(v >> 18) & 0x3F]);
        out.push_back(b64chars[(v >> 12) & 0x3F]);
        if (i + 1 < len) {
            out.push_back(b64chars[(v >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.append("==");
        }
    }
    return out;
}

/* Parse a hex string (with or without "0x" prefix) into a uint32_t.
 * Returns true on success. Empty/garbage input fails. */
bool parseHexU32(const std::string &s, uint32_t &out) {
    if (s.empty()) return false;
    size_t pos = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) pos = 2;
    if (pos >= s.size()) return false;
    uint64_t v = 0;
    for (; pos < s.size(); ++pos) {
        char c = s[pos];
        uint32_t d;
        if (c >= '0' && c <= '9') d = uint32_t(c - '0');
        else if (c >= 'a' && c <= 'f') d = 10 + uint32_t(c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + uint32_t(c - 'A');
        else return false;
        v = (v << 4) | d;
        if (v > 0xFFFFFFFFu) return false;
    }
    out = uint32_t(v);
    return true;
}

/* Parse "SEG:OFF" with both halves hex. Both must fit in 32 bits; seg is
 * typically a 16-bit selector but we accept the wider field for symmetry
 * with how the debugger's GetHexValue treats it. */
bool parseSegOff(const std::string &s, uint32_t &seg, uint32_t &off) {
    size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    return parseHexU32(s.substr(0, colon), seg)
        && parseHexU32(s.substr(colon + 1), off);
}

}  /* anonymous namespace */

/* ---- regs.get --------------------------------------------------------- */

JsonValue handleRegsGet(double id, const JsonValue & /*args*/) {
    JsonObject r;
    r.emplace("eax", JsonValue::makeNumber(double(reg_eax)));
    r.emplace("ebx", JsonValue::makeNumber(double(reg_ebx)));
    r.emplace("ecx", JsonValue::makeNumber(double(reg_ecx)));
    r.emplace("edx", JsonValue::makeNumber(double(reg_edx)));
    r.emplace("esi", JsonValue::makeNumber(double(reg_esi)));
    r.emplace("edi", JsonValue::makeNumber(double(reg_edi)));
    r.emplace("ebp", JsonValue::makeNumber(double(reg_ebp)));
    r.emplace("esp", JsonValue::makeNumber(double(reg_esp)));
    r.emplace("eip", JsonValue::makeNumber(double(reg_eip)));
    r.emplace("cs",  JsonValue::makeNumber(double(SegValue(cs))));
    r.emplace("ds",  JsonValue::makeNumber(double(SegValue(ds))));
    r.emplace("es",  JsonValue::makeNumber(double(SegValue(es))));
    r.emplace("fs",  JsonValue::makeNumber(double(SegValue(fs))));
    r.emplace("gs",  JsonValue::makeNumber(double(SegValue(gs))));
    r.emplace("ss",  JsonValue::makeNumber(double(SegValue(ss))));
    r.emplace("eflags", JsonValue::makeNumber(double(reg_flags)));
    return makeReplyOk(id, std::move(r));
}

/* ---- mem.read --------------------------------------------------------- */

/* Cap a single read at 64 KB. Bigger reads should be chunked; this keeps a
 * single reply comfortably under the 1 MB per-client outbox cap even after
 * base64 inflation (64 KB raw -> ~88 KB encoded). */
constexpr size_t MEM_READ_MAX = 64 * 1024;

JsonValue handleMemRead(double id, const JsonValue &args) {
    const JsonValue *vKind = args.get("kind");
    const JsonValue *vAddr = args.get("addr");
    const JsonValue *vLen  = args.get("len");

    if (!vKind || !vKind->isString())
        return makeReplyError(id, "bad_args", "expected \"kind\": \"seg:off\"|\"linear\"|\"physical\"");
    if (!vAddr || !vAddr->isString())
        return makeReplyError(id, "bad_args", "expected \"addr\": hex string");
    if (!vLen || !vLen->isNumber() || vLen->n < 0)
        return makeReplyError(id, "bad_args", "expected \"len\": non-negative integer");

    size_t len = size_t(vLen->n);
    if (len > MEM_READ_MAX) {
        return makeReplyError(id, "bad_args",
            std::string("len exceeds cap of ") + std::to_string(MEM_READ_MAX) + " bytes");
    }

    const std::string &kind = vKind->s;
    std::vector<uint8_t> buf(len);

    if (kind == "seg:off") {
        uint32_t seg, off;
        if (!parseSegOff(vAddr->s, seg, off))
            return makeReplyError(id, "bad_args", "addr must be \"SEG:OFF\" in hex");
        /* Real-mode linear = seg<<4 + off. The paged mem_readb_inline path
         * inside MEM_BlockRead handles protected-mode TLB translation when
         * the CPU is paged; for real-mode programs it short-circuits to a
         * direct physical access. */
        uint32_t linear = (seg << 4) + off;
        MEM_BlockRead(LinearPt(linear), buf.data(), Bitu(len));
    }
    else if (kind == "linear") {
        uint32_t linear;
        if (!parseHexU32(vAddr->s, linear))
            return makeReplyError(id, "bad_args", "addr must be hex");
        MEM_BlockRead(LinearPt(linear), buf.data(), Bitu(len));
    }
    else if (kind == "physical") {
        uint32_t phys;
        if (!parseHexU32(vAddr->s, phys))
            return makeReplyError(id, "bad_args", "addr must be hex");
        /* phys_readb bypasses paging entirely. Use one byte at a time so we
         * get the same OOB-returns-0xFF semantics as the existing pane. */
        for (size_t i = 0; i < len; ++i)
            buf[i] = phys_readb(PhysPt(phys + uint32_t(i)));
    }
    else {
        return makeReplyError(id, "bad_args",
            std::string("unknown kind: \"") + kind + "\"");
    }

    JsonObject r;
    r.emplace("bytes", JsonValue::makeString(base64Encode(buf.data(), len)));
    r.emplace("len",   JsonValue::makeNumber(double(len)));
    return makeReplyOk(id, std::move(r));
}

}  /* namespace agent */

#endif /* C_DEBUG */
