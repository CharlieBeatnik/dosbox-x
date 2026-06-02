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

#include <cctype>
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

/* ---- bp.set / bp.clear : conditional / Nth-hit breakpoints (4.9.9) ----
 *
 * A conditional breakpoint pairs an execution address with three optional
 * pieces: a `if` condition (a tiny expression over registers / memory / the
 * BP's own hit count), a `do` command macro (allowlisted read-only commands
 * whose output is captured atomically at the trigger instant), and a
 * `continue` flag (run the macro then auto-resume instead of halting). The
 * per-instruction check runs from DEBUG_HeavyIsBreakpoint (AGENT_CondBpCheck),
 * so a condition that does not hold simply doesn't halt — no manual
 * break/inspect/resume round-trip, and the snapshot is taken before the
 * emulator advances.
 *
 * Condition mini-language (one comparison):
 *
 *     operand [ '&' mask ] op operand
 *
 *   op       := == | != | < | <= | > | >=     (unsigned)
 *   operand  := value | [size] '[' value ':' value ']'
 *   value    := register | hits | number      (number: 0xHEX or decimal)
 *   register := ax bx cx dx si di bp sp ip / al ah bl bh cl ch dl dh
 *               / eax..eip / cs ds es ss fs gs / flags eflags
 *   size     := byte | word | dword            (memory access width; word default)
 *
 * Examples: "sp==0x0200", "hits==7", "cx==151", "byte [es:di]==0x5A",
 *           "[ss:01F8]>0x1000", "flags&0x40!=0". */

