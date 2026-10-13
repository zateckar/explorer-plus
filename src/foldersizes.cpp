#include "foldersizes.h"
#include "enumerate.h"
#include "columns.h"
#include "detailspane.h"
#include "perf.h"
#include "workers.h"
#include <algorithm>
#include <cwctype>
#include <memory>
#include <thread>
#include <unordered_map>

namespace {

bool s_enabled = true;
std::atomic<UINT> s_gen{ 0 };             // bumped when switched off or on: running walks stop

constexpr int kWalkers = 4;               // folders walked at the same time
constexpr ULONGLONG kPostMs = 150;        // results go to the UI at most this often
constexpr ULONGLONG kReuseAnyMs = 5 * 60 * 1000;    // SizeReuse::Any
constexpr ULONGLONG kReuseRecentMs = 10 * 1000;     // SizeReuse::Recent
constexpr size_t kCacheMax = 20000;

// a directory whose contents live in the cloud: listing it would download them
constexpr DWORD kCloudDir = FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                            FILE_ATTRIBUTE_OFFLINE;

struct Result {
    std::wstring key;                     // Item::key
    std::wstring path;                    // the \\?\ path walked, lowercased (the cache's key)
    ULONGLONG bytes;
    bool known;                           // false: no size (a size shown from before goes)
    ULONGLONG walkMs;                     // how long the walk took
};

struct Walk {
    Pane* pane = nullptr;
    UINT opId = 0, gen = 0;
    std::vector<std::pair<std::wstring, std::wstring>> folders;   // (Item::key, \\?\ path)
    std::atomic<size_t> next{ 0 };
    std::atomic<int> running{ 0 };
    std::mutex mx;
    std::vector<Result> done;             // not posted yet
    ULONGLONG lastPost = 0;
    bool cancelled() const { return opId != pane->enumId.load() || gen != s_gen.load() || workers_stopping(); }
};

struct Batch {
    Pane* pane = nullptr;
    UINT opId = 0, gen = 0;
    std::vector<Result> results;
    bool final = false;                   // the walk is over
};

struct CachedSize {
    ULONGLONG bytes, tick, walkMs;
};
std::unordered_map<std::wstring, CachedSize> s_cache;      // UI thread only (both panes)

struct DeepWatch;

// each pane's walks and watch (UI thread)
struct SizeState {
    // folders being walked (cache key -> the listing the walk is for)
    std::unordered_map<std::wstring, UINT> walking;
    std::shared_ptr<DeepWatch> deep;
    UINT deepGen = 0;                        // bumped when the watch goes: its late changes are dropped
    std::wstring deepDir;                    // the folder watched (ends in '\')
    std::unordered_set<std::wstring> dirty;  // subfolders (Item::key) changed and not walked again yet
    bool dirtyAll = false;
};
SizeState s_sizes[kPanes];
SizeState& ss() { return s_sizes[g_pane->index]; }

std::wstring lower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

bool is_fs_path(const std::wstring& p) {
    return p.size() >= 3 && p.find(L"::") == std::wstring::npos &&
           ((iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\') || p.rfind(L"\\\\", 0) == 0);
}

// the \\?\ form: no MAX_PATH limit deep down the tree
std::wstring long_path(std::wstring p) {
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

// The walk runs at background priority (CPU and disk): the user's own work goes first.
struct BackgroundMode {
    bool on = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
    ~BackgroundMode() { if (on) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END); }
};

enum class WalkEnd { Done, Cancelled, Unknown };

bool is_link(DWORD attrs, DWORD tag) {
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) && (tag == IO_REPARSE_TAG_MOUNT_POINT || tag == IO_REPARSE_TAG_SYMLINK);
}

// `root` may be walked: a real folder (not a junction or link itself, not in the cloud
// only) that really is on an internal disk. Its path can go through a link higher up,
// to a share or an external disk, which the drive letter doesn't tell.
bool walkable_root(const std::wstring& root) {
    HANDLE h = CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO ti{};
    bool ok = GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof(ti)) &&
              (ti.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(ti.FileAttributes & kCloudDir) &&
              !is_link(ti.FileAttributes, ti.ReparseTag);
    std::wstring real;
    if (ok) {
        DWORD n = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        real.resize(n);
        n = n ? GetFinalPathNameByHandleW(h, real.data(), n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
        real.resize(n < real.size() ? n : 0);
    }
    CloseHandle(h);
    // "\\?\C:\..." (a share: "\\?\UNC\...")
    return ok && real.rfind(L"\\\\?\\", 0) == 0 && path_on_internal_drive(real.substr(4));
}

// the bytes of every file under `root`. Folders inside that can't be opened (no access)
// are left out, as Explorer's Properties does. Unknown: the folder itself can't be
// walked, a listing broke off part way, or part of it is in the cloud only (a size that
// leaves things out would be wrong, not just late).
WalkEnd folder_bytes(const Walk& w, const std::wstring& root, ULONGLONG* out) {
    if (!walkable_root(root)) return WalkEnd::Unknown;
    ULONGLONG total = 0;
    std::vector<std::wstring> dirs{ root };
    bool first = true;
    WIN32_FIND_DATAW fd;
    while (!dirs.empty()) {
        if (w.cancelled()) return WalkEnd::Cancelled;
        std::wstring dir = std::move(dirs.back());
        dirs.pop_back();
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            if (first) return WalkEnd::Unknown;           // the folder itself can't be read
            continue;                                     // a protected folder inside: left out
        }
        first = false;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                continue;
            }
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
            // junctions and links: what they point to is counted where it is (and can't
            // loop). Other reparse points (OneDrive folders) are real folders.
            if (is_link(fd.dwFileAttributes, fd.dwReserved0)) continue;
            if (fd.dwFileAttributes & kCloudDir) { FindClose(h); return WalkEnd::Unknown; }
            dirs.push_back(dir + L'\\' + n);
        } while (FindNextFileW(h, &fd));
        DWORD e = GetLastError();
        FindClose(h);
        if (e != ERROR_NO_MORE_FILES) return WalkEnd::Unknown;   // (the rest of it unread)
    }
    *out = total;
    return WalkEnd::Done;
}

