#include "textfile.h"
#include "ini.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <cwchar>

namespace {

std::wstring from_codepage(UINT cp, const char* p, size_t n, bool strict, bool* ok = nullptr) {
    if (ok) *ok = true;
    if (!n) return {};
    DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    std::wstring w(n, L'\0');                   // (a byte makes a character at most: converted in one go)
    int len = MultiByteToWideChar(cp, flags, p, (int)n, w.data(), (int)n);
    if (len <= 0) {
        if (ok) *ok = false;
        w.clear();
    } else {
        w.resize((size_t)len);
    }
    return w;
}

std::wstring from_utf16(const unsigned char* p, size_t n, bool bigEndian) {
    std::wstring w(n / 2, L'\0');
    for (size_t i = 0; i < w.size(); ++i)
        w[i] = bigEndian ? (wchar_t)(p[2 * i] << 8 | p[2 * i + 1]) : (wchar_t)(p[2 * i] | p[2 * i + 1] << 8);
    return w;
}

} // namespace

const wchar_t* encoding_name(TextEncoding e) {
    switch (e) {
        case TextEncoding::Utf8: return L"UTF-8";
        case TextEncoding::Utf8Bom: return L"UTF-8 with BOM";
        case TextEncoding::Utf16Le: return L"UTF-16 LE";
        case TextEncoding::Utf16Be: return L"UTF-16 BE";
        case TextEncoding::Ansi: return L"ANSI";
        default: return L"";
    }
}

const wchar_t* line_ending_name(LineEnding e) {
    switch (e) {
        case LineEnding::CrLf: return L"Windows (CRLF)";
        case LineEnding::Lf: return L"Unix (LF)";
        case LineEnding::Cr: return L"Macintosh (CR)";
        default: return L"";
    }
}

namespace {

struct LineEnds {
    size_t crs = 0, lfs = 0, pairs = 0;     // lone CRs, lone LFs, CRLFs
};

LineEnds count_line_ends(const std::wstring& s) {
    LineEnds c;
    const wchar_t* p = s.data();
    for (size_t i = 0, n = s.size(); i < n; ++i) {
        if (p[i] == L'\r') {
            if (i + 1 < n && p[i + 1] == L'\n') { ++c.pairs; ++i; }
            else ++c.crs;
        } else if (p[i] == L'\n') {
            ++c.lfs;
        }
    }
    return c;
}

// those all nl already (one or two characters)
bool all_line_ends(const LineEnds& c, const wchar_t* nl) {
    return nl[1] ? !c.crs && !c.lfs : !c.pairs && (nl[0] == L'\r' ? !c.lfs : !c.crs);
}

// s's line endings made nl: in place when that makes it no longer (CRLF to CR, say), the lines
// moved whole (a big file's millions of them)
void set_line_ends(std::wstring& s, const LineEnds& c, const wchar_t* nl) {
    if (all_line_ends(c, nl)) return;
    size_t nlLen = nl[1] ? 2 : 1, n = s.size(), breaks = c.crs + c.lfs + c.pairs;
    size_t size = n - breaks - c.pairs + breaks * nlLen;
    std::wstring grown;
    if (size > n) grown.assign(size, L'\0');
    const wchar_t* p = s.data();
    wchar_t* o = size > n ? grown.data() : s.data();
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && p[j] != L'\r' && p[j] != L'\n') ++j;
        memmove(o, p + i, (j - i) * sizeof(wchar_t));
        o += j - i;
        if (j == n) break;
        if (p[j] == L'\r' && j + 1 < n && p[j + 1] == L'\n') ++j;
        *o++ = nl[0];
        if (nlLen == 2) *o++ = nl[1];
        i = j + 1;
    }
    if (size > n) s.swap(grown);
    else s.resize(size);
}

} // namespace

