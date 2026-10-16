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

static unsigned s_nextId = 1;       // (the entries' ids: from one count for all queues)

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
        e.id = s_nextId++;
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
        e.id = s_nextId++;
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

int PlayQueue::move(std::vector<int> indices, int to) {
    std::vector<bool> moved(m_entries.size(), false);
    for (int i : indices)
        if (i >= 0 && i < size()) moved[(size_t)i] = true;
    std::vector<int> stay, go;                  // (the old indices, in their new order: those staying, those moved)
    int at = 0;                                 // (where among those staying the moved ones go)
    for (int i = 0; i < size(); ++i) {
        (moved[(size_t)i] ? go : stay).push_back(i);
        if (!moved[(size_t)i] && i < to) ++at;
    }
    if (go.empty()) return -1;
    std::vector<int> now(stay.begin(), stay.begin() + at);
    now.insert(now.end(), go.begin(), go.end());
    now.insert(now.end(), stay.begin() + at, stay.end());
    std::vector<int> remap(m_entries.size());
    std::vector<QueueEntry> entries;
    for (size_t k = 0; k < now.size(); ++k) {
        remap[(size_t)now[k]] = (int)k;
        entries.push_back(std::move(m_entries[(size_t)now[k]]));
    }
    m_entries = std::move(entries);
    if (m_shuffle) for (int& i : m_order) i = remap[(size_t)i];
    else std::iota(m_order.begin(), m_order.end(), 0);      // (in order: as listed)
    if (m_current >= 0) m_current = remap[(size_t)m_current];
    return at;
}

int PlayQueue::insert(int at, std::vector<QueueEntry> entries, bool fresh) {
    if (at < 0 || at > size()) at = size();
    int k = (int)entries.size();
    if (!k) return at;
    if (fresh)
        for (auto& e : entries) e.id = s_nextId++;
    m_entries.insert(m_entries.begin() + at, std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
    if (m_current >= at) m_current += k;
    if (!m_shuffle) {
        m_order.resize(m_entries.size());
        std::iota(m_order.begin(), m_order.end(), 0);
        return at;
    }
    for (int& i : m_order)
        if (i >= at) i += k;
    for (int i = at; i < at + k; ++i) {         // (as appended: somewhere among the entries still to come)
        int from = m_current >= 0 ? order_pos(m_current) + 1 : 0;
        m_order.insert(m_order.begin() + std::uniform_int_distribution<int>(from, (int)m_order.size())(m_rng), i);
    }
    return at;
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
        L".m3u", L".m3u8", L".pls", L".wpl", L".zpl", L".asx", L".wax", L".wvx", L".xspf",    // playlists
    };
    for (auto k : kNot)
        if (e == k) return true;
    return false;
}

static bool ext_in(const std::wstring& path, std::initializer_list<const wchar_t*> exts) {
    const wchar_t* dot = PathFindExtensionW(path.c_str());
    for (auto e : exts)
        if (lstrcmpiW(dot, e) == 0) return true;
    return false;
}

bool midi_file(const std::wstring& path) {
    return !media_url(path) && ext_in(path, { L".mid", L".midi", L".rmi", L".kar" });
}

bool cd_track_file(const std::wstring& path) { return !media_url(path) && ext_in(path, { L".cda" }); }

