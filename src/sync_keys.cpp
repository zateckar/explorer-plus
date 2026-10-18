#include "sync_keys.h"
#include "credstore.h"
#include "sync_manifest.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace synckeys {

using sync::Backend;
using sync::Progress;
using sync::RelPath;
using crypto::Bytes;
using crypto::Key;

// ------------------------------------------------------------------ bytes
namespace {
void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
void put64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
void putn(Bytes& b, const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }

struct Reader {
    const Bytes& b;
    size_t pos = 0;
    bool bad = false;
    explicit Reader(const Bytes& x) : b(x) {}
    bool need(size_t n) { if (bad || b.size() - pos < n) { bad = true; return false; } return true; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (uint32_t)b[pos + i] << (8 * i); pos += 4; return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)b[pos + i] << (8 * i); pos += 8; return v; }
    void get(void* out, size_t n) { if (!need(n)) { std::memset(out, 0, n); return; } std::memcpy(out, b.data() + pos, n); pos += n; }
    bool magic(const char* m) { if (!need(4) || std::memcmp(b.data() + pos, m, 4) != 0) { bad = true; return false; } pos += 4; return true; }
    std::string rest() { std::string s(b.begin() + pos, b.end()); pos = b.size(); return s; }
};

Bytes text_bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

// the vault object: "key=value" lines (binary fields Base64)
std::map<std::string, std::string> parse_lines(const Bytes& b) {
    std::map<std::string, std::string> m;
    std::string s(b.begin(), b.end()), line;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        line = s.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
        else if (!line.empty()) m[line] = "";
        i = e + 1;
    }
    return m;
}

uint64_t now_ft() {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
}
constexpr uint64_t kPairLifetime = 15ull * 60 * 10000000;   // 15 minutes in FILETIME units

const RelPath kVaultFile = L".ep-vault/vault";
const RelPath kDevices = L".ep-vault/devices";
const RelPath kPair = L".ep-vault/pair";
const RelPath kRotate = L".ep-vault/rotate";

std::wstring w(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string id_of(const Key& xpub) {
    crypto::Hash32 h = crypto::blake2b(xpub.data(), 32);
    return crypto::hex(h.data(), 8);
}

// ---- device signatures: EdDSA over context || 0 || message, so a signature made for one
// kind of object is never valid for another
Key sign_seed(const Key& secret) { return crypto::kdf(secret, "ep sign"); }

void sign(const DeviceKey& me, const char* ctx, const uint8_t* msg, size_t n, uint8_t sig[crypto::kSig]) {
    Bytes m(ctx, ctx + std::strlen(ctx) + 1);
    putn(m, msg, n);
    crypto::eddsa_sign(sign_seed(me.secret), m.data(), m.size(), sig);
}
bool check(const Key& spub, const char* ctx, const uint8_t* msg, size_t n, const uint8_t sig[crypto::kSig]) {
    Bytes m(ctx, ctx + std::strlen(ctx) + 1);
    putn(m, msg, n);
    return crypto::eddsa_check(spub, m.data(), m.size(), sig);
}
// body || signature
void append_sig(Bytes& b, const DeviceKey& me, const char* ctx) {
    uint8_t sig[crypto::kSig];
    sign(me, ctx, b.data(), b.size(), sig);
    putn(b, sig, crypto::kSig);
}
} // namespace

void sign_as(const DeviceKey& me, const char* ctx, const uint8_t* msg, size_t n, uint8_t sig[crypto::kSig]) {
    sign(me, ctx, msg, n, sig);
}
bool signed_by(const Key& spub, const char* ctx, const uint8_t* msg, size_t n, const uint8_t sig[crypto::kSig]) {
    return check(spub, ctx, msg, n, sig);
}

// ------------------------------------------------------------------ ring
std::string umk_id(const Key& umk) {
    Key k = crypto::kdf(umk, "ep umk id");
    return crypto::hex(k.data(), 8);
}

const Ring::Gen* Ring::find(const std::string& id) const {
    for (auto& g : gens) if (umk_id(g.umk) == id) return &g;
    return nullptr;
}
const Ring::Member* Ring::member(const std::string& id) const {
    for (auto& m : members) if (m.id == id) return &m;
    return nullptr;
}
bool Ring::is_removed(const std::string& id) const { return std::find(removed.begin(), removed.end(), id) != removed.end(); }

Bytes Ring::serialize() const {
    Bytes b;
    putn(b, "EPR2", 4);
    put32(b, (uint32_t)gens.size());
    for (auto& g : gens) { put32(b, g.gen); putn(b, g.umk.data(), 32); }
    putn(b, recSalt, 16);
    put32(b, recParams.memKiB);
    put32(b, recParams.passes);
    putn(b, recKek.data(), 32);
    put32(b, (uint32_t)removed.size());
    for (auto& r : removed) { std::string x = r; x.resize(16, '0'); putn(b, x.data(), 16); }
    put32(b, (uint32_t)members.size());
    for (auto& m : members) { std::string x = m.id; x.resize(16, '0'); putn(b, x.data(), 16); putn(b, m.xpub.data(), 32); putn(b, m.spub.data(), 32); }
    return b;
}

bool Ring::parse(const Bytes& b) {
    Reader r(b);
    Ring x;
    if (!r.magic("EPR2")) return false;                  // EPR1 (no members) is refused
    uint32_t n = r.u32();
    if (n == 0 || n > 64) return false;
    for (uint32_t i = 0; i < n; ++i) {
        Gen g;
        g.gen = r.u32();
        r.get(g.umk.data(), 32);
        x.gens.push_back(g);
    }
    r.get(x.recSalt, 16);
    x.recParams.memKiB = r.u32();
    x.recParams.passes = r.u32();
    r.get(x.recKek.data(), 32);
    uint32_t nr = r.u32();
    if (nr > 1000) return false;
    for (uint32_t i = 0; i < nr; ++i) { char id[16]; r.get(id, 16); x.removed.emplace_back(id, 16); }
    uint32_t nm = r.u32();
    if (nm > 1000) return false;
    for (uint32_t i = 0; i < nm; ++i) {
        Member m;
        char id[16];
        r.get(id, 16);
        m.id.assign(id, 16);
        r.get(m.xpub.data(), 32);
        r.get(m.spub.data(), 32);
        if (r.bad || id_of(m.xpub) != m.id) return false;
        x.members.push_back(m);
    }
    if (r.bad || r.pos != b.size()) return false;
    *this = x;                               // order kept: the current generation is last
    return true;
}

// this device as a member of its own ring (not when it was removed): true when added
static bool ensure_member(Ring& r, const DeviceKey& me) {
    if (r.member(me.id) || r.is_removed(me.id)) return false;
    r.members.push_back(me.member());
    return true;
}

