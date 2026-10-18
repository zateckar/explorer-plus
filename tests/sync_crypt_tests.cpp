// The encrypting layer and the keys: two devices through one store, pairing, recovery,
// rotation, tampering, long names, and a scan of the store for anything in plaintext.
#include "sync_tests_common.h"
#include "sync_test_util.h"
#include "sync_fake.h"
#include "sync_engine.h"
#include "sync_crypt.h"
#include "sync_keys.h"
#include "sync_manifest.h"
#include "crypto.h"
#include "crypto_file.h"
#include "crypto_names.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

using namespace sync;

namespace {

// a device: its own key store, folder and state, an engine over the encrypted store
struct EDev {
    std::wstring root, stateDir, trash;
    std::unique_ptr<synckeys::KeyStore> ks = synckeys::memory_key_store();
    std::unique_ptr<StateStore> st;
    std::unique_ptr<Engine> eng;
    std::function<std::unique_ptr<Backend>()> raw;
    bool noManifest = false;                         // an encrypting layer from before manifests
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
        if (noManifest) v.me = {};
        synckeys::KeyStore* k = ks.get();
        auto enc = make_encrypting(std::move(rb), v, [k](Backend& b, synckeys::Vault& nv) { NoProgress np; return synckeys::open_vault(b, *k, false, nv, np); },
                                   stateDir + L"\\retired.dat");
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
    PassReport sync(bool quiet = false) {
        NoProgress p;
        PassReport r = eng->sync(p);
        if (!r.result.ok() && !quiet) std::printf("  sync: %s\n", to_utf8(r.result.message).c_str());
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
    synckeys::DeviceKey d = synckeys::new_device_key(L"member");
    r.members.push_back(d.member());
    synckeys::Ring x;
    CHECK(x.parse(r.serialize()));
    CHECK(x.gens.size() == 2 && x.current().gen == 2 && x.current().umk == r.current().umk && x.removed == r.removed);
    CHECK(x.find(synckeys::umk_id(r.gens[0].umk)) != nullptr);
    CHECK(x.member(d.id) && x.member(d.id)->spub == d.spub && x.is_removed("0123456789abcdef") && !x.is_removed(d.id));
    auto b = r.serialize();
    b.resize(b.size() - 3);
    CHECK(!x.parse(b));
    // a member whose ID isn't its key's: refused
    r.members[0].id = "0123456789abcdef";
    CHECK(!x.parse(r.serialize()));
    // the format before members (EPR1)
    b = r.serialize();
    b[3] = '1';
    CHECK(!x.parse(b));
}

void test_password_rules() {
    using synckeys::password_problem;
    CHECK(password_problem("Tidal Walrus 1").empty());
    CHECK(!password_problem("Password1234").empty());                         // guessable: a word and a run
    CHECK(!password_problem("Qwerty123456").empty());
    CHECK(!password_problem("Aaaaaaaaaaa1").empty());                         // one character again and again
    CHECK(!password_problem("Welcome2024!").empty());
    CHECK(password_problem("Abcd Kq7mWz2p").empty());                         // a run among enough else
    CHECK(password_problem("Kq7mWz2pLx9r").empty());                          // 12: enough
    CHECK(!password_problem("Kq7mWz2pLx9").empty());                          // 11
    CHECK(!password_problem("abcdefghij123").empty());                        // no capital
    CHECK(!password_problem("ABCDEFGHIJ123").empty());                        // no small letter
    CHECK(!password_problem("Abcdefghijklm").empty());                        // no digit
    CHECK(!password_problem("\xC5\xA0\xC5\xA1\xC5\xA1\xC5\xA1\xC5\xA1\xC5\xA1" "a1").empty());   // 8 characters in 14 bytes
    auto ks = synckeys::memory_key_store();
    CHECK(synckeys::setup_ring(*ks, "too weak").st == Status::Rejected);
    synckeys::Ring none;
    CHECK(!ks->load_ring(none));
    CHECK(synckeys::setup_ring(*ks, "Tidal Walrus 1").ok());
    CHECK(synckeys::check_password(*ks, "Tidal Walrus 1").ok());
    CHECK(synckeys::check_password(*ks, "tidal walrus 1").st == Status::Rejected);
    CHECK(synckeys::change_password(*ks, "weak").st == Status::Rejected);
    CHECK(synckeys::change_password(*ks, "Tidal Walrus 2").ok());
    CHECK(synckeys::check_password(*ks, "Tidal Walrus 2").ok());
    CHECK(synckeys::check_password(*ks, "Tidal Walrus 1").st == Status::Rejected);
    // a ring from before the password: its generated recovery key, typed as it's written
    synckeys::Ring r;
    CHECK(ks->load_ring(r));
    std::string code = crypto::new_code();
    CHECK(crypto::argon2id(code, r.recSalt, 16, r.recParams, r.recKek));
    CHECK(ks->save_ring(r));
    std::string typed;
    for (char ch : crypto::format_code(code)) typed += (char)tolower(ch);
    CHECK(synckeys::check_password(*ks, typed).ok());
}

void test_two_devices(bool tx) {
    auto store = std::make_shared<FakeStore>();
    auto raw = [store, tx] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(store, tx)); };
    EDev a(L"ca", raw), b(L"cb", raw), c(L"cc", raw);
    // first device: keys, a vault in the empty store
    std::string recovery = "Tidal Walrus 1";
    CHECK(synckeys::setup_ring(*a.ks, recovery).ok());
    CHECK(synckeys::check_password(*a.ks, recovery).ok());
    CHECK(synckeys::check_password(*a.ks, "Tidal Walrus 2").st == Status::Rejected);
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
        std::string newRecovery = "Another password 2";
        // the password stays someone else's to know: the same one, or a weak one, is refused
        CHECK(synckeys::remove_device(*r, *a.ks, bId, recovery, p).st == Status::Rejected);
        CHECK(synckeys::remove_device(*r, *a.ks, bId, "short", p).st == Status::Rejected);
        CHECK(synckeys::remove_device(*r, *a.ks, bId, newRecovery, p).ok());
        CHECK(synckeys::check_password(*a.ks, newRecovery).ok());
        CHECK(synckeys::check_password(*a.ks, recovery).st == Status::Rejected);
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
    std::string recovery = "Tidal Walrus 1";
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
        for (auto& [path, o] : store->objs) if (!o.isDir && !synckeys::is_vault_path(path)) files.push_back(path);
        CHECK(files.size() == 2);
        auto& f = store->objs[files[0]];
        f.data[f.data.size() - 20] ^= 1;
        f.rev = store->next_rev();
        ++store->gen;
        store->changelog.push_back({ store->gen, files[0] });
        store->objs[L".0/5"].rev = store->next_rev();      // the storage can move the marker too
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
        store->objs[L".0/5"].rev = store->next_rev();
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
    std::string rec = "Tidal Walrus 1";
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
    std::string rec = "Tidal Walrus 1";
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
    // two long-named siblings: the storage swaps their sidecars (each holds a name that's
    // authentic in this folder). A short name must match its sidecar, so neither shows up
    // under the other's name.
    tu::write(a.root, longDir + L"/" + std::wstring(200, L'm') + L".txt", "long two");
    CHECK(a.sync().result.ok());
    std::map<std::wstring, std::vector<std::filesystem::path>> sidecars;
    for (auto& f : std::filesystem::recursive_directory_iterator(dir))
        if (f.is_regular_file() && cnames::is_sidecar(f.path().filename().string())) sidecars[f.path().parent_path().wstring()].push_back(f.path());
    bool swapped = false;
    for (auto& [parent, files] : sidecars) {
        if (files.size() != 2) continue;
        std::filesystem::path tmp = files[0].wstring() + L".swap";
        std::filesystem::rename(files[0], tmp);
        std::filesystem::rename(files[1], files[0]);
        std::filesystem::rename(tmp, files[1]);
        swapped = true;
    }
    CHECK(swapped);
    EDev c(L"lc", raw);
    {
        auto rr = raw();
        CHECK(synckeys::redeem_recovery(*rr, *c.ks, rec, p).ok());
    }
    CHECK(c.open(false).ok());
    c.sync();
    CHECK(!tu::exists(c.root, longDir + L"/" + longName + L".txt"));
    CHECK(!tu::exists(c.root, longDir + L"/" + std::wstring(200, L'm') + L".txt"));
    CHECK(tu::read(c.root, L"short.txt") == "short, changed");
    tu::rmrf(dir);
}

