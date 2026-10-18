#include "sync_engine.h"
#include "crypto.h"
#include <algorithm>
#include <functional>
#include <thread>

namespace sync {

const wchar_t* pair_state_name(PairState s) {
    switch (s) {
        case PairState::Idle: return L"idle";
        case PairState::Syncing: return L"syncing";
        case PairState::UpToDate: return L"up to date";
        case PairState::Paused: return L"paused";
        case PairState::Offline: return L"offline";
        case PairState::Error: return L"error";
        case PairState::Locked: return L"locked";
        case PairState::AuthNeeded: return L"sign-in needed";
        case PairState::RootMissing: return L"folder missing";
        case PairState::ConfirmDeletes: return L"waiting to delete";
    }
    return L"?";
}

const wchar_t* item_state_name(ItemState s) {
    switch (s) {
        case ItemState::None: return L"none";
        case ItemState::Synced: return L"synced";
        case ItemState::Syncing: return L"syncing";
        case ItemState::Pending: return L"pending";
        case ItemState::Conflict: return L"conflict";
        case ItemState::Error: return L"error";
        case ItemState::Excluded: return L"excluded";
    }
    return L"?";
}

struct Engine::Ctx {
    Progress& prog;
    PassReport& rep;
    Caps caps;
    LocalMap local;
    std::set<std::wstring> movedAway;            // local paths renamed aside this pass (conflicts)
    bool defer = false;                          // transactional: records wait for the commit
    std::vector<std::function<void()>> deferred;
    std::mutex mx;                               // the report and `deferred` (parallel transfers)
    Ctx(Progress& p, PassReport& r) : prog(p), rep(r) {}
    void record(std::function<void()> f) {
        std::lock_guard<std::mutex> lk(mx);
        if (defer) deferred.push_back(std::move(f));
        else f();
    }
    void log(const char* what, const RelPath& p, const RelPath& from = {}) {
        std::lock_guard<std::mutex> lk(mx);
        std::string s = what;
        s += ' ';
        if (!from.empty()) s += to_utf8(from) + " -> ";
        s += to_utf8(p);
        rep.log.push_back(s);
    }
};

Engine::Engine(EngineOptions opt, std::unique_ptr<Backend> backend, StateStore& state)
    : m_opt(std::move(opt)), m_backend(std::move(backend)), m_state(state) {}
Engine::~Engine() = default;

void Engine::set_state(PairState s, const std::wstring& msg) {
    std::lock_guard<std::mutex> lk(m_mx);
    m_status.state = s;
    m_status.message = msg;
}

void Engine::problem(const RelPath& p, const std::wstring& why) {
    std::lock_guard<std::mutex> lk(m_mx);
    m_errors[key_of(p)] = why;
}

EngineStatus Engine::status() const {
    std::lock_guard<std::mutex> lk(m_mx);
    EngineStatus s = m_status;
    s.problems.clear();
    for (auto& [k, why] : m_errors) s.problems.push_back({ k, why });
    return s;
}

void Engine::set_paused(bool p) {
    m_paused = p;
    if (p) set_state(PairState::Paused);
    else set_state(PairState::Idle);
}

// (only while the question is open: a stray yes doesn't approve the next big deletion)
void Engine::confirm_deletes(bool deleteThem) {
    std::lock_guard<std::mutex> lk(m_mx);
    if (!m_asked) return;
    m_confirmed = m_asked;
    m_confirm = deleteThem ? 1 : -1;
}

void Engine::dismiss_conflicts() {
    std::lock_guard<std::mutex> lk(m_mx);
    m_status.conflicts.clear();
}

// ------------------------------------------------------------------ item states
static bool under(const std::wstring& key, const std::wstring& dirKey) {
    return dirKey.empty() || (key.size() > dirKey.size() && key.compare(0, dirKey.size(), dirKey) == 0 && key[dirKey.size()] == L'/');
}

ItemState Engine::item_state(const RelPath& rel, bool isDir) const {
    std::wstring k = key_of(rel);
    bool inBase = m_state.base(k) != nullptr;          // (the store's lock before ours, never inside it)
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_ignore.ignored(rel, isDir)) return ItemState::Excluded;
    auto any = [&](const auto& set) {
        if (set.count(k)) return true;
        if (isDir) for (auto& s : set) if (under(s, k)) return true;
        return false;
    };
    if (m_errors.count(k)) return ItemState::Error;
    if (isDir) for (auto& [e, w] : m_errors) if (under(e, k)) return ItemState::Error;
    if (any(m_active)) return ItemState::Syncing;
    for (auto& c : m_status.conflicts) if (key_of(c) == k) return ItemState::Conflict;
    if (any(m_pending)) return ItemState::Pending;
    if (inBase) return ItemState::Synced;
    return ItemState::Pending;
}

