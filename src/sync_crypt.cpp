#include "sync_crypt.h"
#include "crypto_file.h"
#include "crypto_names.h"
#include "sync_paths.h"
#include <map>
#include <mutex>

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

struct HashSink : ByteSink {
    ByteSink& out;
    crypto::Blake2b h;
    explicit HashSink(ByteSink& o) : out(o) {}
    bool write(const void* b, size_t n) override { h.update(b, n); return out.write(b, n); }
};

class EncBackend : public Backend {
public:
    EncBackend(std::unique_ptr<Backend> raw, const synckeys::Vault& v, VaultReopen reopen)
        : m_raw(std::move(raw)), m_vault(v), m_reopen(std::move(reopen)) {
        m_rawCaps = m_raw->caps();
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
        ChangeBatch rb;
        Result r = m_raw->changes(loaded ? cursor : std::string(), rb, prog);
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
        r = translate(rb, out, prog);
        if (r.st == Status::NotFound && !rb.full) {              // a removal we can't place: list it all
            rb = {};
            r = m_raw->changes("", rb, prog);
            if (!r.ok()) return r;
            out = {};
            r = translate(rb, out, prog);
        }
        return r;
    }

    Result changed_since(const std::string& token, bool& changed, std::string& nt, Progress& p) override {
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
        if (Result r = stored_of(p, stored, prog); !r.ok()) return r;
        RemoteEntry e;
        Result r = m_raw->stat(stored, e, prog);
        if (!r.ok()) return r;
        out = e;
        out.path = plain_of_known(p);
        out.hash = {};
        if (!e.isDir && !cfile::plain_size(e.size, out.size)) return Result::fail(Status::Tampered, L"The stored file has an impossible size.");
        return {};
    }

    // ---- transfers
    Result download(const RemoteEntry& e, ByteSink& sink, FileMeta& meta, Progress& prog) override {
        RelPath stored;
        if (Result r = stored_of(e.path, stored, prog); !r.ok()) return r;
        RemoteEntry re = e;
        re.path = stored;
        re.size = cfile::cipher_size(e.size);
        HashSink hs(sink);
        cfile::DecryptingSink ds(hs, keys());
        FileMeta ignored;
        Result r = m_raw->download(re, ds, ignored, prog);
        Result f = ds.finish();
        if (!r.ok()) return f.st == Status::Tampered && ds.has_meta() ? f : r;
        if (!f.ok()) return f;
        crypto::Hash32 got = hs.h.final();
        const cfile::Meta& m = ds.meta();
        if (got != m.fingerprint) return Result::fail(Status::Tampered, L"The file doesn't match its fingerprint.");
        meta.mtime = m.mtime;
        meta.attributes = m.attributes;
        meta.version = m.version;
        meta.plainHash = got;
        return {};
    }

    Result peek_hash(const RemoteEntry& e, ContentHash& out, Progress& prog) override {
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
        r = cfile::read_meta(s.data.data(), s.data.size(), keys(), m);
        if (!r.ok()) return r;
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
        Result r = m_raw->upload(t.stored, es, FileMeta{}, expectedRev, ro, prog);
        if (es.failed()) return Result::fail(Status::PreconditionFailed, L"The file changed while it was read.");
        if (!r.ok()) return r;
        remember(t.plain, t.stored, false);
        out = {};
        out.path = t.plain;
        out.size = m.size;
        out.rev = ro.rev;
        return {};
    }

    Result remove(const RelPath& p, const std::string& expectedRev, Progress& prog) override {
        RelPath stored;
        if (Result r = stored_of(p, stored, prog); !r.ok()) return r;
        Result r = m_raw->remove(stored, expectedRev, prog);
        if (!r.ok()) return r;
        drop_sidecar(stored, prog);
        forget(p, stored);
        return {};
    }

    Result move(const RelPath& from, const RelPath& to, const std::string& expectedRev, RemoteEntry& out, Progress& prog) override {
        if (!m_rawCaps.serverMove) return Result::fail(Status::Unsupported);
        RelPath storedFrom;
        if (Result r = stored_of(from, storedFrom, prog); !r.ok()) return r;
        Target t;
        if (Result r = target_of(to, t, prog); !r.ok()) return r;
        if (Result r = write_sidecars(t, prog); !r.ok()) return r;
        RemoteEntry ro;
        Result r = m_raw->move(storedFrom, t.stored, expectedRev, ro, prog);
        if (!r.ok()) return r;
        drop_sidecar(storedFrom, prog);
        forget(from, storedFrom);
        remember(t.plain, t.stored, false);
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

    Result begin(Progress& p) override { return m_raw->begin(p); }
    Result commit(const std::wstring& s, std::string& nc, Progress& p) override { return m_raw->commit(s, nc, p); }
    void abort() override { m_raw->abort(); }

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

    cfile::KeyLookup keys() {
        return [this](uint32_t v, crypto::Key& k) {
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
        } else if (r.st == Status::Locked) {
            m_lockedOut = r;
        }
        return r;
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

    Result translate(const ChangeBatch& rb, ChangeBatch& out, Progress& prog) {
        // sidecars first: the full names of shortened components
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
        bool refreshed = false;
        for (int round = 0; round < 2; ++round) {
            std::lock_guard<std::mutex> lk(m_mx);
            ChangeBatch o;
            bool unknownVersion = false;
            if (rb.full) { m_byStored.clear(); m_byPlain.clear(); }
            for (auto& e : rb.upserts) {
                if (synckeys::is_vault_path(e.path) || (!e.isDir && is_sidecar_path(e.path))) continue;
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
        return translate(rb, out, prog);
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
};

} // namespace

std::unique_ptr<Backend> make_encrypting(std::unique_ptr<Backend> raw, const synckeys::Vault& vault, VaultReopen reopen) {
    return std::make_unique<EncBackend>(std::move(raw), vault, std::move(reopen));
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
        if (r.ok() && (next.id != pin->pin.id || next.gen != pin->pin.gen || next.version != pin->pin.version)) {
            pin->pin = next;
            pin->pin.save(pin->file);
        }
        return r;
    };
    synckeys::Vault v;
    r = open(*raw, ks, create, v, prog);
    if (!r.ok()) return r;
    synckeys::KeyStore* keys = &ks;
    out = make_encrypting(std::move(raw), v, [keys, open](Backend& b, synckeys::Vault& nv) {
        NoProgress np;
        return open(b, *keys, false, nv, np);
    });
    return {};
}

} // namespace sync
