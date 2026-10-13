#include "playlist.h"
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
