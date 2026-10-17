#pragma once
#include "sync_backend.h"
#include "sync_keys.h"

// The encrypting layer (docs/sync-plan.md, "Effects on the rest of the design"): a Backend
// over a raw store that the engine sees as a plaintext store. Contents go through the file
// format (crypto_file.h), every path component through the name encryption
// (crypto_names.h); the vault folder is hidden. Stores without a change feed get one cheap
// poll through the ".ep-vault/head" marker.
namespace sync {

// reopen: called when a name or file needs a vault key version this vault object doesn't
// have yet (another device rotated): refreshes the vault from the store
using VaultReopen = std::function<Result(Backend& raw, synckeys::Vault& v)>;
std::unique_ptr<Backend> make_encrypting(std::unique_ptr<Backend> raw, const synckeys::Vault& vault, VaultReopen reopen = {});

// A pair's store: the raw store of its type, with retries, its vault opened (created in an
// empty place when `create`), encrypted. Locked: this device can't open the vault yet.
Result open_pair_backend(const std::wstring& typeId, const BackendConfig& cfg, synckeys::KeyStore& ks, bool create,
                         std::unique_ptr<Backend>& out, Progress& prog);

} // namespace sync
