// Planner, state journal, engine (against the fake store) and Folder store tests
#include "sync_tests_common.h"
#include "sync_test_util.h"
#include "sync_fake.h"
#include "sync_engine.h"
#include "sync_paths.h"
#include "crypto.h"
#include <sddl.h>
#include <cstdio>
#include <set>

using namespace sync;

static Hash hash_of(const std::string& s) { return crypto::blake2b(s.data(), s.size()); }

// ------------------------------------------------------------------ paths
static void test_paths() {
    using namespace syncpath;
    CHECK(windows_valid_name(L"Report.docx"));
    CHECK(!windows_valid_name(L"CON"));
    CHECK(!windows_valid_name(L"con.txt"));
    CHECK(!windows_valid_name(L"COM1"));
    CHECK(windows_valid_name(L"COM10"));
    CHECK(!windows_valid_name(L"a:b"));
    CHECK(!windows_valid_name(L"name."));
    CHECK(!windows_valid_name(L"name "));
    CHECK(!windows_valid_name(L"a*b"));
    CHECK(windows_valid_path(L"a/b/c.txt"));
    CHECK(!windows_valid_path(L"a/nul/c.txt"));
    CHECK(nfc(L"e\x0301") == L"\x00e9");
    CHECK(case_key(L"ABC/\x00C9") == L"abc/\x00e9");
    SYSTEMTIME t{};
    t.wYear = 2026; t.wMonth = 10; t.wDay = 17; t.wHour = 14; t.wMinute = 3;
    CHECK(conflict_name(L"Report.docx", L"PC-1", t) == L"Report (conflicted copy, PC-1, 2026-10-17 14-03).docx");
    CHECK(conflict_name(L"Makefile", L"PC", t) == L"Makefile (conflicted copy, PC, 2026-10-17 14-03)");
    CHECK(is_conflict_copy(conflict_name(L".bashrc", L"PC", t)));
    Ignore ig;
    CHECK(ig.ignored(L"Thumbs.db", false));
    CHECK(ig.ignored(L"a/~$doc.docx", false));
    CHECK(!ig.ignored(L"a/doc.docx", false));
    ig.add_lines(L"# comment\n*.log\n/build/\n!keep.log\ndocs/**/*.bak\n");
    CHECK(ig.ignored(L"x/y.log", false));
    CHECK(!ig.ignored(L"x/keep.log", false));
    CHECK(ig.ignored(L"build", true));
    CHECK(!ig.ignored(L"build", false));
    CHECK(!ig.ignored(L"src/build", true));
    CHECK(ig.ignored(L"docs/a/b/c.bak", false));
    ig.add_excluded_folder(L"Big");
    CHECK(ig.ignored(L"Big/x.txt", false) && ig.ignored(L"big", true) && !ig.ignored(L"Bigger", true));
}

// ------------------------------------------------------------------ planner
static BaseEntry B(const wchar_t* p, const std::string& content, const char* rev, uint64_t id = 1) {
    BaseEntry e; e.path = p; e.size = content.size(); e.mtime = 100; e.fileId = id; e.hash = hash_of(content); e.rev = rev; return e;
}
static LocalItem L_(const wchar_t* p, const std::string& content, uint64_t mtime = 100, uint64_t id = 1) {
    LocalItem l; l.path = p; l.size = content.size(); l.mtime = mtime; l.fileId = id; l.hash = hash_of(content); l.hashed = true; return l;
}
static RemoteItem R_(const wchar_t* p, const char* rev) { RemoteItem r; r.path = p; r.size = 1; r.rev = rev; return r; }

static std::string plan_str(const BaseMap& b, const LocalMap& l, const RemoteMap& r, Caps caps = Caps()) {
    RemoteMap rr = r;
    add_implied_dirs(rr);
    Plan p = make_plan(b, l, rr, caps);
    std::string s;
    for (auto& a : p.actions) {
        if (!s.empty()) s += ' ';
        s += act_name(a.act);
        s += ':';
        if (!a.from.empty()) s += to_utf8(a.from) + ">";
        s += to_utf8(a.path);
    }
    return s;
}