// a rotation hand-over as the format describes it (sync-format.md, EPX2): what a removed
// device that kept its storage access can make with the generations and the device key it
// still has. `from` signs it under its own ID (`claimedId`: another device's ID instead).
crypto::Bytes forge_rotation(const synckeys::Ring& ring, const synckeys::Ring::Gen& under, const synckeys::DeviceKey& from,
                             const synckeys::DeviceKey& to, const std::string& claimedId = {}) {
    std::string sender = claimedId.empty() ? from.id : claimedId;
    crypto::Key eph = crypto::random_key(), ephPub;
    crypto::x25519_public(eph, ephPub);
    crypto::Key shared = crypto::x25519_shared(eph, to.pub);
    crypto::Key k = crypto::kdf(under.umk, "ep rotate", shared.data(), 32);
    crypto::Bytes rb = ring.serialize();
    crypto::Bytes s = crypto::seal(k, "ep rotate " + sender + " " + to.id, rb.data(), rb.size());
    crypto::Bytes b = { 'E', 'P', 'X', '2' };
    std::string uid = synckeys::umk_id(under.umk);
    b.insert(b.end(), uid.begin(), uid.end());
    b.insert(b.end(), sender.begin(), sender.end());
    b.insert(b.end(), ephPub.b.begin(), ephPub.b.end());
    b.insert(b.end(), s.begin(), s.end());
    // the signature: EdDSA over "ep rotate" || 0 || body, the seed KDF(secret, "ep sign")
    crypto::Bytes m = { 'e', 'p', ' ', 'r', 'o', 't', 'a', 't', 'e', 0 };
    m.insert(m.end(), b.begin(), b.end());
    uint8_t sig[crypto::kSig];
    crypto::eddsa_sign(crypto::kdf(from.secret, "ep sign"), m.data(), m.size(), sig);
    b.insert(b.end(), sig, sig + crypto::kSig);
    return b;
}

// writes an object as the storage (or anyone with write access) can, and moves the head
// marker so the devices look
void put_raw(FakeStore& s, const std::wstring& path, const crypto::Bytes& data) {
    std::lock_guard<std::mutex> lk(s.mx);
    auto& o = s.objs[path];
    o.data = data;
    o.isDir = false;
    o.rev = s.next_rev();
    s.changelog.push_back({ ++s.gen, path });
    s.objs[L".0/5"].rev = s.next_rev();
}

void bump_head(FakeStore& s) {
    std::lock_guard<std::mutex> lk(s.mx);
    s.objs[L".0/5"].rev = s.next_rev();
    ++s.gen;
}

std::vector<std::wstring> objects_under(FakeStore& s, const std::wstring& prefix) {
    std::lock_guard<std::mutex> lk(s.mx);
    std::vector<std::wstring> v;
    for (auto& [path, o] : s.objs) if (path.compare(0, prefix.size(), prefix) == 0) v.push_back(path);
    return v;
}

bool has_problem(EDev& d, const std::wstring& what) {
    for (auto& [path, why] : d.eng->status().problems) if (why.find(what) != std::wstring::npos) return true;
    return false;
}

// the vault key version in the header of every stored file not in `before`
std::vector<uint32_t> new_file_versions(FakeStore& s, const std::set<std::wstring>& before) {
    std::lock_guard<std::mutex> lk(s.mx);
    std::vector<uint32_t> v;
    for (auto& [path, o] : s.objs)
        if (!o.isDir && !before.count(path) && !synckeys::is_vault_path(path) && o.data.size() >= 12)
            v.push_back(o.data[8] | (o.data[9] << 8) | (o.data[10] << 16) | ((uint32_t)o.data[11] << 24));
    return v;
}

std::set<std::wstring> object_names(FakeStore& s) {
    std::lock_guard<std::mutex> lk(s.mx);
    std::set<std::wstring> n;
    for (auto& [path, o] : s.objs) n.insert(path);
    return n;
}

// stored paths of the files not in `before`
std::vector<std::wstring> new_files(FakeStore& s, const std::set<std::wstring>& before) {
    std::lock_guard<std::mutex> lk(s.mx);
    std::vector<std::wstring> v;
    for (auto& [path, o] : s.objs)
        if (!o.isDir && !before.count(path) && !synckeys::is_vault_path(path)) v.push_back(path);
    return v;
}