std::vector<std::pair<std::wstring, ItemState>> Engine::children(const RelPath& folder) const {
    std::vector<std::pair<std::wstring, ItemState>> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((full(folder) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (n.compare(0, 6, L".~sync") == 0) continue;
        bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back({ n, item_state(join_path(folder, syncpath::nfc(n)), dir) });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// ------------------------------------------------------------------ the remote side
Result Engine::list_remote(Ctx& c) {
    bool listed = m_state.value("listed") == "1";
    std::string token = m_state.value("token"), newToken;
    bool changed = true;
    Result r = m_backend->changed_since(listed ? token : std::string(), changed, newToken, c.prog);
    if (!r.ok()) return r;
    if (listed && !changed) return {};
    std::string cursor = listed ? m_state.value("cursor") : std::string();
    ChangeBatch b;
    r = m_backend->changes(cursor, b, c.prog);
    if (!r.ok() && !cursor.empty() && r.st == Status::NotFound) {     // the cursor is gone (history rewritten): list it all
        b = {};
        r = m_backend->changes("", b, c.prog);
    }
    if (!r.ok()) return r;
    std::lock_guard<std::recursive_mutex> lk(m_state.mutex());
    if (b.full) m_state.clear_remote();
    for (auto& p : b.removals) {
        std::wstring k = key_of(p);
        m_state.erase_remote(k);
    }
    std::set<std::wstring> seen;
    for (auto& e : b.upserts) {
        std::wstring k = key_of(e.path);
        const RemoteItem* old = m_state.remote(k);
        if (old && old->path != e.path && (b.full ? seen.count(k) > 0 : true)) {
            problem(e.path, L"Another item in the storage has the same name in different case; Windows can't hold both.");
            continue;
        }
        seen.insert(k);
        RemoteItem it;
        it.path = e.path;
        it.isDir = e.isDir;
        it.size = e.size;
        it.rev = e.rev;
        m_state.put_remote(it);
    }
    {
        // a full listing says all the store's problems: the earlier listing's that it
        // doesn't repeat are over (the storage put things right)
        std::lock_guard<std::mutex> elk(m_mx);
        if (b.full) {
            for (auto& [k, why] : m_listErrors) {
                auto f = m_errors.find(k);
                if (f != m_errors.end() && f->second == why) m_errors.erase(f);
            }
            m_listErrors.clear();
        }
        for (auto& [p, why] : b.problems) {
            m_errors[key_of(p)] = why;
            m_listErrors[key_of(p)] = why;
        }
    }
    m_state.set_value("cursor", b.cursor);
    m_state.set_value("token", newToken);
    m_state.set_value("listed", "1");
    return {};
}

// ------------------------------------------------------------------ hashing
void Engine::hash_changed(Ctx& c, LocalMap& local) {
    uint64_t now = now_ft();
    // a base taken within a clock tick (or a FAT file's two seconds) of the scan before
    // could match a file written again right after it, same size and same time: those are
    // hashed. lastScan: when the last scan that got this far started.
    uint64_t lastScan = strtoull(m_state.value("scanned").c_str(), nullptr, 10);
    const uint64_t kTick = 3ull * 10000000;
    for (auto& [k, l] : local) {
        if (l.isDir) continue;
        const BaseEntry* b = m_state.base(k);
        l.racy = b && lastScan && (b->mtime + kTick >= lastScan || b->mtime > now);
        if (b && !b->isDir && !l.racy && b->size == l.size && b->mtime == l.mtime && b->fileId == l.fileId) continue;
        if (now > l.mtime && now - l.mtime < (uint64_t)m_opt.settleMs * 10000) { l.busy = true; continue; }
        HANDLE h = open_for_read(full(l.path));
        if (h == INVALID_HANDLE_VALUE) { l.busy = true; continue; }
        uint64_t size = 0, mtime = 0, id = 0, got = 0;
        bool ok = file_identity(h, size, mtime, id) && hash_handle(h, l.hash, &got) && got == size;
        CloseHandle(h);
        if (!ok || size != l.size || mtime != l.mtime) { l.busy = true; continue; }
        l.hashed = true;
        if (c.prog.cancelled()) return;
    }
}

// ------------------------------------------------------------------ transfers
namespace {
struct HashingSink : ByteSink {
    ByteSink& out;
    crypto::Blake2b h;
    uint64_t n = 0;
    explicit HashingSink(ByteSink& o) : out(o) {}
    bool write(const void* b, size_t k) override { h.update(b, k); n += k; return out.write(b, k); }
};

struct Identity { bool exists = false; uint64_t size = 0, mtime = 0, fileId = 0; bool isDir = false; };
Identity identity_of(const std::wstring& full) {
    Identity id;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &a)) return id;
    id.exists = true;
    id.isDir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    id.size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    id.mtime = ft64(a.ftLastWriteTime);
    return id;
}

std::wstring temp_name() {
    uint8_t r[8];
    crypto::random(r, 8);
    std::string h = crypto::hex(r, 8);
    return L".~sync-" + std::wstring(h.begin(), h.end()) + L".tmp";
}
} // namespace

void Engine::record_local(Ctx& c, const RelPath& path, const RemoteItem* r, const Hash* hash, uint64_t version) {
    HANDLE h = CreateFileW(full(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BaseEntry e;
    e.path = path;
    if (h != INVALID_HANDLE_VALUE) {
        uint32_t attrs = 0;
        file_identity(h, e.size, e.mtime, e.fileId, &attrs);
        e.isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        CloseHandle(h);
    }
    if (e.isDir) e.size = e.mtime = 0;
    if (hash) e.hash = *hash;
    if (r) e.rev = r->rev;
    e.version = version;
    c.record([this, e] { m_state.put_base(e); });
}

Result Engine::do_download(Ctx& c, const RelPath& path, const RelPath& saveAs, bool record) {
    const RemoteItem* ri = m_state.remote(key_of(path));
    if (!ri) return Result::fail(Status::NotFound);
    RemoteItem r = *ri;
    RemoteEntry e;
    e.path = r.path;
    e.size = r.size;
    e.rev = r.rev;
    std::wstring tmpDir = full(L".~sync-tmp");
    if (CreateDirectoryW(tmpDir.c_str(), nullptr)) SetFileAttributesW(tmpDir.c_str(), FILE_ATTRIBUTE_HIDDEN);
    std::wstring tmp = tmpDir + L"\\" + temp_name();
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Result::fail(Status::Fatal, L"Can't write in the folder.");
    FileSink fs(h);
    HashingSink hs(fs);
    FileMeta meta;
    Result res = m_backend->download(e, hs, meta, c.prog);
    Hash got = hs.h.final();
    uint64_t seenVersion = 0;               // non-zero: the storage's version is older than this one seen here
    if (res.ok()) {
        Hash zero{};
        if (meta.plainHash != zero && meta.plainHash != got)
            res = Result::fail(Status::Tampered, L"The downloaded file doesn't match its fingerprint.");
        const BaseEntry* b = m_state.base(key_of(path));
        if (res.ok() && b && meta.version && b->version && meta.version < b->version) seenVersion = b->version;
    }
    if (res.ok() && ft64(meta.mtime)) SetFileTime(h, nullptr, nullptr, &meta.mtime);
    CloseHandle(h);
    if (!res.ok()) { DeleteFileW(tmp.c_str()); return res; }
    // the local file must still be as the scan saw it (or absent, as it was)
    std::wstring target = full(saveAs);
    std::wstring tk = key_of(saveAs);
    Identity now = identity_of(target);
    auto li = c.local.find(tk);
    bool expectAbsent = li == c.local.end() || c.movedAway.count(tk);
    if (expectAbsent ? now.exists : (!now.exists || now.size != li->second.size || now.mtime != li->second.mtime)) {
        DeleteFileW(tmp.c_str());
        std::lock_guard<std::mutex> lk(c.mx);
        c.rep.again = true;
        return Result::fail(Status::PreconditionFailed, L"changed locally meanwhile");
    }
    size_t s = target.find_last_of(L'\\');
    make_dirs(target.substr(0, s));
    // a read-only file is replaced too, and stays read-only
    DWORD oldAttrs = now.exists ? GetFileAttributesW(target.c_str()) : INVALID_FILE_ATTRIBUTES;
    bool readOnly = oldAttrs != INVALID_FILE_ATTRIBUTES && (oldAttrs & FILE_ATTRIBUTE_READONLY);
    if (readOnly) SetFileAttributesW(target.c_str(), oldAttrs & ~FILE_ATTRIBUTE_READONLY);
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        DeleteFileW(tmp.c_str());
        if (readOnly) SetFileAttributesW(target.c_str(), oldAttrs);
        std::lock_guard<std::mutex> lk(c.mx);
        c.rep.again = true;
        return Result::fail(err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED ? Status::PreconditionFailed : Status::Fatal,
                            L"The file is in use.");
    }
    if (readOnly) SetFileAttributesW(target.c_str(), GetFileAttributesW(target.c_str()) | FILE_ATTRIBUTE_READONLY);
    // An older version than one seen: deleted and made anew on another device between two passes
    // here, or rolled back by the storage (which the signed manifest catches before this). Taken,
    // as a download replaces only a file unchanged here (the one before it: in the storage's
    // history), and the version seen kept, so that the next upload is the newest.
    if (seenVersion) c.log("older-version", saveAs);
    if (record) record_local(c, saveAs, &r, &got, std::max(meta.version, seenVersion));
    return {};
}

Result Engine::do_upload(Ctx& c, const Action& a) {
    std::wstring k = key_of(a.path);
    auto li = c.local.find(k);
    if (li == c.local.end()) return Result::fail(Status::NotFound);
    LocalItem l = li->second;
    HANDLE h = open_for_read(full(a.path));
    if (h == INVALID_HANDLE_VALUE) { std::lock_guard<std::mutex> lk(c.mx); c.rep.again = true; return Result::fail(Status::PreconditionFailed, L"in use"); }
    uint64_t size = 0, mtime = 0, id = 0;
    uint32_t attrs = 0;
    file_identity(h, size, mtime, id, &attrs);
    if (size != l.size || mtime != l.mtime || !l.hashed) {            // changed since it was hashed
        uint64_t got = 0;
        if (!hash_handle(h, l.hash, &got) || got != size) { CloseHandle(h); return Result::fail(Status::PreconditionFailed); }
        l.size = size; l.mtime = mtime; l.fileId = id;
    }
    const BaseEntry* b = m_state.base(k);
    FileMeta meta;
    meta.mtime = ft_of(l.mtime);
    meta.attributes = attrs;
    meta.version = (b ? b->version : 0) + 1;
    meta.plainHash = l.hash;
    FileSource src(h);
    RemoteEntry out;
    Result r = m_backend->upload(a.path, src, meta, a.expectedRev, out, c.prog);
    CloseHandle(h);
    if (!r.ok()) {
        if (r.st == Status::PreconditionFailed) { std::lock_guard<std::mutex> lk(c.mx); c.rep.again = true; }
        return r;
    }
    BaseEntry e;
    e.path = a.path;
    e.size = l.size; e.mtime = l.mtime; e.fileId = l.fileId ? l.fileId : id;
    e.hash = l.hash;
    e.rev = out.rev;
    e.version = meta.version;
    RemoteItem ri;
    ri.path = a.path;
    ri.size = out.size;
    ri.rev = out.rev;
    c.record([this, e, ri] { m_state.put_base(e); m_state.put_remote(ri); remember_parents(e.path); });
    return {};
}

// ------------------------------------------------------------------ versions, re-encryption
Result Engine::versions(const RelPath& rel, std::vector<RemoteVersion>& out, Progress& prog) {
    out.clear();
    return m_backend->list_versions(rel, out, prog);
}

Result Engine::restore_version(const RelPath& rel, const std::string& rev, Progress& prog) {
    if (rel.empty() || rev.empty()) return Result::fail(Status::NotFound);
    RemoteEntry e;
    e.path = rel;
    e.rev = rev;
    std::wstring tmpDir = full(L".~sync-tmp");
    if (CreateDirectoryW(tmpDir.c_str(), nullptr)) SetFileAttributesW(tmpDir.c_str(), FILE_ATTRIBUTE_HIDDEN);
    std::wstring tmp = tmpDir + L"\\" + temp_name();
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Result::fail(Status::Fatal, L"Can't write in the folder.");
    FileSink fs(h);
    HashingSink hs(fs);
    FileMeta meta;
    Result res = m_backend->download(e, hs, meta, prog);
    Hash got = hs.h.final();
    Hash zero{};
    if (res.ok() && meta.plainHash != zero && meta.plainHash != got)
        res = Result::fail(Status::Tampered, L"The old version doesn't match its fingerprint.");
    CloseHandle(h);
    if (!res.ok()) { DeleteFileW(tmp.c_str()); return res; }
    // a restore never loses the current file
    std::wstring target = full(rel);
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES && !recycle(target, m_opt.trashDir, rel)) {
        DeleteFileW(tmp.c_str());
        return Result::fail(Status::PreconditionFailed, L"The current file couldn't be moved to the Recycle Bin (is it open?).");
    }
    make_dirs(target.substr(0, target.find_last_of(L'\\')));
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return Result::fail(Status::Fatal, L"The old version couldn't be put in place.");
    }
    // its time: now (the newest change, which the next pass uploads)
    HANDLE t = CreateFileW(target.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (t != INVALID_HANDLE_VALUE) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        SetFileTime(t, nullptr, nullptr, &now);
        CloseHandle(t);
    }
    return {};
}

