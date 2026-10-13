#include "watcher.h"
#include "navigation.h"
#include "tree.h"
#include "enumerate.h"
#include "foldersizes.h"
#include "perf.h"
#include <algorithm>
#include <memory>
#include <thread>
#include <unordered_set>

static HWND s_main = nullptr;
static const size_t kMaxItemChanges = 200;          // more files: re-list the folder instead

struct DirWatch;

// each pane watches its own folder (UI thread)
struct WatchState {
    UINT changeMsg = 0;                             // registered message the shell posts to
    ULONG regId = 0;
    PIDLIST_ABSOLUTE folder = nullptr;              // the watched folder
    PIDLIST_ABSOLUTE parent = nullptr;              // its parent, also watched (local folders)
    bool listChanged = false;                       // a change the list has to show
    // the list's changes item by item (full paths), unless a change needs a full re-listing
    std::vector<std::wstring> changedPaths;
    std::unordered_set<std::wstring> changedSet;    // the same, lowercased: each path once
    bool fullRefresh = false;
    ULONGLONG firstPending = 0;                     // tick of the first change the timer waits for (0: none)
    std::shared_ptr<DirWatch> dirWatch;
    UINT dirGen = 0;                                // bumped when the watch goes: older results are dropped
};
static WatchState s_watch[kPanes];
static WatchState& ws() { return s_watch[g_pane->index]; }

// a file changed (a download growing reports itself many times: it counts once);
// too many different files make it a full re-listing
static void add_changed(const std::wstring& path) {
    if (ws().fullRefresh) return;
    std::wstring k = path;
    CharLowerBuffW(k.data(), (DWORD)k.size());
    if (!ws().changedSet.insert(std::move(k)).second) return;
    ws().changedPaths.push_back(path);
    if (ws().changedPaths.size() > kMaxItemChanges) {
        ws().fullRefresh = true;
        ws().changedPaths.clear();
        ws().changedSet.clear();
    }
}

static void clear_changed() {
    ws().changedPaths.clear();
    ws().changedSet.clear();
}
static const UINT kBusyRetryMs = 300;               // the list is busy: apply the changes after this
static std::vector<PIDLIST_ABSOLUTE> s_treeParents;   // tree nodes whose children changed

static const UINT kDebounceMs = 200;    // coalesce bursts (copying 1000 files = 1 refresh)
static const ULONGLONG kMaxWaitMs = 1000;   // ... but a steady stream (a download) still shows
static int s_hold = 0;                      // our own file operations running (watcher_hold_begin)

// ------------------------------------------------------------------
// Direct directory watch (folders on fixed drives). For changes made outside the shell,
// the shell's notifications mostly come as one "folder updated" (SHCNE_UPDATEDIR) with no
// item: ReadDirectoryChangesW names each file, so the list can update just those.
// (Not on removable or USB disks: the open handle would stop "Safely remove"; not on shares.)

