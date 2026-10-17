// The encrypting layer and the keys: two devices through one store, pairing, recovery,
// rotation, tampering, long names, and a scan of the store for anything in plaintext.
#include "sync_tests_common.h"
#include "sync_test_util.h"
#include "sync_fake.h"
#include "sync_engine.h"
#include "sync_crypt.h"
#include "sync_keys.h"
#include "crypto.h"
#include <algorithm>
#include <cstdio>

using namespace sync;

namespace {

// a device: its own key store, folder and state, an engine over the encrypted store
struct EDev {
    std::wstring root, stateDir, trash;
    std::unique_ptr<synckeys::KeyStore> ks = synckeys::memory_key_store();
    std::unique_ptr<StateStore> st;
    std::unique_ptr<Engine> eng;
    std::function<std::unique_ptr<Backend>()> raw;
    explicit EDev(const std::wstring& name, std::function<std::unique_ptr<Backend>()> r) : raw(std::move(r)) {
        root = tu::temp_root(name);
        stateDir = tu::temp_root(name + L"-state");
        trash = tu::temp_root(name + L"-trash");
    }
    // opens the vault (creating it when allowed) and the engine; the result of the vault
    Result open(bool create) {
        eng.reset();
        st.reset();
        NoProgress p;
        auto rb = raw();
        synckeys::Vault v;
        Result r = synckeys::open_vault(*rb, *ks, create, v, p);
        if (!r.ok()) return r;
        synckeys::KeyStore* k = ks.get();
        auto enc = make_encrypting(std::move(rb), v, [k](Backend& b, synckeys::Vault& nv) { NoProgress np; return synckeys::open_vault(b, *k, false, nv, np); });
        st = std::make_unique<StateStore>(stateDir);
        st->load();
        EngineOptions o;
        o.root = root;
        o.deviceName = L"DEV";
        o.trashDir = trash;
        o.settleMs = 0;
        eng = std::make_unique<Engine>(o, std::move(enc), *st);
        return {};
    }
    PassReport sync() {
        NoProgress p;
        PassReport r = eng->sync(p);
        if (!r.result.ok()) std::printf("  sync: %s\n", to_utf8(r.result.message).c_str());
        if (_wgetenv(L"SYNC_TEST_VERBOSE")) {
            std::printf("  [%ls]\n", root.substr(root.find_last_of(L'\\') + 1).c_str());
            for (auto& l : r.log) std::printf("    %s\n", l.c_str());
            for (auto& [pp, why] : eng->status().problems) std::printf("    problem %ls: %ls\n", pp.c_str(), why.c_str());
        }
        return r;
    }
    ~EDev() { eng.reset(); st.reset(); tu::rmrf(root); tu::rmrf(stateDir); tu::rmrf(trash); }
};

bool same_tree(EDev& a, EDev& b) {
    auto ta = tu::tree(a.root), tb = tu::tree(b.root);
    if (ta != tb) std::printf("  A:\n%s  B:\n%s", tu::dump(ta).c_str(), tu::dump(tb).c_str());
    return ta == tb;
}

bool contains(const std::vector<uint8_t>& hay, const std::string& needle) {
    std::string h(hay.begin(), hay.end());
    return h.find(needle) != std::string::npos;
}

// nothing of the plaintext on the store: names, contents
bool store_is_opaque(FakeStore& s, const std::vector<std::string>& secrets) {
    for (auto& [p, o] : s.objs) {
        std::string path = to_utf8(p);
        for (auto& x : secrets) {
            if (path.find(x) != std::string::npos) { std::printf("  plaintext in a stored name: %s\n", path.c_str()); return false; }
            if (contains(o.data, x)) { std::printf("  plaintext in %s\n", path.c_str()); return false; }
        }
    }
    return true;
}

void test_ring_codec() {
    synckeys::Ring r;
    synckeys::Ring::Gen g;
    g.gen = 1;
    g.umk = crypto::random_key();
    r.gens.push_back(g);
    g.gen = 2;
    g.umk = crypto::random_key();
    r.gens.push_back(g);
    r.recKek = crypto::random_key();
    r.removed.push_back("0123456789abcdef");
    synckeys::Ring x;
    CHECK(x.parse(r.serialize()));
    CHECK(x.gens.size() == 2 && x.current().gen == 2 && x.current().umk == r.current().umk && x.removed == r.removed);
    CHECK(x.find(synckeys::umk_id(r.gens[0].umk)) != nullptr);
    auto b = r.serialize();
    b.resize(b.size() - 3);
    CHECK(!x.parse(b));
}

void test_two_devices(bool tx) {
    auto store = std::make_shared<FakeStore>();
    auto raw = [store, tx] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(store, tx)); };
    EDev a(L"ca", raw), b(L"cb", raw), c(L"cc", raw);
    // first device: keys, a vault in the empty store
    std::string recovery;
    CHECK(synckeys::setup_ring(*a.ks, recovery).ok());
    CHECK(crypto::normalize_code(recovery).size() == crypto::kCodeChars);
    CHECK(b.open(true).st == Status::Locked);        // no keys on B yet
    CHECK(a.open(true).ok());
    tu::write(a.root, L"Plan Q3 secret.txt", "TOPSECRET-CONTENT-1");
    tu::write(a.root, L"Projekty/\x0160koda/notes.md", "TOPSECRET-CONTENT-2");
    tu::write(a.root, L"empty-later/x.txt", "x");
    CHECK(a.sync().result.ok());
    CHECK(store_is_opaque(*store, { "Plan", "secret", "TOPSECRET", "Projekty", "notes", "empty-later" }));
    // B: a vault it can't open, then pairing with a code from A
    CHECK(b.open(false).st == Status::Locked);
    NoProgress p;
    {
        auto ra = raw();
        std::string code;
        CHECK(synckeys::create_pairing(*ra, *a.ks, code, p).ok());
        auto rb = raw();
        std::string wrong = crypto::new_code();
        CHECK(synckeys::redeem_pairing(*rb, *b.ks, wrong, p).st == Status::Rejected);
        // typed sloppily: lowercase, no dashes, O for 0
        std::string typed;
        for (char ch : code) if (ch != '-') typed += (char)tolower(ch == '0' ? 'O' : ch);
        CHECK(synckeys::redeem_pairing(*rb, *b.ks, typed, p).ok());
        CHECK(synckeys::redeem_pairing(*rb, *b.ks, code, p).st == Status::Rejected);   // used up
    }
    CHECK(b.open(false).ok());
    CHECK(b.sync().result.ok());
    CHECK(tu::read(b.root, L"Projekty/\x0160koda/notes.md") == "TOPSECRET-CONTENT-2");
    CHECK(same_tree(a, b));
    // edits, renames, deletes both ways
    tu::write(b.root, L"Plan Q3 secret.txt", "TOPSECRET-CONTENT-1, edited on B");
    CHECK(tu::rename(b.root, L"empty-later/x.txt", L"Projekty/moved x.txt"));
    CHECK(b.sync().result.ok());
    CHECK(a.sync().result.ok());
    if (tx) a.sync();
    CHECK(tu::read(a.root, L"Plan Q3 secret.txt") == "TOPSECRET-CONTENT-1, edited on B");
    CHECK(tu::read(a.root, L"Projekty/moved x.txt") == "x");
    tu::remove(a.root, L"Projekty/\x0160koda");
    a.sync();
    b.sync();
    if (tx) { a.sync(); b.sync(); }
    CHECK(!tu::exists(b.root, L"Projekty/\x0160koda"));
    CHECK(same_tree(a, b));
    // a restart (a fresh encrypting layer over a known cursor) maps paths again
    CHECK(a.open(false).ok());
    tu::write(a.root, L"Projekty/after restart.txt", "r");
    CHECK(a.sync().result.ok());
    CHECK(b.sync().result.ok());
    CHECK(tu::read(b.root, L"Projekty/after restart.txt") == "r");
    // C comes in through the recovery key alone
    {
        auto rc = raw();
        CHECK(synckeys::redeem_recovery(*rc, *c.ks, crypto::new_code(), p).st == Status::Rejected);
        CHECK(synckeys::redeem_recovery(*rc, *c.ks, recovery, p).ok());
    }
    CHECK(c.open(false).ok());
    CHECK(c.sync().result.ok());
    CHECK(same_tree(a, c));
    {
        auto r = raw();
        std::vector<synckeys::DeviceInfo> devs;
        CHECK(synckeys::list_devices(*r, *a.ks, devs, p).ok());
        CHECK(devs.size() == 3);
    }
    // A removes B: a new generation, a new vault key version, a new recovery key; C gets
    // the new ring handed over, B is locked out of what's written from now on
    std::string bId;
    {
        synckeys::DeviceKey bk = synckeys::device_key(*b.ks);
        bId = bk.id;
        auto r = raw();
        std::string newRecovery;
        CHECK(synckeys::remove_device(*r, *a.ks, bId, newRecovery, p).ok());
        CHECK(!newRecovery.empty() && newRecovery != recovery);
        std::vector<synckeys::DeviceInfo> devs;
        CHECK(synckeys::list_devices(*r, *a.ks, devs, p).ok());
        CHECK(devs.size() == 2);
        // the vault's recovery wrap is the new key's now
        auto tmp = synckeys::memory_key_store();
        CHECK(synckeys::redeem_recovery(*r, *tmp, recovery, p).st == Status::Rejected);
        CHECK(synckeys::redeem_recovery(*r, *tmp, newRecovery, p).ok());
    }
    CHECK(b.open(false).st == Status::Locked);
    CHECK(a.open(false).ok());
    tu::write(a.root, L"after removal.txt", "TOPSECRET-CONTENT-3");
    CHECK(a.sync().result.ok());
    CHECK(c.open(false).ok());                        // takes the hand-over
    CHECK(c.sync().result.ok());
    CHECK(tu::read(c.root, L"after removal.txt") == "TOPSECRET-CONTENT-3");
    CHECK(tu::read(c.root, L"Plan Q3 secret.txt") == "TOPSECRET-CONTENT-1, edited on B");   // older files: older key version
    CHECK(store_is_opaque(*store, { "TOPSECRET", "removal", "Projekty" }));
}

