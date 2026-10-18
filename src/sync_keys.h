#pragma once
#include "crypto.h"
#include "sync_backend.h"
#include <map>
#include <memory>
#include <mutex>

// Keys of the end-to-end encryption (docs/sync-plan.md, "Keys"; the stored forms are in
// docs/sync-format.md).
//
//   ring     the user's keys, the same on every device: user master keys (UMK) by
//            generation, and the sync password's Argon2id result (so any device can write
//            password wraps without knowing the password). Kept in Credential Manager.
//   vault    one per remote location, in its ".0" folder (kVaultDir): vault keys (VK) by
//            version, sealed under one UMK generation, plus the password ("recovery") wrap
//            of the ring.
//   device   an X25519 key pair per device; vaults list the devices (sealed records), and a
//            new ring generation reaches the remaining devices sealed to their public keys.
//
// The sync password is the user's own (password_problem: 12 characters or more, lower and
// upper case letters and digits); it unlocks a new device. Pairing codes are 15 Crockford
// Base32 characters. Argon2id turns either into a key; neither touches the storage. (Rings
// made before the password had a generated 15-character recovery key in its place: it
// still unlocks what it wrapped.)
//
// Members: the ring lists the user's devices (ID, X25519 and EdDSA public keys). Vault
// objects, device records and rotation hand-overs are signed by the device that wrote them;
// a signer that isn't a member, or was removed, is refused. A device becomes a member by
// pairing (it adds itself), or when a member reads its record sealed under the member's
// current generation (which a removed device never has). A hand-over goes to members only.
namespace synckeys {

using sync::Result;
using sync::Status;

struct Ring {
    struct Gen { uint32_t gen = 0; crypto::Key umk; };
    // a device of the user's: only members hand over and receive new generations, sign
    // vault objects and device records that are believed
    struct Member { std::string id; crypto::Key xpub, spub; };
    std::vector<Gen> gens;                   // ascending; back() is current
    uint8_t recSalt[16]{};                   // the sync password's Argon2id salt, parameters, result
    crypto::Argon2Params recParams;
    crypto::Key recKek;
    std::vector<std::string> removed;        // device IDs removed (their records go, they get no new generation)
    std::vector<Member> members;
    bool empty() const { return gens.empty(); }
    const Gen& current() const { return gens.back(); }
    const Gen* find(const std::string& umkId) const;
    const Member* member(const std::string& id) const;
    bool is_removed(const std::string& id) const;
    crypto::Bytes serialize() const;
    bool parse(const crypto::Bytes& b);
};
std::string umk_id(const crypto::Key& umk);  // 16 hex: which UMK a vault needs

struct DeviceKey {
    crypto::Key secret, pub;                 // X25519
    crypto::Key spub;                        // EdDSA, from the seed KDF(secret, "ep sign")
    std::string id;                          // 16 hex of BLAKE2b(pub)
    std::wstring name;
    bool valid() const { return !id.empty(); }
    Ring::Member member() const { return { id, pub, spub }; }
};
DeviceKey new_device_key(const std::wstring& name);

// where this device keeps its ring and device key
class KeyStore {
public:
    virtual ~KeyStore() = default;
    virtual bool load_ring(Ring& r) = 0;
    virtual bool save_ring(const Ring& r) = 0;
    virtual bool load_device(DeviceKey& d) = 0;
    virtual bool save_device(const DeviceKey& d) = 0;
    virtual void forget() = 0;               // "forget my keys on this device"
};
std::unique_ptr<KeyStore> credential_key_store();     // Credential Manager (this settings namespace)
std::unique_ptr<KeyStore> memory_key_store();         // tests

// what's wrong with a would-be sync password ("" when it will do): 12 characters or more,
// with lower and upper case letters and digits
std::wstring password_problem(const std::string& password);

// first-time setup on the first device: a new ring (UMK generation 1) under the user's sync
// password, which is kept nowhere
Result setup_ring(KeyStore& ks, const std::string& password);
// the sync password matches this device's ring (Rejected: it doesn't)
Result check_password(KeyStore& ks, const std::string& password);
// this device's key pair (created on first use, named after the computer)
DeviceKey device_key(KeyStore& ks);

// signatures of the user's devices (EdDSA over the context string and the message)
void sign_as(const DeviceKey& me, const char* ctx, const uint8_t* msg, size_t n, uint8_t sig[crypto::kSig]);
bool signed_by(const crypto::Key& spub, const char* ctx, const uint8_t* msg, size_t n, const uint8_t sig[crypto::kSig]);

// An open vault: what the encrypting layer needs. Shared by the pair's backend objects.
struct Vault {
    std::string id;                          // 32 hex
    uint32_t current = 0;                    // VK version new uploads and names use
    std::map<uint32_t, crypto::Key> vks;
    std::string rev;                         // the vault object's revision when it was read (not stored)
    // the manifest (sync_manifest.h): the vault object says the vault keeps one (then a
    // missing manifest is the storage's doing); who signs and who is believed
    bool manifest = false;
    DeviceKey me;                            // this device: signs the manifests it writes
    std::vector<Ring::Member> members;
    std::vector<std::string> removed;
    bool key(uint32_t version, crypto::Key& out) const {
        auto f = vks.find(version);
        if (f == vks.end()) return false;
        out = f->second;
        return true;
    }
};

struct DeviceInfo {
    std::string id;
    std::wstring name;
    FILETIME added{};
    bool thisDevice = false;
};

// Opens the vault of a store (the raw store, below the encrypting layer).
//   no vault, store empty, create: a new vault (VK version 1) under the ring's current UMK
//   no vault, files there: Rejected (they'd be plaintext)
//   a vault the ring can't open: Locked (pair this device first)
// Also: registers this device in the vault, takes a new ring generation handed to this
// device (rotation), and moves the vault to the current generation when it's behind.
//
// pin: what this device saw of the pair's vault before (kept beside the pair's state).
// A vault with another ID (the storage swapped in another of the user's vaults), or sealed
// under an older generation, or with fewer key versions (an old copy brought back), is
// refused as Tampered; on success the pin is updated (the caller saves it).
// A vault object signed by a device that has since been removed is taken only when it's
// the very object this device took before (hash): the removed device wrote it while it was
// a member, and opening it now moves the vault on, signed by this device.
struct VaultPin {
    std::string id;                          // empty: nothing seen yet (trust on first use)
    uint32_t gen = 0, version = 0;           // highest UMK generation and vault key version seen
    std::string hash;                        // BLAKE2b of the vault object last taken (hex)
    bool load(const std::wstring& file);
    bool save(const std::wstring& file) const;
};
Result open_vault(sync::Backend& raw, KeyStore& ks, bool create, Vault& out, sync::Progress& prog, VaultPin* pin = nullptr);

// the vault object says the vault keeps a manifest (set once its first manifest is written;
// new vaults have it from the start): from then on a missing manifest is the storage's doing.
// Only the object this device opened (v.rev) is re-signed.
Result mark_manifest(sync::Backend& raw, const Vault& v, sync::Progress& prog);

// "Add a device": a pairing grant in the vault (the ring under Argon2id(code)), valid 15
// minutes; the code is returned for display.
Result create_pairing(sync::Backend& raw, KeyStore& ks, std::string& code, sync::Progress& prog);
// on the new device: the code from the other device, or the sync password (an old
// 15-character recovery key too); the ring is stored locally and the grant deleted
Result redeem_pairing(sync::Backend& raw, KeyStore& ks, const std::string& code, sync::Progress& prog);
Result redeem_recovery(sync::Backend& raw, KeyStore& ks, const std::string& password, sync::Progress& prog);

Result list_devices(sync::Backend& raw, KeyStore& ks, std::vector<DeviceInfo>& out, sync::Progress& prog);
// "Remove device": a new UMK generation, a new vault key version, the new ring sealed to
// each remaining device, the removed device's record deleted, and a new sync password (one
// the ring didn't have: whoever knew the old one could open the new ring with it)
Result remove_device(sync::Backend& raw, KeyStore& ks, const std::string& deviceId, const std::string& newPassword,
                     sync::Progress& prog);

// "Change password": a new sync password for the ring and a new UMK generation; every vault
// this device then opens or refreshes moves to that generation (a new vault key version,
// hand-overs for the other devices) and carries the new password wrap. The old password
// keeps opening old copies of vaults, and with them what was written before.
Result change_password(KeyStore& ks, const std::string& newPassword);
Result refresh_vault_recovery(sync::Backend& raw, KeyStore& ks, sync::Progress& prog);

// The reserved folder on the store. Its names say nothing of what they hold (nor of the
// app): ".0/1" the vault, "2" device records, "3" pairing grants, "4" rotation hand-overs,
// "5" the change marker (sync_crypt.cpp), "6" the manifest's root, "7" its shards
// (sync_manifest.h).
constexpr wchar_t kVaultDir[] = L".0";
constexpr wchar_t kVaultFileRel[] = L".0/1";
inline bool is_vault_path(const sync::RelPath& p) {
    constexpr size_t n = sizeof(kVaultDir) / sizeof(wchar_t) - 1;
    return p.compare(0, n, kVaultDir) == 0 && (p.size() == n || p[n] == L'/');
}

// small objects on a raw store (in their own transaction where the store has them)
Result get_object(sync::Backend& raw, const sync::RelPath& p, crypto::Bytes& out, std::string* rev, sync::Progress& prog);
Result put_object(sync::Backend& raw, const sync::RelPath& p, const crypto::Bytes& data, const std::string& expectedRev,
                  sync::Progress& prog, std::string* newRev = nullptr);
Result remove_object(sync::Backend& raw, const sync::RelPath& p, sync::Progress& prog);

} // namespace synckeys