struct DirWatch {
    Pane* pane = nullptr;
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

struct DirChanges {
    Pane* pane = nullptr;
    UINT gen = 0;
    std::vector<std::wstring> paths;
    bool overflow = false;            // too many to name (or the folder went): re-list
    bool structure = false;           // a name came or went (the tree may care)
    bool ended = false;               // the watch failed and its thread is gone
};

static void arm_refresh_timer();
static void stop_dir_watch();

static void dir_changes_apply(void* payload) {
    std::unique_ptr<DirChanges> c(static_cast<DirChanges*>(payload));
    PaneScope ps(c->pane);
    if (c->gen != ws().dirGen) return;
    // a dead watch lets go of the folder (its handle would keep a deleted folder
    // "delete pending"); the refresh below opens a new one if the folder is still there
    if (c->ended) stop_dir_watch();
    ws().listChanged = true;
    if (c->overflow) {
        ws().fullRefresh = true;
        clear_changed();
    } else {
        for (auto& p : c->paths) add_changed(p);
    }
    if (c->structure && ws().folder) s_treeParents.push_back(pidl_clone(ws().folder));   // its subfolders
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
        c->pane = w->pane;
        c->gen = w->gen;
        c->ended = failed;
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
    ++ws().dirGen;
    if (!ws().dirWatch) return;
    SetEvent(ws().dirWatch->stop);
    ws().dirWatch.reset();                  // (the thread holds its own reference)
}

static void start_dir_watch(const std::wstring& path) {
    stop_dir_watch();
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return;
    if (!path_on_internal_drive(path)) return;     // (a USB disk reports itself as fixed)
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;   // (a zip is a file)
    auto w = std::make_shared<DirWatch>();
    w->pane = g_pane;
    w->path = path;
    w->gen = ws().dirGen;
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
    ws().dirWatch = w;
}

void watcher_init(HWND main) {
    s_main = main;
    for (int i = 0; i < kPanes; ++i)
        s_watch[i].changeMsg = RegisterWindowMessageW((L"ExplorerPlus.ChangeNotify" + std::to_wstring(i)).c_str());
}

int watcher_pane_of_msg(UINT msg) {
    for (int i = 0; i < kPanes; ++i)
        if (msg && msg == s_watch[i].changeMsg) return i;
    return -1;
}

void deregister_current_watch() {
    stop_dir_watch();
    if (ws().regId) { SHChangeNotifyDeregister(ws().regId); ws().regId = 0; }
    if (ws().folder) { CoTaskMemFree(ws().folder); ws().folder = nullptr; }
    if (ws().parent) { CoTaskMemFree(ws().parent); ws().parent = nullptr; }
    // changes of the folder left behind no longer matter
    ws().listChanged = false;
    ws().fullRefresh = false;
    clear_changed();
}

void watcher_register_for(PCIDLIST_ABSOLUTE folder) {
    if (!s_main) return;
    // a refresh keeps the watch: re-registering would lose the changes in between
    // (the directory watch may have been released for a file operation: open it again)
    if (ws().regId && ws().folder && (folder ? pidl_equal(ws().folder, folder) : pidl_is_desktop(ws().folder))) {
        WCHAR path[MAX_PATH];
        if (!ws().dirWatch && !s_hold && folder && SHGetPathFromIDListW(folder, path)) start_dir_watch(path);
        return;
    }
    deregister_current_watch();
    PIDLIST_ABSOLUTE root = folder ? nullptr : pidl_desktop();
    ws().folder = pidl_clone(folder ? folder : root);
    SHChangeNotifyEntry ent[2]{};
    ent[0].pidl = ws().folder;
    ent[0].fRecursive = FALSE;
    // a watch on a folder is not told when the folder itself goes: the parent's is
    // (local folders only: a share's parent can be slow to watch)
    WCHAR path[MAX_PATH];
    if (folder && SHGetPathFromIDListW(folder, path) && iswalpha(path[0]) && path[1] == L':' &&
        !PathIsRootW(path) && (ws().parent = pidl_clone(folder)) && ILRemoveLastID(ws().parent)) {
        ent[1].pidl = ws().parent;
        ent[1].fRecursive = FALSE;
    } else if (ws().parent) {
        CoTaskMemFree(ws().parent);
        ws().parent = nullptr;
    }
    // InterruptLevel catches changes made outside the shell (cmd, other apps);
    // NewDelivery hands us a lock handle instead of raw pointers.
    ws().regId = SHChangeNotifyRegister(
        s_main,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_ALLEVENTS,
        ws().changeMsg,
        ws().parent ? 2 : 1, ent);
    if (root) CoTaskMemFree(root);
    if (folder && SHGetPathFromIDListW(folder, path)) start_dir_watch(path);
}

// does the change touch the shown folder (its items, or the folder itself)? The
// parent's watch also reports the folder's siblings: those only matter to the tree.
static bool touches_folder(PCIDLIST_ABSOLUTE item) {
    if (!ws().parent || !item) return true;
    if (pidl_equal(item, ws().folder)) return true;
    PIDLIST_ABSOLUTE up = pidl_clone(item);
    bool inside = up && ILRemoveLastID(up) && pidl_equal(up, ws().folder);
    if (up) CoTaskMemFree(up);
    return inside;
}

// Item events (made, deleted, renamed, changed) are kept by path: the list reads just
// those items again. Anything else — the folder itself, a whole-folder update, no item,
// a flood of changes — re-lists the folder.
static void note_change(LONG event, PIDLIST_ABSOLUTE* pidls) {
    if (ws().fullRefresh) return;
    const LONG itemEvents = SHCNE_CREATE | SHCNE_MKDIR | SHCNE_DELETE | SHCNE_RMDIR | SHCNE_RENAMEITEM |
                            SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM | SHCNE_ATTRIBUTES;
    bool itemized = pidls && pidls[0] && (event & itemEvents) && !(event & ~(itemEvents | SHCNE_INTERRUPT)) &&
                    !pidl_equal(pidls[0], ws().folder);
    WCHAR path[MAX_PATH];
    for (int i = 0; itemized && i < 2; ++i) {
        if (i == 1 && !(event & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER))) break;
        if (!pidls[i] || !SHGetPathFromIDListW(pidls[i], path)) itemized = false;   // (not a file)
        else add_changed(path);
    }
    if (!itemized) {
        ws().fullRefresh = true;
        clear_changed();
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
        PIDLIST_ABSOLUTE parent = pidl_clone(pidls[0]);
        if (parent && ILRemoveLastID(parent)) s_treeParents.push_back(parent);
        else if (parent) CoTaskMemFree(parent);
    }
    // the directory watch names the files a file-system "folder updated" is about (and
    // the folder's own date / attributes changing with them): not a reason to re-list.
    // (Shell-level updates stay: an app asking for a refresh.)
    if (ws().dirWatch && (event & SHCNE_INTERRUPT) && (event & (SHCNE_UPDATEDIR | SHCNE_UPDATEITEM | SHCNE_ATTRIBUTES)) &&
        pidls && pidls[0] && pidl_equal(pidls[0], ws().folder)) {
        if (perf_on()) perf_event("watch.covered", -1, (long long)event);
        SHChangeNotification_Unlock(lock);
        return;
    }
    // a whole-folder update may stand for subfolders made or removed: the shell sends one
    // instead of RMDIR when a big folder is deleted (its many events collapsed into one)
    if ((event & SHCNE_UPDATEDIR) && pidls && pidls[0])
        if (PIDLIST_ABSOLUTE f = pidl_clone(pidls[0])) s_treeParents.push_back(f);
    // (a rename can move an item in or out: either name counts)
    bool rename = (event & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER)) != 0;
    if (!pidls || !pidls[0] || touches_folder(pidls[0]) || (rename && pidls[1] && touches_folder(pidls[1]))) {
        ws().listChanged = true;
        note_change(event, pidls);
    }
    SHChangeNotification_Unlock(lock);
    arm_refresh_timer();
}