// a file as an encrypting layer writes it, under a vault key version of the caller's choice
crypto::Bytes encrypt_file(const std::string& text, const crypto::Key& vk, uint32_t version) {
    std::vector<uint8_t> plain(text.begin(), text.end());
    MemorySource src(plain);
    cfile::Meta m;
    m.size = plain.size();
    m.fingerprint = crypto::blake2b(plain.data(), plain.size());
    cfile::EncryptingSource es(src, vk, version, m);
    crypto::Bytes out;
    uint8_t buf[4096];
    size_t got = 0;
    while (es.read(buf, sizeof buf, &got) && got) out.insert(out.end(), buf, buf + got);
    return out;
}

Result upload_text(Backend& b, const std::wstring& path, const std::string& text) {
    std::vector<uint8_t> plain(text.begin(), text.end());
    MemorySource src(plain);
    FileMeta meta;
    meta.plainHash = crypto::blake2b(plain.data(), plain.size());
    RemoteEntry out;
    NoProgress p;
    return b.upload(path, src, meta, "*", out, p);
}

std::wstring wid(const std::string& id) { return std::wstring(id.begin(), id.end()); }

// A removed device that kept its storage access, and a storage that brings back old
// copies of the vault (docs/sync-review.md, S1 and S2)
void test_removal_hardening() {
    auto store = std::make_shared<FakeStore>();
    auto raw = [store] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(store, false)); };
    EDev a(L"ha", raw), b(L"hb", raw), c(L"hc", raw);
    NoProgress p;
    const std::wstring kVault = synckeys::kVaultFileRel, kRotate = L".0/4/";
    std::string recovery = "Tidal Walrus 1";
    CHECK(synckeys::setup_ring(*a.ks, recovery).ok());
    CHECK(a.open(true).ok());
    for (EDev* d : { &b, &c }) {
        auto r = raw();
        CHECK(synckeys::redeem_recovery(*r, *d->ks, recovery, p).ok());
        CHECK(d->open(false).ok());
    }
    synckeys::DeviceKey bk = synckeys::device_key(*b.ks), ck = synckeys::device_key(*c.ks);
    // a second vault of the user's (X), that nobody opens until after the removal
    auto xstore = std::make_shared<FakeStore>();
    auto xraw = [xstore] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(xstore, false)); };
    for (auto* ks : { a.ks.get(), b.ks.get(), c.ks.get() }) {
        auto r = xraw();
        synckeys::Vault v;
        CHECK(synckeys::open_vault(*r, *ks, ks == a.ks.get(), v, p).ok());
    }
    crypto::Bytes xOldVault = xstore->objs[kVault].data;
    const std::wstring bRecord = L".0/2/" + wid(bk.id);
    crypto::Bytes xbRecord = xstore->objs[bRecord].data;
    synckeys::VaultPin pin;                                   // C's view of the vault
    {
        auto r = raw();
        synckeys::Vault v;
        CHECK(synckeys::open_vault(*r, *c.ks, false, v, p, &pin).ok());
        CHECK(pin.id == v.id && pin.gen == 1 && pin.version == 1 && pin.hash.size() == 64);
    }
    tu::write(c.root, L"before.txt", "1");
    tu::write(c.root, L"old2.txt", "2");
    CHECK(c.sync().result.ok());
    CHECK(a.sync().result.ok());
    crypto::Bytes oldVault = store->objs[kVault].data;      // the storage keeps a copy
    synckeys::Ring bRing;
    CHECK(b.ks->load_ring(bRing));
    synckeys::Vault bVault;                                   // B's keys as they are now
    {
        auto r = raw();
        CHECK(synckeys::open_vault(*r, *b.ks, false, bVault, p).ok() && bVault.current == 1);
    }

    // A removes B. C keeps running: its next pass reads the changed vault before it
    // writes, so a new file is under the new key version, which B never got
    std::string newRecovery = "Another password 2";
    {
        auto r = raw();
        CHECK(synckeys::remove_device(*r, *a.ks, bk.id, newRecovery, p).ok());
    }
    // the manifest moved on with the vault: signed by A, under key version 2
    CHECK(syncman::root_signer(store->objs[syncman::kRootRel].data) == synckeys::device_key(*a.ks).id);
    auto before = object_names(*store);
    tu::write(c.root, L"after.txt", "TOPSECRET-AFTER");
    CHECK(c.sync().result.ok());
    auto vers = new_file_versions(*store, before);
    CHECK(vers.size() == 1 && vers[0] == 2);
    auto afterStored = new_files(*store, before);
    CHECK(afterStored.size() == 1);
    synckeys::Ring cRing;
    CHECK(c.ks->load_ring(cRing) && cRing.current().gen == 2 && cRing.is_removed(bk.id) && !cRing.member(bk.id));
    CHECK(a.sync().result.ok());
    CHECK(tu::read(a.root, L"after.txt") == "TOPSECRET-AFTER");

    // the pin: C has seen generation 2 and key version 2
    {
        auto r = raw();
        synckeys::Vault v;
        CHECK(synckeys::open_vault(*r, *c.ks, false, v, p, &pin).ok());
        CHECK(pin.gen == 2 && pin.version == 2);
    }
    // the storage brings back the old vault (generation 1, which B has): refused, where
    // it used to be "upgraded" with hand-overs sealed under generation 1
    crypto::Bytes curVault = store->objs[kVault].data;
    put_raw(*store, kVault, oldVault);
    {
        auto r = raw();
        synckeys::Vault v;
        synckeys::VaultPin pc = pin;
        CHECK(synckeys::open_vault(*r, *c.ks, false, v, p, &pc).st == Status::Tampered);
    }
    // another of the user's vaults in its place: refused too
    {
        auto other = std::make_shared<FakeStore>();
        FakeBackend ob(other, false);
        synckeys::Vault v;
        CHECK(synckeys::open_vault(ob, *a.ks, true, v, p).ok());
        put_raw(*store, kVault, other->objs[kVault].data);
        auto r = raw();
        synckeys::VaultPin pc = pin;
        CHECK(synckeys::open_vault(*r, *c.ks, false, v, p, &pc).st == Status::Tampered);
        // gone altogether: not quietly made again
        {
            std::lock_guard<std::mutex> lk(store->mx);
            store->objs.erase(kVault);
        }
        CHECK(synckeys::open_vault(*r, *c.ks, true, v, p, &pc).st == Status::Tampered);
    }
    // S1 (c): a vault B makes up (its own vault key, sealed under generation 1, signed by
    // B): refused by the devices that know of the removal, with a pin or without
    {
        auto own = std::make_shared<FakeStore>();
        FakeBackend ob(own, false);
        synckeys::Vault v;
        CHECK(synckeys::open_vault(ob, *b.ks, true, v, p).ok());
        put_raw(*store, kVault, own->objs[kVault].data);
        auto r = raw();
        CHECK(synckeys::open_vault(*r, *c.ks, false, v, p).st == Status::Tampered);
        CHECK(synckeys::open_vault(*r, *a.ks, false, v, p).st == Status::Tampered);
    }
    put_raw(*store, kVault, curVault);
    {
        auto r = raw();
        synckeys::Vault v;
        CHECK(synckeys::open_vault(*r, *c.ks, false, v, p, &pin).ok());
    }

    // S1 (b): in vault X, still on generation 1, B plants the record of a device of its own
    // (F: sealed under generation 1, signed by F's key). A moves X on: a hand-over for C
    // alone, none for F or B, B's record gone, F not taken in
    synckeys::DeviceKey fk = synckeys::new_device_key(L"F");
    auto fks = synckeys::memory_key_store();
    fks->save_ring(bRing);
    fks->save_device(fk);
    {
        auto r = xraw();
        synckeys::Vault v;
        CHECK(synckeys::open_vault(*r, *fks, false, v, p).ok());
        CHECK(xstore->objs.count(L".0/2/" + wid(fk.id)));
    }
    {
        auto r = xraw();
        synckeys::Vault v;
        CHECK(synckeys::open_vault(*r, *a.ks, false, v, p).ok() && v.current == 2);
        auto rot = objects_under(*xstore, kRotate);
        CHECK(rot.size() == 1 && rot[0].find(wid(ck.id)) != std::wstring::npos);
        CHECK(!xstore->objs.count(bRecord));
        synckeys::Ring ar;
        CHECK(a.ks->load_ring(ar) && !ar.member(fk.id) && !ar.member(bk.id) && ar.member(ck.id));
        std::vector<synckeys::DeviceInfo> devs;
        CHECK(synckeys::list_devices(*r, *a.ks, devs, p).ok());
        CHECK(devs.size() == 2);                              // A and C; F's record isn't a member's
    }
    synckeys::VaultPin xpin;
    {
        auto r = xraw();
        synckeys::Vault v;
        CHECK(synckeys::open_vault(*r, *c.ks, false, v, p, &xpin).ok() && xpin.gen == 2);
        CHECK(objects_under(*xstore, kRotate).empty());       // nothing new in it for C: dropped
        CHECK(synckeys::open_vault(*r, *fks, false, v, p).st == Status::Locked);
    }
    // S1 (c): the storage brings back X's old copy (generation 1, signed by A while B was a
    // member) and B its record. C, pinned, refuses it; A, without a pin, moves it on again,
    // and again hands over to C alone
    put_raw(*xstore, kVault, xOldVault);
    put_raw(*xstore, bRecord, xbRecord);
    {
        auto r = xraw();
        synckeys::Vault v;
        synckeys::VaultPin pc = xpin;
        CHECK(synckeys::open_vault(*r, *c.ks, false, v, p, &pc).st == Status::Tampered);
        CHECK(synckeys::open_vault(*r, *a.ks, false, v, p).ok());
        auto rot = objects_under(*xstore, kRotate);
        CHECK(rot.size() == 1 && rot[0].find(wid(ck.id)) != std::wstring::npos);
        CHECK(!xstore->objs.count(bRecord));
    }

    // S1 (a): B hands C a ring of its own (generation 99, its own user key), sealed under
    // generation 1 and signed by B. A device still on generation 1 that hasn't heard of the
    // removal takes it (the window the review describes); C refuses it and drops it.
    synckeys::Ring evil = bRing;
    synckeys::Ring::Gen x;
    x.gen = 99;
    x.umk = crypto::random_key();
    evil.gens.push_back(x);
    if (!evil.member(ck.id)) evil.members.push_back(ck.member());
    std::wstring rotPath = kRotate + wid(ck.id) + L"-g99";
    crypto::Bytes forged = forge_rotation(evil, bRing.current(), bk, ck);
    {
        auto lagging = synckeys::memory_key_store();             // C's device key with the old ring
        lagging->save_ring(bRing);
        lagging->save_device(ck);
        put_raw(*store, rotPath, forged);
        auto r = raw();
        synckeys::Vault v;
        synckeys::open_vault(*r, *lagging, false, v, p);
        synckeys::Ring got;
        CHECK(lagging->load_ring(got) && got.current().gen == 99);    // the forgery is well-formed
    }
    put_raw(*store, rotPath, forged);
    {
        auto r = raw();
        synckeys::Vault v;
        CHECK(synckeys::open_vault(*r, *c.ks, false, v, p, &pin).ok());
        synckeys::Ring got;
        CHECK(c.ks->load_ring(got) && got.current().gen == 2 && !got.find(synckeys::umk_id(x.umk)));
        CHECK(!store->objs.count(rotPath));
    }
    // a device that was never a member (F) hands nothing over, even to a lagging device;
    // nor does F's key passed off as B's
    {
        auto lagging = synckeys::memory_key_store();
        lagging->save_ring(bRing);
        lagging->save_device(ck);
        put_raw(*store, rotPath, forge_rotation(evil, bRing.current(), fk, ck));
        auto r = raw();
        synckeys::Vault v;
        synckeys::open_vault(*r, *lagging, false, v, p);
        synckeys::Ring got;
        CHECK(lagging->load_ring(got) && got.current().gen == 1);
        put_raw(*store, rotPath, forge_rotation(evil, bRing.current(), fk, ck, bk.id));
        synckeys::open_vault(*r, *lagging, false, v, p);
        CHECK(lagging->load_ring(got) && got.current().gen == 1);
        CHECK(!store->objs.count(rotPath));                  // a signature that doesn't check: dropped
    }

    // S2: B still has vault key version 1 and write access. A and C noted the storage when
    // version 1 was retired, so what B writes now is refused: a new file, a new folder, an
    // old file overwritten, and a new file's content replaced by one under version 1
    {
        synckeys::Vault bOld = bVault;
        bOld.me = {};                                         // writes no manifest (it'd be refused)
        auto benc = make_encrypting(raw(), bOld);             // B's encrypting layer, as it was
        CHECK(upload_text(*benc, L"evil.txt", "EVIL-NEW").ok());
        CHECK(upload_text(*benc, L"evil dir/x.txt", "EVIL-DIR").ok());
        CHECK(upload_text(*benc, L"before.txt", "EVIL-OVERWRITE").ok());
        crypto::Key vk1;
        CHECK(bVault.key(1, vk1));
        put_raw(*store, afterStored[0], encrypt_file("EVIL-AFTER", vk1, 1));
        bump_head(*store);
    }
    for (EDev* d : { &a, &c }) {
        d->sync();
        CHECK(!tu::exists(d->root, L"evil.txt") && !tu::exists(d->root, L"evil dir"));
        CHECK(tu::read(d->root, L"before.txt") == "1" && tu::read(d->root, L"after.txt") == "TOPSECRET-AFTER");
        CHECK(has_problem(*d, L"retired"));
    }
    // and a manifest of B's own (under key version 1, listing B's file): refused, whoever
    // reads it, as long as it stands
    {
        crypto::Bytes cur = store->objs[syncman::kRootRel].data;
        syncman::Manifest m;
        m.seq = 1000;
        syncman::Entry e;
        e.size = 8;
        e.rev = "x";
        m.files[L"evil.txt"] = e;
        syncman::Encoded enc;
        CHECK(syncman::encode(m, bVault.id, [&](uint32_t v, crypto::Key& k) { return bVault.key(v, k); }, 1, bVault.me, nullptr, enc).ok());
        for (size_t i = 0; i < enc.shards.size(); ++i) put_raw(*store, syncman::shard_rel(enc.hashes[i]), enc.shards[i]);
        put_raw(*store, syncman::kRootRel, enc.root);
        for (EDev* d : { &a, &c }) {
            PassReport r = d->sync(true);
            CHECK(r.result.st == Status::Tampered && r.result.message.find(L"removed") != std::wstring::npos);
            CHECK(!tu::exists(d->root, L"evil.txt") && tu::read(d->root, L"before.txt") == "1");
        }
        put_raw(*store, syncman::kRootRel, cur);
        CHECK(a.sync().result.ok() && c.sync().result.ok());
    }
    // what the remaining devices write still goes through: an old file renamed (uploaded
    // again under the current key, not moved with its old content)
    CHECK(tu::rename(c.root, L"old2.txt", L"moved old2.txt"));
    c.sync();
    a.sync();
    CHECK(tu::read(a.root, L"moved old2.txt") == "2" && !tu::exists(a.root, L"old2.txt"));

    // a new sync password starts a new generation: the ring an old copy of the vault gives
    // to the previous password no longer opens the vault, and C follows
    std::string thirdRecovery = "Third password 3";
    CHECK(synckeys::change_password(*a.ks, thirdRecovery).ok());
    crypto::Bytes gen2Vault = store->objs[kVault].data;
    {
        auto r = raw();
        CHECK(synckeys::refresh_vault_recovery(*r, *a.ks, p).ok());
    }
    {
        auto copy = std::make_shared<FakeStore>();
        put_raw(*copy, kVault, gen2Vault);
        FakeBackend cb(copy, false);
        auto leaked = synckeys::memory_key_store();
        CHECK(synckeys::redeem_recovery(cb, *leaked, newRecovery, p).ok());
        auto r = raw();
        synckeys::Vault v;
        CHECK(synckeys::open_vault(*r, *leaked, false, v, p).st == Status::Locked);
    }
    before = object_names(*store);
    tu::write(c.root, L"third.txt", "3");
    c.sync();
    vers = new_file_versions(*store, before);
    CHECK(vers.size() == 1 && vers[0] == 3);
    // a restart: the note of retired versions is kept, so B's writes stay refused
    CHECK(a.open(false).ok());
    a.sync();
    CHECK(tu::read(a.root, L"third.txt") == "3" && tu::read(a.root, L"after.txt") == "TOPSECRET-AFTER");
    CHECK(!tu::exists(a.root, L"evil.txt") && tu::read(a.root, L"before.txt") == "1");
}