void apply(void* payload);

void post(Walk& w, bool final) {          // (w.mx held)
    auto* b = new Batch;
    b->pane = w.pane;
    b->opId = w.opId;
    b->gen = w.gen;
    b->results.swap(w.done);
    b->final = final;
    w.lastPost = GetTickCount64();
    post_async(apply, b);
}

void walker(const std::shared_ptr<Walk>& w) {
    BackgroundMode bg;
    for (;;) {
        size_t i = w->next++;
        if (i >= w->folders.size() || w->cancelled()) break;
        ULONGLONG bytes = 0, t0 = GetTickCount64();
        WalkEnd end = folder_bytes(*w, w->folders[i].second, &bytes);
        if (end == WalkEnd::Cancelled) break;
        std::lock_guard<std::mutex> lk(w->mx);
        w->done.push_back({ w->folders[i].first, lower(w->folders[i].second), bytes, end == WalkEnd::Done,
                            GetTickCount64() - t0 });
        if (GetTickCount64() - w->lastPost >= kPostMs) post(*w, false);
    }
    if (--w->running == 0) {
        std::lock_guard<std::mutex> lk(w->mx);
        if (!w->cancelled()) post(*w, true);
    }
}

void remember(const Result& r, ULONGLONG now) {
    ss().walking.erase(r.path);
    if (!r.known) { s_cache.erase(r.path); return; }
    if (s_cache.size() >= kCacheMax) s_cache.clear();     // (rarely: start over)
    s_cache[r.path] = { r.bytes, now, r.walkMs };
}

