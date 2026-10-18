#include "sync_crypt.h"
#include "crypto_file.h"
#include "crypto_names.h"
#include "sync_manifest.h"
#include "sync_paths.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace sync {

namespace {

const RelPath kHead = L".ep-vault/head";

std::vector<std::wstring> split(const RelPath& p) {
    std::vector<std::wstring> v;
    size_t i = 0;
    while (i <= p.size()) {
        size_t e = p.find(L'/', i);
        if (e == std::wstring::npos) e = p.size();
        v.push_back(p.substr(i, e - i));
        i = e + 1;
    }
    return v;
}

bool sane_name(const std::string& n) {
    if (n.empty() || n == "." || n == "..") return false;
    for (char c : n) if (c == '/' || c == '\0') return false;
    return true;
}

// ---- retired key versions (docs/sync-review.md, S2). When a vault key version stops being
// current (a device was removed, or the sync password changed), what is on the storage at
// that moment is noted: (stored path, revision) of each file with the lowest content key
// version it may have, and the stored folders. Content under a retired version opens only
// as it was then; a folder name under one only if the folder was there then. A removed
// device knows the retired versions, not the new one, so whatever it writes afterwards is
// refused.
using SnapKey = std::array<uint8_t, 16>;
SnapKey snap_key(const RelPath& stored, const std::string& rev, bool dir) {
    std::string s = to_utf8(stored);
    s.push_back(dir ? 1 : 0);
    if (!dir) s += rev;
    crypto::Hash32 h = crypto::blake2b(s.data(), s.size());
    SnapKey k;
    std::copy(h.begin(), h.begin() + 16, k.begin());
    return k;
}

struct Retired {
    uint32_t below = 0;                      // key versions below this are retired (0: not known yet)
    std::map<SnapKey, uint32_t> files;       // (stored path, rev) -> lowest content key version taken
    std::set<SnapKey> dirs;                  // stored folders

    // "EPS1" | below | files: count, (key, version)... | folders: count, key...
    crypto::Bytes serialize() const {
        crypto::Bytes b = { 'E', 'P', 'S', '1' };
        auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); };
        put32(below);
        put32((uint32_t)files.size());
        for (auto& [k, v] : files) { b.insert(b.end(), k.begin(), k.end()); put32(v); }
        put32((uint32_t)dirs.size());
        for (auto& k : dirs) b.insert(b.end(), k.begin(), k.end());
        return b;
    }
    bool parse(const crypto::Bytes& b) {
        size_t pos = 4;
        bool bad = b.size() < 4 || std::memcmp(b.data(), "EPS1", 4) != 0;
        auto get32 = [&]() -> uint32_t {
            if (bad || b.size() - pos < 4) { bad = true; return 0; }
            uint32_t v = b[pos] | (b[pos + 1] << 8) | (b[pos + 2] << 16) | ((uint32_t)b[pos + 3] << 24);
            pos += 4;
            return v;
        };
        auto getk = [&](SnapKey& k) {
            if (bad || b.size() - pos < 16) { bad = true; return; }
            std::memcpy(k.data(), b.data() + pos, 16);
            pos += 16;
        };
        Retired x;
        x.below = get32();
        uint32_t nf = get32();
        for (uint32_t i = 0; i < nf && !bad; ++i) { SnapKey k; getk(k); x.files[k] = get32(); }
        uint32_t nd = get32();
        for (uint32_t i = 0; i < nd && !bad; ++i) { SnapKey k; getk(k); x.dirs.insert(k); }
        if (bad || pos != b.size()) return false;
        *this = std::move(x);
        return true;
    }
};