static void test_planner() {
    auto k = [](const wchar_t* p) { return key_of(p); };
    // unchanged
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, { { k(L"a"), L_(L"a", "x") } }, { { k(L"a"), R_(L"a", "r1") } }) == "");
    // changed here
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, { { k(L"a"), L_(L"a", "yy", 200) } }, { { k(L"a"), R_(L"a", "r1") } }) == "upload:a");
    // touched, same content: refresh only
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, { { k(L"a"), L_(L"a", "x", 200) } }, { { k(L"a"), R_(L"a", "r1") } }) == "refresh:a");
    // changed in the store
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, { { k(L"a"), L_(L"a", "x") } }, { { k(L"a"), R_(L"a", "r2") } }) == "download:a");
    // both
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, { { k(L"a"), L_(L"a", "yy", 200) } }, { { k(L"a"), R_(L"a", "r2") } }) == "conflict:a");
    // deleted here / there / both
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, {}, { { k(L"a"), R_(L"a", "r1") } }) == "delete-remote:a");
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, { { k(L"a"), L_(L"a", "x") } }, {}) == "delete-local:a");
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, {}, {}) == "forget:a");
    // an edit beats a delete
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, {}, { { k(L"a"), R_(L"a", "r2") } }) == "download:a");
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, { { k(L"a"), L_(L"a", "yy", 200) } }, {}) == "upload:a");
    // new
    CHECK(plan_str({}, { { k(L"n"), L_(L"n", "x") } }, {}) == "upload:n");
    CHECK(plan_str({}, {}, { { k(L"n"), R_(L"n", "r1") } }) == "download:n");
    CHECK(plan_str({}, { { k(L"n"), L_(L"n", "x") } }, { { k(L"n"), R_(L"n", "r1") } }) == "conflict:n");
    // folders: new in the store -> created here first, files after
    CHECK(plan_str({}, {}, { { k(L"d/f"), R_(L"d/f", "r1") } }) == "mkdir-local:d download:d/f");
    // a local rename (same file ID, same content) -> a move in the store
    Caps mv; mv.serverMove = true;
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1", 7) } }, { { k(L"b"), L_(L"b", "x", 100, 7) } }, { { k(L"a"), R_(L"a", "r1") } }, mv) ==
          "move-remote:a>b");
    // a rename in the store (same rev at a new path) -> a local move
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, { { k(L"a"), L_(L"a", "x") } }, { { k(L"b"), R_(L"b", "r1") } }) ==
          "move-local:a>b");
    // busy files wait, and keep their folder
    LocalItem busy = L_(L"d/f", "x"); busy.busy = true;
    BaseEntry bd; bd.path = L"d"; bd.isDir = true;
    LocalItem ld; ld.path = L"d"; ld.isDir = true;
    CHECK(plan_str({ { k(L"d"), bd } }, { { k(L"d"), ld }, { k(L"d/f"), busy } }, {}) == "");
    // file replaced by a folder on one side while the other changed it
    LocalItem dirA; dirA.path = L"a"; dirA.isDir = true;
    CHECK(plan_str({ { k(L"a"), B(L"a", "x", "r1") } }, { { k(L"a"), dirA } }, { { k(L"a"), R_(L"a", "r2") } }) == "type-clash:a");
    // case: the same file whatever its case
    CHECK(plan_str({ { k(L"A.txt"), B(L"A.txt", "x", "r1") } }, { { k(L"a.TXT"), L_(L"a.TXT", "x") } }, { { k(L"A.txt"), R_(L"A.txt", "r1") } }) == "");
}