TextFile text_decode(const std::string& bytes) {
    TextFile f;
    const auto* p = (const unsigned char*)bytes.data();
    size_t n = bytes.size();
    std::wstring w;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        f.enc = TextEncoding::Utf8Bom;
        w = from_codepage(CP_UTF8, bytes.data() + 3, n - 3, false);
    } else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        f.enc = TextEncoding::Utf16Le;
        w = from_utf16(p + 2, n - 2, false);
    } else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        f.enc = TextEncoding::Utf16Be;
        w = from_utf16(p + 2, n - 2, true);
    } else {
        // no BOM: UTF-16 (of mostly Latin text) has a zero in every other byte
        size_t probe = std::min<size_t>(n & ~(size_t)1, 4096), zeroEven = 0, zeroOdd = 0;
        for (size_t i = 0; i < probe; ++i)
            if (!p[i]) ++(i & 1 ? zeroOdd : zeroEven);
        size_t half = probe / 2;
        if (probe >= 4 && zeroOdd * 10 >= half * 6 && zeroEven * 20 <= half) {
            f.enc = TextEncoding::Utf16Le;
            w = from_utf16(p, n, false);
        } else if (probe >= 4 && zeroEven * 10 >= half * 6 && zeroOdd * 20 <= half) {
            f.enc = TextEncoding::Utf16Be;
            w = from_utf16(p, n, true);
        } else {
            bool utf8 = true;
            w = from_codepage(CP_UTF8, bytes.data(), n, true, &utf8);
            if (!utf8) {
                f.enc = TextEncoding::Ansi;
                w = from_codepage(CP_ACP, bytes.data(), n, false);
            }
        }
    }
    f.binary = w.find(L'\0') != std::wstring::npos;
    // (the most used line ending; CRLF when none)
    LineEnds c = count_line_ends(w);
    f.eol = c.lfs > c.pairs && c.lfs >= c.crs ? LineEnding::Lf : c.crs > c.pairs && c.crs > c.lfs ? LineEnding::Cr : LineEnding::CrLf;
    set_line_ends(w, c, L"\r");
    f.text.swap(w);
    return f;
}

std::string text_encode(const std::wstring& text, TextEncoding enc, LineEnding eol, bool* lossy) {
    if (lossy) *lossy = false;
    const wchar_t* nl = eol == LineEnding::Lf ? L"\n" : eol == LineEnding::Cr ? L"\r" : L"\r\n";
    LineEnds c = count_line_ends(text);
    std::wstring converted;
    if (!all_line_ends(c, nl)) {
        converted = text;
        set_line_ends(converted, c, nl);
    }
    const std::wstring& t = all_line_ends(c, nl) ? text : converted;
    std::string out;
    switch (enc) {
        case TextEncoding::Utf16Le:
        case TextEncoding::Utf16Be: {
            bool be = enc == TextEncoding::Utf16Be;
            out.assign(2 + t.size() * 2, '\0');
            char* o = out.data();
            *o++ = be ? '\xFE' : '\xFF';
            *o++ = be ? '\xFF' : '\xFE';
            if (!be) {
                memcpy(o, t.data(), t.size() * 2);  // (as wchar_t is held)
                return out;
            }
            for (wchar_t c : t) {
                *o++ = (char)(c >> 8);
                *o++ = (char)(c & 0xFF);
            }
            return out;
        }
        case TextEncoding::Ansi: {
            if (t.empty()) return out;
            BOOL used = FALSE;
            int len = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, t.data(), (int)t.size(), nullptr, 0, nullptr, &used);
            out.resize(len > 0 ? (size_t)len : 0);
            used = FALSE;
            if (len > 0) WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, t.data(), (int)t.size(), out.data(), len, nullptr, &used);
            if (lossy) *lossy = used != FALSE;
            return out;
        }
        default: {
            if (enc == TextEncoding::Utf8Bom) out = "\xEF\xBB\xBF";
            if (t.empty()) return out;
            int len = WideCharToMultiByte(CP_UTF8, 0, t.data(), (int)t.size(), nullptr, 0, nullptr, nullptr);
            size_t at = out.size();
            out.resize(at + (len > 0 ? (size_t)len : 0));
            if (len > 0) WideCharToMultiByte(CP_UTF8, 0, t.data(), (int)t.size(), out.data() + at, len, nullptr, nullptr);
            return out;
        }
    }
}

