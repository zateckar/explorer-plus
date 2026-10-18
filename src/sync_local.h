#pragma once
#include "sync_plan.h"
#include "sync_paths.h"
#include <atomic>
#include <functional>
#include <thread>

// The local side of a pair (docs/sync-plan.md, "Local side"): a scan of the root that
// reads file IDs with the listing (GetFileInformationByHandleEx, FileIdBothDirectoryInfo),
// hashing, the Recycle Bin, and a recursive ReadDirectoryChangesW watch.
namespace sync {

struct ScanResult {
    LocalMap items;
    bool rootMissing = false;
    std::vector<std::pair<RelPath, std::wstring>> problems;   // unreadable folders...
    // what the scan couldn't see: folders it couldn't open or list completely, links (a
    // folder replaced by a junction). Neither side is touched at or under these: absent from
    // the scan isn't deleted.
    std::vector<RelPath> unknown;
    bool rootIncomplete = false;        // the root itself couldn't be listed completely
};
// symlinks and junctions are not followed (nor listed); cloud placeholders are real files
ScanResult scan_local(const std::wstring& root, const syncpath::Ignore& ignore);

// root identity: volume serial + the root folder's file ID ("" when it isn't there)
std::string root_identity(const std::wstring& root);

// BLAKE2b of a file's content, read through `h` from the start (false: read failed)
bool hash_handle(HANDLE h, Hash& out, uint64_t* size = nullptr);
// open for reading, other writers kept out (FILE_SHARE_READ | FILE_SHARE_DELETE)
HANDLE open_for_read(const std::wstring& full);
// size, mtime, file ID of an open file
bool file_identity(HANDLE h, uint64_t& size, uint64_t& mtime, uint64_t& fileId, uint32_t* attrs = nullptr);

// to the Recycle Bin (trashDir non-empty: moved there instead, under a time-stamped folder)
bool recycle(const std::wstring& full, const std::wstring& trashDir, const RelPath& rel);
bool make_dirs(const std::wstring& full);                     // with the parents

// Recursive change watch of a root: `changed` runs (on the watch thread) after a burst of
// changes settles, or when the buffer overflowed. Paths under ignoredPrefix (the temp
// folder) don't count.
class DirWatch {
public:
    DirWatch() = default;
    ~DirWatch() { stop(); }
    bool start(const std::wstring& root, std::function<void()> changed);
    void stop();
private:
    void run();
    std::wstring m_root;
    std::function<void()> m_changed;
    HANDLE m_dir = INVALID_HANDLE_VALUE, m_stop = nullptr;
    std::thread m_thread;
};

uint64_t ft64(const FILETIME& f);
FILETIME ft_of(uint64_t v);
uint64_t now_ft();

} // namespace sync