// another device's commit in between (a transactional store refuses the whole commit): a pass to
// catch up, then again
Result Engine::reencrypt(Progress& prog, Reencrypted& out) {
    Result r;
    for (int i = 0; i < 4; ++i) {
        r = reencrypt_once(prog, out);
        if (r.st != Status::CommitRaced) return r;
        if (Result s = sync(prog).result; !s.ok()) return s;
    }
    return Result::fail(Status::PreconditionFailed, L"Other devices kept syncing meanwhile: try again later.");
}

Result Engine::reencrypt_once(Progress& prog, Reencrypted& out) {
    out = {};
    std::vector<BaseEntry> files;
    {
        std::lock_guard<std::recursive_mutex> lk(m_state.mutex());
        for (auto& [k, b] : m_state.base_all())
            if (!b.isDir && !b.rev.empty() && !m_ignore.ignored(b.path, false)) files.push_back(b);
    }
    Result r = m_backend->begin(prog);
    if (!r.ok()) return r;
    std::vector<std::pair<BaseEntry, uint64_t>> done;          // new base, stored size
    for (auto& b : files) {
        if (prog.cancelled()) { m_backend->abort(); return Result::fail(Status::Cancelled); }
        HANDLE h = open_for_read(full(b.path));
        if (h == INVALID_HANDLE_VALUE) { ++out.skipped; continue; }
        uint64_t size = 0, mtime = 0, id = 0;
        uint32_t attrs = 0;
        file_identity(h, size, mtime, id, &attrs);
        if (size != b.size || mtime != b.mtime) { CloseHandle(h); ++out.skipped; continue; }   // changed: the next pass has it
        FileMeta meta;
        meta.mtime = ft_of(mtime);
        meta.attributes = attrs;
        meta.version = b.version + 1;
        meta.plainHash = b.hash;
        FileSource src(h);
        RemoteEntry stored;
        Result x = m_backend->upload(b.path, src, meta, b.rev, stored, prog);
        CloseHandle(h);
        if (x.st == Status::PreconditionFailed || x.st == Status::NotFound) { ++out.skipped; continue; }   // changed there: the next pass
        if (!x.ok()) { m_backend->abort(); return x; }
        BaseEntry e = b;
        e.rev = stored.rev;
        e.version = meta.version;
        done.push_back({ e, stored.size });
    }
    std::string cursor;
    r = m_backend->commit(L"re-encrypt", cursor, prog);
    if (!r.ok()) return r.st == Status::PreconditionFailed ? Result::fail(Status::CommitRaced) : r;
    for (auto& [e, sz] : done) {
        RemoteItem ri;
        ri.path = e.path;
        ri.size = sz;
        ri.rev = e.rev;
        m_state.put_base(e);
        m_state.put_remote(ri);
    }
    if (!cursor.empty()) {
        m_state.set_value("cursor", cursor);
        m_state.set_value("token", cursor);
    } else if (!done.empty()) {
        std::string tok;
        m_backend->touch(tok, prog);
    }
    m_state.flush();
    out.files = done.size();
    return {};
}