// ------------------------------------------------------------------ the manifest
void test_manifest_codec() {
    synckeys::DeviceKey me = synckeys::new_device_key(L"me"), other = synckeys::new_device_key(L"other");
    synckeys::Vault v;
    v.id = "00112233445566778899aabbccddeeff";
    v.current = 1;
    v.vks[1] = crypto::random_key();
    v.vks[2] = crypto::random_key();
    syncman::KeyLookup keys = [&](uint32_t ver, crypto::Key& k) { return v.key(ver, k); };
    std::vector<synckeys::Ring::Member> members = { me.member() };
    syncman::MemberLookup member = [&](const std::string& id) -> const synckeys::Ring::Member* {
        for (auto& m : members) if (m.id == id) return &m;
        return nullptr;
    };
    std::vector<std::string> removedIds;
    syncman::RemovedLookup removed = [&](const std::string& id) { return std::find(removedIds.begin(), removedIds.end(), id) != removedIds.end(); };
    syncman::Manifest m;
    m.seq = 7;
    for (int i = 0; i < 2500; ++i) {
        syncman::Entry e;
        e.size = i;
        e.version = i % 5;
        e.hash[0] = (uint8_t)i;
        e.rev = "r" + std::to_string(i);
        m.files[L"dir " + std::to_wstring(i % 9) + L"/f\x0160" + std::to_wstring(i) + L".txt"] = e;
    }
    syncman::Encoded enc;
    CHECK(syncman::encode(m, v.id, keys, 1, me, nullptr, enc).ok());
    CHECK(enc.shards.size() == 4 && syncman::shard_count_for(2500) == 4 && syncman::shard_count_for(1000) == 1);
    auto read_all = [&](const syncman::Encoded& x, syncman::Root& root, std::map<std::wstring, syncman::Entry>& files) {
        files.clear();
        Result r = syncman::read_root(x.root, v.id, keys, member, removed, root);
        for (size_t i = 0; r.ok() && i < root.shards.size(); ++i) r = syncman::read_shard(x.shards[i], root, i, v.id, keys, files);
        return r;
    };
    syncman::Root root;
    std::map<std::wstring, syncman::Entry> files;
    CHECK(read_all(enc, root, files).ok() && root.seq == 7 && root.signer == me.id && files == m.files);
    // one file changed: one shard written again, the others kept as they were
    syncman::Manifest m2 = m;
    m2.seq = 8;
    m2.files.begin()->second.version = 99;
    syncman::Previous prev{ &m, root.placement, enc.shards };
    syncman::Encoded enc2;
    CHECK(syncman::encode(m2, v.id, keys, 2, me, &prev, enc2).ok());
    int same = 0;
    for (size_t i = 0; i < 4; ++i) same += enc2.hashes[i] == enc.hashes[i];
    CHECK(same == 3);
    CHECK(read_all(enc2, root, files).ok() && root.version == 2 && root.placement == 1 && files == m2.files);
    // the shard named in the root, and no other
    std::swap(enc2.shards[0], enc2.shards[1]);
    CHECK(read_all(enc2, root, files).st == Status::Tampered);
    std::swap(enc2.shards[0], enc2.shards[1]);
    // a flipped byte, another vault, an unknown or removed writer, a missing key
    auto flipped = enc.root;
    flipped[40] ^= 1;
    CHECK(syncman::read_root(flipped, v.id, keys, member, removed, root).st == Status::Tampered);
    CHECK(syncman::read_root(enc.root, "ffeeddccbbaa99887766554433221100", keys, member, removed, root).st == Status::Tampered);
    syncman::Encoded byOther;
    CHECK(syncman::encode(m, v.id, keys, 1, other, nullptr, byOther).ok());
    CHECK(syncman::read_root(byOther.root, v.id, keys, member, removed, root).st == Status::Tampered);
    CHECK(syncman::read_root(byOther.root, v.id, keys, member, removed, root, true).ok());       // lenient: the seal decides
    removedIds.push_back(me.id);
    CHECK(syncman::read_root(enc.root, v.id, keys, member, removed, root).st == Status::Tampered);
    removedIds.clear();
    syncman::KeyLookup none = [](uint32_t, crypto::Key&) { return false; };
    CHECK(syncman::read_root(enc.root, v.id, none, member, removed, root).st == Status::Locked);
    // a writer can't pass a signature off under another member's ID
    auto forged = byOther.root;
    std::memcpy(forged.data() + 8, me.id.data(), 16);
    CHECK(syncman::read_root(forged, v.id, keys, member, removed, root).st == Status::Tampered);

    // a later write by another device wins over this device's older one
    syncman::Manifest base;
    syncman::Entry newer;
    newer.version = 5;
    base.files[L"a"] = newer;
    base.files[L"b"] = newer;
    syncman::OwnLog own;
    syncman::Entry older;
    older.version = 4;
    own.put(L"a", older);
    own.removed(L"b", older);
    syncman::Entry mine;
    mine.version = 1;
    own.put(L"c", mine);
    syncman::fold(base, own);
    CHECK(base.files.size() == 3 && base.files[L"a"].version == 5 && base.files.count(L"b") && base.files.count(L"c"));
    own.items.clear();
    own.removed(L"a", newer);
    syncman::fold(base, own);
    CHECK(!base.files.count(L"a"));

    // the log of own writes survives a torn tail; the pin round-trips
    std::wstring dir = tu::temp_root(L"mlog");
    syncman::OwnLog log;
    log.file = dir + L"\\manifest.own";
    log.put(L"x/\x0160.txt", newer);
    log.removed(L"y", older);
    {
        HANDLE h = CreateFileW(log.file.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD n;
        WriteFile(h, "\x01\x05\x00garb", 8, &n, nullptr);
        CloseHandle(h);
    }
    syncman::OwnLog again;
    again.file = log.file;
    again.load();
    CHECK(again.items.size() == 2 && again.items[L"x/\x0160.txt"].second == newer && again.items[L"y"].first);
    again.clear();
    CHECK(!tu::exists(dir, L"manifest.own"));
    syncman::Pin pin{ 42, "abc" }, pin2;
    CHECK(pin.save(dir + L"\\manifest.pin") && pin2.load(dir + L"\\manifest.pin") && pin2.seq == 42 && pin2.hash == "abc");
    tu::rmrf(dir);
}

// the storage as it is (a copy), and changed as only the storage can: the devices look
using Objs = std::map<std::wstring, FakeStore::Obj>;
Objs snapshot(FakeStore& s) {
    std::lock_guard<std::mutex> lk(s.mx);
    return s.objs;
}
void storage_does(FakeStore& s, const std::function<void(Objs&)>& f) {
    std::lock_guard<std::mutex> lk(s.mx);
    f(s.objs);
    ++s.gen;
    s.objs[L".0/5"].rev = s.next_rev();
}

// A storage that withholds, rolls back, brings back or loses files, or the manifest itself
// (docs/sync-review.md, S2: what the per-file checks couldn't see)
void test_manifest_attacks(bool tx) {
    auto store = std::make_shared<FakeStore>();
    auto raw = [store, tx] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(store, tx)); };
    EDev a(L"ma", raw), b(L"mb", raw), c(L"mc", raw), d(L"md", raw);
    NoProgress p;
    std::string recovery = "Tidal Walrus 1";
    CHECK(synckeys::setup_ring(*a.ks, recovery).ok());
    CHECK(a.open(true).ok());
    for (EDev* x : { &b, &c, &d }) {
        auto r = raw();
        CHECK(synckeys::redeem_recovery(*r, *x->ks, recovery, p).ok());
    }
    CHECK(b.open(false).ok());
    tu::write(a.root, L"keep.txt", "KEEP");
    tu::write(a.root, L"sub/x.txt", "X");
    tu::write(a.root, L"old.txt", "OLD");
    CHECK(a.sync().result.ok());
    auto stored_of = [&](const std::wstring& plain, const std::string& content) {
        auto before = object_names(*store);
        tu::write(a.root, plain, content);
        CHECK(a.sync().result.ok());
        auto n = new_files(*store, before);
        CHECK(n.size() == 1);
        return n.empty() ? std::wstring() : n[0];
    };
    std::wstring docStored = stored_of(L"doc.txt", "DOC v1");
    CHECK(b.sync().result.ok() && same_tree(a, b));
    CHECK(store->objs.count(syncman::kRootRel) && !objects_under(*store, L".0/7/").empty());
    Objs early = snapshot(*store);

    // withheld: the storage hides a file. B keeps it (it used to take that for a deletion
    // and move its copy to the Recycle Bin) and says so
    FakeStore::Obj docObj = store->objs[docStored];
    storage_does(*store, [&](Objs& o) { o.erase(docStored); });
    b.sync();
    CHECK(tu::read(b.root, L"doc.txt") == "DOC v1" && has_problem(b, L"doesn't have it"));
    storage_does(*store, [&](Objs& o) { o[docStored] = docObj; });
    CHECK(b.sync().result.ok() && !has_problem(b, L"doesn't have it"));

    // rolled back: an edit, then the storage brings the old bytes back. B keeps the edit;
    // C, new, doesn't take the old bytes for the current version
    tu::write(a.root, L"doc.txt", "DOC v2");
    CHECK(a.sync().result.ok() && b.sync().result.ok() && tu::read(b.root, L"doc.txt") == "DOC v2");
    put_raw(*store, docStored, docObj.data);
    b.sync();
    CHECK(tu::read(b.root, L"doc.txt") == "DOC v2" && has_problem(b, L"isn't the version"));
    CHECK(c.open(false).ok());
    c.sync();
    CHECK(!tu::exists(c.root, L"doc.txt") && tu::read(c.root, L"keep.txt") == "KEEP" && has_problem(c, L"isn't the version"));
    // the next edit repairs it, over what the storage shows
    tu::write(a.root, L"doc.txt", "DOC v3");
    CHECK(a.sync().result.ok());
    for (EDev* x : { &b, &c }) {
        CHECK(x->sync().result.ok());
        CHECK(tu::read(x->root, L"doc.txt") == "DOC v3" && !has_problem(*x, L"isn't the version"));
    }

    // brought back: a deleted file's old object reappears. Not taken again.
    FakeStore::Obj oldObj;
    std::wstring oldStored;
    {
        auto before = snapshot(*store);
        tu::remove(a.root, L"old.txt");
        CHECK(a.sync().result.ok() && b.sync().result.ok() && !tu::exists(b.root, L"old.txt"));
        for (auto& [path, o] : before)
            if (!o.isDir && !synckeys::is_vault_path(path) && !store->objs.count(path)) { oldStored = path; oldObj = o; }
    }
    CHECK(!oldStored.empty());
    storage_does(*store, [&](Objs& o) { o[oldStored] = oldObj; });
    b.sync();
    CHECK(!tu::exists(b.root, L"old.txt") && has_problem(b, L"doesn't list it"));
    CHECK(tu::exists(a.root, L"keep.txt"));
    storage_does(*store, [&](Objs& o) { o.erase(oldStored); });
    CHECK(b.sync().result.ok() && !has_problem(b, L"doesn't list it"));

    // the whole storage as it was earlier, manifest and all: B has seen a later manifest
    Objs now = snapshot(*store);
    storage_does(*store, [&](Objs& o) { o = early; });
    {
        PassReport r = b.sync(true);
        CHECK(r.result.st == Status::Tampered && r.result.message.find(L"rolled back") != std::wstring::npos);
        CHECK(tu::read(b.root, L"doc.txt") == "DOC v3" && !tu::exists(b.root, L"old.txt"));
    }
    storage_does(*store, [&](Objs& o) { o = now; });
    CHECK(b.sync().result.ok());

    // the manifest gone: B saw it; D, new, finds a vault that says it keeps one
    storage_does(*store, [&](Objs& o) {
        for (auto it = o.begin(); it != o.end();)
            it = it->first == syncman::kRootRel || it->first.compare(0, 5, L".0/7/") == 0 ? o.erase(it) : std::next(it);
    });
    {
        PassReport r = b.sync(true);
        CHECK(r.result.st == Status::Tampered && r.result.message.find(L"missing") != std::wstring::npos);
        CHECK(tu::read(b.root, L"doc.txt") == "DOC v3" && tu::read(b.root, L"keep.txt") == "KEEP");
        CHECK(d.open(false).ok());
        r = d.sync(true);
        CHECK(r.result.st == Status::Tampered && tu::tree(d.root).empty());
    }
    storage_does(*store, [&](Objs& o) { o = now; });
    CHECK(b.sync().result.ok() && d.sync().result.ok() && same_tree(b, d));

    // a moved file and a removed one keep the manifest right (no problems on any device)
    CHECK(tu::rename(b.root, L"sub/x.txt", L"sub/y.txt"));
    tu::remove(b.root, L"keep.txt");
    CHECK(b.sync().result.ok());
    for (EDev* x : { &a, &c, &d }) {
        CHECK(x->sync().result.ok());
        CHECK(same_tree(b, *x) && x->eng->status().problems.empty());
    }

    // C, which joined with the sync password and only read so far, writes a manifest: B takes it
    tu::write(c.root, L"from-c.txt", "C");
    CHECK(c.sync().result.ok());
    CHECK(b.sync().result.ok() && tu::read(b.root, L"from-c.txt") == "C");

    // forked: the storage shows B one history and C another, each manifest numbered the same
    Objs before = snapshot(*store);
    tu::write(a.root, L"fork.txt", "A's");
    CHECK(a.sync().result.ok() && b.sync().result.ok() && tu::read(b.root, L"fork.txt") == "A's");
    Objs seenByB = snapshot(*store);
    storage_does(*store, [&](Objs& o) { o = before; });
    tu::write(c.root, L"fork.txt", "C's other");
    CHECK(c.sync().result.ok());
    {
        PassReport r = b.sync(true);
        CHECK(r.result.st == Status::Tampered && r.result.message.find(L"forked") != std::wstring::npos);
        CHECK(tu::read(b.root, L"fork.txt") == "A's");
    }
    storage_does(*store, [&](Objs& o) { o = seenByB; });
    CHECK(b.sync().result.ok());
}