// A ring another member made (a hand-over, a pairing grant, a recovery wrap) replaces this
// device's: its current generation, password wrap key, members and removed list. This device's
// other generations stay as older ones (vaults that haven't moved on yet still need them).
// Members and removals aren't joined with this device's own: a device that was kept in the
// dark (a removed device suppressing hand-overs) may have taken in members it shouldn't.
static Ring adopt(const Ring& mine, const Ring& got, const DeviceKey& me) {
    Ring out = got;
    Ring::Gen cur = got.current();
    for (auto& g : mine.gens) {
        bool have = false;
        for (auto& h : out.gens) if (h.umk == g.umk) have = true;
        if (!have) out.gens.push_back(g);
    }
    std::stable_sort(out.gens.begin(), out.gens.end(), [](const Ring::Gen& a, const Ring::Gen& c) { return a.gen < c.gen; });
    auto it = std::find_if(out.gens.begin(), out.gens.end(), [&](const Ring::Gen& g) { return g.umk == cur.umk; });
    out.gens.erase(it);
    out.gens.push_back(cur);                                      // the current generation stays last
    ensure_member(out, me);
    return out;
}

// ------------------------------------------------------------------ device key
static void derive_device(DeviceKey& d) {
    crypto::x25519_public(d.secret, d.pub);
    crypto::eddsa_public(sign_seed(d.secret), d.spub);
    d.id = id_of(d.pub);
}

DeviceKey new_device_key(const std::wstring& name) {
    DeviceKey d;
    d.secret = crypto::random_key();
    derive_device(d);
    d.name = name;
    return d;
}

static std::wstring computer_name() {
    wchar_t n[256];
    DWORD len = 256;
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, n, &len)) return n;
    return L"PC";
}

namespace {
Bytes device_blob(const DeviceKey& d) {
    Bytes b;
    putn(b, "EPK1", 4);
    putn(b, d.secret.data(), 32);
    std::string n = sync::to_utf8(d.name);
    putn(b, n.data(), n.size());
    return b;
}
bool parse_device_blob(const Bytes& b, DeviceKey& d) {
    Reader r(b);
    if (!r.magic("EPK1")) return false;
    Key s;
    r.get(s.data(), 32);
    if (r.bad) return false;
    std::string name = r.rest();
    DeviceKey x;
    x.secret = s;
    derive_device(x);
    x.name = sync::from_utf8(name);
    d = x;
    return true;
}

class CredKeyStore : public KeyStore {
public:
    bool load_ring(Ring& r) override {
        Bytes b;
        bool ok = credstore::get(L"sync.ring", b) && r.parse(b);
        crypto::wipe(b.data(), b.size());
        return ok;
    }
    bool save_ring(const Ring& r) override {
        Bytes b = r.serialize();
        // never save what load_ring couldn't read back (Ring::parse: at most 64 generations)
        bool ok = r.gens.size() <= 64 && b.size() <= credstore::kMaxBlob && credstore::put(L"sync.ring", b);
        crypto::wipe(b.data(), b.size());
        return ok;
    }
    bool load_device(DeviceKey& d) override {
        Bytes b;
        bool ok = credstore::get(L"sync.device", b) && parse_device_blob(b, d);
        crypto::wipe(b.data(), b.size());
        return ok;
    }
    bool save_device(const DeviceKey& d) override {
        Bytes b = device_blob(d);
        bool ok = credstore::put(L"sync.device", b);
        crypto::wipe(b.data(), b.size());
        return ok;
    }
    void forget() override { credstore::erase(L"sync.ring"); credstore::erase(L"sync.device"); }
};

class MemKeyStore : public KeyStore {
public:
    bool load_ring(Ring& r) override { return !m_ring.empty() && r.parse(m_ring); }
    bool save_ring(const Ring& r) override { m_ring = r.serialize(); return true; }
    bool load_device(DeviceKey& d) override { return !m_dev.empty() && parse_device_blob(m_dev, d); }
    bool save_device(const DeviceKey& d) override { m_dev = device_blob(d); return true; }
    void forget() override { m_ring.clear(); m_dev.clear(); }
private:
    Bytes m_ring, m_dev;
};
} // namespace

std::unique_ptr<KeyStore> credential_key_store() { return std::make_unique<CredKeyStore>(); }
std::unique_ptr<KeyStore> memory_key_store() { return std::make_unique<MemKeyStore>(); }

DeviceKey device_key(KeyStore& ks) {
    DeviceKey d;
    if (ks.load_device(d)) return d;
    d = new_device_key(computer_name());
    ks.save_device(d);
    return d;
}

static const wchar_t kNoMemory[] = L"There isn't enough free memory for the password's key (Argon2id needs 256 MB).";