// the folders above an uploaded item exist in the store now (implied where it has no
// folders of its own): in the base, so that emptying them later removes them everywhere
void Engine::remember_parents(const RelPath& path) {
    for (RelPath p = parent_of(path); !p.empty(); p = parent_of(p)) {
        if (m_state.base(key_of(p))) break;
        BaseEntry d;
        d.path = p;
        d.isDir = true;
        m_state.put_base(d);
    }
}

Result Engine::do_conflict(Ctx& c, const Action& a) {
    std::wstring k = key_of(a.path);
    auto li = c.local.find(k);
    const RemoteItem* ri = m_state.remote(k);
    if (li == c.local.end() || !ri) return Result::fail(Status::NotFound);
    const LocalItem& l = li->second;
    RemoteEntry e;
    e.path = ri->path;
    e.size = ri->size;
    e.rev = ri->rev;
    // the same content on both sides: nothing to keep twice
    ContentHash rh;
    bool peeked = m_backend->peek_hash(e, rh, c.prog).ok();
    if (peeked && l.hashed && rh == m_backend->hash_of_local(l.hash)) {
        c.log("adopt", a.path);
        RemoteItem r = *ri;
        record_local(c, a.path, &r, &l.hash, 0);
        return {};
    }
    // the remote content is still what was synced last (only stored again: re-encrypted, say):
    // just this side changed it. The base takes the new rev; the next pass uploads the edit.
    const BaseEntry* b = m_state.base(k);
    if (peeked && b && !b->isDir && rh == m_backend->hash_of_local(b->hash)) {
        BaseEntry nb = *b;
        nb.rev = ri->rev;
        c.record([this, nb] { m_state.put_base(nb); });
        c.log("rebase", a.path);
        std::lock_guard<std::mutex> lk(c.mx);
        c.rep.again = true;
        return {};
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    RelPath aside = join_path(parent_of(a.path), syncpath::conflict_name(leaf_of(a.path), m_opt.deviceName, now));
    for (int i = 2; GetFileAttributesW(full(aside).c_str()) != INVALID_FILE_ATTRIBUTES && i < 100; ++i) {
        std::wstring n = syncpath::conflict_name(leaf_of(a.path), m_opt.deviceName + L" " + std::to_wstring(i), now);
        aside = join_path(parent_of(a.path), n);
    }
    if (!MoveFileExW(full(a.path).c_str(), full(aside).c_str(), 0)) {
        std::lock_guard<std::mutex> lk(c.mx);
        c.rep.again = true;
        return Result::fail(Status::PreconditionFailed, L"The file is in use.");
    }
    c.movedAway.insert(k);
    c.log("conflict", aside, a.path);
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_status.conflicts.push_back(aside);
    }
    {
        std::lock_guard<std::mutex> lk(c.mx);
        ++c.rep.conflicts;
        c.rep.again = true;                       // the copy goes up in the next pass
    }
    return do_download(c, a.path, a.path, true);
}