// 0: not media, 1: audio, 2: video, by the extension's perceived type (asked for every file
// of a folder walk, on any thread: the answer is kept per extension)
static int media_kind(const std::wstring& path) {
    if (media_url(path)) return 1;              // (a stream: sound as far as anyone knows yet)
    if (midi_file(path) || cd_track_file(path)) return 1;
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
// playlists
// ------------------------------------------------------------------
PlaylistKind playlist_kind(const std::wstring& path) {
    if (media_url(path)) return PlaylistKind::None;
    const wchar_t* dot = PathFindExtensionW(path.c_str());
    static const struct { const wchar_t* ext; PlaylistKind kind; } kExts[] = {
        { L".m3u", PlaylistKind::M3U }, { L".m3u8", PlaylistKind::M3U }, { L".pls", PlaylistKind::PLS },
        { L".wpl", PlaylistKind::XML }, { L".zpl", PlaylistKind::XML }, { L".asx", PlaylistKind::XML },
        { L".wax", PlaylistKind::XML }, { L".wvx", PlaylistKind::XML }, { L".xspf", PlaylistKind::XML },
    };
    for (auto& k : kExts)
        if (lstrcmpiW(dot, k.ext) == 0) return k.kind;
    return PlaylistKind::None;
}

bool playlist_file(const std::wstring& path) { return playlist_kind(path) != PlaylistKind::None; }

bool media_url(const std::wstring& s) {
    for (const wchar_t* scheme : { L"http://", L"https://", L"mms://", L"mmsh://", L"rtsp://" }) {
        size_t n = wcslen(scheme);
        if (s.size() > n && _wcsnicmp(s.c_str(), scheme, n) == 0) return true;
    }
    return false;
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

// "%20" and the like decoded (UTF-8 bytes): a relative URI's path
static std::wstring percent_decode(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    std::string bytes;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
            bytes += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            WCHAR w[2] = { s[i], 0 };
            char b[8];
            int n = WideCharToMultiByte(CP_UTF8, 0, w, 1, b, sizeof(b), nullptr, nullptr);
            bytes.append(b, (size_t)std::max(0, n));
        }
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring out((size_t)std::max(0, n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), out.data(), n);
    return out;
}

// what a playlist lists, as a path or web address: absolute paths as they are, relative
// ones from `dir`, file:/// URLs as paths, web addresses (streams) as they are; false: not
// something to play (another scheme, an empty line). `uri`: a relative one is percent-encoded
// (XSPF)
static bool resolve(std::wstring p, const std::wstring& dir, bool uri, std::wstring& out) {
    p = trim(p);
    if (p.empty()) return false;
    if (media_url(p)) { out = p; return true; }
    size_t colon = p.find(L':');
    if (colon != std::wstring::npos && colon > 1) {             // (a scheme, not a drive letter)
        if (_wcsnicmp(p.c_str(), L"file:", 5) != 0) return false;
        WCHAR buf[MAX_PATH * 4];
        DWORD n = ARRAYSIZE(buf);
        if (FAILED(PathCreateFromUrlW(p.c_str(), buf, &n, 0))) return false;
        p = buf;
    } else if (uri) {
        p = percent_decode(p);
    }
    for (auto& c : p) if (c == L'/') c = L'\\';
    if (p.rfind(L"\\\\", 0) == 0 || (p.size() >= 3 && p[1] == L':' && p[2] == L'\\')) {
        // absolute already
    } else if (p[0] == L'\\') {
        p = path_root(dir) + p;                 // (on the playlist's drive)
    } else if (p.size() >= 2 && p[1] == L':') {
        p = p.substr(0, 2) + L'\\' + p.substr(2);     // ("C:a.mp3": as near as it gets)
    } else {
        std::wstring base = dir;
        if (!base.empty() && base.back() != L'\\') base += L'\\';
        p = base + p;
    }
    out = normalize(p);
    return true;
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
        if (resolve(line, dir, false, pending.path)) out.push_back(std::move(pending));
        pending = {};
    }
    return out;
}

// [playlist] File1=..., Title1=..., Length1=<seconds, -1: a stream>, in the order of their
// numbers
std::vector<PlaylistEntry> playlist_parse_pls(const std::wstring& text, const std::wstring& dir) {
    std::map<long, PlaylistEntry> byNumber;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find_first_of(L"\r\n", pos);
        std::wstring line = trim(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = nl == std::wstring::npos ? text.size() : nl + 1;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        size_t digits = key.find_first_of(L"0123456789");
        if (digits == std::wstring::npos) continue;
        long n = wcstol(key.c_str() + digits, nullptr, 10);
        std::wstring name = key.substr(0, digits);
        PlaylistEntry& e = byNumber[n];
        if (_wcsicmp(name.c_str(), L"File") == 0) {
            if (!resolve(value, dir, false, e.path)) e.path.clear();
        } else if (_wcsicmp(name.c_str(), L"Title") == 0) {
            e.title = value;
        } else if (_wcsicmp(name.c_str(), L"Length") == 0) {
            double secs = wcstod(value.c_str(), nullptr);
            if (secs > 0) e.duration = (long long)(secs * 10000000.0 + 0.5);
        }
    }
    std::vector<PlaylistEntry> out;
    for (auto& [n, e] : byNumber)
        if (!e.path.empty()) out.push_back(std::move(e));
    return out;
}

// &amp; &lt; &#233; &#xE9; ...
static std::wstring xml_unescape(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        size_t semi = s[i] == L'&' ? s.find(L';', i) : std::wstring::npos;
        if (semi == std::wstring::npos || semi - i > 10) { out += s[i]; continue; }
        std::wstring ent = s.substr(i + 1, semi - i - 1);
        unsigned long cp = 0;
        if (ent == L"amp") cp = L'&';
        else if (ent == L"lt") cp = L'<';
        else if (ent == L"gt") cp = L'>';
        else if (ent == L"quot") cp = L'"';
        else if (ent == L"apos") cp = L'\'';
        else if (ent.size() > 1 && ent[0] == L'#')
            cp = ent[1] == L'x' || ent[1] == L'X' ? wcstoul(ent.c_str() + 2, nullptr, 16) : wcstoul(ent.c_str() + 1, nullptr, 10);
        if (!cp || cp > 0x10FFFF) { out += s[i]; continue; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (wchar_t)(0xD800 + (cp >> 10));
            out += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (wchar_t)cp;
        }
        i = semi;
    }
    return out;
}

