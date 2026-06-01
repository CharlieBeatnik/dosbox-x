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
 * regs.get      - all GPRs + segregs + EIP + EFLAGS in one structured reply.
 * mem.read      - read N bytes from guest memory, returned base64-encoded.
 * cpu.step      - structured single-step (proposal 4.9.7).
 * state.save    - snapshot the whole machine to a save slot (proposal 4.9.8).
 * state.restore - restore the machine from a save slot (proposal 4.9.8).
 *
 * regs.get/mem.read exist because the legacy ParseCommand passthrough cannot
 * reach the curses register pane (drawn directly to ncurses windows, never via
 * DEBUG_ShowMsg) and the MEMDUMP commands only produce a file on disk.
 * state.save/state.restore wrap the existing savestate subsystem in a headless,
 * deterministic form so an agent can snapshot the bug window once and re-run
 * from it instantly instead of re-driving the whole guest every iteration. */

#include "config.h"

#if C_DEBUG

#include "agent_internal.h"

#include "dosbox.h"
#include "mem.h"
#include "regs.h"
#include "debug.h"          /* DEBUG_AgentStep, DEBUG_AgentDisasmOne */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

/* Savestate UI-suppression levers, defined as plain globals elsewhere
 * (use_save_file in sdlmain.cpp; noremark/force in savestates.cpp). Declared
 * at global scope here so the references inside namespace agent resolve to the
 * real globals rather than minting agent::-namespaced symbols. */
extern bool use_save_file;
extern bool noremark_save_state;
extern bool force_load_state;

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

/* Render `len` real-mode bytes at seg:off as space-separated uppercase hex
 * ("B8 34 12"). phys_readb matches mem.read's "physical" semantics (no
 * paging, OOB returns 0xFF); for real-mode code physical == linear. */
std::string bytesHex(uint16_t seg, uint16_t off, int len) {
    std::string out;
    uint32_t base = uint32_t(seg) << 4;
    for (int k = 0; k < len; ++k) {
        if (k) out.push_back(' ');
        uint8_t b = phys_readb(PhysPt(base + uint16_t(off + k)));
        char hb[3];
        snprintf(hb, sizeof(hb), "%02X", b);
        out.append(hb);
    }
    return out;
}

/* The 16 architectural registers, as one JSON object. Shared by regs.get and
 * the cpu.step / cpu.step_over result so a client parses one register shape. */
JsonObject buildRegs(void) {
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
    return r;
}

}  /* anonymous namespace */

/* ---- regs.get --------------------------------------------------------- */

JsonValue handleRegsGet(double id, const JsonValue & /*args*/) {
    return makeReplyOk(id, buildRegs());
}

/* ---- cpu.step / cpu.step_over (4.9.7) --------------------------------- */

/* {regs:{...}, cs_ip:"SEG:EIP", insn:{cs_ip,bytes,text}} for the *current*
 * (post-step) CPU state. The insn is the instruction now at CS:IP — i.e. the
 * one about to execute — so a client walking a dispatch sees where control
 * landed and what runs next. Reads live state, so call it after the step. */
JsonObject buildStepResult(void) {
    uint16_t seg = SegValue(cs);
    uint16_t off = uint16_t(reg_eip);

    JsonObject r;
    r.emplace("regs", JsonValue::makeObject(buildRegs()));

    char csip[24];
    snprintf(csip, sizeof(csip), "%04X:%08X", unsigned(seg), unsigned(reg_eip));
    r.emplace("cs_ip", JsonValue::makeString(csip));

    char text[200];
    int len = DEBUG_AgentDisasmOne(seg, off, text, sizeof(text));
    if (len < 1)  len = 1;
    if (len > 16) len = 16;
    JsonObject insn;
    char icsip[24];
    snprintf(icsip, sizeof(icsip), "%04X:%04X", unsigned(seg), unsigned(off));
    insn.emplace("cs_ip", JsonValue::makeString(icsip));
    insn.emplace("bytes", JsonValue::makeString(bytesHex(seg, off, len)));
    insn.emplace("text",  JsonValue::makeString(text));
    r.emplace("insn", JsonValue::makeObject(std::move(insn)));
    return r;
}

JsonValue handleCpuStep(double id, const JsonValue & /*args*/) {
    /* DEBUG_AgentStep(false) returns 0 if the CPU isn't paused, else 1 after
     * executing exactly one instruction (still paused). */
    if (DEBUG_AgentStep(false) == 0)
        return makeReplyError(id, "bad_state",
            "cpu.step requires the CPU to be paused (call cpu.pause first)");
    return makeReplyOk(id, buildStepResult());
}