// ------------------------------------------------------------------
// the remembered tabs: an INI document ([notepad], then [tab1], [tab2], ...)
// ------------------------------------------------------------------
std::wstring note_session_serialize(const NoteSession& s) {
    IniDoc d;
    const wchar_t* m = L"notepad";
    d.set(m, L"active", std::to_wstring(s.active));
    d.set(m, L"wrap", s.wrap ? L"1" : L"0");
    d.set(m, L"statusBar", s.statusBar ? L"1" : L"0");
    d.set(m, L"zoom", std::to_wstring(s.zoom));
    if (s.w > 0 && s.h > 0) {
        wchar_t b[96];
        swprintf(b, 96, L"%d,%d,%d,%d,%d", s.x, s.y, s.w, s.h, s.maximized ? 1 : 0);
        d.set(m, L"window", b);
    }
    for (size_t i = 0; i < s.tabs.size(); ++i) {
        const NoteTabState& t = s.tabs[i];
        std::wstring sec = L"tab" + std::to_wstring(i + 1);
        if (!t.path.empty()) d.set(sec, L"path", t.path);
        if (!t.cache.empty()) d.set(sec, L"cache", t.cache);
        if (t.untitled) d.set(sec, L"untitled", std::to_wstring(t.untitled));
        d.set(sec, L"dirty", t.dirty ? L"1" : L"0");
        d.set(sec, L"encoding", std::to_wstring((int)t.enc));
        d.set(sec, L"lineEnding", std::to_wstring((int)t.eol));
        d.set(sec, L"selection", std::to_wstring(t.selStart) + L"," + std::to_wstring(t.selEnd));
        d.set(sec, L"top", std::to_wstring(t.top));
        if (t.fileTime) {
            wchar_t b[24];
            swprintf(b, 24, L"%llx", t.fileTime);
            d.set(sec, L"fileTime", b);
        }
        if (!t.syntax.empty()) d.set(sec, L"syntax", t.syntax);
    }
    return ini_serialize(d);
}

NoteSession note_session_parse(const std::wstring& text) {
    NoteSession s;
    IniDoc d = ini_parse(text);
    auto num = [&](const std::wstring& sec, const wchar_t* key, int def) {
        const std::wstring* v = d.get(sec, key);
        return v && !v->empty() ? (int)wcstol(v->c_str(), nullptr, 10) : def;
    };
    auto str = [&](const std::wstring& sec, const wchar_t* key) {
        const std::wstring* v = d.get(sec, key);
        return v ? *v : std::wstring();
    };
    s.wrap = num(L"notepad", L"wrap", 1) != 0;
    s.statusBar = num(L"notepad", L"statusBar", 1) != 0;
    s.zoom = std::clamp(num(L"notepad", L"zoom", 100), 10, 500);
    int x, y, w, h, mx;
    if (swscanf(str(L"notepad", L"window").c_str(), L"%d,%d,%d,%d,%d", &x, &y, &w, &h, &mx) == 5 && w > 0 && h > 0) {
        s.x = x; s.y = y; s.w = w; s.h = h;
        s.maximized = mx != 0;
    }
    for (int i = 1; i < 10000; ++i) {
        std::wstring sec = L"tab" + std::to_wstring(i);
        if (!d.find(sec)) break;
        NoteTabState t;
        t.path = str(sec, L"path");
        t.cache = str(sec, L"cache");
        t.untitled = std::max(0, num(sec, L"untitled", 0));
        t.dirty = num(sec, L"dirty", 0) != 0;
        t.enc = (TextEncoding)std::clamp(num(sec, L"encoding", 0), 0, (int)TextEncoding::Count - 1);
        t.eol = (LineEnding)std::clamp(num(sec, L"lineEnding", 0), 0, (int)LineEnding::Count - 1);
        int a = 0, b = 0;
        if (swscanf(str(sec, L"selection").c_str(), L"%d,%d", &a, &b) == 2 && a >= 0 && b >= 0) {
            t.selStart = a;
            t.selEnd = b;
        }
        t.top = std::max(0, num(sec, L"top", 0));
        t.fileTime = wcstoull(str(sec, L"fileTime").c_str(), nullptr, 16);
        t.syntax = str(sec, L"syntax");
        // (a tab with neither a file nor kept text has nothing to show)
        if (t.path.empty() && t.cache.empty() && !t.untitled) continue;
        s.tabs.push_back(std::move(t));
    }
    s.active = s.tabs.empty() ? 0 : std::clamp(num(L"notepad", L"active", 0), 0, (int)s.tabs.size() - 1);
    return s;
}