void apply(void* payload) {
    std::unique_ptr<Batch> b(static_cast<Batch*>(payload));
    PaneScope ps(b->pane);
    ULONGLONG now = GetTickCount64();
    for (auto& r : b->results) remember(r, now);           // (useful even for a listing gone by)
    if (b->opId != g_pane->enumId.load() || b->gen != s_gen.load()) return;     // another listing by now
    bool selectedChanged = false;
    if (!b->results.empty()) {
        std::unordered_map<std::wstring, const Result*> sizes;
        sizes.reserve(b->results.size());
        for (auto& r : b->results) sizes[r.key] = &r;
        std::lock_guard<std::mutex> lk(g_pane->itemsMx);
        for (int i = 0; i < (int)g_pane->items.size(); ++i) {
            Item* it = g_pane->items[i];
            if (!it->isFolder) continue;
            auto f = sizes.find(it->key);
            if (f == sizes.end()) continue;
            const Result& r = *f->second;
            it->bytes = r.known ? r.bytes : 0;
            it->size = r.known ? fmt_size_kb(r.bytes) : std::wstring();
            selectedChanged |= (ListView_GetItemState(g_pane->list, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        }
        InvalidateRect(g_pane->list, nullptr, FALSE);
    }
    if (selectedChanged) detailspane_update();
    // sorted by size: the folders take their places once all are known (not while the
    // user may be clicking them)
    if (b->final && g_pane->sortColumn == COL_SIZE) enum_sort_items();
}

bool has_size_column() {
    for (auto& c : g_pane->columns)
        if (c.kind == COL_SIZE) return true;
    return false;
}

// ------------------------------------------------------------------
// The shown folder's whole tree is watched: a file made, removed or grown anywhere inside
// a subfolder walks that subfolder again. (Changes directly in the folder come through
// the listing's own watcher, and a changed folder item is walked again from there.)
// A subfolder that keeps changing (a browser's cache) is walked at most every
// kDeepMinMs, or ten times as long as its last walk took, whichever is longer.

constexpr ULONGLONG kDeepQuietMs = 1500;     // a burst of changes: walk once it settles...
constexpr ULONGLONG kDeepMaxWaitMs = 5000;   // ... or this long after it began
constexpr ULONGLONG kDeepMinMs = 5000;

struct DeepWatch {
    Pane* pane = nullptr;
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE ev = nullptr;                  // overlapped completion
    HANDLE stop = nullptr;                // set by the UI thread
    UINT gen = 0;
    ~DeepWatch() {
        if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
        if (ev) CloseHandle(ev);
        if (stop) CloseHandle(stop);
    }
};

struct DeepChanges {
    Pane* pane = nullptr;
    UINT gen = 0;
    std::unordered_set<std::wstring> names;   // subfolders changed inside (lowercased)
    bool all = false;                         // too many changes to name: every subfolder
    bool ended = false;                       // the watch failed and its thread is gone
};


void deep_apply(void* payload);

void deep_post(const DeepWatch& w, std::unordered_set<std::wstring>& names, bool& all, bool ended = false) {
    auto* c = new DeepChanges;
    c->pane = w.pane;
    c->gen = w.gen;
    c->names.swap(names);
    c->all = all;
    c->ended = ended;
    all = false;
    post_async(deep_apply, c);
}

void deep_thread(std::shared_ptr<DeepWatch> w) {
    std::vector<DWORD> buf(16 * 1024);                  // 64 KB (DWORD-aligned)
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE;
    std::unordered_set<std::wstring> names;
    bool all = false;
    ULONGLONG first = 0, last = 0;                      // the burst being collected (0: none)
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = w->ev;
        if (!ReadDirectoryChangesW(w->dir, buf.data(), (DWORD)(buf.size() * sizeof(DWORD)), TRUE, filter,
                                   nullptr, &ov, nullptr))
            break;
        DWORD r;
        for (;;) {                                      // waiting for changes, hand on a burst that settled
            DWORD timeout = INFINITE;
            if (first) {
                ULONGLONG due = std::min(last + kDeepQuietMs, first + kDeepMaxWaitMs), now = GetTickCount64();
                timeout = due > now ? (DWORD)(due - now) : 0;
            }
            HANDLE hs[2] = { w->ev, w->stop };
            r = WaitForMultipleObjects(2, hs, FALSE, timeout);
            if (r != WAIT_TIMEOUT) break;
            deep_post(*w, names, all);
            first = 0;
        }
        DWORD got = 0;
        if (r != WAIT_OBJECT_0) {
            CancelIoEx(w->dir, &ov);
            GetOverlappedResult(w->dir, &ov, &got, TRUE);   // (the buffer stays ours until then)
            return;
        }
        if (!GetOverlappedResult(w->dir, &ov, &got, FALSE)) break;
        if (got == 0) {
            all = true;                                 // more than the buffer holds
        } else {
            auto* p = reinterpret_cast<const BYTE*>(buf.data());
            for (;;) {
                auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                std::wstring name(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                size_t cut = name.find(L'\\');
                if (cut != std::wstring::npos) names.insert(lower(name.substr(0, cut)));   // inside a subfolder
                if (!fni->NextEntryOffset) break;
                p += fni->NextEntryOffset;
            }
        }
        if (all || !names.empty()) {
            last = GetTickCount64();
            if (!first) first = last;
        }
    }
    // the watch failed (the folder went): what it saw still counts, and the UI lets go of
    // the folder (the handle would keep a deleted folder "delete pending")
    deep_post(*w, names, all, true);
}

void deep_stop() {
    ++ss().deepGen;
    ss().dirty.clear();
    ss().dirtyAll = false;
    if (g_main) KillTimer(g_main, pane_timer(TIMER_FOLDER_SIZES));
    ss().deepDir.clear();
    if (!ss().deep) return;
    SetEvent(ss().deep->stop);
    ss().deep.reset();                                     // (the thread holds its own reference)
}

void deep_start(const std::wstring& dir) {
    if (ss().deep && CompareStringOrdinal(dir.c_str(), -1, ss().deepDir.c_str(), -1, TRUE) == CSTR_EQUAL) return;
    deep_stop();
    auto w = std::make_shared<DeepWatch>();
    w->pane = g_pane;
    w->gen = ss().deepGen;
    // share everything: the folder and what is in it can still be renamed or deleted
    w->dir = CreateFileW(long_path(dir).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    w->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (w->dir == INVALID_HANDLE_VALUE || !w->ev || !w->stop) return;
    try {
        std::thread(deep_thread, w).detach();
    } catch (...) {
        return;                                         // (sizes still follow F5)
    }
    ss().deep = w;
    ss().deepDir = dir;
}

// the changed subfolders that may be walked now; the others wait for the timer
void walk_dirty() {
    if (g_main) KillTimer(g_main, pane_timer(TIMER_FOLDER_SIZES));
    if (!ss().deep || (ss().dirty.empty() && !ss().dirtyAll)) return;
    std::wstring dir = pidl_folder_path(g_pane->current);
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    if (CompareStringOrdinal(dir.c_str(), -1, ss().deepDir.c_str(), -1, TRUE) != CSTR_EQUAL) {
        ss().dirty.clear();                                // on the way to another folder
        ss().dirtyAll = false;
        return;
    }
    if (!enum_listing_complete()) {                     // a refresh on the way: after it
        SetTimer(g_main, pane_timer(TIMER_FOLDER_SIZES), (UINT)kDeepQuietMs, nullptr);
        return;
    }
    ULONGLONG now = GetTickCount64(), wait = 0;
    auto later = [&](ULONGLONG ms) { wait = wait ? std::min(wait, ms) : ms; };
    std::unordered_set<std::wstring> walk, still;
    {
        std::lock_guard<std::mutex> lk(g_pane->itemsMx);
        for (Item* it : g_pane->items) {
            if (!it->isFolder || it->fileName.empty() || !(ss().dirtyAll || ss().dirty.count(it->key))) continue;
            std::wstring ck = lower(long_path(dir + it->fileName));
            if (auto b = ss().walking.find(ck); b != ss().walking.end() && b->second == g_pane->enumId.load()) {
                still.insert(it->key);                  // being walked: it may have read past the change
                later(kDeepMinMs);
                continue;
            }
            if (auto c = s_cache.find(ck); c != s_cache.end()) {
                ULONGLONG gap = std::max(kDeepMinMs, 10 * c->second.walkMs), age = now - c->second.tick;
                if (age < gap) {
                    still.insert(it->key);
                    later(gap - age);
                    continue;
                }
            }
            walk.insert(it->key);
        }
    }
    ss().dirty.swap(still);
    ss().dirtyAll = false;
    if (perf_on()) perf_event("sizes.changed", -1, (long long)walk.size(), nullptr);
    if (!walk.empty()) foldersizes_start(&walk);
    if (wait) SetTimer(g_main, pane_timer(TIMER_FOLDER_SIZES), (UINT)wait, nullptr);
}

void deep_apply(void* payload) {
    std::unique_ptr<DeepChanges> c(static_cast<DeepChanges*>(payload));
    PaneScope ps(c->pane);
    if (c->gen != ss().deepGen) return;
    ss().dirty.insert(c->names.begin(), c->names.end());
    ss().dirtyAll |= c->all;
    walk_dirty();
    if (c->ended) deep_stop();           // (the next listing of the folder watches it again)
}

}  // namespace

bool foldersizes_enabled() { return s_enabled; }

void foldersizes_start(const std::unordered_set<std::wstring>* keys, SizeReuse reuse) {
    if (!s_enabled || !g_pane->current || !g_pane->list || !has_size_column()) {
        if (!keys) deep_stop();
        return;
    }
    std::wstring dir = pidl_folder_path(g_pane->current);
    bool fsDir = is_fs_path(dir);
    if (fsDir && dir.back() != L'\\') dir += L'\\';
    // a listing complete: watch what is inside its folders (a plain folder on an internal disk)
    if (!keys) {
        if (fsDir && dir[1] == L':' && path_on_internal_drive(dir)) deep_start(dir);
        else deep_stop();
    }
    auto w = std::make_shared<Walk>();
    w->pane = g_pane;
    w->opId = g_pane->enumId.load();
    w->gen = s_gen.load();
    ULONGLONG now = GetTickCount64();
    ULONGLONG maxAge = reuse == SizeReuse::Any ? kReuseAnyMs : reuse == SizeReuse::Recent ? kReuseRecentMs : 0;
    int fromCache = 0;
    bool selectedFromCache = false;
    {
        std::lock_guard<std::mutex> lk(g_pane->itemsMx);
        for (int i = 0; i < (int)g_pane->items.size(); ++i) {
            Item* it = g_pane->items[i];
            if (!it->isFolder || it->driveType || !(it->attrs & SFGAO_FILESYSTEM)) continue;
            if (keys && !keys->count(it->key)) continue;
            // search results and Home give each item's path; a plain folder its name
            std::wstring path = !it->path.empty() ? it->path
                              : fsDir && !it->fileName.empty() ? dir + it->fileName : std::wstring();
            if (!is_fs_path(path) || !path_on_internal_drive(path)) continue;
            std::wstring lp = long_path(path);
            std::wstring ck = lower(lp);
            auto c = s_cache.find(ck);
            if (c != s_cache.end()) {
                if (maxAge && now - c->second.tick <= maxAge) {
                    it->bytes = c->second.bytes;
                    it->size = fmt_size_kb(c->second.bytes);
                    ++fromCache;
                    selectedFromCache |= (ListView_GetItemState(g_pane->list, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                    continue;
                }
                s_cache.erase(c);                            // stale, or it changed
            }
            ss().walking[ck] = w->opId;
            w->folders.push_back({ it->key, std::move(lp) });
        }
    }
    if (fromCache) {
        InvalidateRect(g_pane->list, nullptr, FALSE);
        if (selectedFromCache) detailspane_update();
        if (w->folders.empty() && g_pane->sortColumn == COL_SIZE) enum_sort_items();   // (no walk to wait for)
    }
    if (w->folders.empty()) return;
    int n = (int)std::min<size_t>(kWalkers, w->folders.size());
    w->running = n;
    for (int i = 0; i < n; ++i) worker_submit([w] { walker(w); }, JobKind::Bulk);
}

void foldersizes_on_timer() { walk_dirty(); }

void foldersizes_release_watch() { deep_stop(); }

void foldersizes_forget_shown() {
    if (!g_pane->current) return;
    std::wstring dir = pidl_folder_path(g_pane->current);
    if (is_fs_path(dir)) {                               // a plain folder: everything inside it
        std::wstring prefix = lower(long_path(dir));
        if (prefix.back() != L'\\') prefix += L'\\';
        std::erase_if(s_cache, [&](const auto& e) { return e.first.rfind(prefix, 0) == 0; });
    }
    std::lock_guard<std::mutex> lk(g_pane->itemsMx);           // search results, Home: the items' own paths
    for (Item* it : g_pane->items)
        if (it->isFolder && is_fs_path(it->path)) s_cache.erase(lower(long_path(it->path)));
}

void foldersizes_set_enabled(bool on) {
    if (on == s_enabled) return;
    s_enabled = on;
    ++s_gen;
    for_each_pane([on] {
        ss().walking.clear();                               // (the walks running stop)
        if (!on) deep_stop();
        if (!pane_shown(g_pane)) return;
        if (on) {
            if (enum_listing_complete()) foldersizes_start(nullptr, SizeReuse::Recent);   // (else when it is)
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_pane->itemsMx);
            for (Item* it : g_pane->items)
                if (it->isFolder && !it->driveType) { it->bytes = 0; it->size.clear(); }
        }
        InvalidateRect(g_pane->list, nullptr, FALSE);
        detailspane_update();
        if (g_pane->sortColumn == COL_SIZE) enum_sort_items();
    });
}