// debounce: each change pushes the refresh back, up to kMaxWaitMs after the first
static void arm_refresh_timer() {
    ULONGLONG now = GetTickCount64();
    if (!ws().firstPending) ws().firstPending = now;
    ULONGLONG waited = now - ws().firstPending;
    if (waited >= kMaxWaitMs) return;                // due any moment: let the timer fire
    UINT delay = (UINT)std::min<ULONGLONG>(kDebounceMs, kMaxWaitMs - waited);
    SetTimer(s_main, pane_timer(TIMER_WATCH_REFRESH), delay, nullptr);
}

void watcher_hold_begin(bool releaseDir) {
    ++s_hold;
    if (releaseDir) {
        for_each_pane([] {
            stop_dir_watch();
            foldersizes_release_watch();     // (watching again once the folder is listed again)
        });
    }
}

void watcher_hold_end() {
    if (s_hold > 0) --s_hold;
    if (s_hold) return;
    for_each_pane([] {                       // what was held (the tree's changes go with the first pane)
        if (ws().listChanged || (g_pane->index == 0 && !s_treeParents.empty())) arm_refresh_timer();
    });
}

void watcher_on_timer() {
    KillTimer(s_main, pane_timer(TIMER_WATCH_REFRESH));
    ws().firstPending = 0;
    if (s_hold) return;                     // (watcher_hold_end arms the timer again)
    if (ws().listChanged && !navigation_leave_if_gone()) {
        ChangeApply r = ws().fullRefresh ? ChangeApply::Unsupported : enum_apply_changes(ws().changedPaths);
        if (r == ChangeApply::Busy) {
            SetTimer(s_main, pane_timer(TIMER_WATCH_REFRESH), kBusyRetryMs, nullptr);   // (keeps the changes)
        } else {
            if (r == ChangeApply::Unsupported) navigation_refresh();
            ws().listChanged = false;
            ws().fullRefresh = false;
            clear_changed();
        }
    } else {
        ws().listChanged = false;
        ws().fullRefresh = false;
        clear_changed();
    }
    std::vector<PIDLIST_ABSOLUTE> parents;
    parents.swap(s_treeParents);
    for (size_t i = 0; i < parents.size(); ++i) {
        bool dup = false;
        for (size_t j = 0; j < i && !dup; ++j) dup = pidl_equal(parents[i], parents[j]);
        if (!dup) tree_refresh_node(parents[i]);
    }
    for (auto p : parents) CoTaskMemFree(p);
}
