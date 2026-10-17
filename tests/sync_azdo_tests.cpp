// Azure DevOps Git: the URL rules, the reading of its refusals, the token kept out of
// texts; and, when EXPLORERPLUS_AZDO_TEST_URL and EXPLORERPLUS_AZDO_TEST_PAT are set, the
// end-to-end run: two encrypted devices syncing through a branch of that repository made
// for the run (sync-test/<id>) and deleted afterwards. The token comes from the
// environment only; it's never written anywhere.
#include "sync_tests_common.h"
#include "sync_test_util.h"
#include "sync_azdo.h"
#include "sync_crypt.h"
#include "sync_engine.h"
#include "sync_keys.h"
#include "crypto.h"
#include "json.h"
#include <cstdio>

using namespace sync;

namespace {

void test_rules() {
    std::wstring why;
    CHECK(azdo_clone_url(L"https://org@dev.azure.com/org/Project/_git/Repo", &why) == L"https://dev.azure.com/org/Project/_git/Repo");
    CHECK(azdo_clone_url(L" https://devops.example.com/projects/Coll/sandbox/_git/explorerplus/ ", &why) ==
          L"https://devops.example.com/projects/Coll/sandbox/_git/explorerplus");
    CHECK(azdo_clone_url(L"http://dev.azure.com/org/p/_git/r", &why).empty() && !why.empty());
    CHECK(azdo_clone_url(L"https://dev.azure.com/org/p/repo", &why).empty());
    CHECK(azdo_clone_url(L"https://dev.azure.com/org/p/_git/", &why).empty());
    CHECK(azdo_classify("remote: TF402455: Pushes to this branch are not permitted; you must use a pull request to update this branch.\n").st == Status::Rejected);
    Result r = azdo_classify("To https://x\n ! [remote rejected] c -> b (TF401027: You need the Git 'GenericContribute' permission to perform this action.)\n");
    CHECK(r.st == Status::Rejected && r.message.find(L"TF401027") != std::wstring::npos);
    CHECK(azdo_classify("fatal: Authentication failed for 'https://x/'\n").st == Status::AuthFailed);
    CHECK(azdo_classify("fatal: unable to access 'https://x/': Could not resolve host: x\n").ok());
    std::string pat = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQR";
    std::string token = ":" + pat;
    std::string hdr = "Authorization: Basic " + crypto::base64(token.data(), token.size());
    std::string s = azdo_scrub("token " + pat + " and " + hdr + "\nnext", pat);
    CHECK(s.find(pat) == std::string::npos && s.find(crypto::base64(token.data(), token.size())) == std::string::npos && s.find("next") != std::string::npos);
    // the token reaches git through the environment only, as a header
    GitOptions o = azdo_git_options(L"https://dev.azure.com/o/p/_git/r", from_utf8(pat), L"", L"");
    bool count = false, header = false, plain = false;
    for (auto& [k, v] : o.env) {
        if (k == L"GIT_CONFIG_COUNT") count = true;
        if (v.find(L"Authorization: Basic ") == 0) header = true;
        if (to_utf8(v).find(pat) != std::string::npos) plain = true;
    }
    CHECK(count && header && !plain && o.branch == L"explorerplus-sync" && o.url.find(L"@") == std::wstring::npos);
}

// the REST store's pure parts: JSON, git blob ids, its reading of Azure DevOps' answers
void test_rest_rules() {
    json::Value v;
    CHECK(json::parse(R"({"a":[1,-2.5e3,true,false,null,"x\"\\\/\n\u00e1\ud83d\ude00"],"size":18446744073709551615,"e":{}})", v));
    CHECK(v["a"].size() == 6 && v["a"][0].u64() == 1 && v["a"][1].u64() == 0 && v["a"][2].b && !v["a"][3].b && v["a"][4].is_null());
    CHECK(v["a"][5].str() == "x\"\\/\n\xc3\xa1\xf0\x9f\x98\x80");
    CHECK(v["size"].u64() == 18446744073709551615ull && v["e"].type == json::Value::Object && v["missing"]["deeper"][3].is_null());
    CHECK(!json::parse("{\"a\":1,}", v) && !json::parse("[1 2]", v) && !json::parse("\"\\ud83d\"", v) && !json::parse("{} x", v));
    CHECK(json::parse("\xEF\xBB\xBF [ ]", v) && v.type == json::Value::Array);
    CHECK(json::quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    // git's blob ids (the server's own answers for these bytes)
    CHECK(git_blob_id("", 0) == "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391");
    const uint8_t three[] = { 0, 1, 2 };
    CHECK(git_blob_id(three, 3) == "8352675d67aed6625ece79af41c27fdb4ee2e867");
    CHECK(azdo_api_url(L"https://devops.example.com/projects/Coll/sandbox/_git/explorerplus") ==
          L"https://devops.example.com/projects/Coll/sandbox/_apis/git/repositories/explorerplus");
    CHECK(azdo_api_url(L"https://dev.azure.com/org/p/repo").empty());
    // refusals, as the server words them
    CHECK(azdo_rest_failure(409, R"({"message":"TF401028: The reference 'refs/heads/x' has already been updated by another client, so you cannot update it.","typeKey":"GitReferenceStaleException"})", L"Pushing").st == Status::CommitRaced);
    CHECK(azdo_rest_failure(400, R"({"message":"The maximum request size of 26214400 bytes was exceeded.\r\nParameter name: contentStream"})", L"Pushing").st == Status::TooLarge);
    CHECK(azdo_rest_failure(403, R"({"message":"TF402455: Pushes to this branch are not permitted; you must use a pull request to update this branch."})", L"Pushing").st == Status::Rejected);
    Result r = azdo_rest_failure(403, R"({"message":"TF401027: You need the Git 'GenericContribute' permission to perform this action."})", L"Pushing");
    CHECK(r.st == Status::Rejected && r.message.find(L"TF401027") != std::wstring::npos);
    CHECK(azdo_rest_failure(203, "<html>sign in</html>", L"x").st == Status::AuthFailed);
    CHECK(azdo_rest_failure(401, "", L"x").st == Status::AuthFailed);
    CHECK(azdo_rest_failure(404, R"({"message":"TF401019: The Git repository with name or identifier nope does not exist."})", L"x").st == Status::Rejected);
    CHECK(azdo_rest_failure(503, "", L"x").st == Status::Throttled && azdo_rest_failure(502, "", L"x").st == Status::Offline);
    // a file over the push limit is refused before anything is sent
    AzdoRestOptions o;
    o.url = L"https://dev.azure.com/o/p/_git/r";
    o.pat = L"x";
    o.maxPushBytes = 16 * 1024;
    auto b = make_azdo_rest_backend(o);
    CHECK(b->caps().maxFileSize == (16 * 1024 - 4096) / 4 * 3);
    MemorySource big(std::vector<uint8_t>((size_t)b->caps().maxFileSize + 1, 'x'));
    RemoteEntry out;
    NoProgress np;
    CHECK(b->upload(L"big.bin", big, FileMeta{}, "", out, np).st == Status::TooLarge);
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

// the raw REST store against the server, with a git.exe store reading the same branch
void test_rest_raw(const std::wstring& url, const std::wstring& pat, const std::wstring& branch, const std::wstring& base) {
    AzdoRestOptions o;
    o.url = url;
    o.pat = pat;
    o.branch = branch;
    o.subfolder = L"raw";
    auto a = make_azdo_rest_backend(o);
    auto b = make_azdo_rest_backend(o);
    auto g = make_git_backend(azdo_git_options(url, pat, branch, L"raw", base + L"\\raw.git"));
    NoProgress np;
    {
        AzdoRestOptions bad = o;
        bad.pat = L"not-a-valid-token-0000000000000000000000000000";
        Result r = make_azdo_rest_backend(bad)->connect(np);
        CHECK(r.st == Status::AuthFailed);
        if (r.st != Status::AuthFailed) std::printf("  REST, wrong token gave: %ls\n", r.message.c_str());
    }
    Result c = a->connect(np);
    CHECK(c.ok());
    if (!c.ok()) { std::printf("  REST connect: %ls\n", c.message.c_str()); return; }
    CHECK(b->connect(np).ok() && g->connect(np).ok());
    bool changed = false;
    std::string tok;
    CHECK(a->changed_since("", changed, tok, np).ok() && tok == "none");
    ChangeBatch cb;
    CHECK(a->changes("", cb, np).ok() && cb.full && cb.upserts.empty());
    // A commits two files
    std::string r1, r2, cursor;
    CHECK(a->begin(np).ok());
    CHECK(put(*a, L"one.txt", "one", "", &r1).ok());
    CHECK(put(*a, L"d/two.txt", "two", "", &r2).ok());
    CHECK(put(*a, L"one.txt", "again", "").st == Status::PreconditionFailed);    // exists (staged)
    CHECK(a->commit(L"sync", cursor, np).ok() && cursor.size() == 40);
    CHECK(r1 == git_blob_id("one", 3));
    // B sees them: a full listing, then nothing new; git.exe reads the same revs
    CHECK(b->changed_since("none", changed, tok, np).ok() && changed && tok == cursor);
    CHECK(b->changes("", cb, np).ok() && cb.full && cb.upserts.size() == 2);
    const RemoteEntry* e1 = find(cb.upserts, L"one.txt");
    CHECK(e1 && e1->rev == r1 && e1->size == 3 && get(*b, *e1) == "one");
    std::string bcur = cb.cursor;
    CHECK(b->changes(bcur, cb, np).ok() && !cb.full && cb.upserts.empty() && cb.removals.empty());
    ChangeBatch gb;
    CHECK(g->changes("", gb, np).ok() && gb.upserts.size() == 2 && find(gb.upserts, L"d/two.txt") && find(gb.upserts, L"d/two.txt")->rev == r2);
    // B edits and moves in one push
    CHECK(b->begin(np).ok());
    std::string r3;
    CHECK(put(*b, L"one.txt", "one, edited", "stale-rev").st == Status::PreconditionFailed);
    CHECK(put(*b, L"one.txt", "one, edited", r1, &r3).ok());
    RemoteEntry mv;
    CHECK(b->move(L"d/two.txt", L"e/two.txt", r2, mv, np).ok() && mv.rev == r2);
    CHECK(b->commit(L"sync", cursor, np).ok());
    CHECK(a->changes(bcur, cb, np).ok() && cb.full && cb.upserts.size() == 2);
    CHECK(find(cb.upserts, L"e/two.txt") && find(cb.upserts, L"one.txt") && find(cb.upserts, L"one.txt")->rev == r3);
    // a race: A pushes on a stale base -> CommitRaced; after listing again it goes through
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
    // a move, then an upload onto the moved file in the same pass: the move goes up first
    CHECK(a->begin(np).ok());
    CHECK(a->move(L"a-only.txt", L"f/a.txt", git_blob_id("a", 1), mv, np).ok());
    CHECK(put(*a, L"f/a.txt", "a, edited", git_blob_id("a", 1)).ok());
    CHECK(a->commit(L"sync", cursor, np).ok());
    // remove, stat, list, versions
    RemoteEntry st;
    CHECK(a->stat(L"one.txt", st, np).ok() && st.rev == r3);
    CHECK(a->stat(L"nope.txt", st, np).st == Status::NotFound);
    CHECK(a->stat(L"e", st, np).ok() && st.isDir);
    std::vector<RemoteEntry> lst;
    CHECK(a->list(L"e", lst, np).ok() && lst.size() == 1 && lst[0].path == L"e/two.txt");
    CHECK(a->list(L"f", lst, np).ok() && lst.size() == 1 && lst[0].rev == git_blob_id("a, edited", 9));
    std::vector<RemoteVersion> vers;
    CHECK(a->list_versions(L"one.txt", vers, np).ok() && vers.size() == 2);
    if (vers.size() == 2) CHECK(vers[0].rev == r3 && vers[1].rev == r1 && vers[1].size == 3 && vers[0].when.dwHighDateTime != 0);
    CHECK(a->begin(np).ok());
    CHECK(a->remove(L"one.txt", r3, np).ok());
    CHECK(a->commit(L"sync", cursor, np).ok());
    CHECK(b->stat(L"one.txt", st, np).st == Status::NotFound);
    // a commit with nothing in it pushes nothing
    CHECK(a->begin(np).ok());
    CHECK(a->commit(L"sync", cursor, np).ok() && cursor.empty());
    // a pass over the push limit goes up in parts, each one whole
    AzdoRestOptions small = o;
    small.maxPushBytes = 16 * 1024;
    auto s = make_azdo_rest_backend(small);
    CHECK(s->connect(np).ok() && s->changes("", cb, np).ok());
    CHECK(s->begin(np).ok());
    for (int i = 0; i < 3; ++i) CHECK(put(*s, L"parts/" + std::to_wstring(i) + L".bin", std::string(5000, (char)('a' + i)), "").ok());
    CHECK(s->commit(L"sync", cursor, np).ok() && cursor.size() == 40);
    CHECK(a->list(L"parts", lst, np).ok() && lst.size() == 3);
    // the git.exe store agrees about all of it
    CHECK(g->changes("", gb, np).ok() && gb.upserts.size() == lst.size() + 3);           // + e/two, b-only, f/a
    for (auto& e : lst) CHECK(find(gb.upserts, e.path) && find(gb.upserts, e.path)->rev == e.rev);
}

std::wstring env_of(const wchar_t* n) {
    wchar_t b[4096];
    DWORD k = GetEnvironmentVariableW(n, b, 4096);
    return k && k < 4096 ? std::wstring(b, k) : std::wstring();
}

struct Dev {
    std::wstring root, stateDir, mirror;
    bool rest = false;
    std::unique_ptr<synckeys::KeyStore> ks = synckeys::memory_key_store();
    std::unique_ptr<StateStore> st;
    std::unique_ptr<Engine> eng;
};

void test_e2e(bool rest) {
    std::wstring url = env_of(L"EXPLORERPLUS_AZDO_TEST_URL"), pat = env_of(L"EXPLORERPLUS_AZDO_TEST_PAT");
    if (url.empty() || pat.empty()) { std::printf("  Azure DevOps end-to-end test skipped (EXPLORERPLUS_AZDO_TEST_URL / _PAT not set)\n"); return; }
    url = azdo_clone_url(url, nullptr);
    CHECK(!url.empty());
    uint8_t id[4];
    crypto::random(id, 4);
    std::string hx = crypto::hex(id, 4);
    std::wstring branch = L"sync-test/" + std::wstring(hx.begin(), hx.end()) + (rest ? L"-rest" : L"-git");
    std::wstring base = tu::temp_root(L"azdo");
    std::printf("  Azure DevOps end-to-end (%s): branch %ls\n", rest ? "REST API" : "git.exe", branch.c_str());
    NoProgress np;
    ULONGLONG t0 = GetTickCount64();

    // a wrong token: refused, said so, not retried
    if (!rest) {
        auto bad = make_git_backend(azdo_git_options(url, L"not-a-valid-token-0000000000000000000000000000", branch, L"", base + L"\\bad.git"));
        Result r = bad->connect(np);
        CHECK(r.st == Status::AuthFailed);
        if (r.st != Status::AuthFailed) std::printf("  wrong token gave: %ls\n", r.message.c_str());
    } else
        test_rest_raw(url, pat, branch, base);

    // REST: A and B through the REST API, and C through git.exe joins the same branch
    Dev a, b, c;
    a.root = tu::temp_root(L"aza"); a.stateDir = tu::temp_root(L"aza-state"); a.mirror = base + L"\\a.git"; a.rest = rest;
    b.root = tu::temp_root(L"azb"); b.stateDir = tu::temp_root(L"azb-state"); b.mirror = base + L"\\b.git"; b.rest = rest;
    c.root = tu::temp_root(L"azc"); c.stateDir = tu::temp_root(L"azc-state"); c.mirror = base + L"\\c.git";
    auto raw = [&](Dev& d) -> std::unique_ptr<Backend> {
        if (!d.rest) return make_git_backend(azdo_git_options(url, pat, branch, L"vault", d.mirror));
        AzdoRestOptions o;
        o.url = url;
        o.pat = pat;
        o.branch = branch;
        o.subfolder = L"vault";
        return make_azdo_rest_backend(o);
    };
    auto open = [&](Dev& d, bool create) -> Result {
        d.eng.reset();
        d.st.reset();
        auto rb = raw(d);
        Result r = rb->connect(np);
        if (!r.ok()) return r;
        synckeys::Vault v;
        r = synckeys::open_vault(*rb, *d.ks, create, v, np);
        if (!r.ok()) return r;
        d.st = std::make_unique<StateStore>(d.stateDir);
        d.st->load();
        EngineOptions eo;
        eo.root = d.root;
        eo.deviceName = L"DEV";
        eo.trashDir = d.stateDir + L"\\trash";
        eo.settleMs = 0;
        d.eng = std::make_unique<Engine>(eo, make_encrypting(std::move(rb), v), *d.st);
        return {};
    };
    auto sync = [](Dev& d) {
        NoProgress p;
        PassReport r = d.eng->sync(p);
        if (!r.result.ok()) std::printf("  sync: %s\n", to_utf8(r.result.message).c_str());
        for (auto& [path, why] : d.eng->status().problems) std::printf("  problem %ls: %ls\n", path.c_str(), why.c_str());
        return r;
    };

    std::string rec;
    CHECK(synckeys::setup_ring(*a.ks, rec).ok());
    Result r = open(a, true);
    CHECK(r.ok());
    if (!r.ok()) std::printf("  open: %ls\n", r.message.c_str());
    if (r.ok()) {
        tu::write(a.root, L"Zpr\x00e1vy/report Q3.txt", "AZDO-SECRET-REPORT");
        tu::write(a.root, L"big/blob.bin", std::string(3 * 1024 * 1024 + 17, 'Z'));
        tu::write(a.root, L"note.md", "AZDO-SECRET-NOTE");
        CHECK(sync(a).result.ok());
        // B pairs with a code from A and gets everything
        {
            auto ra = raw(a);
            auto rb = raw(b);
            CHECK(ra->connect(np).ok() && rb->connect(np).ok());
            std::string code;
            CHECK(synckeys::create_pairing(*ra, *a.ks, code, np).ok());
            CHECK(synckeys::redeem_pairing(*rb, *b.ks, code, np).ok());
        }
        CHECK(open(b, false).ok());
        CHECK(sync(b).result.ok());
        CHECK(tu::read(b.root, L"Zpr\x00e1vy/report Q3.txt") == "AZDO-SECRET-REPORT");
        CHECK(tu::read(b.root, L"big/blob.bin").size() == 3 * 1024 * 1024 + 17);
        // both edit: the second keeps both; a rename and a delete travel
        tu::write(a.root, L"note.md", "AZDO-SECRET-NOTE from A");
        tu::write(b.root, L"note.md", "AZDO-SECRET-NOTE from B!");
        CHECK(tu::rename(b.root, L"big/blob.bin", L"blob moved.bin"));
        tu::remove(b.root, L"Zpr\x00e1vy/report Q3.txt");
        CHECK(sync(a).result.ok());
        PassReport pb = sync(b);
        CHECK(pb.result.ok() && pb.conflicts == 1);
        CHECK(sync(a).result.ok());
        CHECK(sync(b).result.ok());
        auto ta = tu::tree(a.root), tb = tu::tree(b.root);
        CHECK(ta == tb);
        if (ta != tb) std::printf("  A:\n%s  B:\n%s", tu::dump(ta).c_str(), tu::dump(tb).c_str());
        CHECK(tu::read(a.root, L"note.md") == "AZDO-SECRET-NOTE from A");
        CHECK(tu::exists(a.root, L"blob moved.bin") && !tu::exists(a.root, L"Zpr\x00e1vy/report Q3.txt"));
        // a poll that finds nothing costs one request and changes nothing
        PassReport idle = sync(a);
        CHECK(idle.log.empty());
        if (rest) {
            // C reaches the branch through git.exe: pairs, gets the same files, and its edit
            // comes back to A through the REST API
            {
                auto ra = raw(a);
                auto rc = raw(c);
                CHECK(ra->connect(np).ok() && rc->connect(np).ok());
                std::string code;
                CHECK(synckeys::create_pairing(*ra, *a.ks, code, np).ok());
                CHECK(synckeys::redeem_pairing(*rc, *c.ks, code, np).ok());
            }
            CHECK(open(c, false).ok());
            CHECK(sync(c).result.ok());
            CHECK(tu::tree(c.root) == tu::tree(a.root));
            tu::write(c.root, L"from git.txt", "AZDO-SECRET-C");
            CHECK(sync(c).result.ok());
            CHECK(sync(a).result.ok());
            CHECK(tu::read(a.root, L"from git.txt") == "AZDO-SECRET-C");
            // a file's history through the REST API, and a restore
            std::vector<RemoteVersion> vs;
            CHECK(a.eng->versions(L"note.md", vs, np).ok() && vs.size() >= 2);
            if (vs.size() >= 2) {
                CHECK(a.eng->restore_version(L"note.md", vs.back().rev, np).ok());
                CHECK(tu::read(a.root, L"note.md") == "AZDO-SECRET-NOTE");
                CHECK(sync(a).result.ok());
                CHECK(sync(c).result.ok());
                CHECK(tu::read(c.root, L"note.md") == "AZDO-SECRET-NOTE");
            }
        }
        // what the server holds: nothing readable (names, contents, commit authors)
        proc::Options o;
        o.env = { { L"GIT_DIR", rest ? c.mirror : a.mirror } };
        proc::Result names = proc::run(find_git(), { L"log", L"--name-only", L"--format=%an <%ae> %s", L"refs/sync/tip" }, o);
        CHECK(names.ok() && names.out.find("report") == std::string::npos && names.out.find("note") == std::string::npos &&
              names.out.find("blob") == std::string::npos && names.out.find("DEV") == std::string::npos && names.out.find("Explorer Plus <sync@localhost>") != std::string::npos);
        proc::Result blobs = proc::run(find_git(), { L"cat-file", L"--batch-all-objects", L"--batch" }, o);
        CHECK(blobs.ok() && blobs.out.find("AZDO-SECRET") == std::string::npos && blobs.out.find("ZZZZZZZZZZZZ") == std::string::npos);
    }
    a.eng.reset(); a.st.reset(); b.eng.reset(); b.st.reset(); c.eng.reset(); c.st.reset();
    // the run's branch goes away
    AzdoRestOptions ro;
    ro.url = url;
    ro.pat = pat;
    ro.branch = branch;
    Result del = rest ? azdo_rest_delete_branch(ro) : git_delete_remote_branch(azdo_git_options(url, pat, branch, L""));
    CHECK(del.ok());
    if (!del.ok()) std::printf("  delete branch: %ls\n", del.message.c_str());
    {
        auto check = make_git_backend(azdo_git_options(url, pat, branch, L"", base + L"\\check.git"));
        bool changed = false;
        std::string tok;
        CHECK(check->changed_since("", changed, tok, np).ok() && tok == "none");
    }
    std::printf("  Azure DevOps end-to-end (%s): %.1f s\n", rest ? "REST API" : "git.exe", (GetTickCount64() - t0) / 1000.0);
    for (Dev* d : { &a, &b, &c }) { tu::rmrf(d->root); tu::rmrf(d->stateDir); }
    tu::rmrf(base);
}

} // namespace

void run_azdo_tests() {
    test_rules();
    test_rest_rules();
    test_e2e(true);
    test_e2e(false);
}