std::wstring password_problem(const std::string& pw) {
    size_t chars = 0;
    bool lower = false, upper = false, digit = false;
    for (unsigned char c : pw) {
        if ((c & 0xC0) != 0x80) ++chars;              // (UTF-8: characters, not bytes)
        lower |= c >= 'a' && c <= 'z';
        upper |= c >= 'A' && c <= 'Z';
        digit |= c >= '0' && c <= '9';
    }
    if (chars < 12) return L"The password needs at least 12 characters.";
    if (!lower || !upper || !digit) return L"The password needs small letters, capital letters and digits.";
    // what guessing tries first doesn't count: common words, runs on the keyboard or in the
    // alphabet (1234, abcd, qwer, and backwards), one character three times or more
    std::string low = pw;
    for (auto& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    std::vector<bool> used(low.size(), false);
    auto mark = [&](const std::string& w) {
        for (size_t i = low.find(w); i != std::string::npos; i = low.find(w, i + 1))
            for (size_t k = 0; k < w.size(); ++k) used[i + k] = true;
    };
    for (const char* w : { "password", "passwort", "heslo", "letmein", "welcome", "admin", "iloveyou", "explorer", "monkey", "dragon" }) mark(w);
    for (std::string row : { "abcdefghijklmnopqrstuvwxyz", "01234567890", "qwertyuiop", "qwertzuiop", "asdfghjkl", "zxcvbnm", "yxcvbnm" }) {
        for (int dir = 0; dir < 2; ++dir) {
            for (size_t i = 0; i + 4 <= row.size(); ++i) mark(row.substr(i, 4));
            std::reverse(row.begin(), row.end());
        }
    }
    for (size_t i = 0; i + 3 <= low.size(); ++i)
        if (low[i] == low[i + 1] && low[i] == low[i + 2]) used[i] = used[i + 1] = used[i + 2] = true;
    size_t left = 0;
    for (size_t i = 0; i < low.size(); ++i)
        if (!used[i] && ((unsigned char)low[i] & 0xC0) != 0x80) ++left;
    if (left < 8) return L"The password is too easy to guess (common words, 1234, abcd, qwer, repeated characters): make it longer or less regular.";
    return {};
}

// the password wrap of the ring: a fresh salt, the password through Argon2id
static Result new_recovery(Ring& r, const std::string& password) {
    if (std::wstring why = password_problem(password); !why.empty()) return Result::fail(Status::Rejected, why);
    crypto::random(r.recSalt, 16);
    r.recParams = crypto::argon2_default();
    if (!crypto::argon2id(password, r.recSalt, 16, r.recParams, r.recKek)) return Result::fail(Status::Fatal, kNoMemory);
    return {};
}

// the password is the ring's (one Argon2id run)
static Result password_matches(const Ring& r, const std::string& password, bool& same) {
    same = false;
    if (!crypto::argon2_params_ok(r.recParams)) return {};
    Key k;
    if (!crypto::argon2id(password, r.recSalt, 16, r.recParams, k)) return Result::fail(Status::Fatal, kNoMemory);
    same = k == r.recKek;
    return {};
}

Result setup_ring(KeyStore& ks, const std::string& password) {
    DeviceKey me = device_key(ks);
    Ring r;
    Ring::Gen g;
    g.gen = 1;
    g.umk = crypto::random_key();
    r.gens.push_back(g);
    r.members.push_back(me.member());
    if (Result x = new_recovery(r, password); !x.ok()) return x;
    if (!ks.save_ring(r)) return Result::fail(Status::Fatal, L"The keys couldn't be saved in Credential Manager.");
    return {};
}

Result check_password(KeyStore& ks, const std::string& password) {
    Ring ring;
    if (!ks.load_ring(ring)) return Result::fail(Status::Locked, L"This device has no keys.");
    bool same = false;
    if (Result r = password_matches(ring, password, same); !r.ok()) return r;
    // (an old ring's recovery key, typed as it's written)
    std::string code = crypto::normalize_code(password);
    if (!same && !code.empty() && code != password) if (Result r = password_matches(ring, code, same); !r.ok()) return r;
    return same ? Result{} : Result::fail(Status::Rejected, L"That isn't your sync password.");
}

// ------------------------------------------------------------------ small objects
Result get_object(Backend& raw, const RelPath& p, Bytes& out, std::string* rev, Progress& prog) {
    sync::RemoteEntry e;
    Result r = raw.stat(p, e, prog);
    if (!r.ok()) return r;
    if (e.isDir) return Result::fail(Status::NotFound);
    if (e.size > 1024 * 1024) return Result::fail(Status::Tampered, L"A key object is far too big.");
    sync::MemorySink s;
    s.limit = 1024 * 1024;
    sync::FileMeta m;
    r = raw.download(e, s, m, prog);
    if (!r.ok()) return r;
    out = std::move(s.data);
    if (rev) *rev = e.rev;
    return {};
}

Result put_object(Backend& raw, const RelPath& p, const Bytes& data, const std::string& expectedRev, Progress& prog, std::string* newRev) {
    Result r = raw.begin(prog);
    if (!r.ok()) return r;
    sync::MemorySource src(data);
    sync::RemoteEntry out;
    r = raw.upload(p, src, sync::FileMeta{}, expectedRev, out, prog);
    if (!r.ok()) { raw.abort(); return r; }
    std::string cursor;
    r = raw.commit(L"sync keys", cursor, prog);
    if (r.st == Status::CommitRaced) r.st = Status::PreconditionFailed;
    if (r.ok() && newRev) *newRev = out.rev;
    return r;
}

Result remove_object(Backend& raw, const RelPath& p, Progress& prog) {
    Result r = raw.begin(prog);
    if (!r.ok()) return r;
    r = raw.remove(p, "*", prog);
    if (!r.ok()) { raw.abort(); return r.st == Status::NotFound ? Result{} : r; }
    std::string cursor;
    r = raw.commit(L"sync keys", cursor, prog);
    if (r.st == Status::CommitRaced) r.st = Status::PreconditionFailed;
    return r;
}

static Result list_dir(Backend& raw, const RelPath& dir, std::vector<sync::RemoteEntry>& out, Progress& prog) {
    out.clear();
    std::vector<sync::RemoteEntry> all;
    Result r = raw.list(dir, all, prog);
    if (r.st == Status::NotFound) return {};
    if (!r.ok()) return r;
    for (auto& e : all)
        if (!e.isDir && sync::parent_of(e.path) == dir) out.push_back(e);
    return {};
}

// ------------------------------------------------------------------ the vault object
namespace {
struct VaultFile {
    std::string umk;                         // umk_id of the generation it's sealed under
    uint32_t gen = 0;
    Bytes data;                              // sealed payload
    Bytes rsalt, recovery;
    crypto::Argon2Params rparams;
    std::string signer;                      // the device that wrote it
    bool manifest = false;                   // the vault keeps a manifest (sync_manifest.h)
    Bytes signedText, sig;                   // the lines up to "sig=", and the signature over them
};

// The signature is the last line and covers every line before it; the fields are read from
// those lines only, each once. (A line after the signature, or a field given twice, would
// otherwise be read without being signed: docs/sync-review.md, I6.)
bool parse_vault(const Bytes& b, VaultFile& v) {
    std::string s(b.begin(), b.end());
    if (s.compare(0, 5, "EPV2\n") != 0) return false;   // EPV1 (unsigned) is refused
    size_t at = s.rfind("\nsig=");
    if (at == std::string::npos) return false;
    std::string sigText = s.substr(at + 5);
    if (!sigText.empty() && sigText.back() == '\n') sigText.pop_back();
    if (sigText.find_first_of("\r\n") != std::string::npos) return false;
    v.signedText.assign(b.begin(), b.begin() + at + 1);
    std::map<std::string, std::string> m;
    std::string text(v.signedText.begin(), v.signedText.end());
    for (size_t i = 0; i < text.size();) {
        size_t e = text.find('\n', i);
        std::string line = text.substr(i, e - i);
        i = e + 1;
        size_t eq = line.find('=');
        std::string key = line.substr(0, eq), value = eq == std::string::npos ? std::string() : line.substr(eq + 1);
        if (key.empty() || key == "sig" || !m.emplace(key, value).second) return false;
    }
    m["sig"] = sigText;
    v.umk = m["umk"];
    v.gen = (uint32_t)strtoul(m["gen"].c_str(), nullptr, 10);
    v.rparams.memKiB = (uint32_t)strtoul(m["rmem"].c_str(), nullptr, 10);
    v.rparams.passes = (uint32_t)strtoul(m["rpasses"].c_str(), nullptr, 10);
    v.signer = m["signer"];
    v.manifest = m["manifest"] == "1";
    return !v.umk.empty() && v.signer.size() == 16 && crypto::unbase64(m["data"], v.data) && crypto::unbase64(m["rsalt"], v.rsalt) &&
           crypto::unbase64(m["recovery"], v.recovery) && crypto::unbase64(m["sig"], v.sig) && v.rsalt.size() == 16 &&
           v.sig.size() == crypto::kSig;
}

Bytes write_vault(const VaultFile& v, const DeviceKey& me) {
    std::string s = "EPV2\n";
    s += "umk=" + v.umk + "\n";
    s += "gen=" + std::to_string(v.gen) + "\n";
    s += "data=" + crypto::base64(v.data) + "\n";
    s += "rsalt=" + crypto::base64(v.rsalt) + "\n";
    s += "rmem=" + std::to_string(v.rparams.memKiB) + "\n";
    s += "rpasses=" + std::to_string(v.rparams.passes) + "\n";
    s += "recovery=" + crypto::base64(v.recovery) + "\n";
    if (v.manifest) s += "manifest=1\n";
    s += "signer=" + me.id + "\n";
    uint8_t sig[crypto::kSig];
    sign(me, "ep vault object", (const uint8_t*)s.data(), s.size(), sig);
    s += "sig=" + crypto::base64(sig, crypto::kSig) + "\n";
    return text_bytes(s);
}

// who wrote the vault object: a member of this ring, not removed, and the signature checks
Result check_vault_signer(const VaultFile& f, const Ring& ring) {
    if (ring.is_removed(f.signer)) return Result::fail(Status::Tampered, L"The vault file was written by a device that was removed.");
    const Ring::Member* m = ring.member(f.signer);
    if (!m) return Result::fail(Status::Tampered, L"The vault file was written by a device this one doesn't know. Pair this device again.");
    if (!check(m->spub, "ep vault object", f.signedText.data(), f.signedText.size(), f.sig.data()))
        return Result::fail(Status::Tampered, L"The vault file's signature doesn't check.");
    return {};
}

Bytes vault_payload(const Vault& v) {
    Bytes b;
    putn(b, "EPVP", 4);
    Bytes id;
    crypto::unhex(v.id, id);
    id.resize(16);
    putn(b, id.data(), 16);
    put32(b, v.current);
    put32(b, (uint32_t)v.vks.size());
    for (auto& [ver, k] : v.vks) { put32(b, ver); putn(b, k.data(), 32); }
    return b;
}

bool parse_payload(const Bytes& b, Vault& v) {
    Reader r(b);
    if (!r.magic("EPVP")) return false;
    uint8_t id[16];
    r.get(id, 16);
    Vault x;
    x.id = crypto::hex(id, 16);
    x.current = r.u32();
    uint32_t n = r.u32();
    if (n == 0 || n > 1000) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t ver = r.u32();
        Key k;
        r.get(k.data(), 32);
        x.vks[ver] = k;
    }
    if (r.bad || !x.vks.count(x.current)) return false;
    v = x;
    return true;
}

// seals a vault under the ring's current generation, with the recovery wrap of the ring
VaultFile seal_vault(const Vault& v, const Ring& ring) {
    VaultFile f;
    const Ring::Gen& g = ring.current();
    f.umk = umk_id(g.umk);
    f.gen = g.gen;
    Bytes p = vault_payload(v);
    f.data = crypto::seal(crypto::kdf(g.umk, "ep vault"), "ep vault", p.data(), p.size());
    crypto::wipe(p.data(), p.size());
    f.rsalt.assign(ring.recSalt, ring.recSalt + 16);
    f.rparams = ring.recParams;
    Bytes rb = ring.serialize();
    f.recovery = crypto::seal(ring.recKek, "ep recovery", rb.data(), rb.size());
    crypto::wipe(rb.data(), rb.size());
    f.manifest = v.manifest;
    return f;
}

// ---- device records: "EPD2" | generation | umk id | sealed(xpub | spub | added | name) | signature
Bytes device_record(const DeviceKey& d, const Ring::Gen& g, uint64_t added) {
    Bytes p;
    putn(p, d.pub.data(), 32);
    putn(p, d.spub.data(), 32);
    put64(p, added);
    std::string n = sync::to_utf8(d.name);
    putn(p, n.data(), n.size());
    Bytes b;
    putn(b, "EPD2", 4);
    put32(b, g.gen);
    Bytes umkid = text_bytes(umk_id(g.umk));
    putn(b, umkid.data(), 16);
    Bytes s = crypto::seal(crypto::kdf(g.umk, "ep devices"), "ep device " + d.id, p.data(), p.size());
    putn(b, s.data(), s.size());
    append_sig(b, d, "ep device record");
    return b;
}

// a record that opens with this ring and is signed by the key inside it; sealedUnder: the
// UMK id it's sealed under
bool open_device_record(const Bytes& b, const Ring& ring, const std::string& id, DeviceInfo& out, Ring::Member& m,
                        std::string* sealedUnder = nullptr) {
    if (b.size() < 4 + 4 + 16 + crypto::kNonce + crypto::kTag + crypto::kSig) return false;
    Bytes body(b.begin(), b.end() - crypto::kSig);
    Reader r(body);
    if (!r.magic("EPD2")) return false;
    r.u32();
    char uid[16];
    r.get(uid, 16);
    if (r.bad) return false;
    const Ring::Gen* g = ring.find(std::string(uid, 16));
    if (!g) return false;
    Bytes sealed(body.begin() + r.pos, body.end()), p;
    if (!crypto::open(crypto::kdf(g->umk, "ep devices"), "ep device " + id, sealed, p)) return false;
    Reader q(p);
    Ring::Member x;
    q.get(x.xpub.data(), 32);
    q.get(x.spub.data(), 32);
    uint64_t added = q.u64();
    if (q.bad || id_of(x.xpub) != id) return false;
    if (!check(x.spub, "ep device record", body.data(), body.size(), b.data() + body.size())) return false;
    x.id = id;
    m = x;
    out.id = id;
    out.name = sync::from_utf8(q.rest());
    out.added.dwLowDateTime = (DWORD)added;
    out.added.dwHighDateTime = (DWORD)(added >> 32);
    if (sealedUnder) sealedUnder->assign(uid, 16);
    return true;
}

// ---- rotation hand-over: "EPX2" | old generation's umk id | sender id | ephemeral public
// key | sealed ring | the sender's signature
Bytes rotation_object(const Ring& newRing, const Ring::Gen& old, const DeviceKey& me, const Ring::Member& to) {
    Key eph = crypto::random_key(), ephPub;
    crypto::x25519_public(eph, ephPub);
    Key shared = crypto::x25519_shared(eph, to.xpub);
    Key k = crypto::kdf(old.umk, "ep rotate", shared.data(), 32);
    Bytes rb = newRing.serialize();
    Bytes s = crypto::seal(k, "ep rotate " + me.id + " " + to.id, rb.data(), rb.size());
    crypto::wipe(rb.data(), rb.size());
    Bytes b;
    putn(b, "EPX2", 4);
    Bytes uid = text_bytes(umk_id(old.umk));
    putn(b, uid.data(), 16);
    putn(b, me.id.data(), 16);
    putn(b, ephPub.data(), 32);
    putn(b, s.data(), s.size());
    append_sig(b, me, "ep rotate");
    return b;
}

// 1: opened (out, the generation it's sealed under, the sender); 0: not for now (a
// generation or sender this ring doesn't have yet); -1: never (from a removed device, or
// a signature that doesn't check)
int open_rotation(const Bytes& b, const Ring& ring, const DeviceKey& me, Ring& out, const Ring::Gen** sealedUnder, std::string& sender) {
    if (b.size() < 4 + 16 + 16 + 32 + crypto::kNonce + crypto::kTag + crypto::kSig) return -1;
    Bytes body(b.begin(), b.end() - crypto::kSig);
    Reader r(body);
    if (!r.magic("EPX2")) return -1;
    char uid[16], sid[16];
    r.get(uid, 16);
    r.get(sid, 16);
    Key ephPub;
    r.get(ephPub.data(), 32);
    if (r.bad) return -1;
    sender.assign(sid, 16);
    if (ring.is_removed(sender)) return -1;
    const Ring::Member* s = ring.member(sender);
    if (!s) return 0;
    if (!check(s->spub, "ep rotate", body.data(), body.size(), b.data() + body.size())) return -1;
    const Ring::Gen* g = ring.find(std::string(uid, 16));
    if (!g) return 0;
    *sealedUnder = g;
    Key shared = crypto::x25519_shared(me.secret, ephPub);
    Key k = crypto::kdf(g->umk, "ep rotate", shared.data(), 32);
    Bytes sealed(body.begin() + r.pos, body.end()), p;
    if (!crypto::open(k, "ep rotate " + sender + " " + me.id, sealed, p)) return -1;
    bool ok = out.parse(p);
    crypto::wipe(p.data(), p.size());
    return ok ? 1 : -1;
}
} // namespace