// an attribute's value in a tag's text (`name` matched without regard to case)
static std::wstring xml_attr(const std::wstring& tag, const wchar_t* name) {
    size_t n = wcslen(name);
    for (size_t i = 0; i + n < tag.size(); ++i) {
        if (_wcsnicmp(tag.c_str() + i, name, n) != 0 || (i && !iswspace(tag[i - 1]))) continue;
        size_t j = i + n;
        while (j < tag.size() && iswspace(tag[j])) ++j;
        if (j >= tag.size() || tag[j] != L'=') continue;
        ++j;
        while (j < tag.size() && iswspace(tag[j])) ++j;
        if (j >= tag.size() || (tag[j] != L'"' && tag[j] != L'\'')) continue;
        size_t end = tag.find(tag[j], j + 1);
        if (end == std::wstring::npos) return {};
        return xml_unescape(tag.substr(j + 1, end - j - 1));
    }
    return {};
}

// Windows Media Player's .wpl and Zune's .zpl (<media src=...>), .asx / .wax / .wvx
// (<entry><title/><ref href=...></entry>, <entryref href=...>), XSPF (<track><location/>
// <title/><duration>ms</duration></track>): read as one tolerant tag scan
std::vector<PlaylistEntry> playlist_parse_xml(const std::wstring& text, const std::wstring& dir) {
    std::vector<PlaylistEntry> out;
    PlaylistEntry cur;
    bool inEntry = false, xspf = false;
    std::wstring field;                         // the element whose text is being read
    std::wstring chars;
    auto element = [](const std::wstring& tag) {
        size_t e = 0;
        while (e < tag.size() && !iswspace(tag[e]) && tag[e] != L'/' && tag[e] != L'>') ++e;
        std::wstring name = tag.substr(0, e);
        size_t colon = name.find(L':');         // (a namespace prefix)
        if (colon != std::wstring::npos) name = name.substr(colon + 1);
        for (auto& c : name) c = (wchar_t)towlower(c);
        return name;
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t lt = text.find(L'<', pos);
        if (lt == std::wstring::npos) break;
        chars += text.substr(pos, lt - pos);
        if (text.compare(lt, 4, L"<!--") == 0) {
            size_t end = text.find(L"-->", lt + 4);
            pos = end == std::wstring::npos ? text.size() : end + 3;
            continue;
        }
        if (text.compare(lt, 9, L"<![CDATA[") == 0) {
            size_t end = text.find(L"]]>", lt + 9);
            size_t stop = end == std::wstring::npos ? text.size() : end;
            chars += text.substr(lt + 9, stop - lt - 9);
            pos = end == std::wstring::npos ? text.size() : end + 3;
            continue;
        }
        size_t gt = text.find(L'>', lt);
        if (gt == std::wstring::npos) break;
        std::wstring tag = text.substr(lt + 1, gt - lt - 1);
        pos = gt + 1;
        if (tag.empty() || tag[0] == L'?' || tag[0] == L'!') continue;
        bool closing = tag[0] == L'/';
        std::wstring name = element(closing ? tag.substr(1) : tag);
        if (closing) {
            if (name == field) {
                std::wstring value = trim(xml_unescape(chars));
                if (name == L"title" && inEntry) cur.title = value;
                else if (name == L"location" && inEntry && cur.path.empty()) {
                    if (!resolve(value, dir, true, cur.path)) cur.path.clear();
                } else if (name == L"duration" && inEntry && xspf) {
                    double ms = wcstod(value.c_str(), nullptr);
                    if (ms > 0) cur.duration = (long long)(ms * 10000.0 + 0.5);
                }
                field.clear();
            }
            if ((name == L"entry" || name == L"track") && inEntry) {
                if (!cur.path.empty()) out.push_back(std::move(cur));
                cur = {};
                inEntry = false;
            }
            continue;
        }
        chars.clear();
        bool selfClosing = tag.back() == L'/';
        if (name == L"playlist" && !xml_attr(tag, L"xmlns").empty()) xspf = true;
        if (name == L"media") {                         // (WPL, ZPL)
            PlaylistEntry e;
            if (resolve(xml_attr(tag, L"src"), dir, false, e.path)) out.push_back(std::move(e));
        } else if (name == L"entryref") {               // (ASX: another playlist)
            PlaylistEntry e;
            if (resolve(xml_attr(tag, L"href"), dir, false, e.path)) out.push_back(std::move(e));
        } else if (name == L"entry" || name == L"track") {
            cur = {};
            inEntry = !selfClosing;
            if (name == L"track") xspf = true;
        } else if (name == L"ref" && inEntry && cur.path.empty()) {     // (the first of its alternatives)
            if (!resolve(xml_attr(tag, L"href"), dir, false, cur.path)) cur.path.clear();
        } else if (!selfClosing && (name == L"title" || name == L"location" || name == L"duration")) {
            field = name;
        }
    }
    return out;
}