// ------------------------------------------------------------------ state journal
static void test_state() {
    std::wstring dir = tu::temp_root(L"state");
    {
        StateStore s(dir);
        CHECK(s.load());
        for (int i = 0; i < 200; ++i) s.put_base(B((L"f" + std::to_wstring(i)).c_str(), "content", "r"));
        s.erase_base(key_of(L"f7"));
        s.put_remote(R_(L"x", "r9"));
        s.set_value("cursor", "c1");
        s.flush();
    }
    {
        StateStore s(dir);
        CHECK(s.load());
        CHECK(s.base_all().size() == 199 && !s.base(key_of(L"f7")) && s.base(key_of(L"f8")));
        CHECK(s.remote(key_of(L"x")) && s.remote(key_of(L"x"))->rev == "r9");
        CHECK(s.value("cursor") == "c1");
        s.compact();
        s.put_base(B(L"after", "y", "r"));
    }
    // a torn tail is dropped; everything before it stays
    {
        HANDLE h = CreateFileW((dir + L"\\base.log").c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD w;
        WriteFile(h, "\x40\x00\x00\x00garbage", 11, &w, nullptr);
        CloseHandle(h);
        StateStore s(dir);
        s.load();
        CHECK(s.base_all().size() == 200 && s.base(key_of(L"after")));
        s.put_base(B(L"after2", "y", "r"));
    }
    {
        StateStore s(dir);
        s.load();
        CHECK(s.base(key_of(L"after2")) != nullptr);
    }
    tu::rmrf(dir);
}

// ------------------------------------------------------------------ engine
struct Dev {
    std::wstring root, stateDir, trash;
    std::unique_ptr<StateStore> st;
    std::unique_ptr<Engine> eng;
    std::shared_ptr<FakeStore> store;
    bool tx;
    std::vector<RelPath> excluded;              // (options for the next open())
    size_t massFiles = 50, massMax = 250;
    Dev(const std::wstring& name, std::shared_ptr<FakeStore> s, bool transactional = false) : store(std::move(s)), tx(transactional) {
        root = tu::temp_root(name);
        stateDir = tu::temp_root(name + L"-state");
        trash = tu::temp_root(name + L"-trash");
        open();
    }
    void open(std::unique_ptr<Backend> backend = nullptr) {
        eng.reset();
        st = std::make_unique<StateStore>(stateDir);
        st->load();
        EngineOptions o;
        o.root = root;
        o.deviceName = L"DEV";
        o.trashDir = trash;
        o.settleMs = 0;
        o.excluded = excluded;
        o.massDeleteFiles = massFiles;
        o.massDeleteMax = massMax;
        eng = std::make_unique<Engine>(o, backend ? std::move(backend) : std::make_unique<FakeBackend>(store, tx), *st);
    }
    PassReport sync() {
        NoProgress p;
        PassReport r = eng->sync(p);
        if (!r.result.ok()) std::printf("  sync: %s\n", to_utf8(r.result.message).c_str());
        return r;
    }
    ~Dev() { eng.reset(); st.reset(); tu::rmrf(root); tu::rmrf(stateDir); tu::rmrf(trash); }
};

static bool same(Dev& a, Dev& b) {
    auto ta = tu::tree(a.root), tb = tu::tree(b.root);
    if (ta != tb) std::printf("  A:\n%s  B:\n%s", tu::dump(ta).c_str(), tu::dump(tb).c_str());
    return ta == tb;
}

static void engine_scenarios(bool tx) {
    auto store = std::make_shared<FakeStore>();
    Dev a(L"a", store, tx), b(L"b", store, tx);
    // create on A: B gets everything
    tu::write(a.root, L"one.txt", "one");
    tu::write(a.root, L"dir/two.txt", "two");
    tu::write(a.root, L"dir/sub/three.txt", "three");
    CreateDirectoryW((a.root + L"\\empty").c_str(), nullptr);
    tu::write(a.root, L"Thumbs.db", "ignored");
    CHECK(a.sync().result.ok());
    CHECK(b.sync().result.ok());
    CHECK(tu::read(b.root, L"dir/sub/three.txt") == "three");
    CHECK(!tu::exists(b.root, L"Thumbs.db"));
    tu::remove(a.root, L"Thumbs.db");
    if (!tx) CHECK(tu::exists(b.root, L"empty"));          // empty folders travel where the store has folders
    else tu::remove(a.root, L"empty");
    CHECK(same(a, b));
    // a second pass changes nothing
    PassReport r = a.sync();
    CHECK(r.log.empty());
    // edit on B
    tu::write(b.root, L"one.txt", "one, edited on B");
    CHECK(b.sync().uploads == 1);
    CHECK(a.sync().downloads == 1);
    CHECK(tu::read(a.root, L"one.txt") == "one, edited on B");
    // delete on A: B's copy goes to the trash, not away
    tu::remove(a.root, L"dir/two.txt");
    CHECK(a.sync().remoteDeletes == 1);
    CHECK(b.sync().localDeletes == 1);
    CHECK(!tu::exists(b.root, L"dir/two.txt"));
    CHECK(same(a, b));
    // rename on A: a move in the store and a move on B
    CHECK(tu::rename(a.root, L"dir/sub/three.txt", L"dir/moved.txt"));
    r = a.sync();
    CHECK(r.moves == 1 && r.uploads == 0);
    r = b.sync();
    if (!(r.moves == 1 && r.downloads == 0)) for (auto& l : r.log) std::printf("    %s\n", l.c_str());
    CHECK(r.moves == 1 && r.downloads == 0);
    CHECK(tu::read(b.root, L"dir/moved.txt") == "three");
    if (tx) a.sync();                               // no folders in the store: A drops the emptied dir/sub on its next pass
    CHECK(same(a, b));
    // conflict: both edit; the second to sync keeps both
    tu::write(a.root, L"one.txt", "A's version");
    tu::write(b.root, L"one.txt", "B's version!");
    CHECK(a.sync().result.ok());
    r = b.sync();
    CHECK(r.conflicts == 1);
    CHECK(a.sync().result.ok());
    CHECK(tu::read(b.root, L"one.txt") == "A's version");
    bool found = false;
    for (auto& [p, v] : tu::tree(a.root)) if (syncpath::is_conflict_copy(p) && v == "B's version!") found = true;
    CHECK(found);
    CHECK(same(a, b));
    // both create the same file with the same content: adopted, no copy
    tu::write(a.root, L"same.txt", "identical");
    tu::write(b.root, L"same.txt", "identical");
    a.sync();
    r = b.sync();
    CHECK(r.conflicts == 0);
    CHECK(same(a, b));
    // an edit beats a delete
    tu::write(a.root, L"keep.txt", "v1");
    a.sync(); b.sync();
    tu::remove(a.root, L"keep.txt");
    tu::write(b.root, L"keep.txt", "v2 longer");
    a.sync();
    b.sync();
    a.sync();
    CHECK(tu::read(a.root, L"keep.txt") == "v2 longer");
    CHECK(same(a, b));
    // a folder deleted on A goes on B too
    tu::write(a.root, L"gone/x.txt", "x");
    tu::write(a.root, L"gone/y/z.txt", "z");
    a.sync(); b.sync();
    CHECK(tu::exists(b.root, L"gone/y/z.txt"));
    tu::remove(a.root, L"gone");
    a.sync(); b.sync(); a.sync();
    CHECK(!tu::exists(b.root, L"gone"));
    CHECK(same(a, b));
    // restart: the state comes back from disk, nothing to do
    b.open();
    r = b.sync();
    CHECK(r.log.empty());
    // unicode names (NFC) and case-only renames
    tu::write(a.root, L"\x0160koda.txt", "s");
    a.sync(); b.sync();
    CHECK(tu::read(b.root, L"\x0160koda.txt") == "s");
    CHECK(same(a, b));
}

static void engine_faults() {
    auto store = std::make_shared<FakeStore>();
    Dev a(L"fa", store), b(L"fb", store);
    for (int i = 0; i < 10; ++i) tu::write(a.root, L"f" + std::to_wstring(i) + L".txt", "file " + std::to_string(i));
    // the store goes away after a few uploads: the pass reports offline, nothing is lost
    int uploads = 0;
    store->fault = [&](const char* op, const RelPath&) {
        if (std::string(op) == "upload" && ++uploads > 4) return Status::Offline;
        return Status::Ok;
    };
    PassReport r = a.sync();
    CHECK(r.result.st == Status::Offline);
    CHECK(a.eng->status().state == PairState::Offline);
    store->fault = nullptr;
    CHECK(a.sync().result.ok());
    CHECK(b.sync().result.ok());
    CHECK(same(a, b));
    // a conditional write that fails (someone else wrote first) is re-planned, not forced
    tu::write(a.root, L"f1.txt", "A edit");
    tu::write(b.root, L"f1.txt", "B edit longer");
    CHECK(b.sync().result.ok());
    r = a.sync();                                   // lists, sees B's version -> conflict copy
    CHECK(r.conflicts == 1);
    b.sync();
    CHECK(same(a, b));
    // the mass-delete guard: 60 files deleted at once wait for a yes
    for (int i = 0; i < 60; ++i) tu::write(a.root, L"many/" + std::to_wstring(i) + L".txt", "m");
    a.sync(); b.sync();
    tu::remove(a.root, L"many");
    r = a.sync();
    CHECK(r.result.st == Status::Rejected && a.eng->status().state == PairState::ConfirmDeletes);
    CHECK(tu::tree(b.root).count(L"many/1.txt") == 1);
    a.eng->confirm_deletes(false);                  // "restore them"
    a.sync();
    CHECK(tu::exists(a.root, L"many/59.txt"));
    tu::remove(a.root, L"many");
    a.sync();
    a.eng->confirm_deletes(true);
    CHECK(a.sync().result.ok());
    r = b.sync();                                   // B is asked too: the guard is on both sides
    CHECK(b.eng->status().state == PairState::ConfirmDeletes);
    b.eng->confirm_deletes(true);
    r = b.sync();
    if (tu::exists(b.root, L"many")) { for (auto& l : r.log) std::printf("    %s\n", l.c_str()); std::printf("%s", tu::dump(tu::tree(b.root)).c_str());
        std::printf("  store has many: %d, B base many: %d, B remote many: %d\n", (int)store->objs.count(L"many"), b.st->base(key_of(L"many")) != nullptr, b.st->remote(key_of(L"many")) != nullptr); }
    CHECK(!tu::exists(b.root, L"many"));
    CHECK(same(a, b));
    // the folder disappears (an unplugged disk): the pair stops, nothing is deleted
    std::wstring moved = a.root + L"-away";
    MoveFileW(a.root.c_str(), moved.c_str());
    r = a.sync();
    CHECK(a.eng->status().state == PairState::RootMissing);
    CHECK(store->objs.size() > 5);
    MoveFileW(moved.c_str(), a.root.c_str());
    CHECK(a.sync().result.ok());
}

// a folder nobody may list (an ACL change, a filter driver): the scan can't see into it
static bool deny_list(const std::wstring& dir, bool deny) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    const wchar_t* sddl = deny ? L"D:P(D;;0x1;;;WD)(A;OICI;FA;;;WD)" : L"D:(A;OICI;FA;;;WD)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    bool ok = SetFileSecurityW(dir.c_str(), DACL_SECURITY_INFORMATION, sd) != 0;
    LocalFree(sd);
    return ok;
}