// takes the rotation hand-overs left for this device: true when the ring moved on.
// A hand-over is taken only when
//   - a member of this device's ring made it (signed, not removed),
//   - its ring keeps this device and the sender as members,
//   - it continues this device's current generation: sealed under it, or carrying it.
// So a removed device can't hand anything to a device that knows of the removal, and
// nobody who isn't a member can at all. (A device that hasn't heard of the removal yet
// still counts the removed one as a member: docs/sync-review.md, S1.)
static bool take_rotations(Backend& raw, KeyStore& ks, Ring& ring, const DeviceKey& me, Progress& prog) {
    std::vector<sync::RemoteEntry> items;
    if (!list_dir(raw, kRotate, items, prog).ok()) return false;
    std::vector<std::pair<RelPath, Bytes>> mine;
    for (auto& e : items) {
        std::wstring leaf = sync::leaf_of(e.path);
        if (leaf.size() <= me.id.size() || leaf.compare(0, me.id.size(), w(me.id)) != 0 || leaf[me.id.size()] != L'-') continue;
        Bytes b;
        if (get_object(raw, e.path, b, nullptr, prog).ok()) mine.push_back({ e.path, std::move(b) });
    }
    bool changed = false;
    std::vector<RelPath> done;
    for (bool progress = true; progress;) {             // a chain of rotations: in order, whatever the listing's
        progress = false;
        for (auto it = mine.begin(); it != mine.end();) {
            Ring got;
            const Ring::Gen* under = nullptr;
            std::string sender;
            int o = open_rotation(it->second, ring, me, got, &under, sender);
            if (o == 0) { ++it; continue; }
            if (o > 0) {
                bool members = got.member(me.id) && !got.is_removed(me.id) && got.member(sender) && !got.is_removed(sender);
                bool continues = under->umk == ring.current().umk || got.find(umk_id(ring.current().umk));
                if (members && continues && (got.current().gen > ring.current().gen || !ring.find(umk_id(got.current().umk)))) {
                    ring = adopt(ring, got, me);
                    changed = progress = true;
                }
            }
            done.push_back(it->first);                     // taken, or stale / not ours to take
            it = mine.erase(it);
        }
    }
    // the hand-overs go once the ring they gave is kept: a ring that couldn't be saved is
    // taken again next time, not lost
    if (changed && !ks.save_ring(ring)) return changed;
    for (auto& p : done) remove_object(raw, p, prog);
    return changed;
}