std::string handleCpuStepOver(double id, const JsonValue & /*args*/) {
    /* Reject a second step-over while one is still running (the CPU is resuming
     * to a temp BP and `debugging` is false, so a new one couldn't proceed
     * anyway). A client that wants to bail out of a stuck step-over should send
     * cpu.pause — that pause delivers the pending reply and frees the slot. */
    if (g_stepOverPending)
        return jsonEncode(makeReplyError(id, "busy",
            "a cpu.step_over is already in progress (wait for its reply, or cpu.pause)"));

    /* Mark a step-over pending before launching: for a CALL/INT/LOOP/REP the
     * step is asynchronous (temp BP + resume) and the reply is deferred to
     * AGENT_OnDebuggerPaused. Setting the slot first is safe because the CPU
     * cannot reach that pause until this handler returns (single-threaded;
     * the normal loop only runs on the next DEBUG_Loop iteration). */
    g_stepOverPending = true;
    g_stepOverId = id;

    int rc = DEBUG_AgentStep(true);
    if (rc == 2)
        return std::string();          /* deferred — reply on the temp-BP pause */

    /* Synchronous outcome — clear the slot and reply now. */
    g_stepOverPending = false;
    if (rc == 0)
        return jsonEncode(makeReplyError(id, "bad_state",
            "cpu.step_over requires the CPU to be paused (call cpu.pause first)"));
    return jsonEncode(makeReplyOk(id, buildStepResult()));
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

/* ---- state.save / state.restore (4.9.8) ------------------------------ */

/* Headless wrappers over the savestate subsystem. The menu/mapper save & load
 * are heavily UI-coupled: a remark input box on save, version/program/memory/
 * machine confirmation dialogs on load, and modal error boxes — every one of
 * which would block the single-threaded agent forever. We:
 *   - require the CPU to be paused (same gate as cpu.step) so save/restore run
 *     at a safe point between instructions and snapshot settled registers;
 *   - pin use_save_file=false so the agent's `slot` argument is always honoured
 *     regardless of any user `savefile=` config;
 *   - suppress the remark prompt (noremark_save_state) and the load confirms
 *     (force_load_state) for the duration of the call;
 *   - pre-validate the cases that would otherwise pop a modal (out-of-range
 *     slot, empty slot on restore, oversize guest memory).
 * A genuine disk I/O or file-corruption failure can still raise a modal, but
 * that cannot occur in the normal save-then-restore cycle and is documented. */

namespace {

/* SaveState slots are [0, SLOT_COUNT*MAX_PAGE) == [0, 100). */
const size_t STATE_SLOT_COUNT = SaveState::SLOT_COUNT * SaveState::MAX_PAGE;

/* SaveState::save/load refuse (and pop a modal) above 1 GB of guest memory. */
bool stateMemoryTooLarge(void) {
    return (size_t(MEM_TotalPages()) * 4096 / 1024 / 1024) > 1024;
}

/* Validate the required "slot" argument into [0, STATE_SLOT_COUNT). On failure
 * fills `err` with a ready-to-return bad_args reply and returns false. */
bool parseStateSlot(double id, const JsonValue &args, size_t &slot, JsonValue &err) {
    const JsonValue *vSlot = args.get("slot");
    if (!vSlot || !vSlot->isNumber() || vSlot->n < 0 ||
        vSlot->n >= double(STATE_SLOT_COUNT)) {
        err = makeReplyError(id, "bad_args",
            std::string("expected \"slot\": integer in [0,") +
            std::to_string(STATE_SLOT_COUNT - 1) + "]");
        return false;
    }
    slot = size_t(vSlot->n);
    return true;
}

}  /* anonymous namespace */

JsonValue handleStateSave(double id, const JsonValue &args) {
    size_t slot; JsonValue err;
    if (!parseStateSlot(id, args, slot, err)) return err;

    if (!DEBUG_AgentIsPaused())
        return makeReplyError(id, "bad_state",
            "state.save requires the CPU to be paused (call cpu.pause first)");

    if (stateMemoryTooLarge())
        return makeReplyError(id, "unsupported",
            "guest memory exceeds the 1 GB savestate limit");

    bool prevUseFile  = use_save_file;
    bool prevNoRemark = noremark_save_state;
    use_save_file     = false;     /* always honour the slot argument */
    noremark_save_state = true;    /* no remark input box */
    SaveState::instance().save(slot);
    noremark_save_state = prevNoRemark;
    use_save_file       = prevUseFile;

    /* save() returns void and reports failure only via the log / a modal; an
     * empty slot afterwards is our proxy for "the write did not land". */
    if (SaveState::instance().isEmpty(slot))
        return makeReplyError(id, "io_error",
            "save failed to write the slot (see emulator log)");

    JsonObject r;
    r.emplace("slot", JsonValue::makeNumber(double(slot)));
    r.emplace("name", JsonValue::makeString(SaveState::instance().getName(slot)));
    return makeReplyOk(id, std::move(r));
}

JsonValue handleStateRestore(double id, const JsonValue &args) {
    size_t slot; JsonValue err;
    if (!parseStateSlot(id, args, slot, err)) return err;

    if (!DEBUG_AgentIsPaused())
        return makeReplyError(id, "bad_state",
            "state.restore requires the CPU to be paused (call cpu.pause first)");

    /* Pre-check empties so we never reach load()'s "empty slot" modal. */
    if (SaveState::instance().isEmpty(slot))
        return makeReplyError(id, "not_found",
            std::string("save slot ") + std::to_string(slot) + " is empty");

    if (stateMemoryTooLarge())
        return makeReplyError(id, "unsupported",
            "guest memory exceeds the 1 GB savestate limit");

    bool prevUseFile = use_save_file;
    bool prevForce   = force_load_state;
    use_save_file    = false;      /* always honour the slot argument */
    force_load_state = true;       /* no version/program/memory/machine confirms */
    SaveState::instance().load(slot);
    force_load_state = prevForce;
    use_save_file    = prevUseFile;

    /* Reply with where the restored CPU will resume — same {regs, cs_ip, insn}
     * shape cpu.step returns — so the client needs no follow-up regs.get to see
     * that the machine snapped back to the saved point. */
    JsonObject r = buildStepResult();
    r.emplace("slot", JsonValue::makeNumber(double(slot)));
    r.emplace("name", JsonValue::makeString(SaveState::instance().getName(slot)));
    return makeReplyOk(id, std::move(r));
}

}  /* namespace agent */

#endif /* C_DEBUG */
