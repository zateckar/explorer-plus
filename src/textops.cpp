#include "textops.h"
#include <windows.h>
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace {

std::vector<std::wstring> split_lines(const std::wstring& s) {
    std::vector<std::wstring> lines;
    size_t a = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == L'\r' || s[i] == L'\n') {
            lines.emplace_back(s, a, i - a);
            if (i < s.size() && s[i] == L'\r' && i + 1 < s.size() && s[i + 1] == L'\n') ++i;
            a = i + 1;
        }
    }
    return lines;
}

std::wstring join_lines(const std::vector<std::wstring>& lines) {
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += L'\r';
        out += lines[i];
    }
    return out;
}

bool blank(wchar_t c) { return c == L' ' || c == L'\t'; }

// (one way out: the static library has no std::string move constructor to return by)
std::string utf8(const std::wstring& w) {
    std::string s;
    int n = w.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        s.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    }
    return s;
}

bool from_utf8(const std::string& s, std::wstring& w) {
    w.clear();
    if (s.empty()) return true;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return false;
    w.resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), w.data(), n);
    return true;
}

} // namespace

std::wstring line_op(LineOp op, const std::wstring& text) {
    std::vector<std::wstring> lines = split_lines(text);
    switch (op) {
        case LineOp::SortAsc:
        case LineOp::SortDesc: {
            // as Explorer sorts names: no case, numbers by their value (file2 before file10)
            auto less = [](const std::wstring& a, const std::wstring& b) {
                return CompareStringEx(LOCALE_NAME_INVARIANT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(), (int)a.size(),
                                       b.c_str(), (int)b.size(), nullptr, nullptr, 0) == CSTR_LESS_THAN;
            };
            if (op == LineOp::SortAsc) std::stable_sort(lines.begin(), lines.end(), less);
            else std::stable_sort(lines.begin(), lines.end(), [&](const std::wstring& a, const std::wstring& b) { return less(b, a); });
            break;
        }
        case LineOp::Unique: {
            std::unordered_set<std::wstring> seen;
            std::vector<std::wstring> kept;
            for (auto& l : lines)
                if (seen.insert(l).second) kept.push_back(l);
            lines.swap(kept);
            break;
        }
        case LineOp::RemoveEmpty:
            lines.erase(std::remove_if(lines.begin(), lines.end(),
                                       [](const std::wstring& l) { return std::all_of(l.begin(), l.end(), blank); }),
                        lines.end());
            break;
        case LineOp::TrimTrailing:
            for (auto& l : lines) {
                size_t e = l.size();
                while (e && blank(l[e - 1])) --e;
                l.resize(e);
            }
            break;
        case LineOp::Reverse:
            std::reverse(lines.begin(), lines.end());
            break;
        case LineOp::Join: {
            std::wstring out;
            for (auto& l : lines) {
                size_t a = 0, e = l.size();
                while (a < e && blank(l[a])) ++a;
                while (e > a && blank(l[e - 1])) --e;
                if (a == e) continue;
                if (!out.empty()) out += L' ';
                out.append(l, a, e - a);
            }
            return out;
        }
    }
    return join_lines(lines);
}

std::wstring change_case(CaseOp op, const std::wstring& s) {
    if (s.empty()) return s;
    DWORD flags = op == CaseOp::Upper ? LCMAP_UPPERCASE : op == CaseOp::Lower ? LCMAP_LOWERCASE : 0x00000300 /*LCMAP_TITLECASE*/;
    int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return s;
    std::wstring out((size_t)n, L'\0');
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr, 0);
    return out;
}

std::wstring base64_encode(const std::wstring& text) {
    static const char kAbc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b = utf8(text);
    std::wstring out;
    out.reserve((b.size() + 2) / 3 * 4);
    for (size_t i = 0; i < b.size(); i += 3) {
        unsigned v = (unsigned char)b[i] << 16;
        if (i + 1 < b.size()) v |= (unsigned char)b[i + 1] << 8;
        if (i + 2 < b.size()) v |= (unsigned char)b[i + 2];
        out += (wchar_t)kAbc[v >> 18 & 63];
        out += (wchar_t)kAbc[v >> 12 & 63];
        out += i + 1 < b.size() ? (wchar_t)kAbc[v >> 6 & 63] : L'=';
        out += i + 2 < b.size() ? (wchar_t)kAbc[v & 63] : L'=';
    }
    return out;
}

// (also the URL-safe alphabet, without padding, broken over lines)
bool base64_decode(const std::wstring& coded, std::wstring& text) {
    std::string bytes;
    unsigned v = 0;
    int bits = 0, pad = 0;
    for (wchar_t c : coded) {
        int d;
        if (c >= L'A' && c <= L'Z') d = c - L'A';
        else if (c >= L'a' && c <= L'z') d = c - L'a' + 26;
        else if (c >= L'0' && c <= L'9') d = c - L'0' + 52;
        else if (c == L'+' || c == L'-') d = 62;
        else if (c == L'/' || c == L'_') d = 63;
        else if (c == L'=') { ++pad; continue; }
        else if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        else return false;
        if (pad) return false;                      // (nothing after the padding)
        v = v << 6 | (unsigned)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes += (char)(v >> bits & 0xFF);
        }
    }
    if (bits >= 6 || pad > 2) return false;         // (a lone sixth of a byte left: not Base64)
    return from_utf8(bytes, text);
}

std::wstring url_encode(const std::wstring& text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : utf8(text)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (wchar_t)c;
        } else {
            out += L'%';
            out += (wchar_t)kHex[c >> 4];
            out += (wchar_t)kHex[c & 15];
        }
    }
    return out;
}

// (a % not followed by two hex digits stays as it is; + stays a plus)
bool url_decode(const std::wstring& coded, std::wstring& text) {
    auto hex = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : (c | 0x20) >= L'a' && (c | 0x20) <= L'f' ? (c | 0x20) - L'a' + 10 : -1; };
    std::string bytes;
    size_t plain = 0;                               // (the text since the last %XX, as UTF-8 in one go)
    for (size_t i = 0; i < coded.size(); ++i) {
        if (coded[i] == L'%' && i + 2 < coded.size() && hex(coded[i + 1]) >= 0 && hex(coded[i + 2]) >= 0) {
            bytes += utf8(coded.substr(plain, i - plain));
            bytes += (char)(hex(coded[i + 1]) << 4 | hex(coded[i + 2]));
            i += 2;
            plain = i + 1;
        }
    }
    bytes += utf8(coded.substr(std::min(plain, coded.size())));
    return from_utf8(bytes, text);
}
