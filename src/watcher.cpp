#include "watcher.h"
#include "navigation.h"
#include "tree.h"
#include "enumerate.h"
#include "perf.h"
#include <algorithm>
#include <memory>
#include <thread>

static HWND s_main = nullptr;
static UINT s_changeMsg = 0;
static ULONG s_regId = 0;
static PITEMID_CHILD s_folder = nullptr;           // the watched folder
static PITEMID_CHILD s_parent = nullptr;           // its parent, also watched (local folders)
static bool s_listChanged = false;                 // a change the list has to show
// the list's changes item by item (full paths), unless a change needs a full re-listing
static std::vector<std::wstring> s_changedPaths;
static bool s_fullRefresh = false;
static const size_t kMaxItemChanges = 200;          // more: re-list the folder instead
static const UINT kBusyRetryMs = 300;               // the list is busy: apply the changes after this
static std::vector<PITEMID_CHILD> s_treeParents;   // tree nodes whose children changed

static const UINT kDebounceMs = 200;    // coalesce bursts (copying 1000 files = 1 refresh)
static const ULONGLONG kMaxWaitMs = 1000;   // ... but a steady stream (a download) still shows
static ULONGLONG s_firstPending = 0;        // tick of the first change the timer waits for (0: none)

// ------------------------------------------------------------------
// Direct directory watch (folders on fixed drives). For changes made outside the shell,
// the shell's notifications mostly come as one "folder updated" (SHCNE_UPDATEDIR) with no
// item: ReadDirectoryChangesW names each file, so the list can update just those.
// (Not on removable drives: the open handle would stop "Safely remove"; not on shares.)

struct DirWatch {
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE ev = nullptr;              // overlapped completion
    HANDLE stop = nullptr;            // set by the UI thread
    std::wstring path;
    UINT gen = 0;
    ~DirWatch() {
        if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
        if (ev) CloseHandle(ev);
        if (stop) CloseHandle(stop);
    }
};
static std::shared_ptr<DirWatch> s_dirWatch;
static UINT s_dirGen = 0;             // bumped when the watch goes: older results are dropped

struct DirChanges {
    UINT gen = 0;
    std::vector<std::wstring> paths;
    bool overflow = false;            // too many to name (or the folder went): re-list
    bool structure = false;           // a name came or went (the tree may care)
};

static void arm_refresh_timer();

static void dir_changes_apply(void* payload) {
    std::unique_ptr<DirChanges> c(static_cast<DirChanges*>(payload));
    if (c->gen != s_dirGen) return;
    s_listChanged = true;
    if (c->overflow) {
        s_fullRefresh = true;
        s_changedPaths.clear();
    } else if (!s_fullRefresh) {
        s_changedPaths.insert(s_changedPaths.end(), c->paths.begin(), c->paths.end());
        if (s_changedPaths.size() > kMaxItemChanges) { s_fullRefresh = true; s_changedPaths.clear(); }
    }
    if (c->structure && s_folder) s_treeParents.push_back(pidl_clone(s_folder));   // its subfolders
    if (perf_on()) perf_event("watch.dir", -1, (long long)c->paths.size(), c->overflow ? L"overflow" : nullptr);
    arm_refresh_timer();
}

static void dir_watch_thread(std::shared_ptr<DirWatch> w) {
    std::vector<DWORD> buf(16 * 1024);                  // 64 KB (DWORD-aligned)
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                         FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = w->ev;
        bool failed = !ReadDirectoryChangesW(w->dir, buf.data(), (DWORD)(buf.size() * sizeof(DWORD)), FALSE,
                                             filter, nullptr, &ov, nullptr);
        DWORD got = 0;
        if (!failed) {
            HANDLE hs[2] = { w->ev, w->stop };
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(w->dir, &ov);
                GetOverlappedResult(w->dir, &ov, &got, TRUE);   // (the buffer stays ours until then)
                return;
            }
            failed = !GetOverlappedResult(w->dir, &ov, &got, FALSE);
        }
        auto* c = new DirChanges;
        c->gen = w->gen;
        if (failed || got == 0) {
            c->overflow = true;          // buffer overflow; or the folder went: the refresh finds out
        } else {
            auto* p = reinterpret_cast<const BYTE*>(buf.data());
            for (;;) {
                auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                std::wstring name(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                c->paths.push_back(w->path + (w->path.back() == L'\\' ? L"" : L"\\") + name);
                if (fni->Action != FILE_ACTION_MODIFIED) c->structure = true;
                if (!fni->NextEntryOffset) break;
                p += fni->NextEntryOffset;
            }
        }
        post_async(dir_changes_apply, c);
        if (failed) return;
    }
}

