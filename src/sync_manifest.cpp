#include "sync_manifest.h"
#include <cstring>

namespace syncman {

namespace {

using crypto::Bytes;

void put16(Bytes& b, uint32_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
void put64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
void putn(Bytes& b, const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }

struct Reader {
    const Bytes& b;
    size_t pos = 0;
    bool bad = false;
    bool need(size_t n) { if (bad || b.size() - pos < n) bad = true; return !bad; }
    uint64_t le(int n) {
        if (!need(n)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= (uint64_t)b[pos + i] << (8 * i);
        pos += n;
        return v;
    }
    bool raw(void* out, size_t n) { if (!need(n)) return false; std::memcpy(out, b.data() + pos, n); pos += n; return true; }
    std::string str(size_t n) { if (!need(n)) return {}; std::string s((const char*)b.data() + pos, n); pos += n; return s; }
};

constexpr char kRootMagic[] = "EPM1";
constexpr char kShardMagic[] = "EPMS";
constexpr char kSignCtx[] = "ep manifest";
constexpr size_t kHeader = 4 + 4 + 16;          // magic, key version, signer
constexpr size_t kMaxShards = 1u << 12;

Bytes vault_id_bytes(const std::string& id) {
    Bytes b;
    crypto::unhex(id, b);
    b.resize(16);
    return b;
}

crypto::Key seal_key(const crypto::Key& vk) { return crypto::kdf(vk, "ep manifest"); }

std::string shard_ad(uint32_t version, uint32_t index, const std::string& vaultId) {
    Bytes id = vault_id_bytes(vaultId);
    Bytes ad(kShardMagic, kShardMagic + 4);
    put32(ad, version);
    put32(ad, index);
    putn(ad, id.data(), 16);
    return std::string(ad.begin(), ad.end());
}

// entry: path length u16 | path | size u64 | version u64 | hash | rev length u16 | rev
void put_entry(Bytes& p, const std::wstring& path, const Entry& e) {
    std::string u = sync::to_utf8(path);
    put16(p, (uint32_t)u.size());
    putn(p, u.data(), u.size());
    put64(p, e.size);
    put64(p, e.version);
    putn(p, e.hash.data(), 32);
    put16(p, (uint32_t)e.rev.size());
    putn(p, e.rev.data(), e.rev.size());
}
bool get_entry(Reader& r, std::wstring& path, Entry& e) {
    std::string u = r.str((size_t)r.le(2));
    e.size = r.le(8);
    e.version = r.le(8);
    r.raw(e.hash.data(), 32);
    e.rev = r.str((size_t)r.le(2));
    if (r.bad) return false;
    path = sync::from_utf8(u);
    return true;
}

using Files = std::vector<std::pair<const std::wstring*, const Entry*>>;

Bytes seal_shard(const Files& files, uint32_t version, uint32_t index, const std::string& vaultId, const crypto::Key& vk) {
    Bytes plain;
    put32(plain, (uint32_t)files.size());
    for (auto& [path, e] : files) put_entry(plain, *path, *e);
    Bytes b(kShardMagic, kShardMagic + 4);
    put32(b, version);
    put32(b, index);
    Bytes sealed = crypto::seal(seal_key(vk), shard_ad(version, index, vaultId), plain.data(), plain.size());
    crypto::wipe(plain.data(), plain.size());
    putn(b, sealed.data(), sealed.size());
    return b;
}

std::vector<Files> split(const Manifest& m, size_t count, const crypto::Key& pk) {
    std::vector<Files> by(count);
    for (auto& [p, e] : m.files) by[shard_of(p, count, pk)].push_back({ &p, &e });
    return by;
}

} // namespace

std::wstring shard_rel(const crypto::Hash32& h) {
    std::string x = crypto::hex(h.data(), 16);
    return std::wstring(kShardDir) + L"/" + std::wstring(x.begin(), x.end());
}

size_t shard_count_for(size_t files) {
    size_t n = 1;
    while (n < kMaxShards && files > n * 1000) n <<= 1;
    return n;
}

size_t shard_of(const std::wstring& path, size_t count, const crypto::Key& vk) {
    if (count <= 1) return 0;
    std::string u = sync::to_utf8(path);
    crypto::Hash32 h = crypto::blake2b_keyed(crypto::kdf(vk, "ep manifest shard"), u.data(), u.size());
    uint32_t v = h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24);
    return v & (count - 1);
}

// the root: "EPM1" | key version | signer | sealed length | sealed body | signature, the
// signature over all before it; body: vault ID | seq | written | placement | count | hashes
Bytes encode_root(const std::string& vaultId, uint32_t version, const crypto::Key& vk, uint32_t placement,
                  const synckeys::DeviceKey& signer, uint64_t seq, const std::vector<crypto::Hash32>& shards) {
    Bytes body;
    Bytes id = vault_id_bytes(vaultId);
    putn(body, id.data(), 16);
    put64(body, seq);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    put64(body, ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime);
    put32(body, placement);
    put32(body, (uint32_t)shards.size());
    for (auto& h : shards) putn(body, h.data(), 32);
    Bytes r(kRootMagic, kRootMagic + 4);
    put32(r, version);
    std::string sid = signer.id;
    sid.resize(16, '0');
    putn(r, sid.data(), 16);
    Bytes sealed = crypto::seal(seal_key(vk), std::string(r.begin(), r.end()), body.data(), body.size());
    put32(r, (uint32_t)sealed.size());
    putn(r, sealed.data(), sealed.size());
    uint8_t sig[crypto::kSig];
    synckeys::sign_as(signer, kSignCtx, r.data(), r.size(), sig);
    putn(r, sig, crypto::kSig);
    return r;
}

Result encode(const Manifest& m, const std::string& vaultId, const KeyLookup& keys, uint32_t version,
              const synckeys::DeviceKey& signer, const Previous* prev, Encoded& out) {
    out = {};
    size_t count = shard_count_for(m.files.size());
    // a previous manifest's shards are kept only when the paths are placed the same way
    if (prev && (!prev->m || prev->shards.size() != count)) prev = nullptr;
    uint32_t placement = prev ? prev->placement : version;
    crypto::Key vk, pk;
    if (!keys(version, vk) || !keys(placement, pk)) return Result::fail(Status::Locked, L"The vault key is missing.");
    std::vector<Files> by = split(m, count, pk), old;
    if (prev) old = split(*prev->m, count, pk);
    out.shards.resize(count);
    out.hashes.resize(count);
    for (size_t i = 0; i < count; ++i) {
        bool same = prev && by[i].size() == old[i].size();
        for (size_t k = 0; same && k < by[i].size(); ++k) same = *by[i][k].first == *old[i][k].first && *by[i][k].second == *old[i][k].second;
        out.shards[i] = same ? prev->shards[i] : seal_shard(by[i], version, (uint32_t)i, vaultId, vk);
        out.hashes[i] = crypto::blake2b(out.shards[i].data(), out.shards[i].size());
    }
    out.root = encode_root(vaultId, version, vk, placement, signer, m.seq, out.hashes);
    return {};
}

std::string root_signer(const Bytes& b) {
    if (b.size() < kHeader || std::memcmp(b.data(), kRootMagic, 4) != 0) return {};
    return std::string((const char*)b.data() + 8, 16);
}

Result read_root(const Bytes& b, const std::string& vaultId, const KeyLookup& keys, const MemberLookup& member,
                 const RemovedLookup& removed, Root& out, bool lenient) {
    auto bad = [](const wchar_t* why) { return Result::fail(Status::Tampered, why); };
    if (b.size() < kHeader + 4 + crypto::kSig || std::memcmp(b.data(), kRootMagic, 4) != 0)
        return bad(L"The vault's manifest on the storage is damaged, or from another version.");
    Reader rd{ b, 4 };
    Root r;
    r.version = (uint32_t)rd.le(4);
    r.signer = rd.str(16);
    uint32_t n = (uint32_t)rd.le(4);
    if (!rd.need(n) || b.size() - rd.pos - n != crypto::kSig) return bad(L"The vault's manifest on the storage is damaged.");
    // who wrote it: a device of the user's, not one that was removed, and the signature checks
    size_t sigAt = b.size() - crypto::kSig;
    bool gone = removed && removed(r.signer);
    const synckeys::Ring::Member* mem = gone || !member ? nullptr : member(r.signer);
    if (mem) {
        if (!synckeys::signed_by(mem->spub, kSignCtx, b.data(), sigAt, b.data() + sigAt)) return bad(L"The vault manifest's signature doesn't check.");
    } else if (!lenient) {
        return bad(gone ? L"The vault's manifest was written by a device that was removed."
                        : L"The vault's manifest was written by a device this one doesn't know.");
    }
    crypto::Key vk;
    if (!keys(r.version, vk)) return Result::fail(Status::Locked, L"The vault's manifest needs a vault key this device doesn't have yet.");
    Bytes sealed(b.begin() + rd.pos, b.begin() + rd.pos + n), body;
    if (!crypto::open(seal_key(vk), std::string((const char*)b.data(), kHeader), sealed, body)) return bad(L"The vault's manifest doesn't authenticate.");
    Reader br{ body };
    Bytes id(16);
    br.raw(id.data(), 16);
    r.seq = br.le(8);
    uint64_t w = br.le(8);
    r.written.dwLowDateTime = (DWORD)w;
    r.written.dwHighDateTime = (DWORD)(w >> 32);
    r.placement = (uint32_t)br.le(4);
    uint32_t count = (uint32_t)br.le(4);
    if (br.bad || count == 0 || count > kMaxShards || (count & (count - 1)) || body.size() - br.pos != (size_t)count * 32)
        return bad(L"The vault's manifest is damaged.");
    if (id != vault_id_bytes(vaultId)) return bad(L"The manifest on the storage belongs to another vault.");
    r.shards.resize(count);
    for (auto& h : r.shards) br.raw(h.data(), 32);
    r.hash = crypto::hex(crypto::blake2b(b.data(), b.size()));
    out = std::move(r);
    return {};
}

Result read_shard(const Bytes& b, const Root& root, size_t index, const std::string& vaultId, const KeyLookup& keys,
                  std::map<std::wstring, Entry>& into) {
    auto bad = [](const wchar_t* why) { return Result::fail(Status::Tampered, why); };
    if (index >= root.shards.size() || crypto::blake2b(b.data(), b.size()) != root.shards[index])
        return bad(L"A part of the vault's manifest on the storage isn't the one its root names.");
    Reader rd{ b };
    if (rd.str(4) != std::string(kShardMagic, 4)) return bad(L"A part of the vault's manifest is damaged.");
    uint32_t version = (uint32_t)rd.le(4), idx = (uint32_t)rd.le(4);
    if (rd.bad || idx != index) return bad(L"A part of the vault's manifest is damaged.");
    crypto::Key vk, pk;
    if (!keys(version, vk) || !keys(root.placement, pk))
        return Result::fail(Status::Locked, L"The vault's manifest needs a vault key this device doesn't have yet.");
    Bytes sealed(b.begin() + rd.pos, b.end()), plain;
    if (!crypto::open(seal_key(vk), shard_ad(version, idx, vaultId), sealed, plain)) return bad(L"A part of the vault's manifest doesn't authenticate.");
    Reader pr{ plain };
    uint32_t n = (uint32_t)pr.le(4);
    bool ok = !pr.bad;
    for (uint32_t i = 0; i < n && ok; ++i) {
        std::wstring wp;
        Entry e;
        ok = get_entry(pr, wp, e) && !wp.empty() && shard_of(wp, root.shards.size(), pk) == index && !into.count(wp);
        if (ok) into[wp] = std::move(e);
    }
    ok = ok && pr.pos == plain.size();
    crypto::wipe(plain.data(), plain.size());
    if (!ok) return bad(L"A part of the vault's manifest is damaged.");
    return {};
}

Result resign_root(sync::Backend& raw, const synckeys::Vault& v, sync::Progress& prog) {
    KeyLookup keys = [&v](uint32_t ver, crypto::Key& k) { return v.key(ver, k); };
    MemberLookup member = [&v](const std::string& id) -> const synckeys::Ring::Member* {
        for (auto& m : v.members) if (m.id == id) return &m;
        return nullptr;
    };
    crypto::Key vk;
    if (!v.key(v.current, vk)) return Result::fail(Status::Locked);
    for (int attempt = 0; attempt < 4; ++attempt) {
        Bytes b;
        std::string rev;
        Result r = synckeys::get_object(raw, kRootRel, b, &rev, prog);
        if (r.st == Status::NotFound) return {};
        if (!r.ok()) return r;
        Root root;
        r = read_root(b, v.id, keys, member, {}, root, true);
        if (!r.ok()) return r;
        if (root.version == v.current && root.signer == v.me.id) return {};
        Bytes nb = encode_root(v.id, v.current, vk, root.placement, v.me, root.seq + 1, root.shards);
        r = synckeys::put_object(raw, kRootRel, nb, rev, prog);
        if (r.st == Status::PreconditionFailed) continue;
        return r;
    }
    return Result::fail(Status::PreconditionFailed, L"The vault's manifest kept changing.");
}

void fold(Manifest& base, const OwnLog& own) {
    for (auto& [path, item] : own.items) {
        auto f = base.files.find(path);
        if (item.first) {
            if (f != base.files.end() && f->second.version <= item.second.version) base.files.erase(f);
        } else if (f == base.files.end() || f->second.version <= item.second.version) {
            base.files[path] = item.second;
        }
    }
}

// ------------------------------------------------------------------ local files
static bool read_all(const std::wstring& path, Bytes& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 30);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool Pin::load(const std::wstring& file) {
    Bytes b;
    if (file.empty() || !read_all(file, b)) return false;
    std::string s(b.begin(), b.end());
    if (s.compare(0, 5, "EPMP\n") != 0) return false;
    size_t a = s.find("\nseq="), h = s.find("\nhash=");
    if (a == std::string::npos || h == std::string::npos) return false;
    seq = strtoull(s.c_str() + a + 5, nullptr, 10);
    size_t e = s.find('\n', h + 1);
    hash = s.substr(h + 6, e == std::string::npos ? std::string::npos : e - h - 6);
    return true;
}

bool Pin::save(const std::wstring& file) const {
    if (file.empty()) return true;
    std::string s = "EPMP\nseq=" + std::to_string(seq) + "\nhash=" + hash + "\n";
    std::wstring tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size() && FlushFileBuffers(h);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// records: kind (1 written, 2 removed) | entry | the first 8 bytes of the record's BLAKE2b
void OwnLog::load() {
    items.clear();
    Bytes b;
    if (file.empty() || !read_all(file, b)) return;
    Reader rd{ b };
    while (rd.pos < b.size()) {
        size_t start = rd.pos;
        uint8_t kind = (uint8_t)rd.le(1);
        std::wstring path;
        Entry e;
        if (!get_entry(rd, path, e) || (kind != 1 && kind != 2)) break;
        crypto::Hash32 h = crypto::blake2b(b.data() + start, rd.pos - start);
        uint8_t check[8];
        if (!rd.raw(check, 8) || std::memcmp(check, h.data(), 8) != 0) break;       // a torn tail
        items[path] = { kind == 2, e };
    }
}

static void append(const std::wstring& file, uint8_t kind, const std::wstring& path, const Entry& e) {
    if (file.empty()) return;
    Bytes r{ kind };
    put_entry(r, path, e);
    crypto::Hash32 sum = crypto::blake2b(r.data(), r.size());
    r.insert(r.end(), sum.begin(), sum.begin() + 8);
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(h, r.data(), (DWORD)r.size(), &n, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

void OwnLog::put(const std::wstring& path, const Entry& e) {
    items[path] = { false, e };
    append(file, 1, path, e);
}

void OwnLog::removed(const std::wstring& path, const Entry& was) {
    items[path] = { true, was };
    append(file, 2, path, was);
}

void OwnLog::clear() {
    items.clear();
    if (!file.empty()) DeleteFileW(file.c_str());
}

} // namespace syncman
