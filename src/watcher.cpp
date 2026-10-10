#include "watcher.h"
#include "navigation.h"
#include "tree.h"

static HWND s_main = nullptr;
static UINT s_changeMsg = 0;
static ULONG s_regId = 0;
static std::vector<PITEMID_CHILD> s_treeParents;   // tree nodes whose children changed

static const UINT kDebounceMs = 200;    // coalesce bursts (copying 1000 files = 1 refresh)

void watcher_init(HWND main) {
    s_main = main;
    s_changeMsg = RegisterWindowMessageW(L"ExplorerPlus.ChangeNotify");
}

UINT watcher_change_msg() { return s_changeMsg; }

void deregister_current_watch() {
    if (s_regId) { SHChangeNotifyDeregister(s_regId); s_regId = 0; }
}

void watcher_register_for(PCIDLIST_ABSOLUTE folder) {
    if (!s_main) return;
    deregister_current_watch();
    PITEMID_CHILD root = folder ? nullptr : pidl_desktop();
    SHChangeNotifyEntry ent{};
    ent.pidl = folder ? folder : root;
    ent.fRecursive = FALSE;
    // InterruptLevel catches changes made outside the shell (cmd, other apps);
    // NewDelivery hands us a lock handle instead of raw pointers.
    s_regId = SHChangeNotifyRegister(
        s_main,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_ALLEVENTS,
        s_changeMsg,
        1, &ent);
    if (root) CoTaskMemFree(root);
}

void watcher_on_message(WPARAM wp, LPARAM lp) {
    LONG event = 0;
    PIDLIST_ABSOLUTE* pidls = nullptr;
    HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &pidls, &event);
    if (!lock) return;
    // folder-level changes also touch the tree: remember the parent node to re-list
    if ((event & (SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEFOLDER | SHCNE_DRIVEADD |
                  SHCNE_DRIVEREMOVED | SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED)) && pidls && pidls[0]) {
        PITEMID_CHILD parent = pidl_clone(pidls[0]);
        if (parent && ILRemoveLastID(parent)) s_treeParents.push_back(parent);
        else if (parent) CoTaskMemFree(parent);
    }
    SHChangeNotification_Unlock(lock);
    SetTimer(s_main, TIMER_WATCH_REFRESH, kDebounceMs, nullptr);
}

void watcher_on_timer() {
    KillTimer(s_main, TIMER_WATCH_REFRESH);
    navigation_refresh();
    std::vector<PITEMID_CHILD> parents;
    parents.swap(s_treeParents);
    for (size_t i = 0; i < parents.size(); ++i) {
        bool dup = false;
        for (size_t j = 0; j < i && !dup; ++j) dup = pidl_equal(parents[i], parents[j]);
        if (!dup) tree_refresh_node(parents[i]);
    }
    for (auto p : parents) CoTaskMemFree(p);
}