namespace {

/* Register identifiers the condition evaluator understands. All sub-registers
 * are derived from the 32-bit reg_e* globals by masking/shifting, so this needs
 * no reg_ax/reg_al macros. */
enum BpReg {
    BPREG_NONE = 0,
    BPREG_AX, BPREG_BX, BPREG_CX, BPREG_DX, BPREG_SI, BPREG_DI, BPREG_BP, BPREG_SP, BPREG_IP,
    BPREG_AL, BPREG_AH, BPREG_BL, BPREG_BH, BPREG_CL, BPREG_CH, BPREG_DL, BPREG_DH,
    BPREG_EAX, BPREG_EBX, BPREG_ECX, BPREG_EDX, BPREG_ESI, BPREG_EDI, BPREG_EBP, BPREG_ESP, BPREG_EIP,
    BPREG_CS, BPREG_DS, BPREG_ES, BPREG_SS, BPREG_FS, BPREG_GS,
    BPREG_FLAGS, BPREG_EFLAGS
};

struct BpRegName { const char *name; int reg; };
const BpRegName kBpRegs[] = {
    {"ax",BPREG_AX},{"bx",BPREG_BX},{"cx",BPREG_CX},{"dx",BPREG_DX},
    {"si",BPREG_SI},{"di",BPREG_DI},{"bp",BPREG_BP},{"sp",BPREG_SP},{"ip",BPREG_IP},
    {"al",BPREG_AL},{"ah",BPREG_AH},{"bl",BPREG_BL},{"bh",BPREG_BH},
    {"cl",BPREG_CL},{"ch",BPREG_CH},{"dl",BPREG_DL},{"dh",BPREG_DH},
    {"eax",BPREG_EAX},{"ebx",BPREG_EBX},{"ecx",BPREG_ECX},{"edx",BPREG_EDX},
    {"esi",BPREG_ESI},{"edi",BPREG_EDI},{"ebp",BPREG_EBP},{"esp",BPREG_ESP},{"eip",BPREG_EIP},
    {"cs",BPREG_CS},{"ds",BPREG_DS},{"es",BPREG_ES},{"ss",BPREG_SS},{"fs",BPREG_FS},{"gs",BPREG_GS},
    {"flags",BPREG_FLAGS},{"eflags",BPREG_EFLAGS},
};

std::string bpLower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool bpLookupReg(const std::string &lc, int &out) {
    for (const BpRegName &r : kBpRegs)
        if (lc == r.name) { out = r.reg; return true; }
    return false;
}

/* 0xHEX or plain decimal into a u32. Rejects partial/garbage input. */
bool bpParseNum(const std::string &w, uint32_t &out) {
    if (w.empty()) return false;
    const char *p = w.c_str();
    int base = 10;
    if (w.size() > 2 && w[0] == '0' && (w[1] == 'x' || w[1] == 'X')) {
        base = 16; p += 2;
        if (*p == '\0') return false;
    } else {
        for (char ch : w) if (!isdigit((unsigned char)ch)) return false;
    }
    char *end = nullptr;
    unsigned long v = strtoul(p, &end, base);
    if (!end || *end != '\0') return false;
    if (v > 0xFFFFFFFFul) return false;
    out = (uint32_t)v;
    return true;
}

/* ---- tokenizer ---- */
enum BpTokKind { BPT_WORD, BPT_OP, BPT_LBRACK, BPT_RBRACK, BPT_COLON, BPT_AMP };
struct BpTok { BpTokKind kind; std::string text; int op; };

bool bpTokenize(const std::string &s, std::vector<BpTok> &out, std::string &err) {
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '[') { out.push_back({BPT_LBRACK, "", 0}); i++; continue; }
        if (c == ']') { out.push_back({BPT_RBRACK, "", 0}); i++; continue; }
        if (c == ':') { out.push_back({BPT_COLON,  "", 0}); i++; continue; }
        if (c == '&') { out.push_back({BPT_AMP,    "", 0}); i++; continue; }
        if (c == '<' || c == '>' || c == '=' || c == '!') {
            int op;
            if (c == '=') {
                if (i + 1 < n && s[i+1] == '=') { op = BP_EQ; i += 2; }
                else { err = "'=' must be '=='"; return false; }
            } else if (c == '!') {
                if (i + 1 < n && s[i+1] == '=') { op = BP_NE; i += 2; }
                else { err = "'!' must be '!='"; return false; }
            } else if (c == '<') {
                if (i + 1 < n && s[i+1] == '=') { op = BP_LE; i += 2; } else { op = BP_LT; i++; }
            } else {
                if (i + 1 < n && s[i+1] == '=') { op = BP_GE; i += 2; } else { op = BP_GT; i++; }
            }
            out.push_back({BPT_OP, "", op});
            continue;
        }
        if (isalnum((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            out.push_back({BPT_WORD, s.substr(i, j - i), 0});
            i = j;
            continue;
        }
        err = std::string("unexpected character '") + c + "'";
        return false;
    }
    return true;
}

/* ---- recursive-descent parse ----
 * `hexNum` selects the numeric convention for a bare (non-register, non-hits)
 * word. Inside a memory reference [seg:off] both halves follow the agent's
 * SEG:OFF convention (bare hex, like mem.read / cpu.probe / cpu.disasm), so a
 * literal there is parsed as hex. A standalone operand (e.g. "cx==151") uses
 * the C-like convention: decimal unless 0x-prefixed. */
bool bpParseValSrc(const std::string &word, BpValSrc &out, std::string &err, bool hexNum) {
    std::string lc = bpLower(word);
    int reg;
    if (bpLookupReg(lc, reg)) { out.kind = BpValSrc::REG; out.reg = reg; return true; }
    if (lc == "hits")         { out.kind = BpValSrc::HITS; return true; }
    uint32_t num;
    if (hexNum ? parseHexU32(word, num) : bpParseNum(word, num)) {
        out.kind = BpValSrc::IMM; out.imm = num; return true;
    }
    err = std::string("unknown operand '") + word + "'";
    return false;
}

bool bpParseOperand(const std::vector<BpTok> &t, size_t &i, BpOperand &out, std::string &err) {
    int size = 2;
    bool sized = false;
    if (i < t.size() && t[i].kind == BPT_WORD) {
        std::string lc = bpLower(t[i].text);
        if (lc == "byte" || lc == "word" || lc == "dword") {
            size = (lc == "byte") ? 1 : (lc == "word") ? 2 : 4;
            sized = true;
            i++;
            if (i >= t.size() || t[i].kind != BPT_LBRACK) {
                err = "expected '[' after size keyword"; return false;
            }
        }
    }
    if (i < t.size() && t[i].kind == BPT_LBRACK) {
        i++;   /* consume '[' */
        if (i >= t.size() || t[i].kind != BPT_WORD) { err = "expected segment in [seg:off]"; return false; }
        BpValSrc seg; if (!bpParseValSrc(t[i].text, seg, err, /*hexNum=*/true)) return false; i++;
        if (i >= t.size() || t[i].kind != BPT_COLON) { err = "expected ':' in [seg:off]"; return false; } i++;
        if (i >= t.size() || t[i].kind != BPT_WORD) { err = "expected offset in [seg:off]"; return false; }
        BpValSrc off; if (!bpParseValSrc(t[i].text, off, err, /*hexNum=*/true)) return false; i++;
        if (i >= t.size() || t[i].kind != BPT_RBRACK) { err = "expected ']' in [seg:off]"; return false; } i++;
        out.isMem = true; out.memSeg = seg; out.memOff = off; out.memSize = size;
        return true;
    }
    if (sized) { err = "size keyword is only valid before '[seg:off]'"; return false; }
    if (i >= t.size() || t[i].kind != BPT_WORD) { err = "expected an operand"; return false; }
    BpValSrc v; if (!bpParseValSrc(t[i].text, v, err, /*hexNum=*/false)) return false; i++;
    out.isMem = false; out.direct = v; out.memSize = 2;
    return true;
}

/* ---- evaluation ---- */
uint32_t bpEvalReg(int reg) {
    switch (reg) {
        case BPREG_AX:  return reg_eax & 0xFFFF;
        case BPREG_BX:  return reg_ebx & 0xFFFF;
        case BPREG_CX:  return reg_ecx & 0xFFFF;
        case BPREG_DX:  return reg_edx & 0xFFFF;
        case BPREG_SI:  return reg_esi & 0xFFFF;
        case BPREG_DI:  return reg_edi & 0xFFFF;
        case BPREG_BP:  return reg_ebp & 0xFFFF;
        case BPREG_SP:  return reg_esp & 0xFFFF;
        case BPREG_IP:  return reg_eip & 0xFFFF;
        case BPREG_AL:  return reg_eax & 0xFF;
        case BPREG_AH:  return (reg_eax >> 8) & 0xFF;
        case BPREG_BL:  return reg_ebx & 0xFF;
        case BPREG_BH:  return (reg_ebx >> 8) & 0xFF;
        case BPREG_CL:  return reg_ecx & 0xFF;
        case BPREG_CH:  return (reg_ecx >> 8) & 0xFF;
        case BPREG_DL:  return reg_edx & 0xFF;
        case BPREG_DH:  return (reg_edx >> 8) & 0xFF;
        case BPREG_EAX: return reg_eax;
        case BPREG_EBX: return reg_ebx;
        case BPREG_ECX: return reg_ecx;
        case BPREG_EDX: return reg_edx;
        case BPREG_ESI: return reg_esi;
        case BPREG_EDI: return reg_edi;
        case BPREG_EBP: return reg_ebp;
        case BPREG_ESP: return reg_esp;
        case BPREG_EIP: return reg_eip;
        case BPREG_CS:  return SegValue(cs);
        case BPREG_DS:  return SegValue(ds);
        case BPREG_ES:  return SegValue(es);
        case BPREG_SS:  return SegValue(ss);
        case BPREG_FS:  return SegValue(fs);
        case BPREG_GS:  return SegValue(gs);
        case BPREG_FLAGS:  return reg_flags & 0xFFFF;
        case BPREG_EFLAGS: return reg_flags;
        default: return 0;
    }
}

uint32_t bpEvalValSrc(const BpValSrc &v, uint64_t hits) {
    switch (v.kind) {
        case BpValSrc::IMM:  return v.imm;
        case BpValSrc::HITS: return (uint32_t)hits;
        case BpValSrc::REG:  return bpEvalReg(v.reg);
    }
    return 0;
}

uint32_t bpEvalOperand(const BpOperand &op, uint64_t hits) {
    if (!op.isMem) return bpEvalValSrc(op.direct, hits);
    uint32_t seg = bpEvalValSrc(op.memSeg, hits) & 0xFFFF;
    uint32_t off = bpEvalValSrc(op.memOff, hits) & 0xFFFF;
    uint32_t lin = (seg << 4) + off;
    uint32_t v = 0;
    for (int k = 0; k < op.memSize; ++k)
        v |= uint32_t(phys_readb(PhysPt(lin + uint32_t(k)))) << (8 * k);
    return v;
}

/* ---- macro execution (allowlisted, read-only) ---- */
bool bpMacroCmdAllowed(const std::string &c) {
    return c == "regs.get" || c == "mem.read" || c == "cpu.disasm"
        || c == "cpu.traceback" || c == "debug.status";
}

JsonValue bpRunMacroCommand(const std::string &cmd, const JsonValue &args) {
    if (cmd == "regs.get")      return handleRegsGet(0, args);
    if (cmd == "mem.read")      return handleMemRead(0, args);
    if (cmd == "cpu.disasm")    return handleCpuDisasm(0, args);
    if (cmd == "cpu.traceback") return handleCpuTraceback(0, args);
    if (cmd == "debug.status")  return handleDebugStatus(0, args);
    return makeReplyError(0, "unknown_cmd", std::string("macro cmd not allowed: ") + cmd);
}

/* Repackage a handler reply ({id,ok,result|error}) as a macro result entry
 * ({cmd,ok,result|error}) — the id is meaningless inside a macro. */
JsonValue bpMacroResultEntry(const std::string &cmd, const JsonValue &reply) {
    JsonObject e;
    e.emplace("cmd", JsonValue::makeString(cmd));
    if (const JsonValue *ok  = reply.get("ok"))     e.emplace("ok", *ok);
    if (const JsonValue *res = reply.get("result")) e.emplace("result", *res);
    if (const JsonValue *err = reply.get("error"))  e.emplace("error", *err);
    return JsonValue::makeObject(std::move(e));
}

constexpr size_t COND_BP_MAX       = 32;
constexpr size_t COND_BP_MACRO_MAX = 16;
uint32_t g_nextCondBpId = 1;

}  /* anonymous namespace */

