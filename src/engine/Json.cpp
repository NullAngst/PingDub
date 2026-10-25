// SPDX-License-Identifier: GPL-3.0-or-later
#include "Json.h"

#include <cstdio>
#include <cstdlib>

namespace pd {

namespace {

struct Parser {
    explicit Parser(const std::string& text) : t(text) {}
    const std::string& t;
    size_t i = 0;
    std::string err;
    int depth = 0;

    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    bool fail(const char* m) {
        if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i);
        return false;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) {
            o += char(0xC0 | (cp >> 6));
            o += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += char(0xE0 | (cp >> 12));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        } else {
            o += char(0xF0 | (cp >> 18));
            o += char(0x80 | ((cp >> 12) & 0x3F));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        ++i;  // opening quote
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') {
                o += c;
                continue;
            }
            if (i >= t.size()) break;
            const char e = t[i++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    i += 2;
                    unsigned lo = 0;
                    if (!hex4(lo)) return fail("bad surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(JsonValue& v) {
        if (++depth > 64) return fail("nested too deep");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        const char c = t[i];
        bool ok = true;
        if (c == '{') {
            v.type = JsonValue::Type::Object;
            ++i;
            ws();
            if (i < t.size() && t[i] == '}') {
                ++i;
            } else {
                while (ok) {
                    ws();
                    if (i >= t.size() || t[i] != '"') {
                        ok = fail("expected a key");
                        break;
                    }
                    std::string k;
                    if (!string(k)) {
                        ok = false;
                        break;
                    }
                    ws();
                    if (i >= t.size() || t[i] != ':') {
                        ok = fail("expected ':'");
                        break;
                    }
                    ++i;
                    JsonValue child;
                    if (!value(child)) {
                        ok = false;
                        break;
                    }
                    v.o[k] = std::move(child);
                    ws();
                    if (i < t.size() && t[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < t.size() && t[i] == '}') {
                        ++i;
                        break;
                    }
                    ok = fail("expected ',' or '}'");
                }
            }
        } else if (c == '[') {
            v.type = JsonValue::Type::Array;
            ++i;
            ws();
            if (i < t.size() && t[i] == ']') {
                ++i;
            } else {
                while (ok) {
                    JsonValue child;
                    if (!value(child)) {
                        ok = false;
                        break;
                    }
                    v.a.push_back(std::move(child));
                    ws();
                    if (i < t.size() && t[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < t.size() && t[i] == ']') {
                        ++i;
                        break;
                    }
                    ok = fail("expected ',' or ']'");
                }
            }
        } else if (c == '"') {
            v.type = JsonValue::Type::String;
            ok = string(v.s);
        } else if (t.compare(i, 4, "true") == 0) {
            v.type = JsonValue::Type::Bool;
            v.b = true;
            i += 4;
        } else if (t.compare(i, 5, "false") == 0) {
            v.type = JsonValue::Type::Bool;
            i += 5;
        } else if (t.compare(i, 4, "null") == 0) {
            v.type = JsonValue::Type::Null;
            i += 4;
        } else {
            const char* start = t.c_str() + i;
            char* end = nullptr;
            v.n = std::strtod(start, &end);
            if (end == start) ok = fail("unexpected character");
            else {
                v.type = JsonValue::Type::Number;
                i += size_t(end - start);
            }
        }
        --depth;
        return ok;
    }
};

}  // namespace

const JsonValue& JsonValue::operator[](const std::string& k) const {
    static const JsonValue null;
    if (type != Type::Object) return null;
    auto it = o.find(k);
    return it == o.end() ? null : it->second;
}

const JsonValue& JsonValue::operator[](size_t idx) const {
    static const JsonValue null;
    return type == Type::Array && idx < a.size() ? a[idx] : null;
}

bool JsonValue::parse(const std::string& text, JsonValue& out, std::string& err) {
    Parser p(text);
    out = JsonValue{};
    if (!p.value(out)) {
        err = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        err = "trailing data after the JSON";
        return false;
    }
    return true;
}

bool readJsonFile(const std::string& path, JsonValue& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    return JsonValue::parse(text, out, err);
}

}  // namespace pd
