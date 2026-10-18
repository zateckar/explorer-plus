#pragma once
#include "crypto.h"
#include "sync_backend.h"
#include "sync_keys.h"
#include <functional>
#include <map>

// The signed per-vault manifest (docs/sync-format.md, "Manifest"): every file of the vault
// with its size, version counter, plaintext BLAKE2b and stored revision, signed by the
// member device that wrote it. A device takes the storage as the manifest says, so files
// the storage withholds, rolls back, replaces or plants are told apart from real changes.
//
//   .ep-vault/manifest       the root: sequence number, the shards' hashes; sealed under a
//                            vault key version, signed (EdDSA) by its writer
//   .ep-vault/m/<32 hex>     the shards: the files, split by a keyed hash of their path;
//                            named after their own hash, so a pass rewrites only the shards
//                            it touched
namespace syncman {

using sync::Result;
using sync::Status;

struct Entry {
    uint64_t size = 0;                          // plaintext
    uint64_t version = 0;                       // the file's version counter
    std::array<uint8_t, 32> hash{};             // BLAKE2b of the plaintext; zero: not known (taken in from a listing)
    std::string rev;                            // the stored object's revision
    bool operator==(const Entry& o) const { return size == o.size && version == o.version && hash == o.hash && rev == o.rev; }
    bool operator!=(const Entry& o) const { return !(*this == o); }
    bool hash_known() const { for (uint8_t b : hash) if (b) return true; return false; }
};

struct Manifest {
    uint64_t seq = 0;                           // 0: none yet
    std::map<std::wstring, Entry> files;        // plaintext path ('/'-separated) -> entry
};

// the root, as read
struct Root {
    std::string signer;                         // device ID
    uint32_t version = 0;                       // the vault key version it's sealed under
    uint32_t placement = 0;                     // the vault key version that places paths in shards
    uint64_t seq = 0;
    FILETIME written{};
    std::vector<crypto::Hash32> shards;         // their hashes, by index
    std::string hash;                           // BLAKE2b of the root object (hex): the pin
};

constexpr wchar_t kRootRel[] = L".ep-vault/manifest";
constexpr wchar_t kShardDir[] = L".ep-vault/m";
std::wstring shard_rel(const crypto::Hash32& h);            // ".ep-vault/m/<32 hex>"

using KeyLookup = std::function<bool(uint32_t version, crypto::Key& out)>;
using MemberLookup = std::function<const synckeys::Ring::Member*(const std::string& id)>;
using RemovedLookup = std::function<bool(const std::string& id)>;

// The objects of a manifest: the root and its shards (by index), sealed under `version`.
// `prev` (may be null): the manifest the new one follows, with its shards' bytes; a shard
// whose files are all the same keeps its bytes and name, so it isn't written again (seals
// are randomized).
struct Encoded {
    crypto::Bytes root;
    std::vector<crypto::Bytes> shards;
    std::vector<crypto::Hash32> hashes;
};
struct Previous {
    const Manifest* m = nullptr;
    uint32_t placement = 0;
    std::vector<crypto::Bytes> shards;          // by index
};
Result encode(const Manifest& m, const std::string& vaultId, const KeyLookup& keys, uint32_t version,
              const synckeys::DeviceKey& signer, const Previous* prev, Encoded& out);
// a root alone (the shards stay as they are)
crypto::Bytes encode_root(const std::string& vaultId, uint32_t version, const crypto::Key& vk, uint32_t placement,
                          const synckeys::DeviceKey& signer, uint64_t seq, const std::vector<crypto::Hash32>& shards);

// the root: format, signature by a member that wasn't removed, the seal, the vault ID.
// lenient: a writer this device can't check (not a member, or removed) is let through; the
// seal still has to open.
Result read_root(const crypto::Bytes& b, const std::string& vaultId, const KeyLookup& keys, const MemberLookup& member,
                 const RemovedLookup& removed, Root& out, bool lenient = false);
std::string root_signer(const crypto::Bytes& b);            // "" when it isn't a root
// one shard of a root: its hash, the seal, and every path in it belongs there
Result read_shard(const crypto::Bytes& b, const Root& root, size_t index, const std::string& vaultId, const KeyLookup& keys,
                  std::map<std::wstring, Entry>& into);

// which shard a path is in, for `count` shards placed under vault key `vk`
size_t shard_of(const std::wstring& path, size_t count, const crypto::Key& vk);
size_t shard_count_for(size_t files);                       // a power of two, about 1,000 files a shard

// After the vault moved to a new key version (a device removed, a new sync password): the
// root again, sealed under that version and signed by this device, its shards as they are.
// What a removed device signed while it was a member stays readable that way; what it
// signs afterwards is refused.
Result resign_root(sync::Backend& raw, const synckeys::Vault& v, sync::Progress& prog);

// what this device saw of the pair's manifest (beside the vault pin): a root with a lower
// sequence number is a rolled-back manifest
struct Pin {
    uint64_t seq = 0;
    std::string hash;
    bool load(const std::wstring& file);
    bool save(const std::wstring& file) const;
};

// this device's writes not yet in a manifest it wrote (non-transactional stores: the files
// go up first, the manifest after them). An append-only log, so a crash in between doesn't
// make the device doubt its own files. A removal keeps the entry it removed (its version).
struct OwnLog {
    std::wstring file;                          // "" = in memory only
    std::map<std::wstring, std::pair<bool, Entry>> items;   // path -> (removed?, entry)
    void load();
    void put(const std::wstring& path, const Entry& e);
    void removed(const std::wstring& path, const Entry& was);
    void clear();
};

// `own` applied onto `base`: a write replaces an entry that isn't newer, a removal removes
// one that isn't newer than what was removed (another device's later write wins)
void fold(Manifest& base, const OwnLog& own);

} // namespace syncman
