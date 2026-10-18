#pragma once
#include "sync_backend.h"
#include <map>
#include <mutex>
#include <unordered_map>

// A pair's state on this machine (docs/sync-plan.md, "State"): the base (what both sides
// agreed on at the last sync), the remote view (the store's tree as last listed, so a
// change feed's deltas apply exactly) and a few values (cursor, token, root identity).
// In memory as hash maps; on disk a snapshot (base.dat) plus an append-only log
// (base.log) of checksummed records: a torn tail is dropped, compaction writes a new
// snapshot beside it and swaps it in. Local only: it holds plaintext paths.
namespace sync {

using Hash = std::array<uint8_t, 32>;

struct BaseEntry {
    RelPath  path;                 // as on disk (case preserved)
    bool     isDir = false;
    uint64_t size = 0, mtime = 0, fileId = 0;   // the local file as synced
    Hash     hash{};               // BLAKE2b of its content
    std::string rev;               // the stored version it matches
    uint64_t version = 0;          // the version counter it carries
};

struct RemoteItem {
    RelPath     path;
    bool        isDir = false;
    uint64_t    size = 0;
    std::string rev;
};

class StateStore {
public:
    explicit StateStore(std::wstring dir);       // "" : memory only (tests)
    ~StateStore();
    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;
    // false: damaged (what could be read is loaded), or unreadable() (in use, a disk error:
    // nothing loaded, and the store mustn't be used, as it would start over as if empty)
    bool load();
    bool unreadable() const { return m_unreadable; }
    const std::wstring& dir() const { return m_dir; }

    // all keys are syncpath::case_key(path)
    const BaseEntry* base(const std::wstring& key) const;
    void put_base(const BaseEntry& e);
    void erase_base(const std::wstring& key);
    const std::unordered_map<std::wstring, BaseEntry>& base_all() const { return m_base; }

    const RemoteItem* remote(const std::wstring& key) const;
    void put_remote(const RemoteItem& e);
    void erase_remote(const std::wstring& key);
    void clear_remote();
    const std::unordered_map<std::wstring, RemoteItem>& remote_all() const { return m_remote; }

    std::string value(const std::string& name) const;
    void set_value(const std::string& name, const std::string& v);

    void flush();                                // to disk (FlushFileBuffers)
    void compact();
    void reset();                                // forget everything (pair removed / re-linked)
    std::recursive_mutex& mutex() { return m_mx; }   // held by callers that iterate while others write

private:
    void append(uint8_t type, const std::string& payload);
    bool apply(uint8_t type, const std::string& payload);
    std::string snapshot() const;
    std::wstring m_dir;
    std::unordered_map<std::wstring, BaseEntry> m_base;
    std::unordered_map<std::wstring, RemoteItem> m_remote;
    std::map<std::string, std::string> m_values;
    HANDLE m_log = INVALID_HANDLE_VALUE;
    uint64_t m_logBytes = 0, m_snapBytes = 0;
    bool m_unreadable = false;
    mutable std::recursive_mutex m_mx;
};

std::wstring key_of(const RelPath& p);           // syncpath::case_key

} // namespace sync
