#include "sync_state.h"
#include "sync_paths.h"
#include "crypto.h"
#include <cstring>

namespace sync {

std::wstring key_of(const RelPath& p) { return syncpath::case_key(p); }

namespace {
enum : uint8_t { R_BASE = 1, R_BASE_DEL = 2, R_REMOTE = 3, R_REMOTE_DEL = 4, R_VALUE = 5, R_REMOTE_CLEAR = 6 };

struct W {
    std::string s;
    void u8(uint8_t v) { s += (char)v; }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) s += (char)(v >> (8 * i)); }
    void str(const std::string& v) { u64(v.size()); s += v; }
    void wstr(const std::wstring& v) { str(to_utf8(v)); }
};
struct R {
    const std::string& s;
    size_t p = 0;
    bool bad = false;
    explicit R(const std::string& x) : s(x) {}
    uint8_t u8() { if (p + 1 > s.size()) { bad = true; return 0; } return (uint8_t)s[p++]; }
    uint64_t u64() {
        if (p + 8 > s.size()) { bad = true; return 0; }
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | (uint8_t)s[p + i];
        p += 8;
        return v;
    }
    std::string str() {
        uint64_t n = u64();
        if (bad || p + n > s.size()) { bad = true; return {}; }
        std::string v = s.substr(p, (size_t)n);
        p += (size_t)n;
        return v;
    }
    std::wstring wstr() { return from_utf8(str()); }
};

std::string enc_base(const BaseEntry& e) {
    W w;
    w.wstr(e.path); w.u8(e.isDir); w.u64(e.size); w.u64(e.mtime); w.u64(e.fileId);
    w.s.append((const char*)e.hash.data(), 32);
    w.str(e.rev); w.u64(e.version);
    return w.s;
}
bool dec_base(const std::string& s, BaseEntry& e) {
    R r(s);
    e.path = r.wstr(); e.isDir = r.u8() != 0; e.size = r.u64(); e.mtime = r.u64(); e.fileId = r.u64();
    if (r.p + 32 > s.size()) return false;
    std::memcpy(e.hash.data(), s.data() + r.p, 32);
    r.p += 32;
    e.rev = r.str(); e.version = r.u64();
    return !r.bad;
}
std::string enc_remote(const RemoteItem& e) {
    W w;
    w.wstr(e.path); w.u8(e.isDir); w.u64(e.size); w.str(e.rev);
    return w.s;
}
bool dec_remote(const std::string& s, RemoteItem& e) {
    R r(s);
    e.path = r.wstr(); e.isDir = r.u8() != 0; e.size = r.u64(); e.rev = r.str();
    return !r.bad;
}

// record: u32 length of (type + payload) | type | payload | 8 bytes of BLAKE2b(type + payload)
std::string record(uint8_t type, const std::string& payload) {
    std::string body;
    body += (char)type;
    body += payload;
    std::string out;
    uint32_t n = (uint32_t)body.size();
    for (int i = 0; i < 4; ++i) out += (char)(n >> (8 * i));
    out += body;
    auto h = crypto::blake2b(body.data(), body.size());
    out.append((const char*)h.data(), 8);
    return out;
}

// false: not there, or (`failed`) there but not read whole
bool read_file(const std::wstring& path, std::string& out, bool* failed = nullptr) {
    out.clear();
    if (failed) *failed = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (failed) *failed = e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND;
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    size_t done = 0;
    while (done < out.size() && ReadFile(h, out.data() + done, (DWORD)std::min<size_t>(out.size() - done, 1 << 30), &got, nullptr) && got) done += got;
    CloseHandle(h);
    if (done != out.size()) { out.clear(); if (failed) *failed = true; return false; }
    return true;
}
} // namespace

StateStore::StateStore(std::wstring dir) : m_dir(std::move(dir)) {}
StateStore::~StateStore() { if (m_log != INVALID_HANDLE_VALUE) CloseHandle(m_log); }

bool StateStore::apply(uint8_t type, const std::string& p) {
    switch (type) {
        case R_BASE: { BaseEntry e; if (!dec_base(p, e)) return false; m_base[key_of(e.path)] = std::move(e); return true; }
        case R_BASE_DEL: m_base.erase(from_utf8(p)); return true;
        case R_REMOTE: { RemoteItem e; if (!dec_remote(p, e)) return false; m_remote[key_of(e.path)] = std::move(e); return true; }
        case R_REMOTE_DEL: m_remote.erase(from_utf8(p)); return true;
        case R_REMOTE_CLEAR: m_remote.clear(); return true;
        case R_VALUE: { R r(p); std::string k = r.str(), v = r.str(); if (r.bad) return false; m_values[k] = v; return true; }
    }
    return false;
}

static size_t parse_records(const std::string& data, const std::function<bool(uint8_t, const std::string&)>& f) {
    size_t pos = 0;
    while (pos + 4 <= data.size()) {
        uint32_t n = 0;
        for (int i = 3; i >= 0; --i) n = (n << 8) | (uint8_t)data[pos + i];
        if (!n || pos + 4 + n + 8 > data.size()) break;
        const char* body = data.data() + pos + 4;
        auto h = crypto::blake2b(body, n);
        if (std::memcmp(h.data(), body + n, 8) != 0) break;
        if (!f((uint8_t)body[0], std::string(body + 1, n - 1))) break;
        pos += 4 + n + 8;
    }
    return pos;
}

