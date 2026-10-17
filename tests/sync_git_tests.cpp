// The git store: the parsers, the raw store against a local bare repository (commits,
// the change feed, races, moves, versions), and two encrypted devices syncing through it.
#include "sync_tests_common.h"
#include "sync_test_util.h"
#include "sync_engine.h"
#include "sync_crypt.h"
#include "sync_git.h"
#include "sync_keys.h"
#include <cstdio>

using namespace sync;

namespace {

void test_parsers() {
    std::vector<GitTreeEntry> t;
    std::string z = std::string("100644 blob 0123456789012345678901234567890123456789      12\ta/b c.txt") + '\0' +
                    "040000 tree 1123456789012345678901234567890123456789       -\tdir" + '\0';
    CHECK(parse_ls_tree(z, t));
    CHECK(t.size() == 2 && t[0].path == "a/b c.txt" && t[0].size == 12 && t[0].type == "blob" && t[1].type == "tree" && t[1].size == 0);
    CHECK(!parse_ls_tree("garbage", t));
    std::vector<GitDiffEntry> d;
    std::string dz = std::string(":100644 100644 0123456789012345678901234567890123456789 1123456789012345678901234567890123456789 M") + '\0' + "x/y" + '\0' +
                     ":000000 100644 0000000000000000000000000000000000000000 2123456789012345678901234567890123456789 A" + '\0' + "new" + '\0' +
                     ":100644 000000 3123456789012345678901234567890123456789 0000000000000000000000000000000000000000 D" + '\0' + "gone" + '\0';
    CHECK(parse_diff_tree(dz, d));
    CHECK(d.size() == 3 && d[0].status == 'M' && d[0].path == "x/y" && d[1].status == 'A' && d[2].status == 'D' && d[2].oldOid[0] == '3');
    CHECK(git_version_at_least("git version 2.55.0.windows.3", 2, 31));
    CHECK(!git_version_at_least("git version 2.30.1", 2, 31));
    CHECK(git_version_at_least("git version 3.0", 2, 31));
}

std::wstring g_git;

bool make_bare(const std::wstring& dir) {
    proc::Result r = proc::run(g_git, { L"init", L"--bare", L"--quiet", dir });
    return r.ok();
}

GitOptions opts(const std::wstring& remote, const std::wstring& mirror, const std::wstring& sub = L"") {
    GitOptions o;
    o.url = remote;
    o.mirror = mirror;
    o.subfolder = sub;
    o.branch = L"sync-test";
    return o;
}

Result put(Backend& b, const RelPath& p, const std::string& text, const std::string& expectedRev, std::string* rev = nullptr) {
    NoProgress np;
    MemorySource src(std::vector<uint8_t>(text.begin(), text.end()));
    RemoteEntry out;
    Result r = b.upload(p, src, FileMeta{}, expectedRev, out, np);
    if (rev) *rev = out.rev;
    return r;
}
std::string get(Backend& b, const RemoteEntry& e) {
    NoProgress np;
    MemorySink s;
    FileMeta m;
    if (!b.download(e, s, m, np).ok()) return "<fail>";
    return std::string(s.data.begin(), s.data.end());
}
const RemoteEntry* find(const std::vector<RemoteEntry>& v, const RelPath& p) {
    for (auto& e : v) if (e.path == p) return &e;
    return nullptr;
}

void test_raw(const std::wstring& sub) {
    std::wstring base = tu::temp_root(L"gitraw");
    std::wstring remote = base + L"\\remote.git";
    CHECK(make_bare(remote));
    auto a = make_git_backend(opts(remote, base + L"\\a.git", sub));
    auto b = make_git_backend(opts(remote, base + L"\\b.git", sub));
    NoProgress np;
    CHECK(a->connect(np).ok());
    CHECK(b->connect(np).ok());
    // empty: no branch yet
    bool changed = false;
    std::string tok;
    CHECK(a->changed_since("", changed, tok, np).ok() && tok == "none");
    ChangeBatch cb;
    CHECK(a->changes("", cb, np).ok() && cb.full && cb.upserts.empty());
    // A commits two files
    std::string r1, r2;
    CHECK(a->begin(np).ok());
    CHECK(put(*a, L"one.txt", "one", "", &r1).ok());
    CHECK(put(*a, L"d/two.txt", "two", "", &r2).ok());
    CHECK(put(*a, L"one.txt", "again", "").st == Status::PreconditionFailed);    // exists (staged)
    std::string cursor;
    CHECK(a->commit(L"sync", cursor, np).ok());
    CHECK(cursor.size() == 40);
    // B sees them: a full listing, then nothing new
    CHECK(b->changed_since("none", changed, tok, np).ok() && changed && tok == cursor);
    CHECK(b->changes("", cb, np).ok() && cb.full && cb.upserts.size() == 2);
    const RemoteEntry* e1 = find(cb.upserts, L"one.txt");
    CHECK(e1 && e1->rev == r1 && e1->size == 3 && get(*b, *e1) == "one");
    std::string bcur = cb.cursor;
    CHECK(b->changes(bcur, cb, np).ok() && !cb.full && cb.upserts.empty() && cb.removals.empty());
    // B edits, moves and deletes; A's incremental feed shows exactly that
    CHECK(b->begin(np).ok());
    std::string r3;
    CHECK(put(*b, L"one.txt", "one, edited", "stale-rev").st == Status::PreconditionFailed);
    CHECK(put(*b, L"one.txt", "one, edited", r1, &r3).ok());
    RemoteEntry mv;
    CHECK(b->move(L"d/two.txt", L"e/two.txt", r2, mv, np).ok() && mv.rev == r2);
    CHECK(b->commit(L"sync", cursor, np).ok());
    CHECK(a->changes(bcur, cb, np).ok() && !cb.full);
    CHECK(cb.upserts.size() == 2 && cb.removals.size() == 1 && cb.removals[0] == L"d/two.txt");
    CHECK(find(cb.upserts, L"e/two.txt") && find(cb.upserts, L"one.txt")->rev == r3);
    // a race: A commits on a stale base -> CommitRaced; after listing again it goes through
    std::string acur = cb.cursor;
    CHECK(b->begin(np).ok());
    CHECK(put(*b, L"b-only.txt", "b", "").ok());
    CHECK(a->begin(np).ok());
    CHECK(put(*a, L"a-only.txt", "a", "").ok());
    CHECK(b->commit(L"sync", cursor, np).ok());
    CHECK(a->commit(L"sync", cursor, np).st == Status::CommitRaced);
    CHECK(a->changes(acur, cb, np).ok() && find(cb.upserts, L"b-only.txt"));
    CHECK(a->begin(np).ok());
    CHECK(put(*a, L"a-only.txt", "a", "").ok());
    CHECK(a->commit(L"sync", cursor, np).ok() && cursor.size() == 40);
    // remove, stat, list, versions
    RemoteEntry st;
    CHECK(a->stat(L"one.txt", st, np).ok() && st.rev == r3);
    CHECK(a->stat(L"nope.txt", st, np).st == Status::NotFound);
    std::vector<RemoteEntry> lst;
    CHECK(a->list(L"e", lst, np).ok() && lst.size() == 1 && lst[0].path == L"e/two.txt");
    std::vector<RemoteVersion> vers;
    CHECK(a->list_versions(L"one.txt", vers, np).ok() && vers.size() == 2 && vers[0].rev == r3 && vers[1].rev == r1 && vers[1].size == 3);
    CHECK(a->begin(np).ok());
    CHECK(a->remove(L"one.txt", r3, np).ok());
    CHECK(a->commit(L"sync", cursor, np).ok());
    CHECK(b->stat(L"one.txt", st, np).st == Status::NotFound);
    // a commit with nothing in it pushes nothing
    CHECK(a->begin(np).ok());
    CHECK(a->commit(L"sync", cursor, np).ok() && cursor.empty());
    if (!sub.empty()) {
        // the rest of the repository is left alone
        proc::Options o;
        o.env = { { L"GIT_DIR", remote } };
        proc::Result r = proc::run(g_git, { L"ls-tree", L"-r", L"--name-only", L"sync-test" }, o);
        CHECK(r.ok() && r.out.find(to_utf8(sub) + "/a-only.txt") != std::string::npos);
    }
    a.reset();
    b.reset();
    tu::rmrf(base);
}

void test_commit_split() {
    std::wstring base = tu::temp_root(L"gitsplit");
    std::wstring remote = base + L"\\remote.git";
    CHECK(make_bare(remote));
    GitOptions o = opts(remote, base + L"\\a.git");
    o.maxCommitBytes = 10;                          // every second file pushes a commit
    auto a = make_git_backend(o);
    NoProgress np;
    CHECK(a->connect(np).ok());
    ChangeBatch cb;
    CHECK(a->changes("", cb, np).ok());
    CHECK(a->begin(np).ok());
    for (int i = 0; i < 5; ++i) CHECK(put(*a, L"f" + std::to_wstring(i), "123456", "").ok());
    std::string cursor;
    CHECK(a->commit(L"sync", cursor, np).ok());
    proc::Options po;
    po.env = { { L"GIT_DIR", remote } };
    proc::Result r = proc::run(g_git, { L"rev-list", L"--count", L"sync-test" }, po);
    CHECK(r.ok() && atoi(r.out.c_str()) == 3);
    CHECK(a->changes("", cb, np).ok() && cb.upserts.size() == 5);
    a.reset();
    tu::rmrf(base);
}

// two devices with their own mirrors, encrypted, through the engine
void test_encrypted_engine() {
    std::wstring base = tu::temp_root(L"gitenc");
    std::wstring remote = base + L"\\remote.git";
    CHECK(make_bare(remote));
    struct Dev {
        std::wstring root, stateDir;
        std::unique_ptr<synckeys::KeyStore> ks = synckeys::memory_key_store();
        std::unique_ptr<StateStore> st;
        std::unique_ptr<Engine> eng;
    } a, b;
    a.root = tu::temp_root(L"gea"); a.stateDir = tu::temp_root(L"gea-state");
    b.root = tu::temp_root(L"geb"); b.stateDir = tu::temp_root(L"geb-state");
    auto open = [&](Dev& d, const std::wstring& mirror, bool create) -> Result {
        d.eng.reset();
        d.st.reset();
        NoProgress np;
        auto raw = make_git_backend(opts(remote, mirror, L"vault"));
        Result r = raw->connect(np);
        if (!r.ok()) return r;
        synckeys::Vault v;
        r = synckeys::open_vault(*raw, *d.ks, create, v, np);
        if (!r.ok()) return r;
        d.st = std::make_unique<StateStore>(d.stateDir);
        d.st->load();
        EngineOptions eo;
        eo.root = d.root;
        eo.deviceName = L"DEV";
        eo.trashDir = d.stateDir + L"\\trash";
        eo.settleMs = 0;
        d.eng = std::make_unique<Engine>(eo, make_encrypting(std::move(raw), v), *d.st);
        return {};
    };
    auto sync = [](Dev& d) {
        NoProgress np;
        PassReport r = d.eng->sync(np);
        if (!r.result.ok()) std::printf("  sync: %s\n", to_utf8(r.result.message).c_str());
        return r;
    };
    std::string rec;
    CHECK(synckeys::setup_ring(*a.ks, rec).ok());
    CHECK(open(a, base + L"\\a.git", true).ok());
    tu::write(a.root, L"report.docx", "REPORT-BYTES");
    tu::write(a.root, L"photos/2026/beach.jpg", std::string(200000, 'J'));
    CHECK(sync(a).result.ok());
    {
        NoProgress np;
        auto raw = make_git_backend(opts(remote, base + L"\\b.git", L"vault"));
        CHECK(raw->connect(np).ok());
        std::string code;
        auto rawA = make_git_backend(opts(remote, base + L"\\a.git", L"vault"));
        CHECK(rawA->connect(np).ok());
        CHECK(synckeys::create_pairing(*rawA, *a.ks, code, np).ok());
        CHECK(synckeys::redeem_pairing(*raw, *b.ks, code, np).ok());
    }
    CHECK(open(b, base + L"\\b.git", false).ok());
    CHECK(sync(b).result.ok());
    CHECK(tu::read(b.root, L"photos/2026/beach.jpg") == std::string(200000, 'J'));
    tu::write(b.root, L"report.docx", "REPORT-BYTES v2");
    CHECK(tu::rename(b.root, L"photos/2026/beach.jpg", L"photos/beach.jpg"));
    CHECK(sync(b).result.ok());
    CHECK(sync(a).result.ok());
    CHECK(tu::read(a.root, L"report.docx") == "REPORT-BYTES v2");
    CHECK(tu::read(a.root, L"photos/beach.jpg") == std::string(200000, 'J'));
    CHECK(!tu::exists(a.root, L"photos/2026"));
    // nothing readable in the repository: every blob and every path
    proc::Options o;
    o.env = { { L"GIT_DIR", remote } };
    proc::Result names = proc::run(g_git, { L"log", L"--all", L"--name-only", L"--format=%an %ae %s" }, o);
    CHECK(names.ok() && names.out.find("report") == std::string::npos && names.out.find("photos") == std::string::npos &&
          names.out.find("beach") == std::string::npos && names.out.find("DEV") == std::string::npos);
    proc::Result blobs = proc::run(g_git, { L"cat-file", L"--batch-all-objects", L"--batch" }, o);
    CHECK(blobs.ok() && blobs.out.size() > 200000 && blobs.out.find("REPORT-BYTES") == std::string::npos && blobs.out.find("JJJJJJJJ") == std::string::npos);
    a.eng.reset(); a.st.reset(); b.eng.reset(); b.st.reset();
    tu::rmrf(a.root); tu::rmrf(a.stateDir); tu::rmrf(b.root); tu::rmrf(b.stateDir);
    tu::rmrf(base);
}

} // namespace

void run_git_tests() {
    test_parsers();
    g_git = find_git();
    if (g_git.empty()) { std::printf("  git tests skipped: no git.exe\n"); return; }
    test_raw(L"");
    test_raw(L"sub/folder");
    test_commit_split();
    test_encrypted_engine();
}