// the records in the vault's devices folder: members admitted (records sealed under this
// ring's current generation, which a removed device never has), and with `all` the
// members' records read for listing. True when the ring took in new members.
static bool read_devices(Backend& raw, Ring& ring, bool all, std::vector<DeviceInfo>* infos, std::set<std::string>* present,
                         Progress& prog) {
    std::vector<sync::RemoteEntry> items;
    if (!list_dir(raw, kDevices, items, prog).ok()) return false;
    std::string cur = umk_id(ring.current().umk);
    bool changed = false;
    for (auto& e : items) {
        std::string id = sync::to_utf8(sync::leaf_of(e.path));
        if (present) present->insert(id);
        if (ring.is_removed(id)) continue;
        bool known = ring.member(id) != nullptr;
        if (known && !all) continue;
        Bytes b;
        DeviceInfo di;
        Ring::Member m;
        std::string under;
        if (!get_object(raw, e.path, b, nullptr, prog).ok() || !open_device_record(b, ring, id, di, m, &under)) continue;
        if (!known) {
            if (under != cur) continue;                  // not sealed under the current generation: not a member
            ring.members.push_back(m);
            changed = true;
        } else if (!(ring.member(id)->spub == m.spub)) continue;
        if (infos) infos->push_back(di);
    }
    return changed;
}

// writes this ring's hand-overs for the members that have a record in this vault (but this
// device), sealed under the generation the vault had, and signed; drops removed devices'
// records. Records of devices that aren't members get nothing.
static void hand_over(Backend& raw, const Ring& ring, const Ring::Gen& old, const DeviceKey& me, Progress& prog) {
    std::vector<sync::RemoteEntry> items;
    if (!list_dir(raw, kDevices, items, prog).ok()) return;
    for (auto& e : items) {
        std::string id = sync::to_utf8(sync::leaf_of(e.path));
        if (id == me.id) continue;
        if (ring.is_removed(id)) {
            remove_object(raw, e.path, prog);
            continue;
        }
        const Ring::Member* m = ring.member(id);
        if (!m) continue;
        Bytes x = rotation_object(ring, old, me, *m);
        put_object(raw, kRotate + L"/" + w(id) + L"-g" + std::to_wstring(ring.current().gen), x, "*", prog);
    }
}