static bool make_junction(const std::wstring& link, const std::wstring& target) {
    std::wstring cmd = L"mklink /J \"" + link + L"\" \"" + target + L"\" >nul";
    return _wsystem(cmd.c_str()) == 0;
}

// what the scan can't see, what the rules leave out, what a yes covers: never a deletion
// the user didn't make
static void engine_no_false_deletes() {
    auto store = std::make_shared<FakeStore>();
    Dev a(L"ua", store), b(L"ub", store);
    tu::write(a.root, L"locked/x.txt", "x");
    tu::write(a.root, L"locked/sub/y.txt", "y");
    tu::write(a.root, L"j/z.txt", "z");
    tu::write(a.root, L"ex/f.txt", "f");
    tu::write(a.root, L"mv.txt", "move me");
    for (int i = 0; i < 60; ++i) tu::write(a.root, L"many/" + std::to_wstring(i) + L".txt", "m");
    CHECK(a.sync().result.ok());
    CHECK(b.sync().result.ok());
    CHECK(same(a, b));

    // an unreadable folder: its files aren't taken for deleted
    std::wstring locked = a.root + L"\\locked";
    CHECK(deny_list(locked, true));
    PassReport r = a.sync();
    CHECK(r.remoteDeletes == 0);
    CHECK(store->objs.count(L"locked/x.txt") == 1 && store->objs.count(L"locked/sub/y.txt") == 1);
    bool seen = false;                              // (the scan really couldn't read it)
    for (auto& [p, why] : a.eng->status().problems) if (p == L"locked") seen = true;
    CHECK(seen);
    CHECK(deny_list(locked, false));
    r = a.sync();
    CHECK(r.remoteDeletes == 0 && r.uploads == 0);
    b.sync();
    CHECK(tu::read(b.root, L"locked/sub/y.txt") == "y");

    // a folder replaced by a junction (moved elsewhere): what it held stays in the storage
    std::wstring away = a.root + L"-jtarget";
    CHECK(MoveFileW((a.root + L"\\j").c_str(), away.c_str()));
    CHECK(make_junction(a.root + L"\\j", away));
    r = a.sync();
    CHECK(r.remoteDeletes == 0 && store->objs.count(L"j/z.txt") == 1);
    RemoveDirectoryW((a.root + L"\\j").c_str());
    CHECK(MoveFileW(away.c_str(), (a.root + L"\\j").c_str()));
    r = a.sync();
    CHECK(r.remoteDeletes == 0 && r.uploads == 0);

    // left out, deleted here to make room, brought back in: it comes back, nothing is deleted
    a.excluded = { L"ex" };
    a.open();
    CHECK(a.sync().result.ok());
    tu::remove(a.root, L"ex");
    CHECK(a.sync().remoteDeletes == 0);
    a.excluded.clear();
    a.open();
    r = a.sync();
    CHECK(r.remoteDeletes == 0);
    CHECK(tu::read(a.root, L"ex/f.txt") == "f");
    b.sync();
    CHECK(tu::read(b.root, L"ex/f.txt") == "f");

    // a yes nobody was asked for approves nothing
    a.eng->confirm_deletes(true);
    tu::remove(a.root, L"many");
    r = a.sync();
    CHECK(r.result.st == Status::Rejected && a.eng->status().state == PairState::ConfirmDeletes);
    a.eng->confirm_deletes(false);                  // restore them
    a.sync();
    CHECK(tu::exists(a.root, L"many/59.txt"));
    // a yes covers the deletions asked about, not more
    for (int i = 0; i < 55; ++i) tu::remove(a.root, L"many/" + std::to_wstring(i) + L".txt");
    r = a.sync();
    CHECK(r.result.st == Status::Rejected);
    a.eng->confirm_deletes(true);
    tu::remove(a.root, L"many");                    // 5 more since the question
    r = a.sync();
    CHECK(r.result.st == Status::Rejected && a.eng->status().pendingDeletes == 60);
    a.eng->confirm_deletes(false);
    a.sync();
    CHECK(tu::exists(a.root, L"many/0.txt"));
    // many files but a small share: a lot is still a lot
    a.massFiles = 5;
    a.massMax = 10;
    a.open();
    for (int i = 0; i < 12; ++i) tu::remove(a.root, L"many/" + std::to_wstring(i) + L".txt");
    r = a.sync();
    CHECK(r.result.st == Status::Rejected);
    a.eng->confirm_deletes(false);
    a.sync();

    // no server-side move and the upload under the new name fails: the old one stays
    store->fault = [](const char* op, const RelPath& p) {
        std::string o = op;
        return o == "move" || (o == "upload" && p == L"mv2.txt") ? Status::Fatal : Status::Ok;
    };
    CHECK(tu::rename(a.root, L"mv.txt", L"mv2.txt"));
    a.sync();
    CHECK(store->objs.count(L"mv.txt") == 1);
    store->fault = nullptr;
    a.sync();
    b.sync();
    CHECK(store->objs.count(L"mv.txt") == 0 && store->objs.count(L"mv2.txt") == 1);
    CHECK(same(a, b));
}