// runs f over items with up to n threads
template <class T, class F> static void parallel(std::vector<T>& items, int n, F f) {
    if (items.empty()) return;
    n = std::max(1, std::min<int>(n, (int)items.size()));
    if (n == 1) { for (auto& x : items) f(x); return; }
    std::atomic<size_t> next{ 0 };
    std::vector<std::thread> t;
    for (int i = 0; i < n; ++i)
        t.emplace_back([&] { for (size_t j; (j = next++) < items.size();) f(items[j]); });
    for (auto& x : t) x.join();
}

// ------------------------------------------------------------------ phases
Result Engine::run_local_phase(Ctx& c, std::vector<Action>& acts) {
    std::vector<Action> downloads;
    auto flush_downloads = [&] {
        parallel(downloads, std::max(1, std::min(c.caps.parallelism, 4)), [&](Action& a) {
            if (c.prog.cancelled()) return;
            std::wstring k = key_of(a.path);
            { std::lock_guard<std::mutex> lk(m_mx); m_active.insert(k); }
            Result r = do_download(c, a.path, a.path, true);
            { std::lock_guard<std::mutex> lk(m_mx); m_active.erase(k); m_pending.erase(k); if (r.ok()) m_errors.erase(k); }
            if (r.ok()) { c.log("download", a.path); std::lock_guard<std::mutex> lk(c.mx); ++c.rep.downloads; }
            else if (r.st != Status::PreconditionFailed && r.st != Status::Cancelled) problem(a.path, r.message.empty() ? status_name(r.st) : r.message);
        });
        downloads.clear();
    };
    for (auto& a : acts) {
        if (c.prog.cancelled()) return Result::fail(Status::Cancelled);
        std::wstring k = key_of(a.path);
        if (a.act == Act::Download) { downloads.push_back(a); continue; }
        flush_downloads();
        switch (a.act) {
            case Act::Forget:
                c.record([this, k] { m_state.erase_base(k); });
                break;
            case Act::Refresh: {
                const BaseEntry* b = m_state.base(k);
                auto li = c.local.find(k);
                if (b && li != c.local.end()) {
                    BaseEntry e = *b;
                    e.size = li->second.size; e.mtime = li->second.mtime; e.fileId = li->second.fileId;
                    c.record([this, e] { m_state.put_base(e); });
                }
                break;
            }
            case Act::Adopt: {
                const RemoteItem* r = m_state.remote(k);
                RemoteItem ri = r ? *r : RemoteItem{};
                record_local(c, a.path, r ? &ri : nullptr, nullptr, 0);
                break;
            }
            case Act::MkdirLocal:
                if (make_dirs(full(a.path))) {
                    c.log("mkdir-local", a.path);
                    ++c.rep.dirs;
                    const RemoteItem* r = m_state.remote(k);
                    RemoteItem ri = r ? *r : RemoteItem{};
                    record_local(c, a.path, r ? &ri : nullptr, nullptr, 0);
                } else problem(a.path, L"The folder can't be created.");
                break;
            case Act::TypeClash: {
                SYSTEMTIME now;
                GetLocalTime(&now);
                RelPath aside = join_path(parent_of(a.path), syncpath::conflict_name(leaf_of(a.path), m_opt.deviceName, now));
                if (GetFileAttributesW(full(a.path).c_str()) != INVALID_FILE_ATTRIBUTES &&
                    !MoveFileExW(full(a.path).c_str(), full(aside).c_str(), 0)) { c.rep.again = true; break; }
                c.movedAway.insert(k);
                c.log("type-clash", aside, a.path);
                { std::lock_guard<std::mutex> lk(m_mx); m_status.conflicts.push_back(aside); }
                c.record([this, k] { m_state.erase_base(k); });
                c.rep.again = true;
                const RemoteItem* r = m_state.remote(k);
                if (r && r->isDir) make_dirs(full(a.path));
                else if (r) do_download(c, a.path, a.path, true);
                break;
            }
            case Act::MoveLocal: {
                std::wstring fk = key_of(a.from);
                const BaseEntry* b = m_state.base(fk);
                auto li = c.local.find(fk);
                Identity now = identity_of(full(a.from));
                bool same = b && li != c.local.end() && now.exists && now.size == b->size && now.mtime == b->mtime;
                if (same && identity_of(full(a.path)).exists == false) {
                    make_dirs(full(parent_of(a.path)));
                    if (MoveFileExW(full(a.from).c_str(), full(a.path).c_str(), 0)) {
                        c.log("move-local", a.path, a.from);
                        ++c.rep.moves;
                        BaseEntry e = *b;
                        e.path = a.path;
                        if (const RemoteItem* r = m_state.remote(k)) e.rev = r->rev;
                        c.record([this, e, fk] { m_state.erase_base(fk); m_state.put_base(e); });
                        break;
                    }
                }
                // can't move it: fetch it under the new name; the old one goes next pass
                Result r = do_download(c, a.path, a.path, true);
                if (r.ok()) { c.log("download", a.path); ++c.rep.downloads; }
                c.rep.again = true;
                break;
            }
            case Act::Conflict: {
                { std::lock_guard<std::mutex> lk(m_mx); m_active.insert(k); }
                Result r = do_conflict(c, a);
                { std::lock_guard<std::mutex> lk(m_mx); m_active.erase(k); m_pending.erase(k); }
                if (!r.ok() && r.st != Status::PreconditionFailed) problem(a.path, r.message);
                break;
            }
            case Act::DeleteLocal: {
                const BaseEntry* b = m_state.base(k);
                Identity now = identity_of(full(a.path));
                if (!now.exists) { c.record([this, k] { m_state.erase_base(k); }); break; }
                if (!b || now.size != b->size || now.mtime != b->mtime) { c.rep.again = true; break; }   // edited meanwhile: upload wins next pass
                if (recycle(full(a.path), m_opt.trashDir, a.path)) {
                    c.log("delete-local", a.path);
                    ++c.rep.localDeletes;
                    c.record([this, k] { m_state.erase_base(k); });
                } else problem(a.path, L"The file couldn't be moved to the Recycle Bin.");
                break;
            }
            case Act::RmdirLocal:
                if (RemoveDirectoryW(full(a.path).c_str())) { c.log("rmdir-local", a.path); ++c.rep.dirs; }
                c.record([this, k] { m_state.erase_base(k); });
                break;
            default: break;
        }
        { std::lock_guard<std::mutex> lk(m_mx); m_pending.erase(k); }
    }
    flush_downloads();
    return {};
}