static Result register_device(Backend& raw, const Ring& ring, const DeviceKey& me, Progress& prog) {
    RelPath p = kDevices + L"/" + w(me.id);
    Bytes cur;
    std::string rev;
    Result r = get_object(raw, p, cur, &rev, prog);
    if (r.ok()) {
        // already there, sealed under the current generation and signed by this device
        DeviceInfo di;
        Ring::Member m;
        std::string under;
        if (open_device_record(cur, ring, me.id, di, m, &under) && under == umk_id(ring.current().umk) && m.spub == me.spub) return {};
    } else if (r.st != Status::NotFound) return r;
    return put_object(raw, p, device_record(me, ring.current(), now_ft()), "*", prog);
}

bool VaultPin::load(const std::wstring& file) {
    *this = {};
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[256];
    DWORD n = 0;
    bool ok = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr) != 0;
    CloseHandle(h);
    if (!ok) return false;
    auto m = parse_lines(Bytes(buf, buf + n));
    if (!m.count("EPN1") || m["id"].size() != 32) return false;
    id = m["id"];
    gen = (uint32_t)strtoul(m["gen"].c_str(), nullptr, 10);
    version = (uint32_t)strtoul(m["version"].c_str(), nullptr, 10);
    hash = m["hash"];
    return true;
}

