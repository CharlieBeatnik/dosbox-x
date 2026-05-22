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

/* Agent control channel — minimal hand-rolled JSON parser/encoder.
 * Subset: numbers (stored as double), strings with the standard escapes
 * including \uXXXX, true/false/null, arrays, objects. Sufficient for the
 * agent protocol; not a general-purpose JSON library. */

#include "config.h"

#if C_DEBUG

#include "agent_internal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace agent {

namespace {

class JsonParser {
public:
    JsonParser(const std::string &text, std::string *error)
        : t(text), p(0), err(error) {}

    bool parseValue(JsonValue &out) {
        skipWs();
        if (p >= t.size()) return fail("unexpected end of input");
        char c = t[p];
        if (c == '"')  return parseString(out);
        if (c == '{')  return parseObject(out);
        if (c == '[')  return parseArray(out);
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == 'n')  return parseNull(out);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
        return fail("unexpected character");
    }

    bool atEndIgnoringWs() {
        skipWs();
        return p >= t.size();
    }

private:
    void skipWs() {
        while (p < t.size()) {
            char c = t[p];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++p;
            else break;
        }
    }

    bool fail(const char *msg) {
        if (err) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s at offset %zu", msg, p);
            *err = buf;
        }
        return false;
    }

    bool parseNull(JsonValue &out) {
        if (t.compare(p, 4, "null") != 0) return fail("expected 'null'");
        p += 4;
        out = JsonValue::makeNull();
        return true;
    }

    bool parseBool(JsonValue &out) {
        if (t.compare(p, 4, "true") == 0)  { p += 4; out = JsonValue::makeBool(true);  return true; }
        if (t.compare(p, 5, "false") == 0) { p += 5; out = JsonValue::makeBool(false); return true; }
        return fail("expected boolean");
    }

    bool parseNumber(JsonValue &out) {
        size_t start = p;
        if (t[p] == '-') ++p;
        bool any = false;
        while (p < t.size() && t[p] >= '0' && t[p] <= '9') { ++p; any = true; }
        if (p < t.size() && t[p] == '.') {
            ++p;
            while (p < t.size() && t[p] >= '0' && t[p] <= '9') { ++p; any = true; }
        }
        if (p < t.size() && (t[p] == 'e' || t[p] == 'E')) {
            ++p;
            if (p < t.size() && (t[p] == '+' || t[p] == '-')) ++p;
            while (p < t.size() && t[p] >= '0' && t[p] <= '9') ++p;
        }
        if (!any) return fail("malformed number");
        out = JsonValue::makeNumber(strtod(t.c_str() + start, nullptr));
        return true;
    }

    bool parseHex4(uint32_t &cp) {
        cp = 0;
        if (p + 4 > t.size()) return false;
        for (int i = 0; i < 4; ++i) {
            char c = t[p++];
            uint32_t d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            cp = (cp << 4) | d;
        }
        return true;
    }

    static void appendUtf8(std::string &dst, uint32_t cp) {
        if (cp < 0x80) {
            dst.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            dst.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            dst.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            dst.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseString(JsonValue &out) {
        if (t[p] != '"') return fail("expected '\"'");
        ++p;
        std::string s;
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') { out = JsonValue::makeString(std::move(s)); return true; }
            if (c == '\\') {
                if (p >= t.size()) return fail("unterminated escape");
                char e = t[p++];
                switch (e) {
                    case '"':  s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/':  s.push_back('/'); break;
                    case 'b':  s.push_back('\b'); break;
                    case 'f':  s.push_back('\f'); break;
                    case 'n':  s.push_back('\n'); break;
                    case 'r':  s.push_back('\r'); break;
                    case 't':  s.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp;
                        if (!parseHex4(cp)) return fail("bad \\u escape");
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            /* high surrogate; expect a low surrogate next */
                            if (p + 2 > t.size() || t[p] != '\\' || t[p+1] != 'u')
                                return fail("missing low surrogate");
                            p += 2;
                            uint32_t lo;
                            if (!parseHex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                                return fail("bad low surrogate");
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                        }
                        appendUtf8(s, cp);
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return fail("unescaped control character in string");
            } else {
                s.push_back(c);
            }
        }
        return fail("unterminated string");
    }

    bool parseArray(JsonValue &out) {
        if (t[p] != '[') return fail("expected '['");
        ++p;
        JsonArray arr;
        skipWs();
        if (p < t.size() && t[p] == ']') { ++p; out = JsonValue::makeArray(std::move(arr)); return true; }
        for (;;) {
            JsonValue v;
            if (!parseValue(v)) return false;
            arr.push_back(std::move(v));
            skipWs();
            if (p >= t.size()) return fail("unterminated array");
            char c = t[p++];
            if (c == ',') { skipWs(); continue; }
            if (c == ']') { out = JsonValue::makeArray(std::move(arr)); return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseObject(JsonValue &out) {
        if (t[p] != '{') return fail("expected '{'");
        ++p;
        JsonObject obj;
        skipWs();
        if (p < t.size() && t[p] == '}') { ++p; out = JsonValue::makeObject(std::move(obj)); return true; }
        for (;;) {
            skipWs();
            JsonValue key;
            if (!parseString(key)) return false;
            skipWs();
            if (p >= t.size() || t[p] != ':') return fail("expected ':'");
            ++p;
            JsonValue v;
            if (!parseValue(v)) return false;
            obj.emplace(std::move(key.s), std::move(v));
            skipWs();
            if (p >= t.size()) return fail("unterminated object");
            char c = t[p++];
            if (c == ',') continue;
            if (c == '}') { out = JsonValue::makeObject(std::move(obj)); return true; }
            return fail("expected ',' or '}'");
        }
    }

    const std::string &t;
    size_t             p;
    std::string       *err;
};

void encodeString(std::string &out, const std::string &s) {
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out.append(buf);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void encodeNumber(std::string &out, double n) {
    if (std::isnan(n) || std::isinf(n)) { out.append("null"); return; }
    /* Prefer integer form when exact. The agent only sends integers in
     * practice (ids, segments, offsets, lengths, port numbers); printing
     * them without a decimal point makes the protocol nicer to consume. */
    if (n >= -9007199254740992.0 && n <= 9007199254740992.0) {
        double r = std::floor(n);
        if (r == n) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
            out.append(buf);
            return;
        }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", n);
    out.append(buf);
}

void encodeValue(std::string &out, const JsonValue &v) {
    switch (v.type) {
        case JsonType::Null:   out.append("null"); break;
        case JsonType::Bool:   out.append(v.b ? "true" : "false"); break;
        case JsonType::Number: encodeNumber(out, v.n); break;
        case JsonType::String: encodeString(out, v.s); break;
        case JsonType::Array:  {
            out.push_back('[');
            if (v.a) {
                bool first = true;
                for (const JsonValue &e : *v.a) {
                    if (!first) out.push_back(',');
                    encodeValue(out, e);
                    first = false;
                }
            }
            out.push_back(']');
            break;
        }
        case JsonType::Object: {
            out.push_back('{');
            if (v.o) {
                bool first = true;
                for (const auto &kv : *v.o) {
                    if (!first) out.push_back(',');
                    encodeString(out, kv.first);
                    out.push_back(':');
                    encodeValue(out, kv.second);
                    first = false;
                }
            }
            out.push_back('}');
            break;
        }
    }
}

}  /* anonymous namespace */

bool jsonParse(const std::string &text, JsonValue &out, std::string *error) {
    JsonParser p(text, error);
    if (!p.parseValue(out)) return false;
    if (!p.atEndIgnoringWs()) {
        if (error) *error = "trailing data after value";
        return false;
    }
    return true;
}

std::string jsonEncode(const JsonValue &v) {
    std::string s;
    encodeValue(s, v);
    return s;
}

}  /* namespace agent */

#endif /* C_DEBUG */