// a store without transactions: B's files go up, its manifest doesn't (the write fails, B
// restarts). A meanwhile doesn't take B's files, nor reports them while they may still be
// on their way; B's log of its own writes gets them into the next manifest.
void test_manifest_window() {
    SetEnvironmentVariableW(L"EXPLORERPLUS_SYNC_MANIFEST_GRACE", L"3600");
    auto store = std::make_shared<FakeStore>();
    auto raw = [store] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(store, false)); };
    EDev a(L"wa", raw), b(L"wb", raw);
    NoProgress p;
    std::string recovery = "Tidal Walrus 1";
    CHECK(synckeys::setup_ring(*a.ks, recovery).ok());
    CHECK(a.open(true).ok());
    tu::write(a.root, L"a.txt", "A");
    CHECK(a.sync().result.ok());
    {
        auto r = raw();
        CHECK(synckeys::redeem_recovery(*r, *b.ks, recovery, p).ok());
    }
    CHECK(b.open(false).ok());
    CHECK(b.sync().result.ok());
    store->fault = [](const char* op, const RelPath& path) {
        return std::strcmp(op, "upload") == 0 && path == syncman::kRootRel ? Status::Offline : Status::Ok;
    };
    tu::write(b.root, L"b.txt", "B");
    tu::write(b.root, L"a.txt", "A, edited on B");
    b.sync(true);
    store->fault = nullptr;
    CHECK(tu::exists(b.stateDir, L"manifest.own"));
    CHECK(a.sync().result.ok());
    CHECK(!tu::exists(a.root, L"b.txt") && tu::read(a.root, L"a.txt") == "A" && a.eng->status().problems.empty());
    CHECK(b.open(false).ok());                                // restarted: the log is all it has
    CHECK(b.sync().result.ok());
    CHECK(!tu::exists(b.stateDir, L"manifest.own"));
    CHECK(a.sync().result.ok());
    CHECK(tu::read(a.root, L"b.txt") == "B" && tu::read(a.root, L"a.txt") == "A, edited on B" && a.eng->status().problems.empty());
    SetEnvironmentVariableW(L"EXPLORERPLUS_SYNC_MANIFEST_GRACE", L"0");
}

