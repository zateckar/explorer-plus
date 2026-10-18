#pragma once
#include "sync_state.h"
#include <unordered_map>

// The three-way reconciliation (docs/sync-plan.md, "Planner"): per path, the local file and
// the stored one compared with the base. Pure: the engine hands it three maps (keyed by
// key_of(path)) and executes what comes back. Unit-tested.
namespace sync {

struct LocalItem {
    RelPath  path;
    bool     isDir = false;
    uint64_t size = 0, mtime = 0, fileId = 0;
    uint32_t attrs = 0;
    Hash     hash{};
    bool     hashed = false;       // hash is valid (files that differ from the base get hashed)
    bool     busy = false;         // still being written / couldn't be read: leave it alone this pass
    bool     racy = false;         // the base's size and time were taken within a clock tick of a write: the hash decides
};

enum class Act {
    Upload,          // path; expectedRev ("" = must be new)
    Download,        // path
    Conflict,        // both changed (or both new): same content -> adopt, else keep both
    DeleteRemote,    // path; expectedRev
    DeleteLocal,     // path (to the Recycle Bin; only if still as in the base)
    MoveRemote,      // from -> path; expectedRev of `from`
    MoveLocal,       // from -> path (the store renamed it)
    MkdirLocal, MkdirRemote,
    RmdirLocal, RmdirRemote,       // only when empty
    Adopt,           // record the base from the local and remote sides as they are (same content)
    Refresh,         // local metadata changed, content didn't: base gets the new size / time / id
    Forget,          // gone on both sides
    TypeClash,       // a file on one side, a folder on the other: the local one is renamed aside
};

struct Action {
    Act act;
    RelPath path;
    RelPath from;
    std::string expectedRev;
};

struct Plan {
    std::vector<Action> actions;
    size_t files = 0;                        // files in the base (for the mass-delete guard)
    size_t localDeletes = 0, remoteDeletes = 0;
    bool deferred = false;                   // something busy was left for the next pass
};

using LocalMap  = std::unordered_map<std::wstring, LocalItem>;
using RemoteMap = std::unordered_map<std::wstring, RemoteItem>;
using BaseMap   = std::unordered_map<std::wstring, BaseEntry>;

// remote: files and the store's folders; folders that only exist through their files are
// added by add_implied_dirs first
Plan make_plan(const BaseMap& base, const LocalMap& local, const RemoteMap& remote, const Caps& caps);
void add_implied_dirs(RemoteMap& remote);
// execution order: local side first (shallow folders, moves, downloads, deletes, deep
// folders), then the store's side likewise
void order_actions(std::vector<Action>& a);
const char* act_name(Act a);

} // namespace sync
