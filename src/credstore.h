#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Where the sync feature keeps its things on this machine.
//
// sync_home(): the "Sync" folder beside the settings file (settings.ini or
// EXPLORERPLUS_SETTINGS'), as the Player and the Notepad keep theirs: tests that point
// EXPLORERPLUS_SETTINGS elsewhere get a sync home of their own.
//
// credstore: secrets (the key ring, PATs, S3 keys) in Windows Credential Manager
// (DPAPI-protected under the Windows account, CRED_PERSIST_LOCAL_MACHINE: never roaming),
// named "ExplorerPlus:<settings hash>:<name>", so a test's secrets never meet the user's.
std::wstring sync_settings_file();       // "" when there is none (EXPLORERPLUS_SETTINGS set but empty)
std::wstring sync_home();                // created on first use; "" when there are no settings
std::wstring sync_namespace();           // 8 hex digits: a hash of the settings file's folder

namespace credstore {
constexpr size_t kMaxBlob = 2560;        // CRED_MAX_CREDENTIAL_BLOB_SIZE
bool put(const std::wstring& name, const std::vector<uint8_t>& blob);
bool get(const std::wstring& name, std::vector<uint8_t>& blob);
bool put_text(const std::wstring& name, const std::wstring& text);
std::wstring get_text(const std::wstring& name);
bool erase(const std::wstring& name);
int  erase_prefix(const std::wstring& namePrefix);   // every entry of this namespace starting so; count
} // namespace credstore