bool read_file(const std::wstring& path, crypto::Bytes& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool write_file(const std::wstring& path, const crypto::Bytes& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &n, nullptr) && n == data.size() && FlushFileBuffers(h);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

const wchar_t kRetiredWhy[] = L"Written with a vault key that was retired when a device was removed (or the sync password changed), "
                              L"after it was retired: not taken.";

struct HashSink : ByteSink {
    ByteSink& out;
    crypto::Blake2b h;
    explicit HashSink(ByteSink& o) : out(o) {}
    bool write(const void* b, size_t n) override { h.update(b, n); return out.write(b, n); }
};

// ---- what the manifest and the storage disagree on (docs/sync-format.md, "Manifest")
enum class Odd { Withheld = 1, Replaced, Unlisted };
const wchar_t* odd_why(Odd k) {
    switch (k) {
        case Odd::Withheld: return L"The vault's manifest lists this file, but the storage doesn't have it (withheld, or deleted by "
                                   L"something other than your devices). The copy here is kept.";
        case Odd::Replaced: return L"The file on the storage isn't the version the vault's manifest lists (rolled back, or replaced "
                                   L"by something other than your devices). The copy here is kept.";
        default:            return L"The storage has this file, but the vault's manifest doesn't list it (not written by your "
                                   L"devices, or by an older version of Explorer Plus): not taken.";
    }
}

// how long a disagreement may last before it's reported: a store without transactions
// shows another device's files before its manifest, and the manifest before the old
// shards are gone (EXPLORERPLUS_SYNC_MANIFEST_GRACE, seconds: tests)
ULONGLONG manifest_grace_ms() {
    wchar_t s[16];
    DWORD n = GetEnvironmentVariableW(L"EXPLORERPLUS_SYNC_MANIFEST_GRACE", s, 16);
    if (n > 0 && n < 16) return (ULONGLONG)_wtoi(s) * 1000;
    return 15ull * 60 * 1000;
}

std::wstring dir_of(const std::wstring& file) {
    size_t sl = file.find_last_of(L"\\/");
    return sl == std::wstring::npos ? std::wstring() : file.substr(0, sl + 1);
}

class EncBackend : public Backend {
public:
    EncBackend(std::unique_ptr<Backend> raw, const synckeys::Vault& v, VaultReopen reopen, const std::wstring& retiredFile)
        : m_raw(std::move(raw)), m_vault(v), m_reopen(std::move(reopen)), m_retiredFile(retiredFile) {
        m_rawCaps = m_raw->caps();
        crypto::Bytes b;
        if (m_retiredFile.empty() || !read_file(m_retiredFile, b) || !m_ret.parse(b)) m_ret = {};
        if (m_ret.below == 0 && m_vault.vks.begin()->first >= m_vault.current) {
            m_ret.below = m_vault.current;               // nothing retired yet: nothing to note
            save_retired_locked();
        }
        m_retirePending = m_ret.below < m_vault.current;  // first sight of retired versions, or rotated meanwhile
        // the manifest: signed by this device, so only with its key (a vault from open_vault)
        m_manOn = m_vault.me.valid();
        m_graceMs = manifest_grace_ms();
        if (std::wstring dir = dir_of(m_retiredFile); !dir.empty()) {
            m_mpinFile = dir + L"manifest.pin";
            if (!m_rawCaps.transactional) m_own.file = dir + L"manifest.own";
        }
        m_mpin.load(m_mpinFile);
        m_own.load();
    }

    Caps caps() const override {
        Caps c = m_rawCaps;
        c.hash = HashKind::Blake2b;              // the plaintext BLAKE2b, inside the encrypted meta block
        c.rangeReads = false;
        c.caseSensitive = true;                  // names are encrypted case and all
        c.maxNameLength = 255;                   // longer stored names are shortened
        c.maxPathLength = 32767;
        if (c.maxFileSize != ~0ull) c.maxFileSize = c.maxFileSize / (cfile::kChunk + crypto::kTag) * cfile::kChunk;
        return c;
    }
    Result connect(Progress& p) override { return m_raw->connect(p); }

    // ---- listing
    Result changes(const std::string& cursor, ChangeBatch& out, Progress& prog) override {
        out = {};
        bool loaded;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (!m_lockedOut.ok()) return m_lockedOut;
            loaded = m_loaded;
        }
        // with a manifest, the storage is checked against it as a whole: every listing is full
        ChangeBatch rb;
        Result r = m_raw->changes(loaded && !m_manOn ? cursor : std::string(), rb, prog);
        if (!r.ok()) return r;
        // the vault object changed (a rotation): read it again before anything is written,
        // so uploads and new names move to the new key version now, not whenever this
        // device next meets a file written under it
        for (auto& e : rb.upserts) {
            if (e.isDir || e.path != synckeys::kVaultFileRel) continue;
            bool stale;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                stale = m_vaultListRev.empty() ? e.rev != m_vault.rev : e.rev != m_vaultListRev;
                m_vaultListRev = e.rev;
            }
            if (stale) {
                Result rr = refresh_vault();
                if (rr.st == Status::Locked) return rr;
            }
        }
        if (Result s = settle_retired(prog, &rb); !s.ok()) return s;
        if (m_manOn) {
            ChangeBatch plain;
            r = translate(rb, plain, prog);
            if (r.ok()) r = settle_retired(prog);
            if (r.ok()) r = as_manifest_says(rb, plain, out, prog);
            return r;
        }
        r = translate(rb, out, prog);
        if (r.st == Status::NotFound && !rb.full) {              // a removal we can't place: list it all
            rb = {};
            r = m_raw->changes("", rb, prog);
            if (!r.ok()) return r;
            out = {};
            r = translate(rb, out, prog);
        }
        if (r.ok()) r = settle_retired(prog);                    // a rotation met while translating
        return r;
    }

    Result changed_since(const std::string& token, bool& changed, std::string& nt, Progress& p) override {
        Result r = changed_since_raw(token, changed, nt, p);
        // writes of this device's that its manifest didn't follow yet: the listing writes it
        if (r.ok() && m_manOn) {
            std::lock_guard<std::mutex> lk(m_mx);
            if (!m_own.items.empty()) changed = true;
        }
        return r;
    }
    Result changed_since_raw(const std::string& token, bool& changed, std::string& nt, Progress& p) {
        if (m_rawCaps.changeFeed) return m_raw->changed_since(token, changed, nt, p);
        RemoteEntry e;
        Result r = m_raw->stat(kHead, e, p);
        if (r.st == Status::NotFound) { nt = "none"; changed = token != nt; return {}; }
        if (!r.ok()) return r;
        nt = "h" + e.rev;
        changed = token != nt;
        return {};
    }
    Result touch(std::string& nt, Progress& p) override {
        if (m_rawCaps.changeFeed) return m_raw->touch(nt, p);
        uint8_t rnd[16];
        crypto::random(rnd, 16);
        MemorySource src(std::vector<uint8_t>(rnd, rnd + 16));
        RemoteEntry out;
        Result r = m_raw->upload(kHead, src, FileMeta{}, "*", out, p);
        nt = r.ok() ? "h" + out.rev : std::string();
        return r;
    }

    Result stat(const RelPath& p, RemoteEntry& out, Progress& prog) override {
        RelPath stored;
        Result r = stored_of(p, stored, prog);
        RemoteEntry e;
        if (r.ok()) r = m_raw->stat(stored, e, prog);
        // with a manifest, a file is what it lists
        syncman::Entry x;
        bool listed = m_manOn && expected(p, x);
        if (m_manOn && (r.ok() ? !e.isDir : r.st == Status::NotFound)) {
            if (!listed) return Result::fail(Status::NotFound);
            if (!r.ok() || e.rev != x.rev) {
                out = {};
                out.path = p;
                out.size = x.size;
                out.rev = x.rev;
                return {};
            }
        }
        if (!r.ok()) return r;
        out = e;
        out.path = plain_of_known(p);
        out.hash = {};
        if (!e.isDir && !cfile::plain_size(e.size, out.size)) return Result::fail(Status::Tampered, L"The stored file has an impossible size.");
        return {};
    }

    // ---- transfers
    Result download(const RemoteEntry& e, ByteSink& sink, FileMeta& meta, Progress& prog) override {
        // the version the manifest lists: what arrives must be that one (other revisions,
        // from "restore a version", aren't in it)
        RelPath stored;
        Result sr = stored_of(e.path, stored, prog);
        syncman::Entry x;
        bool listed = m_manOn && expected(e.path, x) && x.rev == e.rev;
        if (!sr.ok()) return listed && sr.st == Status::NotFound ? odd(e.path, Odd::Withheld) : sr;
        RemoteEntry re = e;
        re.path = stored;
        re.size = cfile::cipher_size(e.size);
        HashSink hs(sink);
        uint32_t ver = 0;
        cfile::DecryptingSink ds(hs, keys(&ver));
        FileMeta ignored;
        Result r = m_raw->download(re, ds, ignored, prog);
        Result f = ds.finish();
        if (!r.ok() && listed && (r.st == Status::NotFound || r.st == Status::PreconditionFailed)) return odd(e.path, Odd::Replaced);
        if (!r.ok()) return f.st == Status::Tampered && ds.has_meta() ? f : r;
        if (!f.ok()) return f;
        crypto::Hash32 got = hs.h.final();
        const cfile::Meta& m = ds.meta();
        if (got != m.fingerprint) return Result::fail(Status::Tampered, L"The file doesn't match its fingerprint.");
        if (listed && x.hash_known() && got != x.hash) return odd(e.path, Odd::Replaced, true);
        if (listed) {
            std::lock_guard<std::mutex> lk(m_mx);
            m_badContent.erase(e.path);
        }
        if (Result c = check_retired(stored, e.rev, ver, prog); !c.ok()) return c;
        meta.mtime = m.mtime;
        meta.attributes = m.attributes;
        meta.version = m.version;
        meta.plainHash = got;
        return {};
    }

    Result peek_hash(const RemoteEntry& e, ContentHash& out, Progress& prog) override {
        syncman::Entry x;
        if (m_manOn && expected(e.path, x) && x.rev == e.rev && x.hash_known()) {
            out.kind = HashKind::Blake2b;                   // the manifest says, and a download checks it
            out.bytes = x.hash;
            return {};
        }
        RelPath stored;
        if (Result r = stored_of(e.path, stored, prog); !r.ok()) return r;
        RemoteEntry re = e;
        re.path = stored;
        re.size = cfile::cipher_size(e.size);
        MemorySink s;
        s.limit = cfile::kPrefix;
        Result r = m_rawCaps.rangeReads ? m_raw->download_range(re, 0, cfile::kPrefix, s, prog) : Result::fail(Status::Unsupported);
        if (!r.ok()) {
            s.data.clear();
            FileMeta ignored;
            r = m_raw->download(re, s, ignored, prog);                  // stops after the prefix
            if (s.data.size() < cfile::kPrefix) return r.ok() ? Result::fail(Status::Tampered) : r;
        }
        cfile::Meta m;
        uint32_t ver = 0;
        r = cfile::read_meta(s.data.data(), s.data.size(), keys(&ver), m);
        if (!r.ok()) return r;
        if (Result c = check_retired(stored, e.rev, ver, prog); !c.ok()) return c;
        out.kind = HashKind::Blake2b;
        out.bytes = m.fingerprint;
        return {};
    }

    Result upload(const RelPath& p, ByteSource& in, const FileMeta& meta, const std::string& expectedRev, RemoteEntry& out,
                  Progress& prog) override {
        Target t;
        if (Result r = target_of(p, t, prog); !r.ok()) return r;
        if (Result r = write_sidecars(t, prog); !r.ok()) return r;
        cfile::Meta m;
        m.size = in.size();
        m.mtime = meta.mtime;
        m.attributes = meta.attributes;
        m.version = meta.version;
        m.fingerprint = meta.plainHash;
        crypto::Key vk;
        uint32_t ver;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            ver = m_vault.current;
            m_vault.key(ver, vk);
        }
        cfile::EncryptingSource es(in, vk, ver, m);
        RemoteEntry ro;
        Result r = m_raw->upload(t.stored, es, FileMeta{}, repair_rev(t.plain, expectedRev), ro, prog);
        if (es.failed()) return Result::fail(Status::PreconditionFailed, L"The file changed while it was read.");
        if (!r.ok()) return r;
        remember(t.plain, t.stored, false);
        if (m_manOn) {
            syncman::Entry x;
            x.size = m.size;
            x.version = meta.version;
            x.hash = meta.plainHash;
            x.rev = ro.rev;
            std::lock_guard<std::mutex> lk(m_mx);
            m_own.put(t.plain, x);
        }
        out = {};
        out.path = t.plain;
        out.size = m.size;
        out.rev = ro.rev;
        return {};
    }

    Result remove(const RelPath& p, const std::string& expectedRev, Progress& prog) override {
        RelPath stored;
        if (Result r = stored_of(p, stored, prog); !r.ok()) {
            if (r.st == Status::NotFound) own_removed(p);            // gone already: out of the manifest too
            return r;
        }
        Result r = m_raw->remove(stored, repair_rev(p, expectedRev), prog);
        if (!r.ok()) return r;
        drop_sidecar(stored, prog);
        forget(p, stored);
        own_removed(p);
        return {};
    }

    Result move(const RelPath& from, const RelPath& to, const std::string& expectedRev, RemoteEntry& out, Progress& prog) override {
        if (!m_rawCaps.serverMove) return Result::fail(Status::Unsupported);
        RelPath storedFrom;
        if (Result r = stored_of(from, storedFrom, prog); !r.ok()) return r;
        // content that may be under a retired key version is taken only at its stored path:
        // upload it again (under the current version) instead of moving it
        if (Result r = settle_retired(prog); !r.ok()) return r;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (m_ret.below > 1 && (expectedRev.empty() || expectedRev == "*" || m_ret.files.count(snap_key(storedFrom, expectedRev, false))))
                return Result::fail(Status::Unsupported);
        }
        Target t;
        if (Result r = target_of(to, t, prog); !r.ok()) return r;
        if (Result r = write_sidecars(t, prog); !r.ok()) return r;
        RemoteEntry ro;
        Result r = m_raw->move(storedFrom, t.stored, repair_rev(from, expectedRev), ro, prog);
        if (!r.ok()) return r;
        drop_sidecar(storedFrom, prog);
        forget(from, storedFrom);
        remember(t.plain, t.stored, false);
        if (syncman::Entry x; m_manOn && expected(from, x)) {
            own_removed(from);
            x.rev = ro.rev;
            std::lock_guard<std::mutex> lk(m_mx);
            m_own.put(t.plain, x);
        }
        out = {};
        out.path = t.plain;
        cfile::plain_size(ro.size, out.size);
        out.rev = ro.rev;
        return {};
    }

    Result make_dir(const RelPath& p, Progress& prog) override {
        Target t;
        if (Result r = target_of(p, t, prog); !r.ok()) return r;
        if (Result r = write_sidecars(t, prog); !r.ok()) return r;
        Result r = m_raw->make_dir(t.stored, prog);
        if (r.ok()) remember(t.plain, t.stored, true);
        return r;
    }
    Result remove_dir(const RelPath& p, Progress& prog) override {
        RelPath stored;
        if (Result r = stored_of(p, stored, prog); !r.ok()) return r;
        Result r = m_raw->remove_dir(stored, prog);
        if (r.ok()) { drop_sidecar(stored, prog); forget(p, stored); }
        return r;
    }

    Result begin(Progress& p) override {
        Result r = m_raw->begin(p);
        m_inTx = r.ok();
        return r;
    }

    // the manifest goes with the files: in the same commit where the store has them,
    // otherwise right after them (a conditional write of the root)
    Result commit(const std::wstring& s, std::string& nc, Progress& p) override {
        m_inTx = false;
        bool pending = false;
        if (m_manOn) {
            std::lock_guard<std::mutex> lk(m_mx);
            pending = !m_own.items.empty();
        }
        if (!pending) return m_raw->commit(s, nc, p);
        if (!m_rawCaps.transactional) {
            Result r = m_raw->commit(s, nc, p);
            if (!r.ok()) return r;
            r = write_manifest(p);
            return r.st == Status::PreconditionFailed ? Result{} : r;          // the next listing tries again
        }
        Staged st;
        Result r = stage_manifest(st, p);
        if (r.ok()) for (auto& sp : st.stale) m_raw->remove(sp, "*", p);
        if (!r.ok()) {
            m_raw->abort();
            drop_own();
            return r;
        }
        r = m_raw->commit(s, nc, p);
        if (!r.ok()) { drop_own(); return r; }
        took_manifest(st, p);
        return r;
    }
    void abort() override {
        m_inTx = false;
        m_raw->abort();
        if (m_rawCaps.transactional) drop_own();
    }

    Result list_versions(const RelPath& p, std::vector<RemoteVersion>& out, Progress& prog) override {
        RelPath stored;
        if (Result r = stored_of(p, stored, prog); !r.ok()) return r;
        Result r = m_raw->list_versions(stored, out, prog);
        for (auto& v : out) { uint64_t s = 0; if (cfile::plain_size(v.size, s)) v.size = s; }
        return r;
    }

