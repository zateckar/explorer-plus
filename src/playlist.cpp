#include "playlist.h"
#include "ini.h"
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <map>
#include <mutex>
#include <numeric>

int PlayQueue::order_pos(int index) const {
    auto it = std::find(m_order.begin(), m_order.end(), index);
    return it == m_order.end() ? -1 : (int)(it - m_order.begin());
}

int PlayQueue::index_of(unsigned id) const {
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].id == id) return (int)i;
    return -1;
}

int PlayQueue::first() const {
    return m_order.empty() ? -1 : m_order[0];
}

void PlayQueue::set_current(int i) {
    m_current = i >= 0 && i < size() ? i : -1;
}

void PlayQueue::replace(const std::vector<std::wstring>& paths) {
    m_entries.clear();
    for (auto& p : paths) {
        QueueEntry e;
        e.path = p;
        e.id = m_nextId++;
        m_entries.push_back(std::move(e));
    }
    m_order.resize(m_entries.size());
    std::iota(m_order.begin(), m_order.end(), 0);
    if (m_shuffle) std::shuffle(m_order.begin(), m_order.end(), m_rng);
    m_current = m_order.empty() ? -1 : m_order[0];
}

int PlayQueue::append(const std::vector<std::wstring>& paths) {
    int first = size();
    for (auto& p : paths) {
        int i = size();
        QueueEntry e;
        e.path = p;
        e.id = m_nextId++;
        m_entries.push_back(std::move(e));
        if (!m_shuffle) { m_order.push_back(i); continue; }
        // somewhere among the entries still to come
        int from = m_current >= 0 ? order_pos(m_current) + 1 : 0;
        int at = std::uniform_int_distribution<int>(from, (int)m_order.size())(m_rng);
        m_order.insert(m_order.begin() + at, i);
    }
    return first;
}

bool PlayQueue::remove(std::vector<int> indices) {
    std::vector<bool> gone(m_entries.size(), false);
    for (int i : indices)
        if (i >= 0 && i < size()) gone[(size_t)i] = true;
    bool hit = m_current >= 0 && gone[(size_t)m_current];
    int cur = m_current;
    if (hit) {
        cur = -1;
        for (size_t p = (size_t)order_pos(m_current) + 1; p < m_order.size(); ++p)
            if (!gone[(size_t)m_order[p]]) { cur = m_order[p]; break; }
    }
    std::vector<int> remap(m_entries.size(), -1);
    std::vector<QueueEntry> kept;
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (!gone[i]) { remap[i] = (int)kept.size(); kept.push_back(std::move(m_entries[i])); }
    std::vector<int> order;
    for (int i : m_order)
        if (!gone[(size_t)i]) order.push_back(remap[(size_t)i]);
    m_entries = std::move(kept);
    m_order = std::move(order);
    m_current = cur >= 0 ? remap[(size_t)cur] : -1;
    return hit;
}

void PlayQueue::clear() {
    m_entries.clear();
    m_order.clear();
    m_current = -1;
}

int PlayQueue::next(bool automatic) const {
    if (m_order.empty()) return -1;
    if (m_current < 0) return m_order[0];
    if (automatic && repeat == Repeat::One) return m_current;
    int p = order_pos(m_current);
    if (p + 1 < (int)m_order.size()) return m_order[(size_t)p + 1];
    return repeat != Repeat::Off ? m_order[0] : -1;
}

int PlayQueue::previous() const {
    if (m_current < 0 || m_order.empty()) return -1;
    int p = order_pos(m_current);
    if (p > 0) return m_order[(size_t)p - 1];
    return repeat != Repeat::Off ? m_order.back() : -1;
}

void PlayQueue::set_shuffle(bool on) {
    if (on == m_shuffle) return;
    m_shuffle = on;
    m_order.resize(m_entries.size());
    std::iota(m_order.begin(), m_order.end(), 0);
    if (!on) return;
    // the current entry first, the rest in a random order after it
    if (m_current >= 0) std::swap(m_order[0], m_order[(size_t)m_current]);
    std::shuffle(m_order.begin() + (m_current >= 0 ? 1 : 0), m_order.end(), m_rng);
}