bool VaultPin::save(const std::wstring& file) const {
    std::string s = "EPN1\nid=" + id + "\ngen=" + std::to_string(gen) + "\nversion=" + std::to_string(version) + "\nhash=" + hash + "\n";
    std::wstring tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size() && FlushFileBuffers(h);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

Result open_vault(Backend& raw, KeyStore& ks, bool create, Vault& out, Progress& prog, VaultPin* pin) {
    Ring ring;
    bool haveRing = ks.load_ring(ring);
    DeviceKey me = device_key(ks);
    if (haveRing && ensure_member(ring, me)) ks.save_ring(ring);
    bool pinned = pin && !pin->id.empty();
    // what this device ends up seeing, into the pin
    auto seen = [&](const Vault& v, uint32_t gen, const Bytes& object) {
        if (!pin) return;
        pin->hash = crypto::hex(crypto::blake2b(object.data(), object.size()));
        pin->id = v.id;
        pin->gen = std::max(pin->gen, gen);
        pin->version = std::max(pin->version, v.vks.rbegin()->first);
    };
    // who signs this device's manifests, and whose it believes
    auto signers = [&](Vault& v) {
        v.me = me;
        v.members = ring.members;
        v.removed = ring.removed;
    };
    for (int attempt = 0; attempt < 4; ++attempt) {
        Bytes vb;
        std::string rev;
        Result r = get_object(raw, kVaultFile, vb, &rev, prog);
        if (r.st == Status::NotFound) {
            // the vault this pair synced with is gone: never quietly start another one
            if (pinned) return Result::fail(Status::Tampered, L"The vault file is missing from the storage.");
            // no vault: a new one, only in an empty place
            std::vector<sync::RemoteEntry> all;
            Result l = raw.list(L"", all, prog);
            if (!l.ok() && l.st != Status::NotFound) return l;
            for (auto& e : all)
                if (!is_vault_path(e.path))
                    return Result::fail(Status::Rejected, L"The storage location already has files that aren't encrypted. Pick an empty one.");
            if (!create) return Result::fail(Status::NotFound, L"There's no encrypted sync folder here yet.");
            if (!haveRing) return Result::fail(Status::Locked, L"Set up the encryption keys on this device first.");
            Vault v;
            uint8_t id[16];
            crypto::random(id, 16);
            v.id = crypto::hex(id, 16);
            v.current = 1;
            v.vks[1] = crypto::random_key();
            v.manifest = true;
            Bytes vo = write_vault(seal_vault(v, ring), me);
            r = put_object(raw, kVaultFile, vo, "", prog, &v.rev);
            if (r.st == Status::PreconditionFailed) continue;          // another device was quicker
            if (!r.ok()) return r;
            r = register_device(raw, ring, me, prog);
            if (!r.ok()) return r;
            seen(v, ring.current().gen, vo);
            signers(v);
            out = v;
            return {};
        }
        if (!r.ok()) return r;
        VaultFile f;
        if (!parse_vault(vb, f)) return Result::fail(Status::Tampered, L"The vault file on the storage is damaged, or from an older version.");
        if (haveRing) {
            take_rotations(raw, ks, ring, me, prog);
            if (read_devices(raw, ring, false, nullptr, nullptr, prog)) ks.save_ring(ring);
        }
        const Ring::Gen* g = haveRing ? ring.find(f.umk) : nullptr;
        if (!g) {
            return Result::fail(Status::Locked, haveRing
                ? L"This folder is encrypted with keys this device doesn't have. Unlock it with your sync password, or a code from another device."
                : L"This folder is encrypted by another of your devices. Unlock it with your sync password, or a code from that device.");
        }
        Bytes p;
        Vault v;
        if (!crypto::open(crypto::kdf(g->umk, "ep vault"), "ep vault", f.data, p) || !parse_payload(p, v)) {
            crypto::wipe(p.data(), p.size());
            return Result::fail(Status::Tampered, L"The vault file on the storage doesn't authenticate.");
        }
        crypto::wipe(p.data(), p.size());
        v.manifest = f.manifest;
        // written by a member: a vault a removed device made up (its own keys, an old
        // generation to make this device hand the new ring over) is refused. What it wrote
        // while it was a member, and this device took then (the pin), still opens.
        if (Result s = check_vault_signer(f, ring); !s.ok()) {
            bool tookBefore = pinned && ring.is_removed(f.signer) && pin->hash == crypto::hex(crypto::blake2b(vb.data(), vb.size()));
            if (!tookBefore) return s;
        }
        if (pinned) {
            if (v.id != pin->id)
                return Result::fail(Status::Tampered, L"The storage holds a different vault than the one this folder syncs with.");
            if (g->gen < pin->gen || v.vks.rbegin()->first < pin->version)
                return Result::fail(Status::Tampered, L"The vault file on the storage is older than one this device already saw (rolled back).");
        }
        v.rev = rev;
        uint32_t gen = g->gen;
        // behind the ring (a device was removed meanwhile): a new vault key version under
        // the current generation, and the hand-overs for this vault's other members
        if (g->gen < ring.current().gen) {
            Ring::Gen old = *g;
            uint32_t nv = v.vks.rbegin()->first + 1;
            if (nv == 0 || nv > 0xFFFF) return Result::fail(Status::Tampered, L"The vault has run out of key versions.");
            v.vks[nv] = crypto::random_key();
            v.current = nv;
            vb = write_vault(seal_vault(v, ring), me);
            r = put_object(raw, kVaultFile, vb, rev, prog, &v.rev);
            if (r.st == Status::PreconditionFailed) continue;
            if (!r.ok()) return r;
            hand_over(raw, ring, old, me, prog);
            gen = ring.current().gen;
            // the manifest moves on with it (its writer may be the removed device)
            signers(v);
            syncman::resign_root(raw, v, prog);
        }
        r = register_device(raw, ring, me, prog);
        if (!r.ok()) return r;
        seen(v, gen, vb);
        signers(v);
        out = v;
        return {};
    }
    return Result::fail(Status::PreconditionFailed, L"The vault kept changing while it was opened.");
}

Result mark_manifest(Backend& raw, const Vault& v, Progress& prog) {
    Bytes vb;
    std::string rev;
    Result r = get_object(raw, kVaultFile, vb, &rev, prog);
    if (!r.ok()) return r;
    if (rev != v.rev) return Result::fail(Status::PreconditionFailed);   // not the object this device opened
    VaultFile f;
    if (!parse_vault(vb, f)) return Result::fail(Status::Tampered);
    if (f.manifest) return {};
    Ring signers;
    signers.members = v.members;
    signers.removed = v.removed;
    if (Result s = check_vault_signer(f, signers); !s.ok()) return s;
    f.manifest = true;
    r = put_object(raw, kVaultFile, write_vault(f, v.me), rev, prog);
    return r;
}

// ------------------------------------------------------------------ pairing, recovery
Result create_pairing(Backend& raw, KeyStore& ks, std::string& code, Progress& prog) {
    Ring ring;
    if (!ks.load_ring(ring)) return Result::fail(Status::Locked, L"This device has no keys to share.");
    ensure_member(ring, device_key(ks));
    std::string c = crypto::new_code();
    uint8_t salt[16];
    crypto::random(salt, 16);
    crypto::Argon2Params ap = crypto::argon2_default();
    Key k;
    if (!crypto::argon2id(c, salt, 16, ap, k)) return Result::fail(Status::Fatal, kNoMemory);
    uint64_t expiry = now_ft() + kPairLifetime;
    Bytes rb = ring.serialize();
    Bytes s = crypto::seal(k, "ep pair " + std::to_string(expiry), rb.data(), rb.size());
    crypto::wipe(rb.data(), rb.size());
    Bytes b;
    putn(b, "EPP2", 4);
    putn(b, salt, 16);
    put32(b, ap.memKiB);
    put32(b, ap.passes);
    put64(b, expiry);
    putn(b, s.data(), s.size());
    uint8_t id[8];
    crypto::random(id, 8);
    Result r = put_object(raw, kPair + L"/" + w(crypto::hex(id, 8)), b, "", prog);
    if (!r.ok()) return r;
    // old grants (expired, or of an older format) go now
    std::vector<sync::RemoteEntry> items;
    if (list_dir(raw, kPair, items, prog).ok())
        for (auto& e : items) {
            Bytes x;
            if (!get_object(raw, e.path, x, nullptr, prog).ok()) continue;
            Reader q(x);
            bool current = q.magic("EPP2");
            uint8_t tmp[24];
            q.get(tmp, 24);
            if (!current || q.bad || q.u64() < now_ft()) remove_object(raw, e.path, prog);
        }
    code = crypto::format_code(c);
    return {};
}

// a ring from a pairing grant or the recovery wrap: the user typed the code, so it replaces
// this device's (adopt). A device that was removed before comes back as a new device.
static Result take_ring(KeyStore& ks, const Ring& got) {
    DeviceKey me = device_key(ks);
    if (got.is_removed(me.id)) {
        me = new_device_key(me.name);
        if (!ks.save_device(me)) return Result::fail(Status::Fatal, L"The keys couldn't be saved in Credential Manager.");
    }
    Ring mine;
    if (!ks.load_ring(mine)) mine = Ring{};
    Ring r = adopt(mine, got, me);
    if (!ks.save_ring(r)) return Result::fail(Status::Fatal, L"The keys couldn't be saved in Credential Manager.");
    return {};
}

Result redeem_pairing(Backend& raw, KeyStore& ks, const std::string& typed, Progress& prog) {
    std::string c = crypto::normalize_code(typed);
    if (c.empty()) return Result::fail(Status::Rejected, L"That isn't a code: 15 letters and digits.");
    std::vector<sync::RemoteEntry> items;
    Result r = list_dir(raw, kPair, items, prog);
    if (!r.ok()) return r;
    bool any = false;
    int tried = 0;
    for (auto& e : items) {
        Bytes b;
        if (!get_object(raw, e.path, b, nullptr, prog).ok()) continue;
        Reader q(b);
        if (!q.magic("EPP2")) continue;
        uint8_t salt[16];
        q.get(salt, 16);
        crypto::Argon2Params ap;
        ap.memKiB = q.u32();
        ap.passes = q.u32();
        uint64_t expiry = q.u64();
        // a planted grant costs one Argon2id run at most, within bounds: no far-future
        // expiry (it would never go away), no huge parameters, a handful of grants per try
        uint64_t now = now_ft();
        if (q.bad || expiry < now || expiry > now + 2 * kPairLifetime || !crypto::argon2_params_ok(ap)) continue;
        if (++tried > 8) break;
        any = true;
        Key k;
        if (!crypto::argon2id(c, salt, 16, ap, k)) return Result::fail(Status::Fatal, kNoMemory);
        Bytes sealed(b.begin() + q.pos, b.end()), p;
        if (!crypto::open(k, "ep pair " + std::to_string(expiry), sealed, p)) continue;
        Ring got;
        bool ok = got.parse(p);
        crypto::wipe(p.data(), p.size());
        if (!ok) continue;
        r = take_ring(ks, got);
        if (!r.ok()) return r;
        remove_object(raw, e.path, prog);
        return {};
    }
    return Result::fail(Status::Rejected, any ? L"The code doesn't match. Check it, or make a new one on the other device."
                                              : L"There's no pairing code waiting (they last 15 minutes). Make a new one on the other device.");
}

Result redeem_recovery(Backend& raw, KeyStore& ks, const std::string& typed, Progress& prog) {
    if (typed.empty()) return Result::fail(Status::Rejected, L"Type your sync password.");
    Bytes vb;
    Result r = get_object(raw, kVaultFile, vb, nullptr, prog);
    if (!r.ok()) return r;
    VaultFile f;
    if (!parse_vault(vb, f)) return Result::fail(Status::Tampered, L"The vault file on the storage is damaged, or from an older version.");
    if (!crypto::argon2_params_ok(f.rparams)) return Result::fail(Status::Tampered, L"The vault file on the storage is damaged.");
    // the password as typed; else, an old ring's recovery key (dashes, case and look-alike
    // letters don't matter in those)
    std::vector<std::string> tries = { typed };
    if (std::string c = crypto::normalize_code(typed); !c.empty() && c != typed) tries.push_back(c);
    Bytes p;
    bool opened = false;
    for (auto& t : tries) {
        Key k;
        if (!crypto::argon2id(t, f.rsalt.data(), 16, f.rparams, k)) return Result::fail(Status::Fatal, kNoMemory);
        if ((opened = crypto::open(k, "ep recovery", f.recovery, p))) break;
    }
    if (!opened) return Result::fail(Status::Rejected, L"That isn't the sync password of this storage.");
    Ring got;
    bool ok = got.parse(p);
    crypto::wipe(p.data(), p.size());
    if (!ok) return Result::fail(Status::Tampered, L"The recovery data is damaged.");
    return take_ring(ks, got);
}

// ------------------------------------------------------------------ devices
Result list_devices(Backend& raw, KeyStore& ks, std::vector<DeviceInfo>& out, Progress& prog) {
    out.clear();
    Ring ring;
    if (!ks.load_ring(ring)) return Result::fail(Status::Locked);
    DeviceKey me = device_key(ks);
    bool changed = ensure_member(ring, me);
    if (read_devices(raw, ring, true, &out, nullptr, prog) || changed) ks.save_ring(ring);
    for (auto& d : out) d.thisDevice = d.id == me.id;
    std::sort(out.begin(), out.end(), [](const DeviceInfo& a, const DeviceInfo& b) { return CompareFileTime(&a.added, &b.added) < 0; });
    return {};
}

Result remove_device(Backend& raw, KeyStore& ks, const std::string& deviceId, const std::string& newPassword, Progress& prog) {
    Ring ring;
    if (!ks.load_ring(ring)) return Result::fail(Status::Locked);
    DeviceKey me = device_key(ks);
    if (deviceId == me.id) return Result::fail(Status::Rejected, L"This is the device you're using.");
    if (std::wstring why = password_problem(newPassword); !why.empty()) return Result::fail(Status::Rejected, why);
    bool same = false;
    if (Result x = password_matches(ring, newPassword, same); !x.ok()) return x;
    if (same) return Result::fail(Status::Rejected, L"Choose a new password: the removed device may know the old one.");
    // the vault must open first (and be on the current generation); it also takes in the
    // members whose records are sealed under it, so they get the new ring
    Vault v;
    Result r = open_vault(raw, ks, false, v, prog);
    if (!r.ok()) return r;
    ks.load_ring(ring);
    const Ring before = ring;                // whom the vault may be signed by (the removed device too)
    Ring::Gen old = ring.current();
    Ring::Gen g;
    g.gen = old.gen + 1;
    g.umk = crypto::random_key();
    ring.gens.push_back(g);
    if (!ring.is_removed(deviceId)) ring.removed.push_back(deviceId);
    ring.members.erase(std::remove_if(ring.members.begin(), ring.members.end(), [&](const Ring::Member& m) { return m.id == deviceId; }),
                       ring.members.end());
    if (Result x = new_recovery(ring, newPassword); !x.ok()) return x;
    if (!ks.save_ring(ring)) return Result::fail(Status::Fatal, L"The keys couldn't be saved in Credential Manager.");
    // this vault moves on now; the others when this device next opens them
    for (int attempt = 0; attempt < 4; ++attempt) {
        Bytes vb;
        std::string rev;
        r = get_object(raw, kVaultFile, vb, &rev, prog);
        if (!r.ok()) return r;
        VaultFile f;
        if (!parse_vault(vb, f)) return Result::fail(Status::Tampered);
        const Ring::Gen* vg = ring.find(f.umk);
        if (!vg) return Result::fail(Status::Locked);
        Bytes p;
        if (!crypto::open(crypto::kdf(vg->umk, "ep vault"), "ep vault", f.data, p) || !parse_payload(p, v)) return Result::fail(Status::Tampered);
        crypto::wipe(p.data(), p.size());
        v.manifest = f.manifest;
        if (Result s = check_vault_signer(f, vg->gen == ring.current().gen ? ring : before); !s.ok()) return s;
        if (vg->gen == ring.current().gen) break;
        uint32_t nv = v.vks.rbegin()->first + 1;
        if (nv == 0 || nv > 0xFFFF) return Result::fail(Status::Tampered, L"The vault has run out of key versions.");
        v.vks[nv] = crypto::random_key();
        v.current = nv;
        r = put_object(raw, kVaultFile, write_vault(seal_vault(v, ring), me), rev, prog);
        if (r.st == Status::PreconditionFailed) continue;
        if (!r.ok()) return r;
        break;
    }
    // the manifest, signed by this device under the new key version: what the removed
    // device signed is taken until now, and not after
    v.me = me;
    v.members = ring.members;
    v.removed = ring.removed;
    syncman::resign_root(raw, v, prog);
    hand_over(raw, ring, old, me, prog);
    r = register_device(raw, ring, me, prog);
    if (!r.ok()) return r;
    return {};
}

Result change_password(KeyStore& ks, const std::string& newPassword) {
    Ring ring;
    if (!ks.load_ring(ring)) return Result::fail(Status::Locked, L"This device has no keys.");
    // a new UMK generation too: the old password opens old password wraps (a storage keeps
    // old copies: git history, S3 versions), and with them the ring of that time. Whoever
    // knows it must not get the user key that seals vaults from now on.
    Ring::Gen g;
    g.gen = ring.current().gen + 1;
    g.umk = crypto::random_key();
    ring.gens.push_back(g);
    if (Result x = new_recovery(ring, newPassword); !x.ok()) return x;
    if (!ks.save_ring(ring)) return Result::fail(Status::Fatal, L"The keys couldn't be saved in Credential Manager.");
    return {};
}

Result refresh_vault_recovery(Backend& raw, KeyStore& ks, Progress& prog) {
    Ring ring;
    if (!ks.load_ring(ring)) return Result::fail(Status::Locked);
    DeviceKey me = device_key(ks);
    for (int attempt = 0; attempt < 4; ++attempt) {
        Bytes vb;
        std::string rev;
        Result r = get_object(raw, kVaultFile, vb, &rev, prog);
        if (!r.ok()) return r;
        VaultFile f;
        if (!parse_vault(vb, f)) return Result::fail(Status::Tampered, L"The vault file on the storage is damaged, or from an older version.");
        const Ring::Gen* g = ring.find(f.umk);
        if (!g) return Result::fail(Status::Locked);
        if (g->gen != ring.current().gen) {                  // behind: opening it moves it on, with the new wrap
            Vault v;
            return open_vault(raw, ks, false, v, prog);
        }
        Bytes p;
        Vault v;
        if (!crypto::open(crypto::kdf(g->umk, "ep vault"), "ep vault", f.data, p) || !parse_payload(p, v)) return Result::fail(Status::Tampered);
        crypto::wipe(p.data(), p.size());
        v.manifest = f.manifest;
        if (Result s = check_vault_signer(f, ring); !s.ok()) return s;
        r = put_object(raw, kVaultFile, write_vault(seal_vault(v, ring), me), rev, prog);
        if (r.st == Status::PreconditionFailed) continue;
        return r;
    }
    return Result::fail(Status::PreconditionFailed);
}
} // namespace synckeys
