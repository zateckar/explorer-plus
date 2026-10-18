#pragma once
#include "sync_backend.h"
#include <functional>

// The sync pairs of this settings folder: Sync\pairs.ini beside settings.ini (UTF-8), one
// section per pair. Fields of kind Secret (a PAT, an S3 secret) never go into the file:
// they're in Credential Manager as "pair.<id>.<key>".
//
//   [pair.3f9a01c2]
//   name=Documents
//   root=D:\Sync\Documents
//   type=azdo
//   paused=0
//   create=1            the first open may make a new vault (cleared once it opened)
//   exclude=Big stuff   one line per left-out folder
//   cfg.url=https://...
namespace syncconf {

struct Pair {
    std::string id;
    std::wstring name, root, type;
    sync::BackendConfig cfg;               // non-secret fields
    bool paused = false, create = false;
    std::vector<std::wstring> excluded;
};

std::wstring pairs_file();                 // "" without a settings folder
// `ok` false: the file is there but couldn't be read (then nothing must be saved over it)
std::vector<Pair> load(bool* ok = nullptr);
bool save(const std::vector<Pair>& pairs);
// load, change, save, with other processes' updates kept out (false: not read, or not written)
bool update(const std::function<void(std::vector<Pair>&)>& change);
std::string new_id();

// the backend config with the pair's secrets filled in from Credential Manager
sync::BackendConfig full_config(const Pair& p);
bool set_secret(const std::string& pairId, const std::wstring& key, const std::wstring& value);
void forget_secrets(const std::string& pairId);
// splits a typed config into what goes in the file and what goes into Credential Manager
void split_secrets(const std::wstring& type, sync::BackendConfig& cfg, sync::BackendConfig& secrets);

std::wstring state_dir(const std::string& pairId);    // Sync\pairs\<id> (created)
void remove_state_dir(const std::string& pairId);     // a removed pair's (its agent stopped first)

// the pair a local path is in (longest root), and the path relative to it ('/'-separated)
const Pair* pair_of(const std::vector<Pair>& pairs, const std::wstring& path, sync::RelPath* rel = nullptr);

// text helpers for the INI (exposed for the tests)
std::string serialize(const std::vector<Pair>& pairs);
std::vector<Pair> parse(const std::string& text);

} // namespace syncconf