// a vault from before manifests: the first device with them takes what's there as the
// start, and the vault says from then on that it keeps one
void test_manifest_upgrade(bool tx) {
    auto store = std::make_shared<FakeStore>();
    auto raw = [store, tx] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(store, tx)); };
    EDev a(L"ua", raw), b(L"ub", raw);
    NoProgress p;
    std::string recovery = "Tidal Walrus 1";
    CHECK(synckeys::setup_ring(*a.ks, recovery).ok());
    a.noManifest = true;
    CHECK(a.open(true).ok());
    tu::write(a.root, L"one.txt", "1");
    tu::write(a.root, L"two/three.txt", "3");
    CHECK(a.sync().result.ok());
    CHECK(!store->objs.count(syncman::kRootRel));
    // the vault object as an older version wrote it: no "manifest=1" (signed again by A)
    {
        std::lock_guard<std::mutex> lk(store->mx);
        auto& o = store->objs[synckeys::kVaultFileRel];
        std::string s(o.data.begin(), o.data.end());
        size_t m = s.find("manifest=1\n");
        CHECK(m != std::string::npos);
        s.erase(m, 11);
        s.erase(s.find("sig="));
        uint8_t sig[crypto::kSig];
        synckeys::sign_as(synckeys::device_key(*a.ks), "ep vault object", (const uint8_t*)s.data(), s.size(), sig);
        s += "sig=" + crypto::base64(sig, crypto::kSig) + "\n";
        o.data.assign(s.begin(), s.end());
        o.rev = store->next_rev();
        ++store->gen;
    }
    a.noManifest = false;
    CHECK(a.open(false).ok());
    CHECK(a.sync().result.ok());
    CHECK(store->objs.count(syncman::kRootRel));
    {
        auto& o = store->objs[synckeys::kVaultFileRel];
        CHECK(std::string(o.data.begin(), o.data.end()).find("manifest=1\n") != std::string::npos);
    }
    {
        auto r = raw();
        CHECK(synckeys::redeem_recovery(*r, *b.ks, recovery, p).ok());
    }
    CHECK(b.open(false).ok());
    CHECK(b.sync().result.ok() && same_tree(a, b) && b.eng->status().problems.empty());
    tu::write(b.root, L"one.txt", "1, edited");
    CHECK(b.sync().result.ok() && a.sync().result.ok() && tu::read(a.root, L"one.txt") == "1, edited");
}