std::vector<CondBp> g_condBps;
bool                g_condBpActive = false;

bool parseBpCondition(const std::string &s, BpCondition &out, std::string &err) {
    out = BpCondition{};
    std::vector<BpTok> toks;
    if (!bpTokenize(s, toks, err)) return false;
    if (toks.empty()) { err = "empty condition"; return false; }

    size_t i = 0;
    if (!bpParseOperand(toks, i, out.lhs, err)) return false;
    if (i < toks.size() && toks[i].kind == BPT_AMP) {
        i++;
        if (i >= toks.size() || toks[i].kind != BPT_WORD) { err = "expected a number after '&'"; return false; }
        uint32_t m;
        if (!bpParseNum(toks[i].text, m)) { err = std::string("mask must be a number: ") + toks[i].text; return false; }
        out.hasMask = true; out.mask = m; i++;
    }
    if (i >= toks.size() || toks[i].kind != BPT_OP) {
        err = "expected a comparison operator (==, !=, <, <=, >, >=)"; return false;
    }
    out.op = toks[i].op; i++;
    if (!bpParseOperand(toks, i, out.rhs, err)) return false;
    if (i != toks.size()) { err = "trailing tokens after the condition"; return false; }
    out.present = true;
    return true;
}

bool evalBpCondition(const BpCondition &c, uint64_t hits) {
    if (!c.present) return true;
    uint32_t l = bpEvalOperand(c.lhs, hits);
    if (c.hasMask) l &= c.mask;
    uint32_t r = bpEvalOperand(c.rhs, hits);
    switch (c.op) {
        case BP_EQ: return l == r;
        case BP_NE: return l != r;
        case BP_LT: return l <  r;
        case BP_LE: return l <= r;
        case BP_GT: return l >  r;
        case BP_GE: return l >= r;
    }
    return false;
}