void test_tamper() {
    auto store = std::make_shared<FakeStore>();
    auto raw = [store] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(store, false)); };
    EDev a(L"ta", raw), b(L"tb", raw);
    std::string recovery;
    CHECK(synckeys::setup_ring(*a.ks, recovery).ok());
    CHECK(a.open(true).ok());
    tu::write(a.root, L"f.txt", "original content");
    tu::write(a.root, L"g.txt", "other content");
    CHECK(a.sync().result.ok());
    NoProgress p;
    {
        auto r = raw();
        CHECK(synckeys::redeem_recovery(*r, *b.ks, recovery, p).ok());
    }
    CHECK(b.open(false).ok());
    CHECK(b.sync().result.ok());
    // the storage flips a byte of one file's ciphertext (truncation and splicing are in
    // the format tests): that file stays as it was locally and is reported
    std::wstring victim;
    {
        std::lock_guard<std::mutex> lk(store->mx);
        std::vector<std::wstring> files;
        for (auto& [path, o] : store->objs) if (!o.isDir && path.compare(0, 9, L".ep-vault") != 0) files.push_back(path);
        CHECK(files.size() == 2);
        auto& f = store->objs[files[0]];
        f.data[f.data.size() - 20] ^= 1;
        f.rev = store->next_rev();
        ++store->gen;
        store->changelog.push_back({ store->gen, files[0] });
        store->objs[L".ep-vault/head"].rev = store->next_rev();      // the storage can move the marker too
    }
    std::string beforeF = tu::read(b.root, L"f.txt"), beforeG = tu::read(b.root, L"g.txt");
    b.sync();
    int flagged = 0;
    for (auto& [path, why] : b.eng->status().problems) if (path == L"f.txt" || path == L"g.txt") { ++flagged; victim = path; }
    CHECK(flagged == 1);
    CHECK(tu::read(b.root, L"f.txt") == beforeF && tu::read(b.root, L"g.txt") == beforeG);   // never overwritten by a bad download    // a planted file the storage made up: rejected (no key)
    {
        std::lock_guard<std::mutex> lk(store->mx);
        FakeStore::Obj o;
        o.data = std::vector<uint8_t>(200, 7);
        o.rev = store->next_rev();
        store->objs[L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"] = o;
        ++store->gen;
        store->changelog.push_back({ store->gen, L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" });
        store->objs[L".ep-vault/head"].rev = store->next_rev();
    }
    b.sync();
    CHECK(tu::tree(b.root).size() == 2);
}

void test_refuse_plaintext() {
    auto store = std::make_shared<FakeStore>();
    {
        FakeStore::Obj o;
        o.data = { 'h', 'i' };
        o.rev = "r0";
        store->objs[L"existing.txt"] = o;
    }
    EDev a(L"ra", [store] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(store, false)); });
    std::string rec;
    CHECK(synckeys::setup_ring(*a.ks, rec).ok());
    CHECK(a.open(true).st == Status::Rejected);
}