// lines the storage adds after the vault object's signature, or a field given twice, aren't
// read: the object is refused (docs/sync-review.md, I6)
void test_vault_unsigned_lines() {
    auto store = std::make_shared<FakeStore>();
    auto raw = [store] { return std::unique_ptr<Backend>(std::make_unique<FakeBackend>(store, false)); };
    EDev a(L"sa", raw);
    CHECK(synckeys::setup_ring(*a.ks, "Tidal Walrus 1").ok());
    CHECK(a.open(true).ok());
    tu::write(a.root, L"a.txt", "A");
    CHECK(a.sync().result.ok());
    std::vector<uint8_t> good;
    auto put = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(store->mx);
        auto& o = store->objs[synckeys::kVaultFileRel];
        o.data.assign(s.begin(), s.end());
        o.rev = store->next_rev();
        ++store->gen;
    };
    {
        std::lock_guard<std::mutex> lk(store->mx);
        good = store->objs[synckeys::kVaultFileRel].data;
    }
    std::string s(good.begin(), good.end());
    CHECK(s.find("manifest=1\n") != std::string::npos);
    put(s + "manifest=0\n");                                  // after the signature
    CHECK(!a.open(false).ok());
    put(s + "sig=" + s.substr(s.rfind("sig=") + 4));          // a second signature line
    CHECK(!a.open(false).ok());
    std::string body = s.substr(0, s.rfind("sig="));          // a field twice, signed by A itself
    body += "manifest=0\n";
    uint8_t sig[crypto::kSig];
    synckeys::sign_as(synckeys::device_key(*a.ks), "ep vault object", (const uint8_t*)body.data(), body.size(), sig);
    put(body + "sig=" + crypto::base64(sig, crypto::kSig) + "\n");
    CHECK(!a.open(false).ok());
    put(s);                                                    // as written: fine again
    CHECK(a.open(false).ok());
    CHECK(a.sync().result.ok());
}

} // namespace

void run_crypt_tests() {
    SetEnvironmentVariableW(L"EXPLORERPLUS_SYNC_KDF_KIB", L"1024");     // Argon2id: 1 MiB, 1 pass (tests)
    test_ring_codec();
    test_password_rules();
    test_refuse_plaintext();
    test_two_devices(false);
    test_two_devices(true);
    test_tamper();
    test_long_names();
    test_removal_hardening();
    test_manifest_codec();
    test_manifest_attacks(false);
    test_manifest_attacks(true);
    test_manifest_window();
    test_manifest_upgrade(false);
    test_manifest_upgrade(true);
    test_vault_unsigned_lines();
}
