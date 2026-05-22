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

/* Agent control channel — JSON key name <-> KBD_KEYS table.
 *
 * Names are lowercase. Every value of KBD_KEYS *except* KBD_NONE / KBD_LAST
 * has an entry; the build-time table is verified by a unit test that
 * iterates the enum range. */

#include "config.h"

#if C_DEBUG

#include "agent_internal.h"
#include "keyboard.h"

#include <unordered_map>

/* The paste driver lives in src/misc/clipboard.cpp; we just append to its
 * input buffer and let the existing 1-char-per-paste_speed-tick pump in
 * src/gui/sdlmain.cpp deliver the keystrokes. */
extern std::string strPasteBuffer;

namespace agent {

namespace {

const std::unordered_map<std::string, KBD_KEYS> &keyTable() {
    static const std::unordered_map<std::string, KBD_KEYS> t = {
        /* Digits */
        {"1", KBD_1}, {"2", KBD_2}, {"3", KBD_3}, {"4", KBD_4}, {"5", KBD_5},
        {"6", KBD_6}, {"7", KBD_7}, {"8", KBD_8}, {"9", KBD_9}, {"0", KBD_0},

        /* Letters */
        {"q", KBD_q}, {"w", KBD_w}, {"e", KBD_e}, {"r", KBD_r}, {"t", KBD_t},
        {"y", KBD_y}, {"u", KBD_u}, {"i", KBD_i}, {"o", KBD_o}, {"p", KBD_p},
        {"a", KBD_a}, {"s", KBD_s}, {"d", KBD_d}, {"f", KBD_f}, {"g", KBD_g},
        {"h", KBD_h}, {"j", KBD_j}, {"k", KBD_k}, {"l", KBD_l}, {"z", KBD_z},
        {"x", KBD_x}, {"c", KBD_c}, {"v", KBD_v}, {"b", KBD_b}, {"n", KBD_n},
        {"m", KBD_m},

        /* Function keys F1-F24 */
        {"f1",  KBD_f1},  {"f2",  KBD_f2},  {"f3",  KBD_f3},  {"f4",  KBD_f4},
        {"f5",  KBD_f5},  {"f6",  KBD_f6},  {"f7",  KBD_f7},  {"f8",  KBD_f8},
        {"f9",  KBD_f9},  {"f10", KBD_f10}, {"f11", KBD_f11}, {"f12", KBD_f12},
        {"f13", KBD_f13}, {"f14", KBD_f14}, {"f15", KBD_f15}, {"f16", KBD_f16},
        {"f17", KBD_f17}, {"f18", KBD_f18}, {"f19", KBD_f19}, {"f20", KBD_f20},
        {"f21", KBD_f21}, {"f22", KBD_f22}, {"f23", KBD_f23}, {"f24", KBD_f24},

        /* Control / navigation */
        {"esc",       KBD_esc},
        {"tab",       KBD_tab},
        {"backspace", KBD_backspace},
        {"enter",     KBD_enter},
        {"space",     KBD_space},
        {"leftalt",   KBD_leftalt},
        {"rightalt",  KBD_rightalt},
        {"leftctrl",  KBD_leftctrl},
        {"rightctrl", KBD_rightctrl},
        {"leftshift", KBD_leftshift},
        {"rightshift",KBD_rightshift},
        {"capslock",  KBD_capslock},
        {"scrolllock",KBD_scrolllock},
        {"numlock",   KBD_numlock},

        /* Punctuation */
        {"grave",        KBD_grave},
        {"minus",        KBD_minus},
        {"equals",       KBD_equals},
        {"backslash",    KBD_backslash},
        {"leftbracket",  KBD_leftbracket},
        {"rightbracket", KBD_rightbracket},
        {"semicolon",    KBD_semicolon},
        {"quote",        KBD_quote},
        {"period",       KBD_period},
        {"comma",        KBD_comma},
        {"slash",        KBD_slash},
        {"extra_lt_gt",  KBD_extra_lt_gt},

        {"printscreen",  KBD_printscreen},
        {"pause",        KBD_pause},

        /* Editing block */
        {"insert",       KBD_insert},
        {"home",         KBD_home},
        {"pageup",       KBD_pageup},
        {"delete",       KBD_delete},
        {"end",          KBD_end},
        {"pagedown",     KBD_pagedown},

        /* Arrows */
        {"left",         KBD_left},
        {"up",           KBD_up},
        {"down",         KBD_down},
        {"right",        KBD_right},

        /* Numeric keypad */
        {"kp1",          KBD_kp1}, {"kp2", KBD_kp2}, {"kp3", KBD_kp3},
        {"kp4",          KBD_kp4}, {"kp5", KBD_kp5}, {"kp6", KBD_kp6},
        {"kp7",          KBD_kp7}, {"kp8", KBD_kp8}, {"kp9", KBD_kp9},
        {"kp0",          KBD_kp0},
        {"kpdivide",     KBD_kpdivide},
        {"kpmultiply",   KBD_kpmultiply},
        {"kpminus",      KBD_kpminus},
        {"kpplus",       KBD_kpplus},
        {"kpenter",      KBD_kpenter},
        {"kpperiod",     KBD_kpperiod},
        {"kpequals",     KBD_kpequals},
        {"kpcomma",      KBD_kpcomma},

        /* Windows keys */
        {"lwindows",     KBD_lwindows},
        {"rwindows",     KBD_rwindows},
        {"rwinmenu",     KBD_rwinmenu},

        /* Japanese */
        {"jp_hankaku",   KBD_jp_hankaku},
        {"jp_muhenkan",  KBD_jp_muhenkan},
        {"jp_henkan",    KBD_jp_henkan},
        {"jp_hiragana",  KBD_jp_hiragana},
        {"yen",          KBD_yen},
        {"underscore",   KBD_underscore},
        {"ax",           KBD_ax},
        {"conv",         KBD_conv},
        {"nconv",        KBD_nconv},
        {"jp_yen",       KBD_jp_yen},
        {"jp_backslash", KBD_jp_backslash},
        {"colon",        KBD_colon},
        {"caret",        KBD_caret},
        {"atsign",       KBD_atsign},
        {"jp_ro",        KBD_jp_ro},
        {"help",         KBD_help},
        {"kana",         KBD_kana},
        {"nfer",         KBD_nfer},
        {"xfer",         KBD_xfer},

        /* Korean */
        {"kor_hancha",   KBD_kor_hancha},
        {"kor_hanyong",  KBD_kor_hanyong},

        /* Misc PC-98 / other vendor */
        {"stop",         KBD_stop},
        {"copy",         KBD_copy},
        {"vf1",          KBD_vf1}, {"vf2", KBD_vf2}, {"vf3", KBD_vf3},
        {"vf4",          KBD_vf4}, {"vf5", KBD_vf5},
    };
    return t;
}

}  /* anonymous namespace */

bool keyboardNameToKey(const std::string &name, KBD_KEYS &out) {
    const auto &t = keyTable();
    auto it = t.find(name);
    if (it == t.end()) return false;
    out = it->second;
    return true;
}

size_t keyboardTableSize() {
    return keyTable().size();
}

/* ---- Command handlers -------------------------------------------------- */

namespace {

/* Pull a string-typed `key` argument and look it up in the table. */
bool resolveKey(const JsonValue &args, KBD_KEYS &out, std::string &errMsg) {
    const JsonValue *k = args.get("key");
    if (!k || !k->isString()) {
        errMsg = "expected {\"key\":\"...\"}";
        return false;
    }
    if (!keyboardNameToKey(k->s, out)) {
        errMsg = std::string("unknown key: ") + k->s;
        return false;
    }
    return true;
}

}  /* anonymous namespace */

JsonValue handleKeyboardType(double id, const JsonValue &args) {
    const JsonValue *text = args.get("text");
    if (!text || !text->isString())
        return makeReplyError(id, "bad_args", "expected {\"text\":\"...\"}");

    strPasteBuffer.append(text->s);

    JsonObject r;
    r.emplace("queued", JsonValue::makeNumber(static_cast<double>(text->s.size())));
    return makeReplyOk(id, std::move(r));
}

JsonValue handleKeyboardPress(double id, const JsonValue &args) {
    KBD_KEYS k;
    std::string err;
    if (!resolveKey(args, k, err)) return makeReplyError(id, "bad_args", err);
    KEYBOARD_AddKey(k, true);
    return makeReplyOk(id, JsonObject{});
}

JsonValue handleKeyboardRelease(double id, const JsonValue &args) {
    KBD_KEYS k;
    std::string err;
    if (!resolveKey(args, k, err)) return makeReplyError(id, "bad_args", err);
    KEYBOARD_AddKey(k, false);
    return makeReplyOk(id, JsonObject{});
}

JsonValue handleKeyboardTap(double id, const JsonValue &args) {
    KBD_KEYS k;
    std::string err;
    if (!resolveKey(args, k, err)) return makeReplyError(id, "bad_args", err);
    KEYBOARD_AddKey(k, true);
    KEYBOARD_AddKey(k, false);
    return makeReplyOk(id, JsonObject{});
}

}  /* namespace agent */

#endif /* C_DEBUG */