JsonValue handleBpSet(double id, const JsonValue &args) {
#if !C_HEAVY_DEBUG
    (void)args;
    return makeReplyError(id, "unsupported",
        "conditional breakpoints require a heavy-debug build (C_HEAVY_DEBUG)");
#else
    const JsonValue *vaddr = args.get("addr");
    if (!vaddr || !vaddr->isString())
        return makeReplyError(id, "bad_args", "expected {\"addr\":\"SEG:OFF\"}");
    uint32_t seg, off;
    if (!parseSegOff(vaddr->s, seg, off))
        return makeReplyError(id, "bad_args", "addr must be \"SEG:OFF\" in hex");

    CondBp bp;
    bp.seg = (uint16_t)seg;
    bp.off = (uint16_t)off;

    if (const JsonValue *vif = args.get("if")) {
        if (!vif->isString())
            return makeReplyError(id, "bad_args", "'if' must be a string condition");
        std::string cerr;
        if (!parseBpCondition(vif->s, bp.cond, cerr))
            return makeReplyError(id, "bad_args", std::string("bad condition: ") + cerr);
        bp.condStr = vif->s;
    }

    if (const JsonValue *vdo = args.get("do")) {
        if (!vdo->isArray())
            return makeReplyError(id, "bad_args", "'do' must be an array of {cmd,args}");
        if (vdo->a && vdo->a->size() > COND_BP_MACRO_MAX)
            return makeReplyError(id, "bad_args",
                std::string("too many macro commands (max ") + std::to_string(COND_BP_MACRO_MAX) + ")");
        if (vdo->a) for (const JsonValue &e : *vdo->a) {
            const JsonValue *c = e.isObject() ? e.get("cmd") : nullptr;
            if (!c || !c->isString())
                return makeReplyError(id, "bad_args",
                    "each 'do' entry must be {\"cmd\":\"...\",\"args\":{...}}");
            if (!bpMacroCmdAllowed(c->s))
                return makeReplyError(id, "bad_args",
                    std::string("macro cmd not allowed: ") + c->s +
                    " (allowed: regs.get, mem.read, cpu.disasm, cpu.traceback, debug.status)");
            BpMacroCmd mc;
            mc.cmd = c->s;
            const JsonValue *a = e.get("args");
            mc.args = (a && a->isObject()) ? *a : JsonValue::makeObject();
            bp.macro.push_back(std::move(mc));
        }
    }

    if (const JsonValue *vc = args.get("continue")) {
        if (!vc->isBool())
            return makeReplyError(id, "bad_args", "'continue' must be a bool");
        bp.cont = vc->b;
    }

    if (g_condBps.size() >= COND_BP_MAX)
        return makeReplyError(id, "bad_args",
            std::string("too many conditional breakpoints (max ") +
            std::to_string(COND_BP_MAX) + "); bp.clear first");

    bp.id = g_nextCondBpId++;
    g_condBps.push_back(std::move(bp));
    g_condBpActive = true;

    const CondBp &added = g_condBps.back();
    JsonObject r;
    r.emplace("bp_id", JsonValue::makeNumber(double(added.id)));
    char addr[24];
    snprintf(addr, sizeof(addr), "%04X:%04X", unsigned(added.seg), unsigned(added.off));
    r.emplace("addr", JsonValue::makeString(addr));
    r.emplace("seg",  JsonValue::makeNumber(double(added.seg)));
    r.emplace("off",  JsonValue::makeNumber(double(added.off)));
    r.emplace("condition", added.condStr.empty()
              ? JsonValue::makeNull() : JsonValue::makeString(added.condStr));
    r.emplace("macro_len", JsonValue::makeNumber(double(added.macro.size())));
    r.emplace("continue",  JsonValue::makeBool(added.cont));
    return makeReplyOk(id, std::move(r));
#endif
}