Result Engine::run_remote_phase(Ctx& c, std::vector<Action>& acts) {
    if (acts.empty()) return {};
    Result r = m_backend->begin(c.prog);
    if (!r.ok()) return r;
    c.defer = c.caps.transactional;
    std::vector<Action> uploads;
    Result fatal;
    auto stop_on = [&](const Result& x) {
        if (x.st == Status::AuthFailed || x.st == Status::Offline || x.st == Status::Locked || x.st == Status::Cancelled) {
            std::lock_guard<std::mutex> lk(c.mx);
            if (fatal.ok()) fatal = x;
            return true;
        }
        return false;
    };
    auto flush_uploads = [&] {
        parallel(uploads, std::max(1, std::min(c.caps.parallelism, 4)), [&](Action& a) {
            if (c.prog.cancelled() || !fatal.ok()) return;
            std::wstring k = key_of(a.path);
            { std::lock_guard<std::mutex> lk(m_mx); m_active.insert(k); }
            Result x = do_upload(c, a);
            { std::lock_guard<std::mutex> lk(m_mx); m_active.erase(k); m_pending.erase(k); if (x.ok()) m_errors.erase(k); }
            if (x.ok()) { c.log("upload", a.path); std::lock_guard<std::mutex> lk(c.mx); ++c.rep.uploads; }
            else if (!stop_on(x) && x.st != Status::PreconditionFailed) problem(a.path, x.message.empty() ? status_name(x.st) : x.message);
        });
        uploads.clear();
    };
    for (auto& a : acts) {
        if (c.prog.cancelled()) { m_backend->abort(); return Result::fail(Status::Cancelled); }
        if (!fatal.ok()) break;
        std::wstring k = key_of(a.path);
        if (a.act == Act::Upload) { uploads.push_back(a); continue; }
        flush_uploads();
        switch (a.act) {
            case Act::MkdirRemote: {
                Result x = m_backend->make_dir(a.path, c.prog);
                if (x.ok()) {
                    c.log("mkdir-remote", a.path);
                    ++c.rep.dirs;
                    RemoteItem ri; ri.path = a.path; ri.isDir = true; ri.rev = "dir";
                    BaseEntry e; e.path = a.path; e.isDir = true; e.rev = "dir";
                    c.record([this, e, ri] { m_state.put_base(e); m_state.put_remote(ri); });
                } else if (!stop_on(x)) problem(a.path, x.message);
                break;
            }
            case Act::MoveRemote: {
                RemoteEntry out;
                Result x = m_backend->move(a.from, a.path, a.expectedRev, out, c.prog);
                if (x.ok()) {
                    c.log("move-remote", a.path, a.from);
                    ++c.rep.moves;
                    std::wstring fk = key_of(a.from);
                    const BaseEntry* b = m_state.base(fk);
                    BaseEntry e = b ? *b : BaseEntry{};
                    auto li = c.local.find(k);
                    e.path = a.path;
                    e.rev = out.rev;
                    if (li != c.local.end()) { e.size = li->second.size; e.mtime = li->second.mtime; e.fileId = li->second.fileId; }
                    RemoteItem ri; ri.path = a.path; ri.size = out.size; ri.rev = out.rev;
                    c.record([this, e, ri, fk] { m_state.erase_base(fk); m_state.erase_remote(fk); m_state.put_base(e); m_state.put_remote(ri); remember_parents(e.path); });
                } else if (!stop_on(x)) {
                    // no server-side move: upload under the new name; the old one goes once that's stored
                    { std::lock_guard<std::mutex> lk(m_mx); m_active.insert(k); }
                    Result u = do_upload(c, { Act::Upload, a.path, {}, "" });
                    { std::lock_guard<std::mutex> lk(m_mx); m_active.erase(k); if (u.ok()) m_errors.erase(k); }
                    if (!u.ok()) {
                        if (!stop_on(u) && u.st != Status::PreconditionFailed) problem(a.path, u.message.empty() ? status_name(u.st) : u.message);
                        c.rep.again = true;
                        break;
                    }
                    c.log("upload", a.path);
                    ++c.rep.uploads;
                    Result d = m_backend->remove(a.from, a.expectedRev, c.prog);
                    std::wstring fk = key_of(a.from);
                    if (d.ok() || d.st == Status::NotFound) c.record([this, fk] { m_state.erase_base(fk); m_state.erase_remote(fk); });
                    else c.rep.again = true;
                }
                break;
            }
            case Act::DeleteRemote: {
                Result x = m_backend->remove(a.path, a.expectedRev, c.prog);
                if (x.ok() || x.st == Status::NotFound) {
                    c.log("delete-remote", a.path);
                    ++c.rep.remoteDeletes;
                    c.record([this, k] { m_state.erase_base(k); m_state.erase_remote(k); });
                } else if (x.st == Status::PreconditionFailed) c.rep.again = true;
                else if (!stop_on(x)) problem(a.path, x.message);
                break;
            }
            case Act::RmdirRemote: {
                Result x = m_backend->remove_dir(a.path, c.prog);
                if (x.ok() || x.st == Status::NotFound) {
                    c.log("rmdir-remote", a.path);
                    c.record([this, k] { m_state.erase_base(k); m_state.erase_remote(k); });
                } else if (x.st == Status::PreconditionFailed) c.rep.again = true;   // not empty there: something new arrived
                else if (!stop_on(x)) problem(a.path, x.message);
                break;
            }
            default: break;
        }
        { std::lock_guard<std::mutex> lk(m_mx); m_pending.erase(k); }
    }
    flush_uploads();
    if (!fatal.ok()) { m_backend->abort(); c.deferred.clear(); return fatal; }
    std::string newCursor;
    r = m_backend->commit(L"sync", newCursor, c.prog);
    if (!r.ok()) {
        c.deferred.clear();
        if (r.st == Status::CommitRaced || r.st == Status::PreconditionFailed) { c.rep.again = true; return {}; }
        return r;
    }
    for (auto& f : c.deferred) f();
    c.deferred.clear();
    c.defer = false;
    if (!newCursor.empty()) {
        m_state.set_value("cursor", newCursor);
        m_state.set_value("token", newCursor);
    } else {
        std::string tok;
        m_backend->touch(tok, c.prog);                    // let the other devices notice
    }
    return {};
}

