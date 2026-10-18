#pragma once
#include "sync_backend.h"
#include "sync_keys.h"

// The encrypting layer (docs/sync-plan.md, "Effects on the rest of the design"): a Backend
// over a raw store that the engine sees as a plaintext store. Contents go through the file
// format (crypto_file.h), every path component through the name encryption
// (crypto_names.h); the vault folder is hidden. Stores without a change feed get one cheap
// poll through the vault folder's change marker (".0/5").
namespace sync {

// reopen: called when a name or file needs a vault key version this vault object doesn't
// have yet, or when the vault object changed since it was read (another device rotated:
// new uploads must use the new key version at once): refreshes the vault from the store
using VaultReopen = std::function<Result(Backend& raw, synckeys::Vault& v)>;
// retiredFile: where the note of retired key versions is kept (docs/sync-format.md,
// "Retired key versions"); the manifest's pin (manifest.pin) and, for stores without
// transactions, the log of this device's writes not in a manifest yet (manifest.own) go
// beside it. Empty: in memory only (tests).
//
// The vault's manifest (sync_manifest.h) is kept when the vault came from open_vault (it
// carries this device's signing key): the listing is the manifest's, files the storage
// withholds, rolls back or plants are reported, and every commit writes the next manifest.
std::unique_ptr<Backend> make_encrypting(std::unique_ptr<Backend> raw, const synckeys::Vault& vault, VaultReopen reopen = {},
                                         const std::wstring& retiredFile = {});

// A pair's store: the raw store of its type, with retries, its vault opened (created in an
// empty place when `create`), encrypted. Locked: this device can't open the vault yet.
// pinFile: where the pair keeps what it saw of its vault (synckeys::VaultPin); empty: none.
// The note of retired key versions goes beside it (retired.dat).
Result open_pair_backend(const std::wstring& typeId, const BackendConfig& cfg, synckeys::KeyStore& ks, bool create,
                         std::unique_ptr<Backend>& out, Progress& prog, const std::wstring& pinFile = {});

} // namespace sync