std::wstring fmt_duration(long long hns) {
    if (hns < 0) return {};
    long long s = hns / 10000000;
    WCHAR b[32];
    if (s >= 3600) swprintf_s(b, L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else swprintf_s(b, L"%lld:%02lld", s / 60, s % 60);
    return b;
}

bool media_extension_excluded(const std::wstring& ext) {
    std::wstring e = ext;
    for (auto& c : e) c = (wchar_t)towlower(c);
    static const wchar_t* const kNot[] = {
        L".m3u", L".m3u8", L".pls", L".wpl", L".zpl", L".asx", L".wax", L".wvx",     // playlists
        L".mid", L".midi", L".rmi", L".kar",                                        // MIDI
        L".cda",                                                                    // CD track stubs
    };
    for (auto k : kNot)
        if (e == k) return true;
    return false;
}

// 0: not media, 1: audio, 2: video, by the extension's perceived type (asked for every file
// of a folder walk, on any thread: the answer is kept per extension)
static int media_kind(const std::wstring& path) {
    const wchar_t* dot = PathFindExtensionW(path.c_str());
    if (!*dot) return 0;
    std::wstring ext = dot;
    for (auto& c : ext) c = (wchar_t)towlower(c);
    static std::mutex mx;
    static std::map<std::wstring, int> known;
    {
        std::lock_guard<std::mutex> lk(mx);
        if (auto it = known.find(ext); it != known.end()) return it->second;
    }
    int kind = 0;
    if (!media_extension_excluded(ext)) {
        PERCEIVED type = PERCEIVED_TYPE_UNSPECIFIED;
        PERCEIVEDFLAG flags = 0;
        if (SUCCEEDED(AssocGetPerceivedType(ext.c_str(), &type, &flags, nullptr)))
            kind = type == PERCEIVED_TYPE_AUDIO ? 1 : type == PERCEIVED_TYPE_VIDEO ? 2 : 0;
    }
    std::lock_guard<std::mutex> lk(mx);
    known[ext] = kind;
    return kind;
}

bool media_file_playable(const std::wstring& path) { return media_kind(path) != 0; }
bool media_file_video(const std::wstring& path) { return media_kind(path) == 2; }

std::wstring pack_paths(const std::vector<std::wstring>& paths) {
    std::wstring out;
    for (auto& p : paths) {
        if (p.empty()) continue;
        out += p;
        out += L'\0';
    }
    return out;
}

std::vector<std::wstring> unpack_paths(const wchar_t* data, size_t chars) {
    std::vector<std::wstring> out;
    if (!data) return out;
    size_t start = 0;
    for (size_t i = 0; i <= chars; ++i) {
        if (i < chars && data[i] != L'\0') continue;
        if (i > start) out.emplace_back(data + start, i - start);
        start = i + 1;
    }
    return out;
}

// ------------------------------------------------------------------
// M3U / M3U8 playlists
// ------------------------------------------------------------------
bool playlist_file(const std::wstring& path) {
    const wchar_t* dot = PathFindExtensionW(path.c_str());
    return lstrcmpiW(dot, L".m3u") == 0 || lstrcmpiW(dot, L".m3u8") == 0;
}

static std::wstring trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// "C:" or "\\server\share" of an absolute path
static std::wstring path_root(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return p.substr(0, 2);
    if (p.rfind(L"\\\\", 0) == 0) {
        size_t server = p.find(L'\\', 2);
        if (server == std::wstring::npos) return p;
        size_t share = p.find(L'\\', server + 1);
        return share == std::wstring::npos ? p : p.substr(0, share);
    }
    return {};
}

// "." and ".." taken out (never above the root)
static std::wstring normalize(const std::wstring& p) {
    std::wstring root = path_root(p);
    std::vector<std::wstring> parts;
    size_t i = root.size();
    while (i <= p.size()) {
        size_t s = p.find(L'\\', i);
        if (s == std::wstring::npos) s = p.size();
        std::wstring part = p.substr(i, s - i);
        if (part == L"..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != L".") parts.push_back(part);
        i = s + 1;
    }
    std::wstring out = root;
    for (auto& part : parts) out += L'\\' + part;
    return out.empty() || out == root ? root + L'\\' : out;
}

std::vector<PlaylistEntry> playlist_parse(const std::wstring& text, const std::wstring& dir) {
    std::vector<PlaylistEntry> out;
    PlaylistEntry pending;                      // (an #EXTINF waits for its path)
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find_first_of(L"\r\n", pos);
        std::wstring line = trim(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = nl == std::wstring::npos ? text.size() : nl + 1;
        if (!line.empty() && line[0] == L'\xFEFF') line = trim(line.substr(1));    // (a BOM left in)
        if (line.empty()) continue;
        if (line[0] == L'#') {
            // #EXTINF:<seconds>[ key="value" ...],<title>
            if (line.size() > 8 && _wcsnicmp(line.c_str(), L"#EXTINF:", 8) == 0) {
                pending = {};
                size_t comma = line.find(L',', 8);
                double secs = wcstod(line.c_str() + 8, nullptr);
                if (secs > 0) pending.duration = (long long)(secs * 10000000.0 + 0.5);
                if (comma != std::wstring::npos) pending.title = trim(line.substr(comma + 1));
            }
            continue;
        }
        std::wstring p = line;
        size_t scheme = p.find(L"://");
        if (scheme != std::wstring::npos && scheme > 1) {
            if (_wcsnicmp(p.c_str(), L"file:", 5) != 0) { pending = {}; continue; }     // (a web address)
            WCHAR buf[MAX_PATH * 4];
            DWORD n = ARRAYSIZE(buf);
            if (FAILED(PathCreateFromUrlW(p.c_str(), buf, &n, 0))) { pending = {}; continue; }
            p = buf;
        }
        for (auto& c : p) if (c == L'/') c = L'\\';
        if (p.rfind(L"\\\\", 0) == 0 || (p.size() >= 3 && p[1] == L':' && p[2] == L'\\')) {
            // absolute already
        } else if (p[0] == L'\\') {
            p = path_root(dir) + p;             // (on the playlist's drive)
        } else if (p.size() >= 2 && p[1] == L':') {
            p = p.substr(0, 2) + L'\\' + p.substr(2);     // ("C:a.mp3": as near as it gets)
        } else {
            std::wstring base = dir;
            if (!base.empty() && base.back() != L'\\') base += L'\\';
            p = base + p;
        }
        pending.path = normalize(p);
        out.push_back(std::move(pending));
        pending = {};
    }
    return out;
}

std::wstring playlist_write(const std::vector<PlaylistEntry>& entries, const std::wstring& dir) {
    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\') base += L'\\';
    std::wstring out = L"#EXTM3U\r\n";
    for (auto& e : entries) {
        if (e.path.empty()) continue;
        if (!e.title.empty() || e.duration >= 0) {
            long long secs = e.duration >= 0 ? (e.duration + 5000000) / 10000000 : -1;
            std::wstring title = e.title;
            for (auto& c : title) if (c == L'\r' || c == L'\n') c = L' ';
            out += L"#EXTINF:" + std::to_wstring(secs) + L"," + title + L"\r\n";
        }
        bool inside = !base.empty() && e.path.size() > base.size() &&
                      CompareStringOrdinal(e.path.c_str(), (int)base.size(), base.c_str(), (int)base.size(), TRUE) == CSTR_EQUAL;
        out += (inside ? e.path.substr(base.size()) : e.path) + L"\r\n";
    }
    return out;
}

std::string playlist_encode(const std::wstring& text, bool m3u8) {
    auto narrow = [&](UINT cp, DWORD flags, BOOL* lossy) {
        std::string s;
        int n = WideCharToMultiByte(cp, flags, text.data(), (int)text.size(), nullptr, 0, nullptr, lossy);
        if (n <= 0) return s;
        s.resize((size_t)n);
        WideCharToMultiByte(cp, flags, text.data(), (int)text.size(), s.data(), n, nullptr, nullptr);
        return s;
    };
    if (!m3u8 && GetACP() != CP_UTF8) {
        BOOL lossy = FALSE;
        std::string ansi = narrow(CP_ACP, WC_NO_BEST_FIT_CHARS, &lossy);
        if (!lossy && (!ansi.empty() || text.empty())) return ansi;
    }
    return narrow(CP_UTF8, 0, nullptr);
}

bool playlist_load(const std::wstring& file, std::vector<PlaylistEntry>& out) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20);     // (no playlist is bigger)
    std::string raw;
    if (ok) {
        raw.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = raw.empty() || (ReadFile(h, raw.data(), (DWORD)raw.size(), &got, nullptr) && got == raw.size());
    }
    CloseHandle(h);
    if (!ok) return false;
    std::wstring dir = file.substr(0, file.find_last_of(L"\\/") + 1);
    out = playlist_parse(ini_decode(raw), dir);
    return true;
}

bool playlist_save(const std::wstring& file, const std::vector<PlaylistEntry>& entries) {
    std::wstring dir = file.substr(0, file.find_last_of(L"\\/") + 1);
    bool m3u8 = lstrcmpiW(PathFindExtensionW(file.c_str()), L".m3u8") == 0;
    std::string bytes = playlist_encode(playlist_write(entries, dir), m3u8);
    // (written beside it, then put in its place: a failed write leaves the old one)
    std::wstring tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &put, nullptr) && put == bytes.size();
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}