PassReport Engine::pass(Progress& prog) {
    PassReport rep;
    Ctx c(prog, rep);
    if (m_paused) { set_state(PairState::Paused); return rep; }
    c.caps = m_backend->caps();
    set_state(PairState::Syncing);
    // the folder: there, and still the same one
    std::string id = root_identity(m_opt.root), known = m_state.value("rootId");
    if (id.empty() || (!known.empty() && known != id)) {
        rep.result = Result::fail(Status::NotFound, L"The sync folder is missing or was replaced.");
        set_state(PairState::RootMissing, rep.result.message);
        return rep;
    }
    if (known.empty()) m_state.set_value("rootId", id);
    // downloads a crash or a power cut left half written (an hour old: none of this pass's)
    {
        std::wstring tmpDir = full(L".~sync-tmp");
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((tmpDir + L"\\.~sync-*.tmp").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            uint64_t hourAgo = now_ft() - 3600ull * 10000000;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && ft64(fd.ftLastWriteTime) < hourAgo)
                    DeleteFileW((tmpDir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(f, &fd));
            FindClose(f);
            RemoveDirectoryW(tmpDir.c_str());           // (when empty)
        }
    }
    // rules: built-in, the folder's .syncignore, folders left out
    syncpath::Ignore ignore;
    {
        std::string text;
        HANDLE h = CreateFileW(full(m_opt.ignoreFile).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[65536];
            DWORD got = 0;
            if (ReadFile(h, buf, sizeof(buf), &got, nullptr)) text.assign(buf, got);
            CloseHandle(h);
            ignore.add_lines(from_utf8(text));
        }
    }
    for (auto& x : m_opt.excluded) ignore.add_excluded_folder(x);
    { std::lock_guard<std::mutex> lk(m_mx); m_ignore = ignore; }

    Result r = list_remote(c);
    if (!r.ok()) {
        rep.result = r;
        PairState s = r.st == Status::AuthFailed ? PairState::AuthNeeded : r.st == Status::Offline ? PairState::Offline
                    : r.st == Status::Locked ? PairState::Locked : PairState::Error;
        set_state(s, r.message.empty() ? status_name(r.st) : r.message);
        return rep;
    }
    uint64_t scanStart = now_ft();
    ScanResult scan = scan_local(m_opt.root, ignore);
    if (scan.rootMissing) {
        rep.result = Result::fail(Status::NotFound, L"The sync folder is missing.");
        set_state(PairState::RootMissing, rep.result.message);
        return rep;
    }
    if (scan.rootIncomplete) {                            // (what's missing from a listing isn't deleted)
        rep.result = Result::fail(Status::Fatal, L"The sync folder couldn't be listed completely.");
        set_state(PairState::Error, rep.result.message);
        return rep;
    }
    for (auto& [p, why] : scan.problems) problem(p, why);
    c.local = std::move(scan.items);
    hash_changed(c, c.local);
    if (!c.prog.cancelled()) m_state.set_value("scanned", std::to_string(scanStart));

    // what the scan couldn't see (an unreadable folder, a link): left as it is on both sides
    std::vector<std::wstring> unknown;
    for (auto& u : scan.unknown) unknown.push_back(key_of(u));
    auto frozen = [&](const std::wstring& k) {
        for (auto& u : unknown) if (k == u || under(k, u)) return true;
        return false;
    };
    // left out by the rules: the item, or a folder above it
    auto hidden = [&](const RelPath& path, bool isDir) {
        if (ignore.ignored(path, isDir)) return true;
        for (RelPath p = parent_of(path); !p.empty(); p = parent_of(p))
            if (ignore.ignored(p, true)) return true;
        return false;
    };

    RemoteMap remote;
    BaseMap base;
    {
        std::lock_guard<std::recursive_mutex> lk(m_state.mutex());
        for (auto& [k, it] : m_state.remote_all()) {
            if (hidden(it.path, it.isDir) || frozen(k)) continue;
            if (!syncpath::windows_valid_path(it.path)) { problem(it.path, L"Windows can't hold this name."); continue; }
            remote[k] = it;
        }
        // base items left out by the rules are forgotten: brought back in, the two sides meet
        // as new (same content adopted, different kept both), never as "deleted here"
        std::vector<std::wstring> forget;
        for (auto& [k, b] : m_state.base_all()) {
            if (hidden(b.path, b.isDir)) forget.push_back(k);
            else if (!frozen(k)) base[k] = b;
            else if (std::find(unknown.begin(), unknown.end(), k) != unknown.end() && !c.local.count(k))
                problem(b.path, L"Links aren't synced: what was here stays as it is in the storage.");
        }
        for (auto& k : forget) m_state.erase_base(k);
    }
    for (auto it = c.local.begin(); it != c.local.end();) {
        if (frozen(it->first)) it = c.local.erase(it);
        else ++it;
    }
    add_implied_dirs(remote);
    Plan plan = make_plan(base, c.local, remote, c.caps);
    if (plan.deferred) rep.again = true;

    // the mass-delete guard: many files, and a good share of the pair or simply a lot. A
    // confirmation covers the deletions it was asked for, not more.
    size_t deletes = plan.localDeletes + plan.remoteDeletes;
    int confirm = 0;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        confirm = m_confirm;
        if (confirm == 1 && deletes > m_confirmed) confirm = 0;
        m_confirm = 0;
        m_asked = 0;
        bool many = deletes > m_opt.massDeleteFiles &&
                    ((double)deletes > m_opt.massDeleteShare * (double)plan.files || deletes > m_opt.massDeleteMax);
        if (confirm == 0 && many) {
            m_asked = deletes;
            m_status.state = PairState::ConfirmDeletes;
            m_status.pendingDeletes = deletes;
            m_status.message = std::to_wstring(deletes) + L" files would be deleted.";
            rep.result = Result::fail(Status::Rejected, m_status.message);
            return rep;
        }
    }
    if (confirm == -1) {                                  // "Restore them": deletions become copies back
        for (auto& a : plan.actions) {
            if (a.act == Act::DeleteLocal) { a.act = Act::Upload; a.expectedRev.clear(); }
            else if (a.act == Act::DeleteRemote) a.act = Act::Download;
            else if (a.act == Act::RmdirRemote) a.act = Act::MkdirLocal;
            else if (a.act == Act::RmdirLocal) a.act = Act::MkdirRemote;
        }
        // restoring locally deleted files: the store still has them; uploads need the local copy
        for (auto& a : plan.actions)
            if (a.act == Act::Upload && !c.local.count(key_of(a.path))) a.act = Act::Forget;
        order_actions(plan.actions);
    }

    // what's waiting, for the badges and the status
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_pending.clear();
        m_status.pendingFiles = 0;
        m_status.pendingBytes = 0;
        m_status.pendingDeletes = 0;
        for (auto& a : plan.actions) {
            if (a.act == Act::Forget || a.act == Act::Refresh || a.act == Act::Adopt) continue;
            m_pending.insert(key_of(a.path));
            if (a.act == Act::Upload) {
                ++m_status.pendingFiles;
                auto li = c.local.find(key_of(a.path));
                if (li != c.local.end()) m_status.pendingBytes += li->second.size;
            } else if (a.act == Act::Download || a.act == Act::Conflict) {
                ++m_status.pendingFiles;
                auto ri = remote.find(key_of(a.path));
                if (ri != remote.end()) m_status.pendingBytes += ri->second.size;
            }
        }
    }

    std::vector<Action> localActs, remoteActs;
    for (auto& a : plan.actions) {
        switch (a.act) {
            case Act::MkdirRemote: case Act::MoveRemote: case Act::Upload: case Act::DeleteRemote: case Act::RmdirRemote:
                remoteActs.push_back(a); break;
            default: localActs.push_back(a);
        }
    }
    r = run_local_phase(c, localActs);
    if (r.ok()) r = run_remote_phase(c, remoteActs);
    m_state.flush();
    rep.result = r;
    if (!r.ok()) {
        PairState s = r.st == Status::AuthFailed ? PairState::AuthNeeded : r.st == Status::Offline ? PairState::Offline
                    : r.st == Status::Locked ? PairState::Locked : r.st == Status::Cancelled ? PairState::Idle : PairState::Error;
        set_state(s, r.message.empty() ? status_name(r.st) : r.message);
        return rep;
    }
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_pending.clear();
        if (!rep.again) {
            m_status.state = PairState::UpToDate;
            m_status.message.clear();
            m_status.lastSync = now_ft();
            m_status.pendingFiles = 0;
            m_status.pendingBytes = 0;
        }
    }
    return rep;
}

PassReport Engine::sync(Progress& prog) {
    PassReport total;
    for (int i = 0; i < m_opt.maxPasses; ++i) {
        PassReport r = pass(prog);
        total.uploads += r.uploads; total.downloads += r.downloads; total.localDeletes += r.localDeletes;
        total.remoteDeletes += r.remoteDeletes; total.moves += r.moves; total.conflicts += r.conflicts; total.dirs += r.dirs;
        total.log.insert(total.log.end(), r.log.begin(), r.log.end());
        total.result = r.result;
        total.again = r.again;
        if (!r.result.ok() || !r.again || prog.cancelled()) break;
        // busy files settle before the next try
        bool onlyBusy = !r.uploads && !r.downloads && !r.conflicts && !r.moves && !r.localDeletes && !r.remoteDeletes;
        if (onlyBusy) Sleep(std::min(m_opt.settleMs, 3000));
    }
    if (total.result.ok() && !total.again) set_state(PairState::UpToDate);
    return total;
}

} // namespace sync