static void stop_dir_watch() {
    ++s_dirGen;
    if (!s_dirWatch) return;
    SetEvent(s_dirWatch->stop);
    s_dirWatch.reset();                  // (the thread holds its own reference)
}

static void start_dir_watch(const std::wstring& path) {
    stop_dir_watch();
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return;
    WCHAR root[4] = { path[0], L':', L'\\', 0 };
    if (GetDriveTypeW(root) != DRIVE_FIXED) return;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;   // (a zip is a file)
    auto w = std::make_shared<DirWatch>();
    w->path = path;
    w->gen = s_dirGen;
    // share everything: the folder can still be renamed or deleted while watched
    w->dir = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    w->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (w->dir == INVALID_HANDLE_VALUE || !w->ev || !w->stop) return;
    try {
        std::thread(dir_watch_thread, w).detach();
    } catch (...) {
        return;                          // (the shell's notifications still come)
    }
    s_dirWatch = w;
}

void watcher_init(HWND main) {
    s_main = main;
    s_changeMsg = RegisterWindowMessageW(L"ExplorerPlus.ChangeNotify");
}

UINT watcher_change_msg() { return s_changeMsg; }

void deregister_current_watch() {
    stop_dir_watch();
    if (s_regId) { SHChangeNotifyDeregister(s_regId); s_regId = 0; }
    if (s_folder) { CoTaskMemFree(s_folder); s_folder = nullptr; }
    if (s_parent) { CoTaskMemFree(s_parent); s_parent = nullptr; }
    // changes of the folder left behind no longer matter
    s_listChanged = false;
    s_fullRefresh = false;
    s_changedPaths.clear();
}

void watcher_register_for(PCIDLIST_ABSOLUTE folder) {
    if (!s_main) return;
    // a refresh keeps the watch: re-registering would lose the changes in between
    if (s_regId && s_folder && (folder ? pidl_equal(s_folder, folder) : pidl_is_desktop(s_folder)))
        return;
    deregister_current_watch();
    PITEMID_CHILD root = folder ? nullptr : pidl_desktop();
    s_folder = pidl_clone(folder ? folder : root);
    SHChangeNotifyEntry ent[2]{};
    ent[0].pidl = s_folder;
    ent[0].fRecursive = FALSE;
    // a watch on a folder is not told when the folder itself goes: the parent's is
    // (local folders only: a share's parent can be slow to watch)
    WCHAR path[MAX_PATH];
    if (folder && SHGetPathFromIDListW(folder, path) && iswalpha(path[0]) && path[1] == L':' &&
        !PathIsRootW(path) && (s_parent = pidl_clone(folder)) && ILRemoveLastID(s_parent)) {
        ent[1].pidl = s_parent;
        ent[1].fRecursive = FALSE;
    } else if (s_parent) {
        CoTaskMemFree(s_parent);
        s_parent = nullptr;
    }
    // InterruptLevel catches changes made outside the shell (cmd, other apps);
    // NewDelivery hands us a lock handle instead of raw pointers.
    s_regId = SHChangeNotifyRegister(
        s_main,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_ALLEVENTS,
        s_changeMsg,
        s_parent ? 2 : 1, ent);
    if (root) CoTaskMemFree(root);
    if (folder && SHGetPathFromIDListW(folder, path)) start_dir_watch(path);
}

// does the change touch the shown folder (its items, or the folder itself)? The
// parent's watch also reports the folder's siblings: those only matter to the tree.
static bool touches_folder(PCIDLIST_ABSOLUTE item) {
    if (!s_parent || !item) return true;
    if (pidl_equal(item, s_folder)) return true;
    PITEMID_CHILD up = pidl_clone(item);
    bool inside = up && ILRemoveLastID(up) && pidl_equal(up, s_folder);
    if (up) CoTaskMemFree(up);
    return inside;
}

// Item events (made, deleted, renamed, changed) are kept by path: the list reads just
// those items again. Anything else — the folder itself, a whole-folder update, no item,
// a flood of changes — re-lists the folder.
static void note_change(LONG event, PIDLIST_ABSOLUTE* pidls) {
    if (s_fullRefresh) return;
    const LONG itemEvents = SHCNE_CREATE | SHCNE_MKDIR | SHCNE_DELETE | SHCNE_RMDIR | SHCNE_RENAMEITEM |
                            SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM | SHCNE_ATTRIBUTES;
    bool itemized = pidls && pidls[0] && (event & itemEvents) && !(event & ~(itemEvents | SHCNE_INTERRUPT)) &&
                    !pidl_equal(pidls[0], s_folder);
    WCHAR path[MAX_PATH];
    for (int i = 0; itemized && i < 2; ++i) {
        if (i == 1 && !(event & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER))) break;
        if (!pidls[i] || !SHGetPathFromIDListW(pidls[i], path)) itemized = false;   // (not a file)
        else s_changedPaths.push_back(path);
    }
    if (!itemized || s_changedPaths.size() > kMaxItemChanges) {
        s_fullRefresh = true;
        s_changedPaths.clear();
    }
    if (perf_on()) {
        WCHAR d[MAX_PATH + 40] = L"";
        if (pidls && pidls[0] && SHGetPathFromIDListW(pidls[0], path)) swprintf_s(d, L"%ls", path);
        perf_event(itemized ? "watch.item" : "watch.full", -1, (long long)event, d);
    }
}