bool StateStore::load() {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    m_base.clear(); m_remote.clear(); m_values.clear();
    if (m_dir.empty()) return true;
    CreateDirectoryW(m_dir.c_str(), nullptr);
    std::string snap, log;
    bool ok = true, failed = false;
    m_unreadable = false;
    if (read_file(m_dir + L"\\base.dat", snap, &failed)) {
        if (snap.compare(0, 6, "EPSB1\n") != 0) ok = false;
        else {
            std::string body = snap.substr(6);
            size_t used = parse_records(body, [&](uint8_t t, const std::string& p) { return apply(t, p); });
            if (used != body.size()) ok = false;
            m_snapBytes = snap.size();
        }
        // damaged: what could be read is used (a smaller base only ever adopts or keeps both, never
        // deletes); the file kept beside for a look
        if (!ok) CopyFileW((m_dir + L"\\base.dat").c_str(), (m_dir + L"\\base.dat.damaged").c_str(), FALSE);
    } else if (failed) {
        m_unreadable = true;                            // (in use, a disk error: not as if empty)
        return false;
    }
    read_file(m_dir + L"\\base.log", log, &failed);
    if (failed) { m_unreadable = true; m_base.clear(); m_remote.clear(); m_values.clear(); return false; }
    size_t used = parse_records(log, [&](uint8_t t, const std::string& p) { return apply(t, p); });
    m_log = CreateFileW((m_dir + L"\\base.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (m_log != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)used;                   // a torn tail goes
        SetFilePointerEx(m_log, at, nullptr, FILE_BEGIN);
        SetEndOfFile(m_log);
        m_logBytes = used;
    } else {
        m_unreadable = true;                            // (changes couldn't be kept)
        return false;
    }
    return ok;
}

void StateStore::append(uint8_t type, const std::string& payload) {
    if (m_log == INVALID_HANDLE_VALUE) return;
    std::string r = record(type, payload);
    DWORD w = 0;
    WriteFile(m_log, r.data(), (DWORD)r.size(), &w, nullptr);
    m_logBytes += w;
    if (m_logBytes > 4 * 1024 * 1024 && m_logBytes > m_snapBytes) compact();
}

const BaseEntry* StateStore::base(const std::wstring& key) const {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    auto f = m_base.find(key);
    return f == m_base.end() ? nullptr : &f->second;
}
void StateStore::put_base(const BaseEntry& e) {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    m_base[key_of(e.path)] = e;
    append(R_BASE, enc_base(e));
}
void StateStore::erase_base(const std::wstring& key) {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    if (m_base.erase(key)) append(R_BASE_DEL, to_utf8(key));
}
const RemoteItem* StateStore::remote(const std::wstring& key) const {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    auto f = m_remote.find(key);
    return f == m_remote.end() ? nullptr : &f->second;
}
void StateStore::put_remote(const RemoteItem& e) {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    m_remote[key_of(e.path)] = e;
    append(R_REMOTE, enc_remote(e));
}
void StateStore::erase_remote(const std::wstring& key) {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    if (m_remote.erase(key)) append(R_REMOTE_DEL, to_utf8(key));
}
void StateStore::clear_remote() {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    m_remote.clear();
    append(R_REMOTE_CLEAR, {});
}
std::string StateStore::value(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    auto f = m_values.find(name);
    return f == m_values.end() ? std::string() : f->second;
}
void StateStore::set_value(const std::string& name, const std::string& v) {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    if (m_values.count(name) && m_values[name] == v) return;
    m_values[name] = v;
    W w;
    w.str(name);
    w.str(v);
    append(R_VALUE, w.s);
}

void StateStore::flush() {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    if (m_log != INVALID_HANDLE_VALUE) FlushFileBuffers(m_log);
}

std::string StateStore::snapshot() const {
    std::string s = "EPSB1\n";
    for (auto& [k, v] : m_values) { W w; w.str(k); w.str(v); s += record(R_VALUE, w.s); }
    for (auto& [k, e] : m_base) s += record(R_BASE, enc_base(e));
    for (auto& [k, e] : m_remote) s += record(R_REMOTE, enc_remote(e));
    return s;
}

void StateStore::compact() {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    if (m_dir.empty()) return;
    std::string s = snapshot();
    std::wstring tmp = m_dir + L"\\base.tmp", dat = m_dir + L"\\base.dat";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), dat.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return;
    }
    m_snapBytes = s.size();
    if (m_log != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER z{};
        SetFilePointerEx(m_log, z, nullptr, FILE_BEGIN);
        SetEndOfFile(m_log);
        m_logBytes = 0;
    }
}

void StateStore::reset() {
    std::lock_guard<std::recursive_mutex> lk(m_mx);
    m_base.clear(); m_remote.clear(); m_values.clear();
    compact();
}

} // namespace sync