private:
    struct Node { RelPath plain, stored; bool isDir = false; };
    struct Target {
        RelPath plain, stored;
        std::vector<std::pair<RelPath, std::string>> sidecars;   // stored path, content
        std::vector<std::pair<RelPath, RelPath>> newDirs;        // plain, stored (prefixes made up here)
    };

    // usedVersion: the key version the content asked for
    cfile::KeyLookup keys(uint32_t* usedVersion = nullptr) {
        return [this, usedVersion](uint32_t v, crypto::Key& k) {
            if (usedVersion) *usedVersion = v;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                if (m_vault.key(v, k)) return true;
            }
            refresh_vault();
            std::lock_guard<std::mutex> lk(m_mx);
            return m_vault.key(v, k);
        };
    }

    // the vault again (a new key version). Locked: this device was removed (the vault moved
    // to keys it never got): every pass from then on says so.
    Result refresh_vault() {
        if (!m_reopen) return {};
        synckeys::Vault v;
        Result r = m_reopen(*m_raw, v);
        std::lock_guard<std::mutex> lk(m_mx);
        if (r.ok()) {
            m_vault = v;
            m_names.clear();
            if (m_ret.below < m_vault.current) m_retirePending = true;
        } else if (r.st == Status::Locked) {
            m_lockedOut = r;
        }
        return r;
    }

    void save_retired_locked() {
        if (!m_retiredFile.empty()) write_file(m_retiredFile, m_ret.serialize());
    }

    // a key version was retired since the last note (or this is the first sight of retired
    // ones): note what's on the storage now, from a full listing (`given` when it is one)
    Result settle_retired(Progress& prog, const ChangeBatch* given = nullptr) {
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (!m_retirePending) return {};
        }
        ChangeBatch own;
        if (!given || !given->full) {
            Result r = m_raw->changes("", own, prog);
            if (!r.ok()) return r;
            given = &own;
        }
        load_sidecars(*given, prog);
        std::lock_guard<std::mutex> lk(m_mx);
        bool first = m_ret.below == 0;                   // trust on first use: all of it
        Retired next;
        next.below = m_vault.current;
        for (auto& e : given->upserts) {
            if (synckeys::is_vault_path(e.path) || (!e.isDir && is_sidecar_path(e.path))) continue;
            // the folders on the way (stores without folder objects list only files)
            std::vector<std::wstring> comps = split(e.path);
            RelPath sp;
            for (size_t i = 0; i < comps.size(); ++i) {
                RelPath nsp = join_path(sp, comps[i]);
                if (i + 1 < comps.size() || e.isDir) {
                    SnapKey k = snap_key(nsp, {}, true);
                    uint32_t v;
                    if (first || m_ret.dirs.count(k) || (comp_version_locked(sp, comps[i], v) && v >= m_ret.below)) next.dirs.insert(k);
                }
                sp = nsp;
            }
            if (e.isDir) continue;
            // a file noted before keeps what it was allowed; one written since must be at
            // least the version that was current then (which a device removed before
            // never had)
            SnapKey k = snap_key(e.path, e.rev, false);
            auto f = m_ret.files.find(k);
            next.files[k] = first ? 0 : f != m_ret.files.end() ? f->second : m_ret.below;
        }
        m_ret = std::move(next);
        m_retirePending = false;
        save_retired_locked();
        return {};
    }

    // content under key version `ver` at a stored path, listed with revision `rev`
    Result check_retired(const RelPath& stored, const std::string& rev, uint32_t ver, Progress& prog) {
        if (Result r = settle_retired(prog); !r.ok()) return r;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (ver >= m_ret.below) return {};
        }
        // a retired version: only what was there when it was retired, as it was (the
        // revision now, so an overwrite after the listing isn't taken for the listed one)
        RemoteEntry now;
        Result r = m_raw->stat(stored, now, prog);
        if (!r.ok()) return r.st == Status::NotFound ? Result::fail(Status::Tampered, kRetiredWhy) : r;
        std::lock_guard<std::mutex> lk(m_mx);
        auto f = m_ret.files.find(snap_key(stored, now.rev, false));
        if ((!rev.empty() && now.rev != rev) || f == m_ret.files.end() || ver < f->second) return Result::fail(Status::Tampered, kRetiredWhy);
        return {};
    }

    // the key version of a stored component (m_mx held)
    bool comp_version_locked(const RelPath& storedParent, const std::wstring& comp, uint32_t& v) {
        std::string full = to_utf8(comp);
        if (cnames::is_short(full)) {
            auto f = m_sidecar.find(join_path(storedParent, comp + L".name"));
            if (f == m_sidecar.end() || cnames::shortened(f->second) != full) return false;
            full = f->second;
        }
        uint16_t x;
        if (!cnames::stored_version(full, x)) return false;
        v = x;
        return true;
    }

    // every folder on a stored path (the path itself when it's one) named under a key
    // version that is current, or there when its version was retired (m_mx held)
    bool folders_allowed_locked(const RelPath& stored, bool isDir) {
        if (m_ret.below <= 1) return true;
        std::vector<std::wstring> comps = split(stored);
        RelPath sp;
        for (size_t i = 0; i < comps.size(); ++i) {
            RelPath nsp = join_path(sp, comps[i]);
            if (i + 1 < comps.size() || isDir) {
                uint32_t v;
                if (!comp_version_locked(sp, comps[i], v)) return false;
                if (v < m_ret.below && !m_ret.dirs.count(snap_key(nsp, {}, true))) return false;
            }
            sp = nsp;
        }
        return true;
    }

    // name keys of a version (m_mx held)
    const cnames::NameKeys* names_locked(uint32_t ver) {
        auto f = m_names.find(ver);
        if (f != m_names.end()) return f->second.get();
        crypto::Key vk;
        if (!m_vault.key(ver, vk)) return nullptr;
        return (m_names[ver] = std::make_unique<cnames::NameKeys>(vk)).get();
    }

    // one stored component under a plaintext parent -> the plaintext name
    enum class Dec { Ok, Unknown, Bad, NeedSidecar };
    Dec decrypt_component(const std::wstring& storedParent, const RelPath& plainParent, const std::wstring& comp, std::wstring& name) {
        std::string full = to_utf8(comp);
        if (cnames::is_short(full)) {
            auto f = m_sidecar.find(join_path(storedParent, comp + L".name"));
            if (f == m_sidecar.end()) return Dec::NeedSidecar;
            // the sidecar must be the one this short name was made from: otherwise the
            // storage could swap two long-named siblings' sidecars (both authentic here)
            if (cnames::shortened(f->second) != full) return Dec::Bad;
            full = f->second;
        }
        uint16_t ver;
        if (!cnames::stored_version(full, ver)) return Dec::Bad;
        const cnames::NameKeys* k = names_locked(ver);
        if (!k) return Dec::Unknown;
        std::string n;
        if (!cnames::decrypt(*k, to_utf8(plainParent), full, n) || !sane_name(n)) return Dec::Bad;
        name = from_utf8(n);
        return Dec::Ok;
    }

    // a stored path -> plaintext (m_mx held); records the prefixes it passes through
    Dec decrypt_path(const RelPath& stored, RelPath& plain, bool isDir) {
        auto known = m_byStored.find(stored);
        if (known != m_byStored.end()) { plain = known->second; return Dec::Ok; }
        std::vector<std::wstring> comps = split(stored);
        RelPath sp, pp;
        for (size_t i = 0; i < comps.size(); ++i) {
            RelPath next = join_path(sp, comps[i]);
            auto f = m_byStored.find(next);
            RelPath np;
            if (f != m_byStored.end()) np = f->second;
            else {
                std::wstring name;
                Dec d = decrypt_component(sp, pp, comps[i], name);
                if (d != Dec::Ok) return d;
                np = join_path(pp, name);
                bool last = i + 1 == comps.size();
                if (!last || isDir) remember_locked(np, next, true);
            }
            sp = next;
            pp = np;
        }
        plain = pp;
        return Dec::Ok;
    }

    static bool is_sidecar_path(const RelPath& p) { return cnames::is_sidecar(to_utf8(leaf_of(p))); }

    // the sidecars of a listing: the full names of shortened components
    void load_sidecars(const ChangeBatch& rb, Progress& prog) {
        for (auto& e : rb.upserts) {
            if (e.isDir || synckeys::is_vault_path(e.path) || !is_sidecar_path(e.path)) continue;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                auto f = m_sidecarRev.find(e.path);
                if (f != m_sidecarRev.end() && f->second == e.rev) continue;
            }
            crypto::Bytes b;
            std::string rev;
            if (!synckeys::get_object(*m_raw, e.path, b, &rev, prog).ok()) continue;
            std::lock_guard<std::mutex> lk(m_mx);
            m_sidecar[e.path] = std::string(b.begin(), b.end());
            m_sidecarRev[e.path] = e.rev;
        }
    }

    Result translate(const ChangeBatch& rb, ChangeBatch& out, Progress& prog) {
        load_sidecars(rb, prog);
        bool refreshed = false;
        for (int round = 0; round < 2; ++round) {
            std::lock_guard<std::mutex> lk(m_mx);
            ChangeBatch o;
            bool unknownVersion = false;
            if (rb.full) { m_byStored.clear(); m_byPlain.clear(); m_refused.clear(); }
            for (auto& e : rb.upserts) {
                if (synckeys::is_vault_path(e.path) || (!e.isDir && is_sidecar_path(e.path))) continue;
                // before its names are taken in (they'd be used for new uploads)
                if (!folders_allowed_locked(e.path, e.isDir)) {
                    m_refused.insert(e.path);
                    o.problems.push_back({ e.path, kRetiredWhy });
                    continue;
                }
                RelPath plain;
                Dec d = decrypt_path(e.path, plain, e.isDir);
                if (d == Dec::Unknown && !refreshed) { unknownVersion = true; continue; }
                if (d != Dec::Ok) {
                    o.problems.push_back({ e.path, L"A name on the storage doesn't decrypt: damaged, or not written by this vault's devices." });
                    continue;
                }
                RemoteEntry pe = e;
                pe.path = plain;
                pe.hash = {};
                if (!e.isDir && !cfile::plain_size(e.size, pe.size)) {
                    o.problems.push_back({ plain, L"The stored file has an impossible size (damaged)." });
                    continue;
                }
                remember_locked(plain, e.path, e.isDir);
                o.upserts.push_back(pe);
            }
            for (auto& sp : rb.removals) {
                if (synckeys::is_vault_path(sp)) continue;
                if (is_sidecar_path(sp)) { m_sidecar.erase(sp); m_sidecarRev.erase(sp); continue; }
                if (m_refused.erase(sp)) continue;                             // never passed on
                RelPath plain;
                Dec d = decrypt_path(sp, plain, false);
                if (d == Dec::Unknown && !refreshed) { unknownVersion = true; continue; }
                if (d != Dec::Ok) return Result::fail(Status::NotFound);     // the caller lists everything instead
                forget_locked(plain, sp);
                o.removals.push_back(plain);
            }
            if (unknownVersion && !refreshed && round == 0) {
                refreshed = true;
                m_mx.unlock();
                Result rr = refresh_vault();
                m_mx.lock();
                if (rr.st == Status::Locked) return rr;
                continue;
            }
            o.full = rb.full;
            o.cursor = rb.cursor;
            for (auto& pr : rb.problems) o.problems.push_back(pr);
            out = std::move(o);
            m_loaded = true;
            return {};
        }
        return {};
    }

    void remember_locked(const RelPath& plain, const RelPath& stored, bool isDir) {
        m_byStored[stored] = plain;
        m_byPlain[syncpath::case_key(plain)] = { plain, stored, isDir };
    }
    void remember(const RelPath& plain, const RelPath& stored, bool isDir) {
        std::lock_guard<std::mutex> lk(m_mx);
        remember_locked(plain, stored, isDir);
    }
    void forget_locked(const RelPath& plain, const RelPath& stored) {
        m_byStored.erase(stored);
        auto f = m_byPlain.find(syncpath::case_key(plain));
        if (f != m_byPlain.end() && f->second.stored == stored) m_byPlain.erase(f);
    }
    void forget(const RelPath& plain, const RelPath& stored) {
        std::lock_guard<std::mutex> lk(m_mx);
        forget_locked(plain, stored);
    }

    // the store's tree is known before paths are mapped (a first listing in this process)
    Result ensure_loaded(Progress& prog) {
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (m_loaded) return {};
        }
        ChangeBatch rb, out;
        Result r = m_raw->changes("", rb, prog);
        if (!r.ok()) return r;
        if (Result s = settle_retired(prog, &rb); !s.ok()) return s;
        if (!m_manOn) return translate(rb, out, prog);
        ChangeBatch plain;
        r = translate(rb, plain, prog);
        if (r.ok()) r = as_manifest_says(rb, plain, out, prog);
        return r;
    }

    // ---- the manifest (sync_manifest.h)
    struct Anomaly { Odd kind = Odd::Withheld; std::string listedRev; ULONGLONG since = 0; };
    struct Staged {
        syncman::Manifest base, next;
        syncman::Encoded enc;
        uint32_t placement = 0;
        std::string rootRev;
        std::vector<RelPath> stale;              // shards of the old root the new one doesn't name
    };

    // what the manifest says of a path, with this device's writes not in it yet
    bool expected(const RelPath& p, syncman::Entry& x) {
        std::lock_guard<std::mutex> lk(m_mx);
        auto o = m_own.items.find(p);
        if (o != m_own.items.end()) {
            if (o->second.first) return false;
            x = o->second.second;
            return true;
        }
        auto f = m_man.files.find(p);
        if (f == m_man.files.end()) return false;
        x = f->second;
        return true;
    }
    void own_removed(const RelPath& p) {
        syncman::Entry x;
        if (!m_manOn || !expected(p, x)) return;
        std::lock_guard<std::mutex> lk(m_mx);
        m_own.removed(p, x);
    }
    void drop_own() {
        std::lock_guard<std::mutex> lk(m_mx);
        m_own.items.clear();
    }

    // a disagreement met by a transfer: quiet while it may still be another device's write
    // under way, Tampered after that
    Result odd(const RelPath& p, Odd kind, bool content = false) {
        std::lock_guard<std::mutex> lk(m_mx);
        ULONGLONG now = GetTickCount64();
        auto a = m_anom.find(p);
        ULONGLONG since = a != m_anom.end() ? a->second.since : now;
        if (content) {                           // the bytes, not the listing: remembered across listings
            auto b = m_badContent.find(p);
            if (b == m_badContent.end()) b = m_badContent.insert({ p, Anomaly{ kind, {}, now } }).first;
            since = std::min(since, b->second.since);
        } else if (a == m_anom.end()) {
            m_anom[p] = Anomaly{ kind, {}, now };
        }
        if (now - since >= m_graceMs) return Result::fail(Status::Tampered, odd_why(kind));
        return Result::fail(Status::PreconditionFailed, odd_why(kind));
    }

    // a write over a disagreement that has stood past the grace period: against what the
    // storage has, so the copy here repairs it (the engine only knows the manifest's version)
    std::string repair_rev(const RelPath& p, const std::string& expectedRev) {
        if (!m_manOn) return expectedRev;
        syncman::Entry x;
        bool listed = expected(p, x);
        std::lock_guard<std::mutex> lk(m_mx);
        auto a = m_anom.find(p);
        if (a == m_anom.end() || GetTickCount64() - a->second.since < m_graceMs) return expectedRev;
        if (a->second.kind == Odd::Unlisted) return expectedRev.empty() ? a->second.listedRev : expectedRev;
        if (!listed || expectedRev != x.rev) return expectedRev;
        return a->second.kind == Odd::Withheld ? std::string() : a->second.listedRev;
    }

    bool is_member(const std::string& id) {
        std::lock_guard<std::mutex> lk(m_mx);
        for (auto& m : m_vault.members) if (m.id == id) return true;
        return false;
    }

    // the root and its shards -> m_man. rb: the listing they're from (revisions for stores
    // that read by revision); null: read by path.
    Result take_root(const crypto::Bytes& b, const std::string& rev, const ChangeBatch* rb, Progress& prog) {
        std::string pinHash;
        uint64_t pinSeq;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            pinHash = m_mpin.hash;
            pinSeq = m_mpin.seq;
        }
        // the very root this device took before is taken again, whoever signed it (a device
        // since removed signed it while it was a member)
        bool tookBefore = !pinHash.empty() && crypto::hex(crypto::blake2b(b.data(), b.size())) == pinHash;
        std::string signer = syncman::root_signer(b);
        if (!tookBefore && !signer.empty() && !is_member(signer)) refresh_vault();      // a device paired since
        std::vector<synckeys::Ring::Member> members;
        std::vector<std::string> removed;
        std::string vaultId;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            members = m_vault.members;
            removed = m_vault.removed;
            vaultId = m_vault.id;
        }
        syncman::MemberLookup member = [&](const std::string& id) -> const synckeys::Ring::Member* {
            for (auto& m : members) if (m.id == id) return &m;
            return nullptr;
        };
        syncman::RemovedLookup gone = [&](const std::string& id) { return std::find(removed.begin(), removed.end(), id) != removed.end(); };
        syncman::Root root;
        Result r = syncman::read_root(b, vaultId, keys(), member, gone, root, tookBefore);
        if (!r.ok()) return r;
        if (root.seq < pinSeq)
            return Result::fail(Status::Tampered, L"The vault's manifest on the storage is older than one this device already saw (rolled back).");
        // the shards: the ones already read are kept
        std::map<crypto::Hash32, const crypto::Bytes*> had;
        std::vector<crypto::Bytes> old;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            old = m_shardBytes;
        }
        for (auto& s : old) had[crypto::blake2b(s.data(), s.size())] = &s;
        std::map<RelPath, const RemoteEntry*> listed;
        if (rb)
            for (auto& e : rb->upserts)
                if (!e.isDir && e.path.compare(0, wcslen(syncman::kShardDir), syncman::kShardDir) == 0) listed[e.path] = &e;
        std::vector<crypto::Bytes> shards(root.shards.size());
        std::map<std::wstring, syncman::Entry> files;
        for (size_t i = 0; i < root.shards.size(); ++i) {
            auto h = had.find(root.shards[i]);
            if (h != had.end()) shards[i] = *h->second;
            else {
                RemoteEntry se;
                se.path = syncman::shard_rel(root.shards[i]);
                if (auto l = listed.find(se.path); l != listed.end()) se = *l->second;
                MemorySink s;
                FileMeta ignored;
                r = m_raw->download(se, s, ignored, prog);
                if (r.st == Status::NotFound) return Result::fail(Status::NotFound, L"A part of the vault's manifest is missing from the storage.");
                if (!r.ok()) return r;
                shards[i] = std::move(s.data);
            }
            r = syncman::read_shard(shards[i], root, i, vaultId, keys(), files);
            if (!r.ok()) return r;
        }
        std::lock_guard<std::mutex> lk(m_mx);
        m_man.seq = root.seq;
        m_man.files = std::move(files);
        m_rootRev = rev;
        m_placement = root.placement;
        m_shardBytes = std::move(shards);
        m_manRead = true;
        m_takeListing = false;
        if (root.seq != m_mpin.seq || root.hash != m_mpin.hash) {
            m_mpin.seq = root.seq;
            m_mpin.hash = root.hash;
            m_mpin.save(m_mpinFile);
        }
        return {};
    }

    // the manifest as the listing has it
    Result load_root(const ChangeBatch& rb, Progress& prog) {
        const RemoteEntry* re = nullptr;
        bool files = false;
        for (auto& e : rb.upserts) {
            if (e.isDir) continue;
            if (e.path == syncman::kRootRel) re = &e;
            else if (!synckeys::is_vault_path(e.path) && !is_sidecar_path(e.path)) files = true;
        }
        std::lock_guard<std::mutex> lk(m_mx);
        if (!re) {
            // none: a pair that saw one, or a vault that says it keeps one, has lost it
            if (m_mpin.seq > 0) return Result::fail(Status::Tampered, L"The vault's manifest is missing from the storage (this device saw it before).");
            if (m_vault.manifest && files) return Result::fail(Status::Tampered, L"The vault's manifest is missing from the storage.");
            m_man = {};
            m_rootRev.clear();
            m_shardBytes.clear();
            m_manRead = true;
            m_takeListing = files;           // a vault from before manifests: what's there now is the start
            return {};
        }
        if (m_manRead && re->rev == m_rootRev && !m_rootRev.empty()) return {};
        RemoteEntry root = *re;
        m_mx.unlock();
        Result r;
        for (int attempt = 0; attempt < 3; ++attempt) {
            MemorySink s;
            FileMeta ignored;
            r = m_raw->download(root, s, ignored, prog);
            if (r.ok()) r = take_root(s.data, root.rev, attempt == 0 ? &rb : nullptr, prog);
            if (r.st != Status::NotFound) break;
            // a shard gone: another device wrote a newer root meanwhile (and removed the
            // old one's shards), or the storage withholds it
            RemoteEntry now;
            Result st = m_raw->stat(syncman::kRootRel, now, prog);
            if (!st.ok() || now.rev == root.rev) { r = Result::fail(Status::Tampered, r.message); break; }
            root = now;
        }
        m_mx.lock();
        return r;
    }

    // the plaintext listing as the manifest says: its files at its revisions (the ones the
    // storage withholds or shows otherwise too, so they aren't deleted or replaced here),
    // files it doesn't list left out; the disagreements reported once they've stood a while
    Result as_manifest_says(const ChangeBatch& rb, const ChangeBatch& plain, ChangeBatch& out, Progress& prog) {
        out = {};
        if (Result r = load_root(rb, prog); !r.ok()) return r;
        bool take;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            take = m_takeListing;
            if (take) {
                m_man = {};
                for (auto& e : plain.upserts) {
                    if (e.isDir) continue;
                    syncman::Entry x;
                    x.size = e.size;
                    x.rev = e.rev;
                    m_man.files[e.path] = x;
                }
            }
            std::map<RelPath, const RemoteEntry*> listed;
            for (auto& e : plain.upserts) {
                if (e.isDir) out.upserts.push_back(e);
                else listed[e.path] = &e;
            }
            std::map<RelPath, syncman::Entry> want = m_man.files;
            for (auto& [p, it] : m_own.items) {
                if (it.first) want.erase(p);
                else want[p] = it.second;
            }
            ULONGLONG now = GetTickCount64();
            std::map<RelPath, Anomaly> next;
            auto note = [&](const RelPath& p, Odd kind, const std::string& listedRev) {
                Anomaly a{ kind, listedRev, now };
                auto f = m_anom.find(p);
                if (f != m_anom.end() && f->second.kind == kind && f->second.listedRev == listedRev) a.since = f->second.since;
                next[p] = a;
            };
            for (auto& [p, x] : want) {
                auto l = listed.find(p);
                if (l != listed.end() && l->second->rev == x.rev) { out.upserts.push_back(*l->second); continue; }
                RemoteEntry pe;
                pe.path = p;
                pe.size = x.size;
                pe.rev = x.rev;
                out.upserts.push_back(pe);
                note(p, l == listed.end() ? Odd::Withheld : Odd::Replaced, l == listed.end() ? std::string() : l->second->rev);
            }
            for (auto& [p, e] : listed)
                if (!want.count(p)) note(p, Odd::Unlisted, e->rev);
            m_anom = std::move(next);
            for (auto& [p, a] : m_anom)
                if (now - a.since >= m_graceMs) out.problems.push_back({ p, odd_why(a.kind) });
            for (auto& [p, a] : m_badContent)
                if (!m_anom.count(p) && now - a.since >= m_graceMs) out.problems.push_back({ p, odd_why(a.kind) });
            for (auto& pr : plain.problems) out.problems.push_back(pr);
            out.full = true;
            out.cursor = plain.cursor;
            m_loaded = true;
        }
        // the first manifest of a vault from before them; this device's writes a store
        // without transactions took while its manifest didn't follow (a crash, offline)
        bool pending;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            pending = !m_own.items.empty() && !m_rawCaps.transactional;
        }
        // (inside a transaction, its commit writes it)
        if ((take || pending) && !m_inTx) {
            Result r = write_manifest(prog);
            if (!r.ok() && r.st != Status::PreconditionFailed) return r;
            std::string nt;
            if (r.ok() && !m_rawCaps.changeFeed) touch(nt, prog);         // the other devices look
        }
        return {};
    }

    // the next manifest's objects on the store (in the open transaction, if the store has
    // them): new shards, then the root over the one read (conditional)
    Result stage_manifest(Staged& st, Progress& prog) {
        std::vector<crypto::Bytes> prevShards;
        synckeys::DeviceKey me;
        std::string vaultId, baseRev;
        uint32_t version;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            st.base = m_man;
            st.next = m_man;
            syncman::fold(st.next, m_own);
            st.next.seq = m_man.seq + 1;
            st.placement = m_placement;
            prevShards = m_shardBytes;
            baseRev = m_rootRev;
            me = m_vault.me;
            vaultId = m_vault.id;
            version = m_vault.current;
        }
        syncman::Previous prev{ &st.base, st.placement, prevShards };
        Result r = syncman::encode(st.next, vaultId, keys(), version, me, prevShards.empty() ? nullptr : &prev, st.enc);
        if (!r.ok()) return r;
        if (prevShards.empty() || prevShards.size() != st.enc.shards.size()) st.placement = version;
        std::set<crypto::Hash32> old, now(st.enc.hashes.begin(), st.enc.hashes.end());
        for (auto& s : prevShards) old.insert(crypto::blake2b(s.data(), s.size()));
        for (size_t i = 0; i < st.enc.shards.size(); ++i) {
            if (old.count(st.enc.hashes[i])) continue;
            MemorySource src(st.enc.shards[i]);
            RemoteEntry out;
            r = m_raw->upload(syncman::shard_rel(st.enc.hashes[i]), src, FileMeta{}, "*", out, prog);
            if (!r.ok()) return r;
        }
        MemorySource src(st.enc.root);
        RemoteEntry out;
        r = m_raw->upload(syncman::kRootRel, src, FileMeta{}, baseRev, out, prog);
        if (!r.ok()) return r;
        st.rootRev = out.rev;
        for (auto& h : old)
            if (!now.count(h)) st.stale.push_back(syncman::shard_rel(h));
        return {};
    }

    // written: the manifest this device now has; the vault says it keeps one
    void took_manifest(const Staged& st, Progress& prog) {
        synckeys::Vault v;
        bool mark;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_man = st.next;
            m_rootRev = st.rootRev;
            m_shardBytes = st.enc.shards;
            m_placement = st.placement;
            m_manRead = true;
            m_takeListing = false;
            m_own.clear();
            m_mpin.seq = st.next.seq;
            m_mpin.hash = crypto::hex(crypto::blake2b(st.enc.root.data(), st.enc.root.size()));
            m_mpin.save(m_mpinFile);
            mark = !m_vault.manifest;
            if (mark) v = m_vault;
        }
        if (mark && synckeys::mark_manifest(*m_raw, v, prog).ok()) {
            std::lock_guard<std::mutex> lk(m_mx);
            m_vault.manifest = true;
        }
    }

    // a manifest write of its own: one transaction where the store has them, otherwise the
    // conditional root write, again over a newer root another device wrote meanwhile
    Result write_manifest(Progress& prog) {
        for (int attempt = 0; attempt < 6; ++attempt) {
            Staged st;
            if (m_rawCaps.transactional) {
                Result r = m_raw->begin(prog);
                if (!r.ok()) return r;
                r = stage_manifest(st, prog);
                if (r.ok()) for (auto& sp : st.stale) m_raw->remove(sp, "*", prog);
                if (!r.ok()) { m_raw->abort(); return r; }
                std::string cursor;
                r = m_raw->commit(L"sync manifest", cursor, prog);
                if (r.st == Status::CommitRaced) r.st = Status::PreconditionFailed;
                if (!r.ok()) return r;                    // the next listing reads the newer one
                took_manifest(st, prog);
                return {};
            }
            Result r = stage_manifest(st, prog);
            if (r.st == Status::PreconditionFailed) {
                crypto::Bytes b;
                std::string rev;
                Result g = synckeys::get_object(*m_raw, syncman::kRootRel, b, &rev, prog);
                if (g.st == Status::NotFound) {
                    std::lock_guard<std::mutex> lk(m_mx);
                    if (m_mpin.seq > 0) return Result::fail(Status::Tampered, L"The vault's manifest is missing from the storage (this device saw it before).");
                    m_man = {};
                    m_rootRev.clear();
                    m_shardBytes.clear();
                    continue;
                }
                if (!g.ok()) return g;
                g = take_root(b, rev, nullptr, prog);
                if (!g.ok() && g.st != Status::NotFound) return g;
                std::lock_guard<std::mutex> lk(m_mx);
                if (m_own.items.empty()) return {};      // another device's first manifest: nothing of ours to add
                continue;
            }
            if (!r.ok()) return r;
            took_manifest(st, prog);
            for (auto& sp : st.stale) m_raw->remove(sp, "*", prog);
            return {};
        }
        return Result::fail(Status::PreconditionFailed, L"Other devices kept writing the vault's manifest.");
    }

    RelPath plain_of_known(const RelPath& p) {
        std::lock_guard<std::mutex> lk(m_mx);
        auto f = m_byPlain.find(syncpath::case_key(p));
        return f == m_byPlain.end() ? p : f->second.plain;
    }

    // an existing item's stored path
    Result stored_of(const RelPath& p, RelPath& stored, Progress& prog) {
        if (Result r = ensure_loaded(prog); !r.ok()) return r;
        std::lock_guard<std::mutex> lk(m_mx);
        auto f = m_byPlain.find(syncpath::case_key(p));
        if (f == m_byPlain.end()) return Result::fail(Status::NotFound);
        stored = f->second.stored;
        return {};
    }

    // where a path goes: existing prefixes keep their stored names, new components are
    // encrypted with the current vault key (shortened past the store's name limit)
    Result target_of(const RelPath& p, Target& t, Progress& prog) {
        if (Result r = ensure_loaded(prog); !r.ok()) return r;
        std::lock_guard<std::mutex> lk(m_mx);
        auto exact = m_byPlain.find(syncpath::case_key(p));
        if (exact != m_byPlain.end()) { t.plain = exact->second.plain; t.stored = exact->second.stored; return {}; }
        const cnames::NameKeys* k = names_locked(m_vault.current);
        if (!k) return Result::fail(Status::Locked, L"The vault key is missing.");
        std::vector<std::wstring> comps = split(p);
        RelPath pp, sp;
        for (size_t i = 0; i < comps.size(); ++i) {
            RelPath np = join_path(pp, comps[i]);
            auto f = m_byPlain.find(syncpath::case_key(np));
            if (f != m_byPlain.end()) { pp = f->second.plain; sp = f->second.stored; continue; }
            std::string name = to_utf8(comps[i]);
            if (!sane_name(name)) return Result::fail(Status::InvalidName);
            std::string enc = cnames::encrypt(*k, (uint16_t)m_vault.current, to_utf8(pp), name);
            std::wstring comp;
            if ((int)enc.size() > m_rawCaps.maxNameLength) {
                std::string sh = cnames::shortened(enc);
                comp = from_utf8(sh);
                t.sidecars.push_back({ join_path(sp, comp + L".name"), enc });
            } else comp = from_utf8(enc);
            RelPath nsp = join_path(sp, comp);
            if (i + 1 < comps.size()) t.newDirs.push_back({ np, nsp });
            pp = np;
            sp = nsp;
        }
        if ((int)sp.size() > m_rawCaps.maxPathLength) return Result::fail(Status::TooLarge, L"The path is too long for the storage once encrypted.");
        t.plain = pp;
        t.stored = sp;
        return {};
    }

    Result write_sidecars(const Target& t, Progress& prog) {
        for (auto& [path, content] : t.sidecars) {
            MemorySource src(std::vector<uint8_t>(content.begin(), content.end()));
            RemoteEntry out;
            Result r = m_raw->upload(path, src, FileMeta{}, "*", out, prog);
            if (!r.ok()) return r;
            std::lock_guard<std::mutex> lk(m_mx);
            m_sidecar[path] = content;
            m_sidecarRev[path] = out.rev;
        }
        std::lock_guard<std::mutex> lk(m_mx);
        for (auto& [plain, stored] : t.newDirs) remember_locked(plain, stored, true);
        return {};
    }

    void drop_sidecar(const RelPath& stored, Progress& prog) {
        if (!cnames::is_short(to_utf8(leaf_of(stored)))) return;
        RelPath sc = stored + L".name";
        m_raw->remove(sc, "*", prog);
        std::lock_guard<std::mutex> lk(m_mx);
        m_sidecar.erase(sc);
        m_sidecarRev.erase(sc);
    }

    std::unique_ptr<Backend> m_raw;
    Caps m_rawCaps;
    std::mutex m_mx;
    synckeys::Vault m_vault;
    VaultReopen m_reopen;
    Result m_lockedOut;                    // refresh_vault found this device locked out
    std::string m_vaultListRev;            // the vault object's revision in the last listing
    std::map<uint32_t, std::unique_ptr<cnames::NameKeys>> m_names;
    std::map<RelPath, RelPath> m_byStored;               // stored path -> plaintext
    std::map<std::wstring, Node> m_byPlain;              // case key of the plaintext -> both
    std::map<RelPath, std::string> m_sidecar, m_sidecarRev;
    bool m_loaded = false;
    std::wstring m_retiredFile;            // where m_ret is kept (empty: in memory only)
    Retired m_ret;
    bool m_retirePending = false;          // a version was retired: note the storage at the next listing
    std::set<RelPath> m_refused;           // stored paths refused for a retired folder name
    // the manifest
    bool m_manOn = false;
    std::atomic<bool> m_inTx{ false };     // between begin and commit / abort
    ULONGLONG m_graceMs = 0;
    syncman::Manifest m_man;               // as the root last read or written says
    bool m_manRead = false;
    bool m_takeListing = false;            // no manifest yet in a vault from before them: the listing is the start
    std::string m_rootRev;                 // the root's revision on the store ("": none)
    uint32_t m_placement = 0;
    std::vector<crypto::Bytes> m_shardBytes;   // the root's shards, by index (kept when unchanged)
    std::wstring m_mpinFile;
    syncman::Pin m_mpin;
    syncman::OwnLog m_own;                 // this device's writes not in the manifest yet
    std::map<RelPath, Anomaly> m_anom;     // plaintext path -> what the listing and the manifest disagree on
    std::map<RelPath, Anomaly> m_badContent;   // downloads that weren't what the manifest lists
};

} // namespace