void watcher_on_message(WPARAM wp, LPARAM lp) {
    LONG event = 0;
    PIDLIST_ABSOLUTE* pidls = nullptr;
    HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &pidls, &event);
    if (!lock) return;
    // system-wide events carry numbers, not items: an icon cache update or an extended
    // event says nothing about this folder; new file associations change type names
    if (event & (SHCNE_UPDATEIMAGE | SHCNE_EXTENDED_EVENT)) { SHChangeNotification_Unlock(lock); return; }
    if (event & SHCNE_ASSOCCHANGED) pidls = nullptr;   // (refresh, no item)
    // folder-level changes also touch the tree: remember the parent node to re-list
    if ((event & (SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEFOLDER | SHCNE_DRIVEADD |
                  SHCNE_DRIVEREMOVED | SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED)) && pidls && pidls[0]) {
        PITEMID_CHILD parent = pidl_clone(pidls[0]);
        if (parent && ILRemoveLastID(parent)) s_treeParents.push_back(parent);
        else if (parent) CoTaskMemFree(parent);
    }
    // the directory watch names the files a file-system "folder updated" is about (and
    // the folder's own date / attributes changing with them): not a reason to re-list.
    // (Shell-level updates stay: an app asking for a refresh.)
    if (s_dirWatch && (event & SHCNE_INTERRUPT) && (event & (SHCNE_UPDATEDIR | SHCNE_UPDATEITEM | SHCNE_ATTRIBUTES)) &&
        pidls && pidls[0] && pidl_equal(pidls[0], s_folder)) {
        if (perf_on()) perf_event("watch.covered", -1, (long long)event);
        SHChangeNotification_Unlock(lock);
        return;
    }
    // (a rename can move an item in or out: either name counts)
    bool rename = (event & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER)) != 0;
    if (!pidls || !pidls[0] || touches_folder(pidls[0]) || (rename && pidls[1] && touches_folder(pidls[1]))) {
        s_listChanged = true;
        note_change(event, pidls);
    }
    SHChangeNotification_Unlock(lock);
    arm_refresh_timer();
}

// debounce: each change pushes the refresh back, up to kMaxWaitMs after the first
static void arm_refresh_timer() {
    ULONGLONG now = GetTickCount64();
    if (!s_firstPending) s_firstPending = now;
    ULONGLONG waited = now - s_firstPending;
    if (waited >= kMaxWaitMs) return;                // due any moment: let the timer fire
    UINT delay = (UINT)std::min<ULONGLONG>(kDebounceMs, kMaxWaitMs - waited);
    SetTimer(s_main, TIMER_WATCH_REFRESH, delay, nullptr);
}

void watcher_on_timer() {
    KillTimer(s_main, TIMER_WATCH_REFRESH);
    s_firstPending = 0;
    if (s_listChanged && !navigation_leave_if_gone()) {
        ChangeApply r = s_fullRefresh ? ChangeApply::Unsupported : enum_apply_changes(s_changedPaths);
        if (r == ChangeApply::Busy) {
            SetTimer(s_main, TIMER_WATCH_REFRESH, kBusyRetryMs, nullptr);   // (keeps the changes)
        } else {
            if (r == ChangeApply::Unsupported) navigation_refresh();
            s_listChanged = false;
            s_fullRefresh = false;
            s_changedPaths.clear();
        }
    } else {
        s_listChanged = false;
        s_fullRefresh = false;
        s_changedPaths.clear();
    }
    std::vector<PITEMID_CHILD> parents;
    parents.swap(s_treeParents);
    for (size_t i = 0; i < parents.size(); ++i) {
        bool dup = false;
        for (size_t j = 0; j < i && !dup; ++j) dup = pidl_equal(parents[i], parents[j]);
        if (!dup) tree_refresh_node(parents[i]);
    }
    for (auto p : parents) CoTaskMemFree(p);
}