std::vector<PlaylistEntry> playlist_parse_any(const std::wstring& text, const std::wstring& dir, PlaylistKind kind) {
    // (the content decides where the extension misleads: an .m3u that is really a .pls)
    size_t start = 0;
    while (start < text.size() && (iswspace(text[start]) || text[start] == L'\xFEFF')) ++start;
    if (start + 10 <= text.size() && _wcsnicmp(text.c_str() + start, L"[playlist]", 10) == 0)
        kind = PlaylistKind::PLS;
    else if (start < text.size() && text[start] == L'<')
        kind = PlaylistKind::XML;
    else if (kind != PlaylistKind::PLS)
        kind = PlaylistKind::M3U;
    switch (kind) {
        case PlaylistKind::PLS: return playlist_parse_pls(text, dir);
        case PlaylistKind::XML: return playlist_parse_xml(text, dir);
        default: return playlist_parse(text, dir);
    }
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
    out = playlist_parse_any(ini_decode(raw), dir, playlist_kind(file));
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

// ------------------------------------------------------------------
// the player's queues as they were left
// ------------------------------------------------------------------
// an entry's line: "path <tab> duration <tab> title" (no path holds a tab; the title, last,
// may hold anything but a line break)
std::wstring player_session_serialize(const PlayerSession& s) {
    IniDoc d;
    d.set(L"player", L"active", std::to_wstring(s.active));
    for (size_t i = 0; i < s.queues.size(); ++i) {
        const QueueState& q = s.queues[i];
        KeyValues kv;
        kv.push_back({ L"name", L"\"" + q.name + L"\"" });      // (quoted: blanks at its ends kept)
        kv.push_back({ L"current", std::to_wstring(q.current) });
        kv.reserve(q.entries.size() + 2);
        for (size_t k = 0; k < q.entries.size(); ++k) {
            const PlaylistEntry& e = q.entries[k];
            std::wstring title = e.title;
            for (auto& c : title) if (c == L'\r' || c == L'\n') c = L' ';
            kv.push_back({ std::to_wstring(k + 1), e.path + L'\t' + std::to_wstring(e.duration) + L'\t' + title });
        }
        d.replace(L"queue" + std::to_wstring(i + 1), kv);    // (not set() one by one: thousands of entries)
    }
    return ini_serialize(d);
}

PlayerSession player_session_parse(const std::wstring& text) {
    PlayerSession s;
    IniDoc d = ini_parse(text);
    for (int i = 1; i < 10000; ++i) {
        std::wstring sec = L"queue" + std::to_wstring(i);
        if (!d.find(sec)) break;
        QueueState q;
        int current = -1;
        for (auto& [key, value] : d.entries(sec)) {
            if (ini_ieq(key, L"name")) { q.name = value; continue; }
            if (ini_ieq(key, L"current")) { current = (int)wcstol(value.c_str(), nullptr, 10); continue; }
            if (key.empty() || !iswdigit(key[0])) continue;
            size_t t1 = value.find(L'\t');
            PlaylistEntry e;
            e.path = value.substr(0, t1);
            if (e.path.empty()) continue;
            if (t1 != std::wstring::npos) {
                size_t t2 = value.find(L'\t', t1 + 1);
                e.duration = std::max(-1LL, wcstoll(value.substr(t1 + 1, t2 == std::wstring::npos ? std::wstring::npos : t2 - t1 - 1).c_str(), nullptr, 10));
                if (t2 != std::wstring::npos) e.title = value.substr(t2 + 1);
            }
            q.entries.push_back(std::move(e));
        }
        q.current = current >= 0 && current < (int)q.entries.size() ? current : -1;
        s.queues.push_back(std::move(q));
    }
    const std::wstring* a = d.get(L"player", L"active");
    int active = a ? (int)wcstol(a->c_str(), nullptr, 10) : 0;
    s.active = s.queues.empty() ? 0 : std::clamp(active, 0, (int)s.queues.size() - 1);
    return s;
}
