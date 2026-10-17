#pragma once
// FakeBackend: an in-memory store for the engine tests. Plaintext, with the file meta kept
// beside each object; caps adjustable (transactional with a change feed, like git, or
// object-at-a-time like S3); faults injected per operation.
#include "sync_backend.h"
#include <map>
#include <mutex>
#include <set>

struct FakeStore {
    struct Obj { std::vector<uint8_t> data; bool isDir = false; std::string rev; sync::FileMeta meta; };
    std::mutex mx;
    std::map<std::wstring, Obj> objs;            // exact paths (case-sensitive store)
    uint64_t gen = 0;                            // bumps on every visible change (tokens, the feed)
    uint64_t revs = 0;
    std::vector<std::pair<uint64_t, std::wstring>> changelog;   // gen, path (the change feed)
    std::function<sync::Status(const char* op, const sync::RelPath&)> fault;   // non-Ok: fail that call
    int calls = 0;
    std::string next_rev() { return "r" + std::to_string(++revs); }
};

class FakeBackend : public sync::Backend {
public:
    FakeBackend(std::shared_ptr<FakeStore> s, bool transactional = false) : m_s(std::move(s)), m_tx(transactional) {}
    sync::Caps caps() const override {
        sync::Caps c;
        c.changeFeed = m_tx;
        c.transactional = m_tx;
        c.conditionalWrite = true;
        c.serverMove = true;
        c.emptyDirs = !m_tx;
        c.parallelism = m_tx ? 1 : 3;
        c.hash = sync::HashKind::Blake2b;
        return c;
    }
    sync::Result connect(sync::Progress&) override { return fault("connect", L""); }
    sync::Result changed_since(const std::string& token, bool& changed, std::string& nt, sync::Progress&) override {
        if (auto r = fault("changed_since", L""); !r.ok()) return r;
        std::lock_guard<std::mutex> lk(m_s->mx);
        nt = std::to_string(m_s->gen);
        changed = token != nt;
        return {};
    }
    sync::Result changes(const std::string& cursor, sync::ChangeBatch& out, sync::Progress&) override {
        if (auto r = fault("changes", L""); !r.ok()) return r;
        std::lock_guard<std::mutex> lk(m_s->mx);
        out = {};
        out.cursor = std::to_string(m_s->gen);
        if (!m_tx || cursor.empty()) {
            out.full = true;
            for (auto& [p, o] : m_s->objs) out.upserts.push_back(entry(p, o));
            return {};
        }
        uint64_t since = std::stoull(cursor);
        std::set<std::wstring> touched;
        for (auto& [g, p] : m_s->changelog) if (g > since) touched.insert(p);
        for (auto& p : touched) {
            auto f = m_s->objs.find(p);
            if (f == m_s->objs.end()) out.removals.push_back(p);
            else out.upserts.push_back(entry(p, f->second));
        }
        return {};
    }
    sync::Result download(const sync::RemoteEntry& e, sync::ByteSink& out, sync::FileMeta& meta, sync::Progress&) override {
        if (auto r = fault("download", e.path); !r.ok()) return r;
        std::vector<uint8_t> d;
        {
            std::lock_guard<std::mutex> lk(m_s->mx);
            auto f = m_s->objs.find(e.path);
            if (f == m_s->objs.end()) return sync::Result::fail(sync::Status::NotFound);
            d = f->second.data;
            meta = f->second.meta;
        }
        out.write(d.data(), d.size());
        return {};
    }
    sync::Result peek_hash(const sync::RemoteEntry& e, sync::ContentHash& out, sync::Progress&) override {
        std::lock_guard<std::mutex> lk(m_s->mx);
        auto f = m_s->objs.find(e.path);
        if (f == m_s->objs.end()) return sync::Result::fail(sync::Status::NotFound);
        out.kind = sync::HashKind::Blake2b;
        out.bytes = f->second.meta.plainHash;
        return {};
    }
    sync::Result upload(const sync::RelPath& p, sync::ByteSource& in, const sync::FileMeta& meta, const std::string& expectedRev,
                        sync::RemoteEntry& out, sync::Progress& prog) override {
        if (auto r = fault("upload", p); !r.ok()) return r;
        sync::MemorySink sink;
        if (auto r = sync::pump(in, sink, prog); !r.ok()) return r;
        std::lock_guard<std::mutex> lk(m_s->mx);
        auto& objs = m_tx ? m_stage : m_s->objs;
        if (m_tx && !m_staging) return sync::Result::fail(sync::Status::Fatal, L"upload outside a transaction");
        if (!check(p, expectedRev)) return sync::Result::fail(sync::Status::PreconditionFailed);
        FakeStore::Obj o;
        o.data = std::move(sink.data);
        o.meta = meta;
        o.rev = m_s->next_rev();
        objs[p] = o;
        if (m_tx) m_staged.push_back(p);
        else m_s->changelog.push_back({ ++m_s->gen, p });
        out = entry(p, o);
        return {};
    }
    sync::Result remove(const sync::RelPath& p, const std::string& expectedRev, sync::Progress&) override {
        if (auto r = fault("remove", p); !r.ok()) return r;
        std::lock_guard<std::mutex> lk(m_s->mx);
        if (!check(p, expectedRev)) return sync::Result::fail(sync::Status::PreconditionFailed);
        if (m_tx) { m_stage.erase(p); m_removed.insert(p); m_staged.push_back(p); }
        else { m_s->objs.erase(p); m_s->changelog.push_back({ ++m_s->gen, p }); }
        return {};
    }
    sync::Result move(const sync::RelPath& from, const sync::RelPath& to, const std::string& expectedRev, sync::RemoteEntry& out, sync::Progress&) override {
        if (auto r = fault("move", from); !r.ok()) return r;
        std::lock_guard<std::mutex> lk(m_s->mx);
        if (!check(from, expectedRev) || current(to)) return sync::Result::fail(sync::Status::PreconditionFailed);
        FakeStore::Obj o = *current(from);
        if (m_tx) { m_stage.erase(from); m_removed.insert(from); m_stage[to] = o; m_removed.erase(to); m_staged.push_back(from); m_staged.push_back(to); }
        else {
            m_s->objs.erase(from);
            m_s->objs[to] = o;
            ++m_s->gen;
            m_s->changelog.push_back({ m_s->gen, from });
            m_s->changelog.push_back({ m_s->gen, to });
        }
        out = entry(to, o);
        return {};
    }
    sync::Result make_dir(const sync::RelPath& p, sync::Progress&) override {
        std::lock_guard<std::mutex> lk(m_s->mx);
        FakeStore::Obj o; o.isDir = true; o.rev = "dir";
        m_s->objs[p] = o;
        m_s->changelog.push_back({ ++m_s->gen, p });
        return {};
    }
    sync::Result remove_dir(const sync::RelPath& p, sync::Progress&) override {
        std::lock_guard<std::mutex> lk(m_s->mx);
        if (!m_s->objs.count(p)) return sync::Result::fail(sync::Status::NotFound);
        for (auto& [k, o] : m_s->objs) if (k.size() > p.size() && k.compare(0, p.size(), p) == 0 && k[p.size()] == L'/') return sync::Result::fail(sync::Status::PreconditionFailed);
        m_s->objs.erase(p);
        m_s->changelog.push_back({ ++m_s->gen, p });
        return {};
    }
    sync::Result begin(sync::Progress&) override {
        std::lock_guard<std::mutex> lk(m_s->mx);
        m_staging = true;
        m_base = m_s->gen;
        m_stage.clear(); m_removed.clear(); m_staged.clear();
        return {};
    }
    sync::Result commit(const std::wstring&, std::string& newCursor, sync::Progress&) override {
        if (auto r = fault("commit", L""); !r.ok()) { abort(); return r; }
        std::lock_guard<std::mutex> lk(m_s->mx);
        newCursor.clear();
        if (!m_tx) return {};
        m_staging = false;
        if (m_staged.empty()) return {};
        if (m_s->gen != m_base && m_tx_strict) { m_stage.clear(); m_removed.clear(); m_staged.clear(); return sync::Result::fail(sync::Status::CommitRaced); }
        ++m_s->gen;
        for (auto& p : m_removed) m_s->objs.erase(p);
        for (auto& [p, o] : m_stage) m_s->objs[p] = o;
        for (auto& p : m_staged) m_s->changelog.push_back({ m_s->gen, p });
        m_stage.clear(); m_removed.clear(); m_staged.clear();
        newCursor = std::to_string(m_s->gen);
        // like git: the cursor is ours only when nobody else committed in between (checked above)
        return {};
    }
    void abort() override { std::lock_guard<std::mutex> lk(m_s->mx); m_staging = false; m_stage.clear(); m_removed.clear(); m_staged.clear(); }
    bool m_tx_strict = true;

private:
    sync::Result fault(const char* op, const sync::RelPath& p) {
        std::function<sync::Status(const char*, const sync::RelPath&)> f;
        { std::lock_guard<std::mutex> lk(m_s->mx); ++m_s->calls; f = m_s->fault; }
        if (f) { sync::Status s = f(op, p); if (s != sync::Status::Ok) return sync::Result::fail(s, L"injected"); }
        return {};
    }
    const FakeStore::Obj* current(const sync::RelPath& p) {
        if (m_tx && m_staging) {
            if (m_removed.count(p)) return nullptr;
            auto f = m_stage.find(p);
            if (f != m_stage.end()) return &f->second;
        }
        auto f = m_s->objs.find(p);
        return f == m_s->objs.end() ? nullptr : &f->second;
    }
    bool check(const sync::RelPath& p, const std::string& expectedRev) {
        if (expectedRev == "*") return true;
        const FakeStore::Obj* o = current(p);
        return expectedRev.empty() ? o == nullptr : (o && o->rev == expectedRev);
    }
    static sync::RemoteEntry entry(const std::wstring& p, const FakeStore::Obj& o) {
        sync::RemoteEntry e;
        e.path = p;
        e.isDir = o.isDir;
        e.size = o.data.size();
        e.rev = o.rev;
        return e;
    }
    std::shared_ptr<FakeStore> m_s;
    bool m_tx;
    bool m_staging = false;
    uint64_t m_base = 0;
    std::map<std::wstring, FakeStore::Obj> m_stage;
    std::set<std::wstring> m_removed;
    std::vector<std::wstring> m_staged;
};
