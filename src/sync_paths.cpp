#include "sync_paths.h"
#include <cwchar>
#include <cwctype>

namespace syncpath {

std::wstring nfc(const std::wstring& s) {
    if (s.empty()) return s;
    bool ascii = true;
    for (wchar_t c : s) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return s;
    int n = NormalizeString(NormalizationC, s.c_str(), (int)s.size(), nullptr, 0);
    for (int tries = 0; n > 0 && tries < 4; ++tries) {
        std::wstring out(n, L'\0');
        int got = NormalizeString(NormalizationC, s.c_str(), (int)s.size(), out.data(), n);
        if (got > 0) { out.resize(got); return out; }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
        n = -got;
    }
    return s;
}

std::wstring case_key(const std::wstring& path) {
    if (path.empty()) return path;
    std::wstring out(path.size(), L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.c_str(), (int)path.size(), out.data(), (int)out.size(),
                          nullptr, nullptr, 0);
    if (n <= 0) return path;
    out.resize(n);
    return out;
}

bool windows_valid_name(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L"..") return false;
    for (wchar_t c : name)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) return false;
    if (name.back() == L'.' || name.back() == L' ') return false;
    std::wstring base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    std::wstring u = case_key(base);
    static const wchar_t* dev[] = { L"con", L"prn", L"aux", L"nul", L"conin$", L"conout$" };
    for (auto d : dev) if (u == d) return false;
    if (u.size() == 4 && (u.compare(0, 3, L"com") == 0 || u.compare(0, 3, L"lpt") == 0) &&
        ((u[3] >= L'1' && u[3] <= L'9') || u[3] == L'\x00b9' || u[3] == L'\x00b2' || u[3] == L'\x00b3'))
        return false;
    return true;
}

bool windows_valid_path(const std::wstring& rel) {
    size_t s = 0;
    while (s <= rel.size()) {
        size_t e = rel.find(L'/', s);
        if (e == std::wstring::npos) e = rel.size();
        if (!windows_valid_name(rel.substr(s, e - s))) return false;
        s = e + 1;
    }
    return true;
}

std::wstring to_local(const std::wstring& root, const std::wstring& rel) {
    std::wstring p = root;
    if (p.compare(0, 4, L"\\\\?\\") != 0) {
        if (p.compare(0, 2, L"\\\\") == 0) p = L"\\\\?\\UNC\\" + p.substr(2);
        else p = L"\\\\?\\" + p;
    }
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    if (!rel.empty()) {
        p += L'\\';
        for (wchar_t c : rel) p += c == L'/' ? L'\\' : c;
    }
    return p;
}

std::wstring conflict_name(const std::wstring& name, const std::wstring& device, const SYSTEMTIME& t) {
    size_t dot = name.find_last_of(L'.');
    if (dot == 0 || dot == std::wstring::npos) dot = name.size();
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04u-%02u-%02u %02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    std::wstring dev = device;
    for (wchar_t& c : dev) if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'-';
    return name.substr(0, dot) + L" (conflicted copy, " + dev + L", " + stamp + L")" + name.substr(dot);
}

bool is_conflict_copy(const std::wstring& name) { return name.find(L" (conflicted copy, ") != std::wstring::npos; }

// ------------------------------------------------------------------ ignore rules
static bool match_seg(const wchar_t* p, const wchar_t* s) {
    // '*' and '?' within one component ('/' never matched), '**' anywhere
    for (;;) {
        if (*p == 0) return *s == 0;
        if (p[0] == L'*' && p[1] == L'*') {
            p += 2;
            if (*p == L'/') ++p;
            for (const wchar_t* t = s;; ++t) {
                if (match_seg(p, t)) return true;
                if (!*t) return false;
            }
        }
        if (*p == L'*') {
            ++p;
            for (const wchar_t* t = s;; ++t) {
                if (match_seg(p, t)) return true;
                if (!*t || *t == L'/') return false;
            }
        }
        if (!*s) return false;
        if (*p == L'?') { if (*s == L'/') return false; }
        else if (towlower(*p) != towlower(*s)) return false;
        ++p;
        ++s;
    }
}

bool glob_match(const std::wstring& pat, const std::wstring& s) { return match_seg(pat.c_str(), s.c_str()); }

Ignore::Ignore() {
    add_lines(L"desktop.ini\nThumbs.db\nehthumbs.db\n~$*\n*.tmp\n.~sync*\n.DS_Store\n.syncignore.local\n$RECYCLE.BIN/\nSystem Volume Information/\n");
}

void Ignore::add_lines(const std::wstring& text) {
    size_t s = 0;
    while (s < text.size()) {
        size_t e = text.find_first_of(L"\r\n", s);
        if (e == std::wstring::npos) e = text.size();
        std::wstring l = text.substr(s, e - s);
        s = e + 1;
        while (!l.empty() && (l.back() == L' ' || l.back() == L'\t')) l.pop_back();
        while (!l.empty() && (l[0] == L' ' || l[0] == L'\t')) l.erase(0, 1);
        if (l.empty() || l[0] == L'#') continue;
        Rule r;
        if (l[0] == L'!') { r.neg = true; l.erase(0, 1); }
        if (!l.empty() && l.back() == L'/') { r.dirOnly = true; l.pop_back(); }
        if (!l.empty() && l[0] == L'/') { r.anchored = true; l.erase(0, 1); }
        else if (l.find(L'/') != std::wstring::npos) r.anchored = true;   // gitignore: a middle '/' anchors too
        if (l.empty()) continue;
        r.pat = l;
        m_rules.push_back(r);
    }
}

void Ignore::add_excluded_folder(const std::wstring& rel) { if (!rel.empty()) m_excluded.push_back(rel); }

bool Ignore::ignored(const std::wstring& rel, bool isDir) const {
    for (auto& x : m_excluded) {
        if (rel.size() >= x.size() && _wcsnicmp(rel.c_str(), x.c_str(), x.size()) == 0 &&
            (rel.size() == x.size() || rel[x.size()] == L'/'))
            return true;
    }
    bool out = false;
    std::wstring leaf = rel.substr(rel.find_last_of(L'/') == std::wstring::npos ? 0 : rel.find_last_of(L'/') + 1);
    for (auto& r : m_rules) {
        if (r.dirOnly && !isDir) continue;
        bool m = r.anchored ? glob_match(r.pat, rel) : glob_match(r.pat, leaf);
        if (m) out = !r.neg;
    }
    return out;
}

} // namespace syncpath