static void engine_commit_race() {
    // transactional store (git-like): two devices commit at once; the later one re-plans
    auto store = std::make_shared<FakeStore>();
    Dev a(L"ta", store, true), b(L"tb", store, true);
    tu::write(a.root, L"x.txt", "x");
    a.sync(); b.sync();
    tu::write(a.root, L"a.txt", "from a");
    tu::write(b.root, L"b.txt", "from b");
    // B commits in the middle of A's pass (just before A's commit)
    bool once = true;
    store->fault = [&](const char* op, const RelPath&) {
        if (std::string(op) == "commit" && once) {
            once = false;
            store->fault = nullptr;
            NoProgress p;
            b.eng->sync(p);
        }
        return Status::Ok;
    };
    PassReport r = a.sync();
    CHECK(r.result.ok());
    b.sync();
    a.sync();
    CHECK(tu::read(a.root, L"b.txt") == "from b");
    CHECK(tu::read(b.root, L"a.txt") == "from a");
    CHECK(same(a, b));
}

void run_engine_only() {
    test_paths();
    test_planner();
    test_state();
    engine_scenarios(false);
    engine_scenarios(true);
    engine_faults();
    engine_no_false_deletes();
    engine_commit_race();
}

// the shell's Recycle Bin takes a local file; a network share has none, and the file stays
// (never deleted for good). Touches the real Recycle Bin: run by hand (--recycle).
void run_recycle_test() {
    std::wstring dir = tu::temp_root(L"recycle");
    tu::write(dir, L"bin.txt", "to the Recycle Bin");
    CHECK(recycle(dir + L"\\bin.txt", L"", L"bin.txt"));
    CHECK(!tu::exists(dir, L"bin.txt"));
    // \\localhost\C$\...: the same file through the admin share, which has no Recycle Bin
    tu::write(dir, L"share.txt", "stays");
    if (dir.size() > 2 && dir[1] == L':') {
        std::wstring unc = L"\\\\localhost\\" + dir.substr(0, 1) + L"$" + dir.substr(2) + L"\\share.txt";
        if (GetFileAttributesW(unc.c_str()) != INVALID_FILE_ATTRIBUTES) {
            CHECK(!recycle(unc, L"", L"share.txt"));
            CHECK(tu::read(dir, L"share.txt") == "stays");
        } else std::printf("  (admin share not reachable: share case skipped)\n");
    }
    tu::rmrf(dir);
}

int run_engine_tests() {
    run_engine_only();
    extern void run_crypt_tests();
    run_crypt_tests();
    extern void run_git_tests();
    run_git_tests();
    extern void run_s3_tests();
    run_s3_tests();
    extern void run_azdo_tests();
    run_azdo_tests();
    return 0;
}