JsonValue handleBpClear(double id, const JsonValue &args) {
    if (const JsonValue *vid = args.get("bp_id")) {
        if (!vid->isNumber())
            return makeReplyError(id, "bad_args", "'bp_id' must be a number");
        uint32_t want = (uint32_t)vid->n;
        bool found = false;
        for (size_t i = 0; i < g_condBps.size(); ++i) {
            if (g_condBps[i].id == want) {
                g_condBps.erase(g_condBps.begin() + (long)i);
                found = true;
                break;
            }
        }
        g_condBpActive = !g_condBps.empty();
        if (!found)
            return makeReplyError(id, "not_found",
                std::string("no conditional breakpoint with bp_id ") + std::to_string(want));
        JsonObject r;
        r.emplace("cleared",   JsonValue::makeNumber(1.0));
        r.emplace("remaining", JsonValue::makeNumber(double(g_condBps.size())));
        return makeReplyOk(id, std::move(r));
    }

    size_t n = g_condBps.size();
    g_condBps.clear();
    g_condBpActive = false;
    JsonObject r;
    r.emplace("cleared",   JsonValue::makeNumber(double(n)));
    r.emplace("remaining", JsonValue::makeNumber(0.0));
    return makeReplyOk(id, std::move(r));
}

/* The per-instruction worker behind AGENT_CondBpCheck. Runs from the heavy-
 * debug hook with the CPU at (cur_cs, cur_off). For each conditional BP there
 * it bumps the reach counter, evaluates the condition, and — if it holds —
 * fires: runs the macro (output captured now), emits bp.cond, and votes to
 * halt unless the BP is run-and-continue. Returns true if any matching BP
 * wants to halt. */
