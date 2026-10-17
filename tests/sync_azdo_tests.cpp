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

std::wstring env_of(const wchar_t* n) {
    wchar_t b[4096];
    DWORD k = GetEnvironmentVariableW(n, b, 4096);
    return k && k < 4096 ? std::wstring(b, k) : std::wstring();
}

struct Dev {
    std::wstring root, stateDir, mirror;
    std::unique_ptr<synckeys::KeyStore> ks = synckeys::memory_key_store();
    std::unique_ptr<StateStore> st;
    std::unique_ptr<Engine> eng;
};

void test_e2e() {
    std::wstring url = env_of(L"EXPLORERPLUS_AZDO_TEST_URL"), pat = env_of(L"EXPLORERPLUS_AZDO_TEST_PAT");
    if (url.empty() || pat.empty()) { std::printf("  Azure DevOps end-to-end test skipped (EXPLORERPLUS_AZDO_TEST_URL / _PAT not set)\n"); return; }
    url = azdo_clone_url(url, nullptr);
    CHECK(!url.empty());
    uint8_t id[4];
    crypto::random(id, 4);
    std::string hx = crypto::hex(id, 4);
    std::wstring branch = L"sync-test/" + std::wstring(hx.begin(), hx.end());
    std::wstring base = tu::temp_root(L"azdo");
    std::printf("  Azure DevOps end-to-end: branch %ls\n", branch.c_str());
    NoProgress np;
    ULONGLONG t0 = GetTickCount64();

    // a wrong token: refused, said so, not retried
    {
        auto bad = make_git_backend(azdo_git_options(url, L"not-a-valid-token-0000000000000000000000000000", branch, L"", base + L"\\bad.git"));
        Result r = bad->connect(np);
        CHECK(r.st == Status::AuthFailed);
        if (r.st != Status::AuthFailed) std::printf("  wrong token gave: %ls\n", r.message.c_str());
    }

    Dev a, b;
    a.root = tu::temp_root(L"aza"); a.stateDir = tu::temp_root(L"aza-state"); a.mirror = base + L"\\a.git";
    b.root = tu::temp_root(L"azb"); b.stateDir = tu::temp_root(L"azb-state"); b.mirror = base + L"\\b.git";
    auto raw = [&](Dev& d) { return make_git_backend(azdo_git_options(url, pat, branch, L"vault", d.mirror)); };
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
        // a poll that finds nothing costs one ls-remote and changes nothing
        PassReport idle = sync(a);
        CHECK(idle.log.empty());
        // what the server holds: nothing readable (names, contents, commit authors)
        proc::Options o;
        o.env = { { L"GIT_DIR", a.mirror } };
        proc::Result names = proc::run(find_git(), { L"log", L"--name-only", L"--format=%an <%ae> %s", L"refs/sync/tip" }, o);
        CHECK(names.ok() && names.out.find("report") == std::string::npos && names.out.find("note") == std::string::npos &&
              names.out.find("blob") == std::string::npos && names.out.find("DEV") == std::string::npos && names.out.find("Explorer Plus <sync@localhost>") != std::string::npos);
        proc::Result blobs = proc::run(find_git(), { L"cat-file", L"--batch-all-objects", L"--batch" }, o);
        CHECK(blobs.ok() && blobs.out.find("AZDO-SECRET") == std::string::npos && blobs.out.find("ZZZZZZZZZZZZ") == std::string::npos);
    }
    a.eng.reset(); a.st.reset(); b.eng.reset(); b.st.reset();
    // the run's branch goes away
    Result del = git_delete_remote_branch(azdo_git_options(url, pat, branch, L""));
    CHECK(del.ok());
    if (!del.ok()) std::printf("  delete branch: %ls\n", del.message.c_str());
    {
        auto check = make_git_backend(azdo_git_options(url, pat, branch, L"", base + L"\\check.git"));
        bool changed = false;
        std::string tok;
        CHECK(check->changed_since("", changed, tok, np).ok() && tok == "none");
    }
    std::printf("  Azure DevOps end-to-end: %.1f s\n", (GetTickCount64() - t0) / 1000.0);
    tu::rmrf(a.root); tu::rmrf(a.stateDir); tu::rmrf(b.root); tu::rmrf(b.stateDir);
    tu::rmrf(base);
}

} // namespace

void run_azdo_tests() {
    test_rules();
    test_e2e();
}
