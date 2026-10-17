#pragma once
#include "sync_local.h"
#include "sync_plan.h"
#include "sync_state.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>

// One pair's sync engine (docs/sync-plan.md, "Executor"): a pass lists both sides, plans
// and executes; passes repeat until nothing is left to do. The engine sees plaintext only
// (the backend it gets is already the encrypting one) and decides everything itself.
namespace sync {

struct EngineOptions {
    std::wstring root;                  // the local folder
    std::wstring deviceName;            // in conflict copies' names
    std::wstring trashDir;              // non-empty: local deletes go there instead of the Recycle Bin
    std::wstring ignoreFile = L".syncignore";
    std::vector<RelPath> excluded;      // selective sync
    int settleMs = 2000;                // files changed more recently wait
    size_t massDeleteFiles = 50;        // a pass deleting more than this many files...
    double massDeleteShare = 0.25;      // ...and this share of the pair waits for confirmation
    int maxPasses = 5;                  // repeats of a pass that left work behind
};

enum class PairState { Idle, Syncing, UpToDate, Paused, Offline, Error, Locked, AuthNeeded, RootMissing, ConfirmDeletes };
const wchar_t* pair_state_name(PairState s);

// per-item state for the badges
enum class ItemState { None, Synced, Syncing, Pending, Conflict, Error, Excluded };
const wchar_t* item_state_name(ItemState s);

struct EngineStatus {
    PairState state = PairState::Idle;
    std::wstring message;               // the last error / why it waits
    uint64_t lastSync = 0;              // FILETIME of the last complete pass
    size_t pendingFiles = 0;
    uint64_t pendingBytes = 0;
    size_t pendingDeletes = 0;          // ConfirmDeletes: how many
    std::vector<RelPath> conflicts;     // conflict copies made (until dismissed)
    std::vector<std::pair<RelPath, std::wstring>> problems;   // items that couldn't sync
};

struct PassReport {
    Result result;
    size_t uploads = 0, downloads = 0, localDeletes = 0, remoteDeletes = 0, moves = 0, conflicts = 0, dirs = 0;
    bool again = false;                 // another pass is needed (conflict copies, busy files...)
    std::vector<std::string> log;       // one line per action (tests, diagnostics)
};

class Engine {
public:
    Engine(EngineOptions opt, std::unique_ptr<Backend> backend, StateStore& state);
    ~Engine();
    // passes until stable (or maxPasses); the last pass's report, counts summed
    PassReport sync(Progress& prog);
    PassReport pass(Progress& prog);
    EngineStatus status() const;
    ItemState item_state(const RelPath& rel, bool isDir) const;
    // children of a folder with their states (the file list's badges)
    std::vector<std::pair<std::wstring, ItemState>> children(const RelPath& folder) const;
    void confirm_deletes(bool deleteThem);      // ConfirmDeletes: go ahead, or restore (download / upload back)
    void dismiss_conflicts();
    Backend& backend() { return *m_backend; }
    const EngineOptions& options() const { return m_opt; }
    void set_paused(bool p);
    bool paused() const { return m_paused; }

    // Version history (stores that keep it: git). Restoring writes the old version over the
    // local file (the current one goes to the Recycle Bin) with the time now, so the next
    // pass uploads it as the newest version. Not during a pass (the agent serializes).
    Result versions(const RelPath& rel, std::vector<RemoteVersion>& out, Progress& prog);
    Result restore_version(const RelPath& rel, const std::string& rev, Progress& prog);
    // "Re-encrypt everything" (after removing a device): every synced file uploaded again,
    // so its content is under the vault's current key. Files changed locally since the last
    // pass are left to it. Names keep the key version they were made with.
    struct Reencrypted { size_t files = 0, skipped = 0; };
    Result reencrypt(Progress& prog, Reencrypted& out);

private:
    Result reencrypt_once(Progress& prog, Reencrypted& out);
    struct Ctx;
    Result list_remote(Ctx& c);
    void hash_changed(Ctx& c, LocalMap& local);
    Result run_local_phase(Ctx& c, std::vector<Action>& acts);
    Result run_remote_phase(Ctx& c, std::vector<Action>& acts);
    Result do_download(Ctx& c, const RelPath& path, const RelPath& saveAs, bool record);
    Result do_upload(Ctx& c, const Action& a);
    Result do_conflict(Ctx& c, const Action& a);
    void record_local(Ctx& c, const RelPath& path, const RemoteItem* r, const Hash* hash, uint64_t version);
    void remember_parents(const RelPath& path);
    void set_state(PairState s, const std::wstring& msg = {});
    std::wstring full(const RelPath& rel) const { return syncpath::to_local(m_opt.root, rel); }
    void problem(const RelPath& p, const std::wstring& why);

    EngineOptions m_opt;
    std::unique_ptr<Backend> m_backend;
    StateStore& m_state;
    syncpath::Ignore m_ignore;
    mutable std::mutex m_mx;            // guards the status below
    EngineStatus m_status;
    std::set<std::wstring> m_active;    // keys being transferred now
    std::set<std::wstring> m_pending;   // keys with work planned
    std::map<std::wstring, std::wstring> m_errors;   // key -> why
    std::atomic<bool> m_paused{ false };
    int m_confirm = 0;                  // 1: delete confirmed, -1: restore
};

} // namespace sync