bool condBpDispatch(uint16_t cur_cs, uint16_t cur_off, uint16_t from_cs, uint16_t from_ip) {
    bool halt = false;
    for (CondBp &bp : g_condBps) {
        if (bp.seg != cur_cs || bp.off != cur_off) continue;
        bp.hits++;
        if (bp.cond.present && !evalBpCondition(bp.cond, bp.hits)) continue;
        bp.fires++;

        JsonArray results;
        for (const BpMacroCmd &m : bp.macro)
            results.push_back(bpMacroResultEntry(m.cmd, bpRunMacroCommand(m.cmd, m.args)));

        JsonObject ev;
        ev.emplace("event", JsonValue::makeString("bp.cond"));
        ev.emplace("bp_id", JsonValue::makeNumber(double(bp.id)));
        char addr[24];
        snprintf(addr, sizeof(addr), "%04X:%04X", unsigned(bp.seg), unsigned(bp.off));
        ev.emplace("addr",   JsonValue::makeString(addr));
        ev.emplace("seg",    JsonValue::makeNumber(double(bp.seg)));
        ev.emplace("off",    JsonValue::makeNumber(double(bp.off)));
        ev.emplace("hits",   JsonValue::makeNumber(double(bp.hits)));
        ev.emplace("halted", JsonValue::makeBool(!bp.cont));
        char froms[24];
        snprintf(froms, sizeof(froms), "%04X:%04X", unsigned(from_cs), unsigned(from_ip));
        ev.emplace("from",    JsonValue::makeString(froms));
        ev.emplace("from_cs", JsonValue::makeNumber(double(from_cs)));
        ev.emplace("from_ip", JsonValue::makeNumber(double(from_ip)));
        if (!bp.macro.empty())
            ev.emplace("results", JsonValue::makeArray(std::move(results)));
        serverBroadcastLine(jsonEncode(JsonValue::makeObject(std::move(ev))));

        if (!bp.cont) halt = true;
    }
    return halt;
}

}  /* namespace agent */

/* ---- Public hot-path hooks (called from DEBUG_HeavyIsBreakpoint) ------ */

bool AGENT_CondBpActive(void) { return agent::g_condBpActive; }

bool AGENT_CondBpCheck(uint16_t cur_cs, uint16_t cur_off,
                       uint16_t from_cs, uint16_t from_ip) {
    return agent::condBpDispatch(cur_cs, cur_off, from_cs, from_ip);
}

#endif /* C_DEBUG */
