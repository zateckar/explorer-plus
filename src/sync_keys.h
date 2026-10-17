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
//            generation, and the recovery key's Argon2id result (so any device can write
//            recovery wraps without knowing the code). Kept in Credential Manager.
//   vault    one per remote location, in its ".ep-vault" folder: vault keys (VK) by
//            version, sealed under one UMK generation, plus the recovery wrap of the ring.
//   device   an X25519 key pair per device; vaults list the devices (sealed records), and a
//            new ring generation reaches the remaining devices sealed to their public keys.
//
// Codes (pairing, recovery) are 15 Crockford Base32 characters; Argon2id turns them into
// keys, and they never touch the storage.
namespace synckeys {

using sync::Result;
using sync::Status;

struct Ring {
    struct Gen { uint32_t gen = 0; crypto::Key umk; };
    std::vector<Gen> gens;                   // ascending; back() is current
    uint8_t recSalt[16]{};                   // the recovery key's Argon2id salt, parameters, result
    crypto::Argon2Params recParams;
    crypto::Key recKek;
    std::vector<std::string> removed;        // device IDs removed (their records go, they get no new generation)
    bool empty() const { return gens.empty(); }
    const Gen& current() const { return gens.back(); }
    const Gen* find(const std::string& umkId) const;
    crypto::Bytes serialize() const;
    bool parse(const crypto::Bytes& b);
};
std::string umk_id(const crypto::Key& umk);  // 16 hex: which UMK a vault needs

struct DeviceKey {
    crypto::Key secret, pub;
    std::string id;                          // 16 hex of BLAKE2b(pub)
    std::wstring name;
    bool valid() const { return !id.empty(); }
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

// first-time setup on the first device: a new ring (UMK generation 1) and recovery key.
// The code is shown once and kept nowhere.
Result setup_ring(KeyStore& ks, std::string& recoveryCode);
// this device's key pair (created on first use, named after the computer)
DeviceKey device_key(KeyStore& ks);

// An open vault: what the encrypting layer needs. Shared by the pair's backend objects.
struct Vault {
    std::string id;                          // 32 hex
    uint32_t current = 0;                    // VK version new uploads and names use
    std::map<uint32_t, crypto::Key> vks;
    std::string rev;                         // the vault object's revision when it was read (not stored)
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
struct VaultPin {
    std::string id;                          // empty: nothing seen yet (trust on first use)
    uint32_t gen = 0, version = 0;           // highest UMK generation and vault key version seen
    bool load(const std::wstring& file);
    bool save(const std::wstring& file) const;
};
Result open_vault(sync::Backend& raw, KeyStore& ks, bool create, Vault& out, sync::Progress& prog, VaultPin* pin = nullptr);

// "Add a device": a pairing grant in the vault (the ring under Argon2id(code)), valid 15
// minutes; the code is returned for display.
Result create_pairing(sync::Backend& raw, KeyStore& ks, std::string& code, sync::Progress& prog);
// on the new device: the code from the other device, or the recovery key; the ring is
// stored locally and the grant deleted
Result redeem_pairing(sync::Backend& raw, KeyStore& ks, const std::string& code, sync::Progress& prog);
Result redeem_recovery(sync::Backend& raw, KeyStore& ks, const std::string& code, sync::Progress& prog);

Result list_devices(sync::Backend& raw, KeyStore& ks, std::vector<DeviceInfo>& out, sync::Progress& prog);
// "Remove device": a new UMK generation, a new vault key version, the new ring sealed to
// each remaining device, the removed device's record deleted, a new recovery key (shown)
Result remove_device(sync::Backend& raw, KeyStore& ks, const std::string& deviceId, std::string& newRecoveryCode,
                     sync::Progress& prog);

// "New recovery key": a new code for the ring (shown once) and a new UMK generation;
// every vault this device then opens or refreshes moves to that generation (a new vault
// key version, hand-overs for the other devices) and carries the new recovery wrap. The
// old code keeps opening old copies of vaults, and with them what was written before.
Result new_recovery_key(KeyStore& ks, std::string& code);
Result refresh_vault_recovery(sync::Backend& raw, KeyStore& ks, sync::Progress& prog);

// the reserved folder on the store
constexpr wchar_t kVaultDir[] = L".ep-vault";
constexpr wchar_t kVaultFileRel[] = L".ep-vault/vault";
inline bool is_vault_path(const sync::RelPath& p) {
    return p.compare(0, 9, kVaultDir) == 0 && (p.size() == 9 || p[9] == L'/');
}

// small objects on a raw store (in their own transaction where the store has them)
Result get_object(sync::Backend& raw, const sync::RelPath& p, crypto::Bytes& out, std::string* rev, sync::Progress& prog);
Result put_object(sync::Backend& raw, const sync::RelPath& p, const crypto::Bytes& data, const std::string& expectedRev,
                  sync::Progress& prog, std::string* newRev = nullptr);
Result remove_object(sync::Backend& raw, const sync::RelPath& p, sync::Progress& prog);

} // namespace synckeys