std::unique_ptr<Backend> make_encrypting(std::unique_ptr<Backend> raw, const synckeys::Vault& vault, VaultReopen reopen,
                                         const std::wstring& retiredFile) {
    return std::make_unique<EncBackend>(std::move(raw), vault, std::move(reopen), retiredFile);
}

Result open_pair_backend(const std::wstring& typeId, const BackendConfig& cfg, synckeys::KeyStore& ks, bool create,
                         std::unique_ptr<Backend>& out, Progress& prog, const std::wstring& pinFile) {
    std::wstring why;
    std::unique_ptr<Backend> raw = create_raw_backend(typeId, cfg, &why);
    if (!raw) return Result::fail(Status::Fatal, why);
    raw = make_retrying(std::move(raw));
    Result r = raw->connect(prog);
    if (!r.ok()) return r;
    // the pin: every open of this pair's vault checks it and moves it on
    struct Pin { std::mutex mx; std::wstring file; synckeys::VaultPin pin; };
    auto pin = std::make_shared<Pin>();
    pin->file = pinFile;
    if (!pinFile.empty()) pin->pin.load(pinFile);
    auto open = [pin](Backend& b, synckeys::KeyStore& k, bool cr, synckeys::Vault& v, Progress& p) {
        std::lock_guard<std::mutex> lk(pin->mx);
        if (pin->file.empty()) return synckeys::open_vault(b, k, cr, v, p);
        synckeys::VaultPin next = pin->pin;
        Result r = synckeys::open_vault(b, k, cr, v, p, &next);
        if (r.ok() && (next.id != pin->pin.id || next.gen != pin->pin.gen || next.version != pin->pin.version || next.hash != pin->pin.hash)) {
            pin->pin = next;
            pin->pin.save(pin->file);
        }
        return r;
    };
    synckeys::Vault v;
    r = open(*raw, ks, create, v, prog);
    if (!r.ok()) return r;
    synckeys::KeyStore* keys = &ks;
    // the retired key versions' note beside the pin
    std::wstring retired;
    if (size_t sl = pinFile.find_last_of(L"\\/"); sl != std::wstring::npos) retired = pinFile.substr(0, sl + 1) + L"retired.dat";
    out = make_encrypting(std::move(raw), v, [keys, open](Backend& b, synckeys::Vault& nv) {
        NoProgress np;
        return open(b, *keys, false, nv, np);
    }, retired);
    return {};
}

} // namespace sync