void test_long_names() {
    // a Folder store (NTFS: 255-character names): long names are shortened with a sidecar
    std::wstring dir = tu::temp_root(L"lnstore");
    BackendConfig cfg = { { L"path", dir } };
    register_builtin_backends();
    auto raw = [cfg] { std::wstring why; return create_raw_backend(L"folder", cfg, &why); };
    EDev a(L"la", raw), b(L"lb", raw);
    std::string rec;
    CHECK(synckeys::setup_ring(*a.ks, rec).ok());
    CHECK(a.open(true).ok());
    std::wstring longName(200, L'n');
    std::wstring longDir(150, L'd');
    tu::write(a.root, longDir + L"/" + longName + L".txt", "long");
    tu::write(a.root, L"short.txt", "short");
    CHECK(a.sync().result.ok());
    NoProgress p;
    {
        auto r = raw();
        CHECK(synckeys::redeem_recovery(*r, *b.ks, rec, p).ok());
    }
    CHECK(b.open(false).ok());
    CHECK(b.sync().result.ok());
    CHECK(tu::read(b.root, longDir + L"/" + longName + L".txt") == "long");
    CHECK(same_tree(a, b));
    // the head marker: a store without a change feed polls one object
    tu::write(a.root, L"short.txt", "short, changed");
    CHECK(a.sync().result.ok());
    CHECK(b.sync().downloads == 1);
    PassReport r = b.sync();
    CHECK(r.log.empty());
    tu::rmrf(dir);
}

} // namespace

void run_crypt_tests() {
    SetEnvironmentVariableW(L"EXPLORERPLUS_SYNC_KDF_KIB", L"1024");     // Argon2id: 1 MiB, 1 pass (tests)
    test_ring_codec();
    test_refuse_plaintext();
    test_two_devices(false);
    test_two_devices(true);
    test_tamper();
    test_long_names();
}
