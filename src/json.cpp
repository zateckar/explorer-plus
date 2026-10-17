#include "json.h"
#include <cstdlib>

namespace json {

static const Value kNull;
static const std::string kEmpty;

const Value& Value::operator[](std::string_view key) const {
    if (type != Object) return kNull;
    for (auto& [k, v] : members) if (k == key) return v;
    return kNull;
}
const Value& Value::operator[](size_t i) const { return type == Array && i < items.size() ? items[i] : kNull; }
const std::string& Value::str() const { return type == String ? text : kEmpty; }
uint64_t Value::u64() const {
    if (type != Number || text.empty() || text[0] == '-') return 0;
    for (char c : text) if (c < '0' || c > '9') return 0;
    return strtoull(text.c_str(), nullptr, 10);
}

namespace {

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;
    int depth = 0;

    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(i); return false; }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool lit(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (s.substr(i, n) != w) return fail("unexpected text");
        i += n;
        return true;
    }
    static void utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(uint32_t& v) {
        if (i + 4 > s.size()) return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& o) {
        if (i >= s.size() || s[i] != '"') return fail("expected a string");
        ++i;
        for (;;) {
            if (i >= s.size()) return fail("unterminated string");
            char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return fail("control character in a string");
            if (c != '\\') { o += c; continue; }
            if (i >= s.size()) return fail("unterminated escape");
            char e = s[i++];
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
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {               // a surrogate pair
                        uint32_t lo;
                        if (s.substr(i, 2) != "\\u") return fail("lone surrogate");
                        i += 2;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo >= 0xE000) return fail("lone surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) return fail("lone surrogate");
                    utf8(o, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }
    bool number(Value& v) {
        size_t a = i;
        if (i < s.size() && s[i] == '-') ++i;
        size_t d = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == d) return fail("bad number");
        if (i < s.size() && s[i] == '.') { ++i; size_t f = i; while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; if (i == f) return fail("bad number"); }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            size_t f = i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            if (i == f) return fail("bad number");
        }
        v.type = Value::Number;
        v.text = std::string(s.substr(a, i - a));
        return true;
    }
    bool value(Value& v) {
        ws();
        if (i >= s.size()) return fail("unexpected end");
        if (++depth > 200) return fail("nested too deeply");
        bool ok = true;
        char c = s[i];
        if (c == '{') {
            v.type = Value::Object;
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') ++i;
            else for (;;) {
                ws();
                std::pair<std::string, Value> m;
                if (!string(m.first)) { ok = false; break; }
                ws();
                if (i >= s.size() || s[i] != ':') { ok = fail("expected ':'"); break; }
                ++i;
                if (!value(m.second)) { ok = false; break; }
                v.members.push_back(std::move(m));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                ok = fail("expected ',' or '}'");
                break;
            }
        } else if (c == '[') {
            v.type = Value::Array;
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') ++i;
            else for (;;) {
                Value x;
                if (!value(x)) { ok = false; break; }
                v.items.push_back(std::move(x));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                ok = fail("expected ',' or ']'");
                break;
            }
        } else if (c == '"') { v.type = Value::String; ok = string(v.text); }
        else if (c == 't') { v.type = Value::Bool; v.b = true; ok = lit("true"); }
        else if (c == 'f') { v.type = Value::Bool; ok = lit("false"); }
        else if (c == 'n') { v.type = Value::Null; ok = lit("null"); }
        else ok = number(v);
        --depth;
        return ok;
    }
};

} // namespace

bool parse(std::string_view text, Value& out, std::string* error) {
    out = Value();
    Parser p{ text };
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) p.i = 3;
    bool ok = p.value(out);
    if (ok) { p.ws(); if (p.i != text.size()) ok = p.fail("text after the value"); }
    if (!ok) { out = Value(); if (error) *error = p.err; }
    return ok;
}

std::string quote(std::string_view u) {
    static const char* hx = "0123456789abcdef";
    std::string o = "\"";
    for (unsigned char c : u) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { o += "\\u00"; o += hx[c >> 4]; o += hx[c & 15]; }
                else o += (char)c;
        }
    }
    o += '"';
    return o;
}

} // namespace json
