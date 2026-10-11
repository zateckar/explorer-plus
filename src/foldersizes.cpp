#include "foldersizes.h"
#include "enumerate.h"
#include "columns.h"
#include "detailspane.h"
#include "workers.h"
#include <algorithm>
#include <cwctype>
#include <memory>
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
};

struct Walk {
    UINT opId = 0, gen = 0;
    std::vector<std::pair<std::wstring, std::wstring>> folders;   // (Item::key, \\?\ path)
    std::atomic<size_t> next{ 0 };
    std::atomic<int> running{ 0 };
    std::mutex mx;
    std::vector<Result> done;             // not posted yet
    ULONGLONG lastPost = 0;
    bool cancelled() const { return opId != g_enumId.load() || gen != s_gen.load() || workers_stopping(); }
};

struct Batch {
    UINT opId = 0, gen = 0;
    std::vector<Result> results;
    bool final = false;                   // the walk is over
};

struct CachedSize {
    ULONGLONG bytes, tick;
};
std::unordered_map<std::wstring, CachedSize> s_cache;      // UI thread only

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

// the bytes of every file under `root`. Unknown: the folder can't be read, or part of it
// is in the cloud only (a size that leaves things out would be wrong, not just late)
WalkEnd folder_bytes(const Walk& w, const std::wstring& root, ULONGLONG* out) {
    DWORD rootAttrs = GetFileAttributesW(root.c_str());
    if (rootAttrs == INVALID_FILE_ATTRIBUTES || (rootAttrs & kCloudDir)) return WalkEnd::Unknown;
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
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK))
                continue;
            if (fd.dwFileAttributes & kCloudDir) { FindClose(h); return WalkEnd::Unknown; }
            dirs.push_back(dir + L'\\' + n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    *out = total;
    return WalkEnd::Done;
}

void apply(void* payload);

void post(Walk& w, bool final) {          // (w.mx held)
    auto* b = new Batch;
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
        ULONGLONG bytes = 0;
        WalkEnd end = folder_bytes(*w, w->folders[i].second, &bytes);
        if (end == WalkEnd::Cancelled) break;
        if (end == WalkEnd::Unknown) continue;            // (no size shown)
        std::lock_guard<std::mutex> lk(w->mx);
        w->done.push_back({ w->folders[i].first, lower(w->folders[i].second), bytes });
        if (GetTickCount64() - w->lastPost >= kPostMs) post(*w, false);
    }
    if (--w->running == 0) {
        std::lock_guard<std::mutex> lk(w->mx);
        if (!w->cancelled()) post(*w, true);
    }
}

void remember(const Result& r, ULONGLONG now) {
    if (s_cache.size() >= kCacheMax) s_cache.clear();     // (rarely: start over)
    s_cache[r.path] = { r.bytes, now };
}

void apply(void* payload) {
    std::unique_ptr<Batch> b(static_cast<Batch*>(payload));
    ULONGLONG now = GetTickCount64();
    for (auto& r : b->results) remember(r, now);           // (useful even for a listing gone by)
    if (b->opId != g_enumId.load() || b->gen != s_gen.load()) return;     // another listing by now
    bool selectedChanged = false;
    if (!b->results.empty()) {
        std::unordered_map<std::wstring, ULONGLONG> sizes;
        sizes.reserve(b->results.size());
        for (auto& r : b->results) sizes[r.key] = r.bytes;
        std::lock_guard<std::mutex> lk(g_itemsMx);
        for (int i = 0; i < (int)g_items.size(); ++i) {
            Item* it = g_items[i];
            if (!it->isFolder) continue;
            auto f = sizes.find(it->key);
            if (f == sizes.end()) continue;
            it->bytes = f->second;
            it->size = fmt_size_kb(f->second);
            selectedChanged |= (ListView_GetItemState(g_list, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        }
        InvalidateRect(g_list, nullptr, FALSE);
    }
    if (selectedChanged) detailspane_update();
    // sorted by size: the folders take their places once all are known (not while the
    // user may be clicking them)
    if (b->final && g_sortColumn == COL_SIZE) enum_sort_items();
}

bool has_size_column() {
    for (auto& c : g_columns)
        if (c.kind == COL_SIZE) return true;
    return false;
}

}  // namespace

bool foldersizes_enabled() { return s_enabled; }

void foldersizes_start(const std::unordered_set<std::wstring>* keys, SizeReuse reuse) {
    if (!s_enabled || !g_current || !g_list || !has_size_column()) return;
    std::wstring dir = pidl_folder_path(g_current);
    bool fsDir = is_fs_path(dir);
    if (fsDir && dir.back() != L'\\') dir += L'\\';
    auto w = std::make_shared<Walk>();
    w->opId = g_enumId.load();
    w->gen = s_gen.load();
    ULONGLONG now = GetTickCount64();
    ULONGLONG maxAge = reuse == SizeReuse::Any ? kReuseAnyMs : reuse == SizeReuse::Recent ? kReuseRecentMs : 0;
    int fromCache = 0;
    bool selectedFromCache = false;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        for (int i = 0; i < (int)g_items.size(); ++i) {
            Item* it = g_items[i];
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
                    selectedFromCache |= (ListView_GetItemState(g_list, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                    continue;
                }
                s_cache.erase(c);                            // stale, or it changed
            }
            w->folders.push_back({ it->key, std::move(lp) });
        }
    }
    if (fromCache) {
        InvalidateRect(g_list, nullptr, FALSE);
        if (selectedFromCache) detailspane_update();
        if (w->folders.empty() && g_sortColumn == COL_SIZE) enum_sort_items();   // (no walk to wait for)
    }
    if (w->folders.empty()) return;
    int n = (int)std::min<size_t>(kWalkers, w->folders.size());
    w->running = n;
    for (int i = 0; i < n; ++i) worker_submit([w] { walker(w); }, JobKind::Bulk);
}

void foldersizes_set_enabled(bool on) {
    if (on == s_enabled) return;
    s_enabled = on;
    ++s_gen;
    if (!g_list) return;
    if (on) {
        if (enum_listing_complete()) foldersizes_start(nullptr, SizeReuse::Recent);   // (else when it is)
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        for (Item* it : g_items)
            if (it->isFolder && !it->driveType) { it->bytes = 0; it->size.clear(); }
    }
    InvalidateRect(g_list, nullptr, FALSE);
    detailspane_update();
    if (g_sortColumn == COL_SIZE) enum_sort_items();
}
