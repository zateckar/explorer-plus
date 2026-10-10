#include "tree.h"
#include "navigation.h"
#include "fileops.h"
#include "tabs.h"
#include "theme.h"
#include "searchbox.h"
#include "perf.h"
#include "settings.h"
#include <propsys.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include "workers.h"
#include <unordered_map>
#include <unordered_set>

// ------------------------------------------------------------------
// node data

enum class NodeKind { Folder, Separator };

// absolute pidl (empty pidl = Desktop root); `listed` once children were enumerated
struct NodeData {
    PITEMID_CHILD abs = nullptr;
    UINT serial = 0;               // unique per node: a freed node's address can be reused
    bool listed = false;
    bool listing = false;          // a worker is enumerating the children
    bool listFailed = false;       // the last listing failed: syncs don't retry it (expanding does)
    bool refreshing = false;       // a worker is re-listing the children
    bool refreshAgain = false;     // another refresh was asked for meanwhile
    bool pinned = false;           // Quick access pin (glyph on the right)
    NodeKind kind = NodeKind::Folder;
};

static std::unordered_set<NodeData*> s_live;     // nodes still in the tree (for async results)
static UINT s_nodeSerial = 0;
static HTREEITEM s_thisPC = nullptr;

// an async result's node is still the one it was started for
static bool node_alive(NodeData* d, UINT serial) { return s_live.count(d) && d->serial == serial; }

static void free_node(NodeData* d) {
    if (!d) return;
    s_live.erase(d);
    if (d->abs) CoTaskMemFree(d->abs);
    delete d;
}

static NodeData* node_data(HWND tree, HTREEITEM h) {
    if (!h) return nullptr;
    TVITEMW ti{};
    ti.mask = TVIF_PARAM; ti.hItem = h;
    if (TreeView_GetItem(tree, &ti)) return (NodeData*)ti.lParam;
    return nullptr;
}

static int id_count(LPCITEMIDLIST p) {
    int n = 0;
    for (; p && !ILIsEmpty(p); p = ILNext(p)) ++n;
    return n;
}

// is `anc` a strict ancestor of `target`? compares the target's prefix canonically
// (byte-wise prefix checks fail for fs pidls carrying different timestamps).
static bool pidl_is_ancestor(LPCITEMIDLIST anc, LPCITEMIDLIST target) {
    int na = id_count(anc), nt = id_count(target);
    if (na >= nt) return false;
    if (na == 0) return true;               // Desktop contains everything
    PITEMID_CHILD prefix = pidl_clone(target);
    if (!prefix) return false;
    for (int i = nt; i > na; --i) ILRemoveLastID(prefix);
    bool r = pidl_equal(anc, prefix);
    CoTaskMemFree(prefix);
    return r;
}

struct NodeInfo {                  // everything needed to insert a node (worker-safe)
    PITEMID_CHILD abs = nullptr;   // owned until inserted
    std::wstring name;
    int icon = -1, openIcon = -1;
    bool hasKids = false;
};

static NodeInfo describe(PITEMID_CHILD abs, bool hasKids) {
    NodeInfo n;
    n.abs = abs;
    n.name = pidl_abs_dispname(abs);
    if (n.name.empty()) n.name = pidl_folder_path(abs);
    // tree nodes are folders: without a shell icon they get the folder one, not "unknown file"
    n.icon = sys_icon_index(abs, false, sys_folder_icon_index(false));
    n.openIcon = sys_icon_index(abs, true, sys_folder_icon_index(true));
    n.hasKids = hasKids;
    return n;
}

// insert folder node; ownership of `info.abs` moves to the node
static HTREEITEM insert_node(HWND tree, HTREEITEM parent, NodeInfo& info, bool pinned = false,
                             HTREEITEM after = TVI_LAST) {
    NodeData* d = new NodeData{ info.abs };
    d->serial = ++s_nodeSerial;
    d->pinned = pinned;
    info.abs = nullptr;
    s_live.insert(d);
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = after;
    ins.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_CHILDREN;
    ins.item.pszText = const_cast<LPWSTR>(info.name.c_str());
    ins.item.iImage = info.icon;
    ins.item.iSelectedImage = info.openIcon;
    ins.item.lParam = (LPARAM)d;
    ins.item.cChildren = info.hasKids ? 1 : 0;
    return TreeView_InsertItem(tree, &ins);
}

static HTREEITEM insert_separator(HWND tree, HTREEITEM after = TVI_LAST) {
    NodeData* d = new NodeData{};
    d->serial = ++s_nodeSerial;
    d->kind = NodeKind::Separator;
    s_live.insert(d);
    TVINSERTSTRUCTW ins{};
    ins.hParent = TVI_ROOT;
    ins.hInsertAfter = after;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    ins.item.pszText = const_cast<LPWSTR>(L"");
    ins.item.iImage = ins.item.iSelectedImage = I_IMAGENONE;
    ins.item.lParam = (LPARAM)d;
    return TreeView_InsertItem(tree, &ins);
}

// ------------------------------------------------------------------
// listing children (worker-safe part)

// does a file-system folder have a subfolder the tree would list? Reads only up to the
// first one: SFGAO_HASSUBFOLDER costs ~5 ms per folder (D:\DEV, 98 folders: 525 ms vs 2 ms)
static bool fs_has_subfolder(const std::wstring& dir, bool showHidden) {
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_ACCESS_DENIED;   // let expanding tell
    const DWORD superHidden = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;   // protected OS folders
    bool found = false;
    do {
        DWORD a = fd.dwFileAttributes;
        if (!(a & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if ((a & superHidden) == superHidden || (!showHidden && (a & FILE_ATTRIBUTE_HIDDEN))) continue;
        found = true;
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// is `folder` the file-system path `path` or one of its ancestors? (case-insensitive)
static bool path_within(const std::wstring& folder, const std::wstring& path) {
    size_t n = folder.size();
    if (!n || path.size() < n) return false;
    if (CompareStringOrdinal(folder.c_str(), (int)n, path.c_str(), (int)n, TRUE) != CSTR_EQUAL) return false;
    return path.size() == n || path[n] == L'\\' || folder.back() == L'\\';
}

// false: the folder couldn't be read (unreachable, access denied) — not the same as empty.
// `keep`: the path of the folder the tree follows; with hidden items off, a hidden child
// on the way there is listed anyway (like Explorer: AppData shows while a folder inside
// it is open). Paths, not pidls: the same folder has a pidl under This PC and another
// under the user's folder.
static bool list_children(PCIDLIST_ABSOLUTE parent, bool showHidden, std::vector<NodeInfo>& out,
                          const std::wstring& keep = {}) {
    out.clear();
    com_ptr<IShellFolder> sf;
    if (FAILED(bind_list(parent, IID_PPV_ARGS(sf.put()))) || !sf) return false;
    // children of a plain directory are checked for subfolders directly
    std::wstring dir = pidl_folder_path(parent);
    bool fsParent = dir.size() >= 2 && (dir[1] == L':' || dir.rfind(L"\\\\", 0) == 0);
    bool keepHidden = !showHidden && fsParent && keep.size() > dir.size() && path_within(dir, keep);
    com_ptr<IEnumIDList> e;
    SHCONTF flags = SHCONTF_FOLDERS | (showHidden || keepHidden ? SHCONTF_INCLUDEHIDDEN : 0);
    HRESULT hrEnum = sf->EnumObjects(nullptr, flags, e.put());
    if (hrEnum == S_FALSE) return true;             // nothing to list
    if (hrEnum != S_OK || !e) return false;
    struct Kid { PITEMID_CHILD child; bool hasKids; };
    std::vector<Kid> kids;
    double t0 = perf_on() ? perf_now() : 0, tEnum = 0, tAttr = 0;
    PITEMID_CHILD child = nullptr; ULONG fetched = 0;
    for (;;) {
        double a = perf_on() ? perf_now() : 0;
        if (!(e->Next(1, &child, &fetched) == S_OK && fetched)) break;
        double b = perf_on() ? perf_now() : 0;
        tEnum += b - a;
        SFGAOF attrs = SFGAO_FOLDER | SFGAO_STREAM | SFGAO_FILESYSTEM | SFGAO_HIDDEN;
        sf->GetAttributesOf(1, (PCUITEMID_CHILD*)&child, &attrs);
        // archives (zip) are browsable but Explorer keeps them out of the tree
        if ((attrs & SFGAO_FOLDER) == 0 || (attrs & SFGAO_STREAM)) { CoTaskMemFree(child); continue; }
        if (keepHidden && (attrs & SFGAO_HIDDEN)) {
            PIDLIST_ABSOLUTE full = ILCombine(parent, child);
            bool onTheWay = full && path_within(pidl_folder_path(full), keep);
            CoTaskMemFree(full);
            if (!onTheWay) { CoTaskMemFree(child); continue; }
        }
        // cloud placeholders are not opened (listing one would download it): assume children
        const DWORD remote = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
        WIN32_FIND_DATAW fd{};
        bool hasKids;
        if (fsParent && (attrs & SFGAO_FILESYSTEM) &&
            SUCCEEDED(SHGetDataFromIDListW(sf.get(), child, SHGDFIL_FINDDATA, &fd, sizeof(fd))) && fd.cFileName[0]) {
            hasKids = (fd.dwFileAttributes & remote) ||
                      fs_has_subfolder(dir + (dir.back() == L'\\' ? L"" : L"\\") + fd.cFileName, showHidden);
        } else {
            SFGAOF sub = SFGAO_HASSUBFOLDER;
            sf->GetAttributesOf(1, (PCUITEMID_CHILD*)&child, &sub);
            hasKids = (sub & SFGAO_HASSUBFOLDER) != 0;
        }
        if (perf_on()) tAttr += perf_now() - b;
        kids.push_back({ child, hasKids });
    }
    // the folder's own order; a comparison that fails falls back to the names. (stable_sort:
    // std::sort can read out of bounds when a namespace extension's order is inconsistent)
    IShellFolder* raw = sf.get();
    std::stable_sort(kids.begin(), kids.end(), [raw](const Kid& a, const Kid& b) {
        HRESULT hr = raw->CompareIDs(0, a.child, b.child);
        if (SUCCEEDED(hr)) return (SHORT)HRESULT_CODE(hr) < 0;
        return StrCmpLogicalW(child_name(raw, a.child, SHGDN_INFOLDER).c_str(),
                              child_name(raw, b.child, SHGDN_INFOLDER).c_str()) < 0;
    });
    double tSort = perf_on() ? perf_now() : 0;
    for (auto& k : kids) {
        PITEMID_CHILD full = static_cast<PITEMID_CHILD>(ILCombine(parent, k.child));
        CoTaskMemFree(k.child);
        if (full) out.push_back(describe(full, k.hasKids));
    }
    if (perf_on()) {
        WCHAR d[400];
        swprintf_s(d, L"enum=%.1f attrs=%.1f describe=%.1f %ls", tEnum, tAttr, perf_now() - tSort,
                   pidl_folder_path(parent).c_str());
        perf_event("tree.list", perf_now() - t0, (long long)out.size(), d);
    }
    return true;
}

// drawing off while the tree changes in several steps (each expand would otherwise cost
// a 2-3 ms repaint); nests: the outermost scope turns it back on and repaints once
static int s_noRedraw = 0;
struct TreeNoRedraw {
    HWND tree;
    explicit TreeNoRedraw(HWND t) : tree(t) { if (s_noRedraw++ == 0) SendMessageW(tree, WM_SETREDRAW, FALSE, 0); }
    ~TreeNoRedraw() {
        if (--s_noRedraw) return;
        SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(tree, nullptr, FALSE);
    }
    TreeNoRedraw(const TreeNoRedraw&) = delete;
    TreeNoRedraw& operator=(const TreeNoRedraw&) = delete;
};

static void insert_children(HWND tree, HTREEITEM parent, std::vector<NodeInfo>& kids) {
    TreeNoRedraw nr(tree);
    for (auto& k : kids) insert_node(tree, parent, k);
    if (kids.empty()) {
        TVITEMW ti{}; ti.mask = TVIF_CHILDREN; ti.hItem = parent; ti.cChildren = 0;
        TreeView_SetItem(tree, &ti);
    }
}

// asynchronous listing: slow / network folders never block the UI. Expanding a node and
// syncing to the current folder both list children on a worker.
struct ExpandJob {
    NodeData* node;
    UINT serial;
    HTREEITEM item;
    PITEMID_CHILD abs;              // owned copy for the worker
    bool showHidden;
    std::vector<NodeInfo> kids;
    bool ok = false;
    std::wstring keep;              // the folder the tree follows (see list_children)
};

// what listings keep in view although hidden: the folder the tree follows
static std::wstring keep_target() {
    if (g_showHidden || !g_current) return {};
    PCIDLIST_ABSOLUTE t = search_scope_of(g_current);
    return pidl_folder_path(t ? t : g_current);
}

static bool s_syncPending = false;  // a sync waits for an ancestor's children

static void free_infos(std::vector<NodeInfo>& v) {
    for (auto& k : v) CoTaskMemFree(k.abs);
    v.clear();
}

static void resume_sync() {
    if (s_syncPending) {            // walk on down to the current folder
        s_syncPending = false;
        tree_sync_to_current();
    }
}

static void expand_apply(void* payload) {
    std::unique_ptr<ExpandJob> job(static_cast<ExpandJob*>(payload));
    CoTaskMemFree(job->abs);
    if (!g_tree || !node_alive(job->node, job->serial) || node_data(g_tree, job->item) != job->node) {
        free_infos(job->kids);
        return;
    }
    NodeData* d = job->node;
    d->listing = false;
    if (d->listed) {
        free_infos(job->kids);
    } else if (!job->ok) {
        // unreachable / denied: not "listed" (expanding again retries), and a sync walking
        // through here stops instead of asking again
        d->listFailed = true;
    } else {
        d->listed = true;
        d->listFailed = false;
        TreeNoRedraw nr(g_tree);
        insert_children(g_tree, job->item, job->kids);
        if (!job->kids.empty()) TreeView_Expand(g_tree, job->item, TVE_EXPAND);
    }
    resume_sync();
}

static void expand_worker(ExpandJob* job) {
    ComInit ci;
    job->ok = list_children(job->abs, job->showHidden, job->kids, job->keep);
    post_async(expand_apply, job);
}

static void start_listing(HTREEITEM h, NodeData* d) {
    if (d->listing) return;
    d->listing = true;
    auto* job = new ExpandJob{ d, d->serial, h, pidl_clone(d->abs), g_showHidden, {} };
    job->keep = keep_target();
    worker_submit([job] { expand_worker(job); });
}

static std::wstring node_key(PCIDLIST_ABSOLUTE abs) {
    std::wstring k = pidl_folder_path(abs);
    CharLowerBuffW(k.data(), (DWORD)k.size());
    return k;
}

// a prefetch entry is for one route to the folder: under This PC and by way of the
// user's folder the same path has pidls of different depth, and the children carry it
static std::wstring prefetch_key(PCIDLIST_ABSOLUTE abs) {
    std::wstring k = node_key(abs);
    return k.empty() ? k : k + L'|' + std::to_wstring(id_count(abs));
}

static PITEMID_CHILD this_pc_route(LPCITEMIDLIST target, const std::wstring& path);

// The ancestors of the folder a sync walks to are listed all at once, each on its own
// worker: at startup before the window exists (tree_prefetch), later the ones below the
// node the walk waits for. Their nodes don't exist yet, so the results wait here (by
// path) until the walk gets there; whatever is left is dropped when the sync completes.
// Workers fill the entries directly (the startup ones can finish before g_main exists).
struct Prefetch { bool ready = false; std::vector<NodeInfo> kids; };
static std::mutex s_prefetchMx;
static std::unordered_map<std::wstring, Prefetch> s_prefetch;
static UINT s_prefetchGen = 0;      // bumped by clear_prefetch (under s_prefetchMx)
static int s_waitDepth = 0;         // id count of the node a sync waits for

struct PrefetchJob {
    std::wstring key;
    PITEMID_CHILD abs;              // owned
    bool showHidden;
    UINT gen;
    std::wstring keep;
};

static void clear_prefetch() {
    std::lock_guard<std::mutex> lk(s_prefetchMx);
    for (auto& [key, p] : s_prefetch) free_infos(p.kids);
    s_prefetch.clear();
    ++s_prefetchGen;                // workers still running belong to the old entries
}

static void prefetch_ready(void*) { resume_sync(); }   // the walk may be waiting for this one

static void prefetch_worker(PrefetchJob* job) {
    std::unique_ptr<PrefetchJob> own(job);
    ComInit ci;
    std::vector<NodeInfo> kids;
    bool ok = list_children(job->abs, job->showHidden, kids, job->keep);
    CoTaskMemFree(job->abs);
    {
        std::lock_guard<std::mutex> lk(s_prefetchMx);
        auto it = s_prefetch.find(job->key);
        if (job->gen != s_prefetchGen || it == s_prefetch.end() || it->second.ready) {   // sync done meanwhile
            free_infos(kids);
            return;
        }
        if (ok) {
            it->second.ready = true;
            it->second.kids = std::move(kids);
        } else {
            s_prefetch.erase(it);   // the walk lists the node itself (and learns it fails)
        }
    }
    // (before the main window exists the walk in WM_CREATE finds the entry ready)
    if (g_main) post_async(prefetch_ready, nullptr);
}

// list target's ancestors deeper than `depth` ids
static void prefetch_ancestors(LPCITEMIDLIST target, int depth) {
    PITEMID_CHILD prefix = pidl_clone(target);
    if (!prefix) return;
    for (int n = id_count(prefix) - 1; n > depth; --n) {
        ILRemoveLastID(prefix);
        std::wstring key = prefetch_key(prefix);
        if (key.empty()) continue;
        UINT gen;
        {
            std::lock_guard<std::mutex> lk(s_prefetchMx);
            if (s_prefetch.count(key)) continue;
            s_prefetch[key];
            gen = s_prefetchGen;
        }
        auto* job = new PrefetchJob{ key, pidl_clone(prefix), g_showHidden, gen,
                                     g_showHidden ? std::wstring() : pidl_folder_path(target) };
        worker_submit([job] { prefetch_worker(job); });
    }
    CoTaskMemFree(prefix);
}

void tree_prefetch(PCIDLIST_ABSOLUTE folder) {
    if (!folder) return;
    std::wstring path = pidl_folder_path(folder);
    if (path.rfind(L"\\\\", 0) == 0) return;       // network: never walked
    // the walk goes under This PC: list that route's levels
    PITEMID_CHILD pc = this_pc_route(folder, path);
    prefetch_ancestors(pc ? pc : folder, 0);
    CoTaskMemFree(pc);
}

// a node's children from a prefetch: 1 = listed now, 0 = still on the way, -1 = none
static int use_prefetch(HWND tree, HTREEITEM h, NodeData* d) {
    std::vector<NodeInfo> kids;
    {
        std::lock_guard<std::mutex> lk(s_prefetchMx);
        auto it = s_prefetch.find(prefetch_key(d->abs));
        if (it == s_prefetch.end()) return -1;
        if (!it->second.ready) return 0;
        kids = std::move(it->second.kids);
        s_prefetch.erase(it);
    }
    d->listed = true;
    insert_children(tree, h, kids);                 // (takes the pidls)
    return 1;
}

// ------------------------------------------------------------------
// roots

static PITEMID_CHILD parse_shell(const wchar_t* path) {
    PIDLIST_ABSOLUTE p = nullptr;
    if (FAILED(SHParseDisplayName(path, nullptr, &p, 0, nullptr))) return nullptr;
    return reinterpret_cast<PITEMID_CHILD>(p);
}

static PITEMID_CHILD known_folder(REFKNOWNFOLDERID id) {
    PIDLIST_ABSOLUTE p = nullptr;
    if (FAILED(SHGetKnownFolderIDList(id, KF_FLAG_DONT_VERIFY, nullptr, &p))) return nullptr;
    return reinterpret_cast<PITEMID_CHILD>(p);
}

static bool has_subfolders(PCIDLIST_ABSOLUTE abs, bool showHidden) {
    std::wstring path = pidl_folder_path(abs);
    if (path.size() >= 2 && path[1] == L':' && PathIsDirectoryW(path.c_str())) return fs_has_subfolder(path, showHidden);
    com_ptr<IShellItem> si;
    if (FAILED(shell_item_for(abs, si.put())) || !si) return true;
    SFGAOF a = 0;
    si->GetAttributes(SFGAO_HASSUBFOLDER, &a);
    return (a & SFGAO_HASSUBFOLDER) != 0;
}

// pinned folders of Quick access, in the user's order
static std::vector<PITEMID_CHILD> pinned_folders() {
    std::vector<PITEMID_CHILD> out;
    PITEMID_CHILD qa = parse_shell(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}");
    if (!qa) return out;
    com_ptr<IShellFolder2> sf;
    PROPERTYKEY kPinned{};
    if (SUCCEEDED(bind_list(qa, IID_PPV_ARGS(sf.put()))) && sf &&
        SUCCEEDED(PSGetPropertyKeyFromName(L"System.Home.IsPinned", &kPinned))) {
        com_ptr<IEnumIDList> e;
        if (sf->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, e.put()) == S_OK && e) {
            PITEMID_CHILD c = nullptr;
            while (e->Next(1, &c, nullptr) == S_OK) {
                SFGAOF a = SFGAO_FOLDER | SFGAO_STREAM;
                VARIANT v; VariantInit(&v);
                bool pinned = SUCCEEDED(sf->GetDetailsEx(c, &kPinned, &v)) && v.vt == VT_BOOL && v.boolVal;
                VariantClear(&v);
                bool folder = SUCCEEDED(sf->GetAttributesOf(1, (PCUITEMID_CHILD_ARRAY)&c, &a)) &&
                              (a & SFGAO_FOLDER) && !(a & SFGAO_STREAM);
                if (pinned && folder) {
                    // the item's target as a normal namespace pidl (not a Quick access child)
                    PIDLIST_ABSOLUTE target = nullptr;
                    com_ptr<IShellItem> si;
                    if (SUCCEEDED(SHCreateItemWithParent(qa, sf.get(), c, IID_PPV_ARGS(si.put()))) && si &&
                        SUCCEEDED(SHGetIDListFromObject(si.get(), &target)) && target) {
                        // re-parse the location ("D:\DEV", "::{645FF040-...}") so it is a normal namespace pidl
                        std::wstring path = pidl_folder_path(target);
                        PITEMID_CHILD reparsed = path.empty() ? nullptr : parse_shell(path.c_str());
                        if (reparsed) { CoTaskMemFree(target); out.push_back(reparsed); }
                        else out.push_back(reinterpret_cast<PITEMID_CHILD>(target));
                    }
                }
                CoTaskMemFree(c);
            }
        }
    }
    CoTaskMemFree(qa);
    return out;
}

// The Quick access folder is slow to read (~100 ms), so the pins shown at startup are the
// ones cached in settings; a worker reads the current ones and replaces them if they differ.
// They sit between OneDrive and This PC.
static UINT s_treeGen = 0;               // bumped by every initialize_tree
static HTREEITEM s_pinsAfter = nullptr;  // the root item the pins follow
static HTREEITEM s_pinSep = nullptr;     // the separator above the pins (null: no pins)
static std::vector<std::wstring> s_shownPins;   // keys ("<hasKids>|<path>") of the pins shown

static std::wstring pin_key(const NodeInfo& n) {
    return (n.hasKids ? L"1|" : L"0|") + pidl_folder_path(n.abs);
}

// replace the pins in the tree (takes the pidls)
static void show_pins(HWND tree, std::vector<NodeInfo>& pins, const std::vector<std::wstring>& keys) {
    TreeNoRedraw nr(tree);
    for (HTREEITEM r = TreeView_GetRoot(tree), next; r; r = next) {
        next = TreeView_GetNextSibling(tree, r);
        NodeData* d = node_data(tree, r);
        if (r == s_pinSep || (d && d->pinned)) TreeView_DeleteItem(tree, r);   // TVN_DELETEITEM frees
    }
    s_pinSep = nullptr;
    s_shownPins = keys;
    if (pins.empty()) return;
    HTREEITEM after = s_pinSep = insert_separator(tree, s_pinsAfter);
    for (auto& p : pins) after = insert_node(tree, TVI_ROOT, p, true, after);
}

struct PinsJob {
    UINT gen;
    std::vector<NodeInfo> pins;
    std::vector<std::wstring> keys;
};

static bool s_pinsCurrent = false;       // the worker's pins arrived (a later cache result is stale)

static void pins_apply(void* payload) {
    std::unique_ptr<PinsJob> job(static_cast<PinsJob*>(payload));
    if (g_tree && job->gen == s_treeGen) s_pinsCurrent = true;
    if (!g_tree || job->gen != s_treeGen || job->keys == s_shownPins) {   // the cache was right
        free_infos(job->pins);
        return;
    }
    show_pins(g_tree, job->pins, job->keys);
    settings_set_tree_pins(job->keys);
    if (perf_on()) perf_event("tree.pins.changed", -1, (long long)job->pins.size());
    tree_sync_to_current();             // a pin is the better match for itself
}

static void pins_worker(PinsJob* job) {
    ComInit ci;
    double t0 = perf_on() ? perf_now() : 0;
    for (auto p : pinned_folders()) {
        job->pins.push_back(describe(p, has_subfolders(p, g_showHidden)));
        job->keys.push_back(pin_key(job->pins.back()));
    }
    if (perf_on()) perf_event("tree.pins.read", perf_now() - t0, (long long)job->pins.size());
    post_async(pins_apply, job);
}

// the cached pins' names and icons (~3 ms per pin) come from a worker too; it is done
// well before the window's first message loop turn, so they show with the first paint
static void cached_pins_apply(void* payload) {
    std::unique_ptr<PinsJob> job(static_cast<PinsJob*>(payload));
    if (!g_tree || job->gen != s_treeGen || s_pinsCurrent) { free_infos(job->pins); return; }
    show_pins(g_tree, job->pins, job->keys);
    if (perf_on()) perf_event("tree.pins.cached", -1, (long long)job->keys.size());
    tree_sync_to_current();             // a pin is the better match for itself
}

static void cached_pins_worker(PinsJob* job) {
    ComInit ci;
    std::vector<std::wstring> keys;
    for (auto& key : job->keys) {
        if (key.size() < 3 || key[1] != L'|') continue;
        std::wstring path = key.substr(2);
        if (path.rfind(L"\\\\", 0) == 0) continue;     // network pins: only from the Quick access read
        if (PITEMID_CHILD abs = parse_shell(path.c_str())) {
            job->pins.push_back(describe(abs, key[0] == L'1'));
            keys.push_back(key);
        }
    }
    job->keys = std::move(keys);
    post_async(cached_pins_apply, job);
}

// row height and indent follow the DPI and the text size
void tree_apply_metrics() {
    if (!g_tree) return;
    TreeView_SetItemHeight(g_tree, SC(28));
    TreeView_SetIndent(g_tree, S(12));
}

void initialize_tree(HWND tree) {
    TreeView_SetImageList(tree, sys_imagelist(SHIL_SMALL), TVSIL_NORMAL);
    TreeView_SetExtendedStyle(tree, TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS | TVS_EX_AUTOHSCROLL,
                              TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS | TVS_EX_AUTOHSCROLL);
    tree_apply_metrics();
    TreeNoRedraw nr(tree);
    // Home, Gallery, OneDrive
    for (const wchar_t* p : { L"shell:::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}",      // Home
                              L"shell:::{e88865ea-0e1c-4e20-9aa6-edcd0212c87c}" }) {  // Gallery
        if (PITEMID_CHILD abs = parse_shell(p)) {
            NodeInfo n = describe(abs, false);
            insert_node(tree, TVI_ROOT, n);
        }
    }
    if (PITEMID_CHILD od = known_folder(FOLDERID_SkyDrive)) {
        if (!pidl_folder_path(od).empty() && PathIsDirectoryW(pidl_folder_path(od).c_str())) {
            NodeInfo n = describe(od, true);
            insert_node(tree, TVI_ROOT, n);
        } else {
            CoTaskMemFree(od);
        }
    }
    // pinned Quick access folders: the cached ones first, then the current ones
    s_pinsAfter = TreeView_GetRoot(tree);
    for (HTREEITEM n; s_pinsAfter && (n = TreeView_GetNextSibling(tree, s_pinsAfter));) s_pinsAfter = n;
    if (!s_pinsAfter) s_pinsAfter = TVI_FIRST;
    s_pinSep = nullptr;
    s_shownPins.clear();
    s_pinsCurrent = false;
    ++s_treeGen;
    auto* cached = new PinsJob{ s_treeGen, {}, settings_tree_pins() };
    auto* live = new PinsJob{ s_treeGen, {}, {} };
    worker_submit([cached] { cached_pins_worker(cached); });
    worker_submit([live] { pins_worker(live); });
    // This PC, Network
    insert_separator(tree);
    s_thisPC = nullptr;
    if (PITEMID_CHILD pc = known_folder(FOLDERID_ComputerFolder)) {
        NodeInfo n = describe(pc, true);
        s_thisPC = insert_node(tree, TVI_ROOT, n);
    }
    if (PITEMID_CHILD net = known_folder(FOLDERID_NetworkFolder)) {
        NodeInfo n = describe(net, true);
        insert_node(tree, TVI_ROOT, n);
    }
}

// ------------------------------------------------------------------
// sync / refresh

// find the node for target by walking down from h, expanding as needed. An ancestor
// whose children are not listed yet gets listed on a worker: `*waiting` is set and the
// walk stops there (expand_apply / prefetch_apply / refresh_apply resume it).
static HTREEITEM locate_in(HWND tree, HTREEITEM h, LPCITEMIDLIST target, bool expand, int depth,
                           bool* waiting = nullptr);
static void start_refresh(HTREEITEM h, NodeData* d);

// Nodes re-listed because the walk to the current target found its next level missing:
// each at most once per target, so a folder the tree doesn't show (inside a zip, hidden)
// costs one re-list, not one per sync.
static std::unordered_set<UINT> s_relisted;      // node serials
static ULONGLONG s_relistTarget = 0;

// the walk one level down from the listed node h; *missing: h has no child on the way
static HTREEITEM locate_below(HWND tree, HTREEITEM h, NodeData* d, LPCITEMIDLIST target, bool expand, int depth,
                              bool* waiting, bool* missing) {
    *missing = false;
    // fast path: the child whose ID is byte-equal to the target's next one (a shell
    // comparison binds every level: ~0.1 ms per child, 17 ms in a 98-folder directory)
    if (PITEMID_CHILD next = pidl_clone(target)) {
        int want = id_count(d->abs) + 1;
        for (int n = id_count(next); n > want; --n) ILRemoveLastID(next);
        LPCITEMIDLIST nextId = ILFindLastID(next);
        UINT cb = nextId->mkid.cb;
        HTREEITEM found = nullptr;
        for (HTREEITEM c = TreeView_GetChild(tree, h); c && !found; c = TreeView_GetNextSibling(tree, c)) {
            NodeData* cd = node_data(tree, c);
            if (!cd || cd->kind != NodeKind::Folder || ILIsEmpty(cd->abs)) continue;
            LPCITEMIDLIST id = ILFindLastID(cd->abs);
            if (id->mkid.cb == cb && memcmp(id, nextId, cb) == 0) found = c;
        }
        bool last = id_count(target) == want;
        CoTaskMemFree(next);
        if (found) return last ? found : locate_in(tree, found, target, expand, depth + 1, waiting);
    }
    bool onTheWay = false;
    for (HTREEITEM c = TreeView_GetChild(tree, h); c; c = TreeView_GetNextSibling(tree, c)) {
        NodeData* cd = node_data(tree, c);
        if (!cd || cd->kind != NodeKind::Folder) continue;
        if (pidl_equal(cd->abs, target)) return c;
        if (pidl_is_ancestor(cd->abs, target)) {
            onTheWay = true;
            HTREEITEM r = locate_in(tree, c, target, expand, depth + 1, waiting);
            if (r || (waiting && *waiting)) return r;
        }
    }
    *missing = !onTheWay;
    return nullptr;
}

static HTREEITEM locate_in(HWND tree, HTREEITEM h, LPCITEMIDLIST target, bool expand, int depth, bool* waiting) {
    if (!h || depth > 64) return nullptr;
    NodeData* d = node_data(tree, h);
    if (!d || d->kind != NodeKind::Folder) return nullptr;
    if (pidl_equal(d->abs, target)) return h;
    if (!pidl_is_ancestor(d->abs, target)) return nullptr;
    if (!d->listed) {
        if (!expand || !waiting || d->listFailed) return nullptr;
        int p = use_prefetch(tree, h, d);
        if (p <= 0) {
            if (p < 0) start_listing(h, d);
            *waiting = true;
            s_waitDepth = id_count(d->abs);
            return nullptr;
        }
    }
    if (expand && !(TreeView_GetItemState(tree, h, TVIS_EXPANDED) & TVIS_EXPANDED))
        TreeView_Expand(tree, h, TVE_EXPAND);
    bool missing = false;
    HTREEITEM r = locate_below(tree, h, d, target, expand, depth, waiting, &missing);
    // the next level isn't among h's children: made after h was listed, its change
    // notification not in yet (or never: only the folder shown is watched) — re-list h
    // on a worker, once, and walk on when it is back. (Only the level where it is missing:
    // a deeper failure doesn't re-list the ancestors.)
    if (!r && missing && expand && waiting && !*waiting && tree == g_tree && !s_relisted.count(d->serial)) {
        s_relisted.insert(d->serial);
        start_refresh(h, d);
        *waiting = true;
        s_waitDepth = id_count(target);     // nothing below h to prefetch
    }
    return r;
}

// the node showing `target`: a top-level item itself (Home, a pin, This PC), else
// the location under This PC (like Explorer's "expand to open folder"), else
// under any other root that contains it (OneDrive, Network, a pin)
// `pcTarget`: the same folder by way of This PC (see this_pc_route); *walked: the pidl
// the walk that is waiting followed
static HTREEITEM locate(HWND tree, LPCITEMIDLIST target, LPCITEMIDLIST pcTarget, bool expand, bool* waiting,
                        LPCITEMIDLIST* walked) {
    *walked = target;
    for (HTREEITEM r = TreeView_GetRoot(tree); r; r = TreeView_GetNextSibling(tree, r)) {
        NodeData* d = node_data(tree, r);
        if (d && d->kind == NodeKind::Folder && pidl_equal(d->abs, target)) return r;
    }
    if (pcTarget) *walked = pcTarget;
    if (HTREEITEM h = locate_in(tree, s_thisPC, pcTarget ? pcTarget : target, expand, 0, waiting)) return h;
    if (*waiting) return nullptr;
    *walked = target;
    for (HTREEITEM r = TreeView_GetRoot(tree); r; r = TreeView_GetNextSibling(tree, r)) {
        if (r == s_thisPC) continue;
        if (HTREEITEM h = locate_in(tree, r, target, expand, 0, waiting)) return h;
        if (*waiting) return nullptr;
    }
    return nullptr;
}

// A folder in the user's profile (C:\Users\me\AppData\...) parses to a pidl by way of the
// user's folder on the Desktop, not This PC > C: > Users: the walk under This PC wouldn't
// find it. This PC parses the path into its own route. Null: the target is fine as it is.
static PITEMID_CHILD this_pc_route(LPCITEMIDLIST target, const std::wstring& path) {
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return nullptr;
    PITEMID_CHILD pc = known_folder(FOLDERID_ComputerFolder);
    if (!pc) return nullptr;
    PITEMID_CHILD full = nullptr;
    com_ptr<IShellFolder> sf;
    if (!pidl_is_ancestor(pc, target) &&
        SUCCEEDED(SHBindToObject(nullptr, pc, nullptr, IID_PPV_ARGS(sf.put()))) && sf) {
        PIDLIST_RELATIVE rel = nullptr;
        std::wstring p = path;
        if (SUCCEEDED(sf->ParseDisplayName(nullptr, nullptr, p.data(), nullptr, &rel, nullptr)) && rel) {
            full = static_cast<PITEMID_CHILD>(ILCombine(pc, rel));
            CoTaskMemFree(rel);
        }
    }
    CoTaskMemFree(pc);
    return full;
}
static PITEMID_CHILD s_pcRoute = nullptr;        // for s_relistTarget

void tree_sync_to_current_deferred() {
    // tree sync on the UI thread can hit the disk; post so navigation isn't blocked.
    PostMessageW(g_main, WM_TREE_SYNC, 0, 0);
}

void tree_sync_to_current() {
    if (!g_tree || !g_treeVisible) return;     // (showing the pane syncs it)
    // search results keep the searched folder highlighted
    PCIDLIST_ABSOLUTE target = search_scope_of(g_current);
    if (!target) target = g_current;
    // network locations are listed only on demand: walking there could block
    std::wstring path = pidl_folder_path(target);
    bool remote = path.rfind(L"\\\\", 0) == 0;
    if (ULONGLONG sig = pidl_sig(target); sig != s_relistTarget) {
        s_relistTarget = sig;
        s_relisted.clear();
        if (s_pcRoute) CoTaskMemFree(s_pcRoute);
        s_pcRoute = remote ? nullptr : this_pc_route(target, path);
    }
    bool waiting = false;
    LPCITEMIDLIST walked = target;
    TreeNoRedraw nr(g_tree);                // one repaint for all the levels expanded
    HTREEITEM h = locate(g_tree, target, s_pcRoute, !remote, &waiting, &walked);
    s_syncPending = waiting;
    if (waiting) {                          // the selection follows when the listings arrive
        prefetch_ancestors(walked, s_waitDepth);
        return;
    }
    clear_prefetch();
    if (perf_on()) perf_event("tree.synced", -1, h ? 1 : 0);
    if (h) {
        TreeView_SelectItem(g_tree, h);     // programmatic: TVN_SELCHANGED ignores it
        TreeView_EnsureVisible(g_tree, h);
    } else {
        TreeView_SelectItem(g_tree, nullptr);
    }
}

void tree_refresh_all() {
    if (!g_tree) return;
    TreeNoRedraw nr(g_tree);
    clear_prefetch();                         // (may be stale: e.g. hidden files toggled)
    TreeView_DeleteAllItems(g_tree);          // TVN_DELETEITEM frees every node's data
    initialize_tree(g_tree);
    tree_sync_to_current();
}

static void refresh_all_cb(void*) { tree_refresh_all(); }
void tree_refresh_all_deferred() { post_async(refresh_all_cb, nullptr); }

static void set_has_children(HTREEITEM h, bool has) {
    TVITEMW ti{}; ti.mask = TVIF_CHILDREN; ti.hItem = h; ti.cChildren = has ? 1 : 0;
    TreeView_SetItem(g_tree, &ti);
}

// Re-listing a node on a worker. A listed node's children are diffed in place: unchanged
// children keep their item (and so their expanded subtree and the selection), vanished
// ones go, new ones are slotted into sort order. A node not listed yet only gets its
// expander updated (a first subfolder made, a card inserted into an empty drive).
struct RefreshJob {
    NodeData* node;
    UINT serial;
    HTREEITEM item;
    PITEMID_CHILD abs;              // owned copy for the worker
    bool showHidden;
    bool listed;                    // the node was listed when the job started
    std::vector<NodeInfo> kids;
    bool ok = false;
    bool hasKids = false;           // !listed: has a subfolder
    std::wstring keep;              // (see list_children)
};

static void refresh_apply(void* payload) {
    std::unique_ptr<RefreshJob> job(static_cast<RefreshJob*>(payload));
    CoTaskMemFree(job->abs);
    if (!g_tree || !node_alive(job->node, job->serial) || node_data(g_tree, job->item) != job->node) {
        free_infos(job->kids);
        return;
    }
    NodeData* d = job->node;
    HTREEITEM h = job->item;
    d->refreshing = false;
    HTREEITEM selBefore = TreeView_GetSelection(g_tree);
    if (!job->listed) {
        if (!d->listed) set_has_children(h, job->hasKids);
    } else if (job->ok && d->listed) {
        std::unordered_map<std::wstring, HTREEITEM> existing;
        for (HTREEITEM c = TreeView_GetChild(g_tree, h); c; c = TreeView_GetNextSibling(g_tree, c))
            if (NodeData* cd = node_data(g_tree, c)) existing.emplace(node_key(cd->abs), c);
        TreeNoRedraw nr(g_tree);
        HTREEITEM prev = TVI_FIRST;
        for (auto& k : job->kids) {
            auto it = existing.find(node_key(k.abs));
            if (it != existing.end()) {
                prev = it->second;
                // a child not listed yet: its expander follows what it has now
                if (NodeData* cd = node_data(g_tree, prev); cd && !cd->listed) set_has_children(prev, k.hasKids);
                existing.erase(it);
                CoTaskMemFree(k.abs);
                k.abs = nullptr;
            } else {
                prev = insert_node(g_tree, h, k, false, prev);
            }
        }
        for (auto& [key, c] : existing) TreeView_DeleteItem(g_tree, c);   // TVN_DELETEITEM frees
        set_has_children(h, !job->kids.empty());
        job->kids.clear();
    } else {
        free_infos(job->kids);      // couldn't be read now: keep what is shown
    }
    if (d->refreshAgain) {
        d->refreshAgain = false;
        start_refresh(h, d);
    }
    // the selected node went away (renamed / deleted): back on the current folder
    if (TreeView_GetSelection(g_tree) != selBefore) tree_sync_to_current();
    resume_sync();
}

static void refresh_worker(RefreshJob* job) {
    ComInit ci;
    if (job->listed) job->ok = list_children(job->abs, job->showHidden, job->kids, job->keep);
    else job->hasKids = has_subfolders(job->abs, job->showHidden);
    post_async(refresh_apply, job);
}

static void start_refresh(HTREEITEM h, NodeData* d) {
    if (!d || d->kind != NodeKind::Folder) return;
    if (d->refreshing) { d->refreshAgain = true; return; }
    if (!d->listed && d->listing) return;           // its listing is on the way anyway
    d->refreshing = true;
    auto* job = new RefreshJob{ d, d->serial, h, pidl_clone(d->abs), g_showHidden, d->listed, {} };
    job->keep = keep_target();
    worker_submit([job] { refresh_worker(job); });
}

void tree_refresh_node(PCIDLIST_ABSOLUTE folder) {
    if (!g_tree) return;
    // the folder can be shown more than once (under This PC and as a pin)
    for (HTREEITEM r = TreeView_GetRoot(g_tree); r; r = TreeView_GetNextSibling(g_tree, r))
        if (HTREEITEM h = locate_in(g_tree, r, folder, false, 0)) start_refresh(h, node_data(g_tree, h));
    // a profile folder's pidl doesn't lead under This PC: walk there by its This PC route
    std::wstring path = pidl_folder_path(folder);
    if (PITEMID_CHILD pc = path.rfind(L"\\\\", 0) == 0 ? nullptr : this_pc_route(folder, path)) {
        if (HTREEITEM h = locate_in(g_tree, s_thisPC, pc, false, 0)) start_refresh(h, node_data(g_tree, h));
        CoTaskMemFree(pc);
    }
}

PITEMID_CHILD tree_get_pidl_of_item(HTREEITEM h) {
    NodeData* d = node_data(g_tree, h);
    if (!d || d->kind != NodeKind::Folder) return nullptr;
    return pidl_clone(d->abs);
}

void tree_rename_selected() {
    if (HTREEITEM h = TreeView_GetSelection(g_tree)) TreeView_EditLabel(g_tree, h);
}

void tree_rename_menu_node() {
    if (HTREEITEM h = TreeView_GetDropHilight(g_tree)) TreeView_EditLabel(g_tree, h);
}

// the folder node after h (else the one before it, else its parent) — where the
// selection goes when h's folder is deleted
static HTREEITEM neighbor_folder(HTREEITEM h) {
    auto folder = [](HTREEITEM c) { NodeData* d = node_data(g_tree, c); return d && d->kind == NodeKind::Folder; };
    for (HTREEITEM c = TreeView_GetNextSibling(g_tree, h); c; c = TreeView_GetNextSibling(g_tree, c))
        if (folder(c)) return c;
    for (HTREEITEM c = TreeView_GetPrevSibling(g_tree, h); c; c = TreeView_GetPrevSibling(g_tree, c))
        if (folder(c)) return c;
    return TreeView_GetParent(g_tree, h);
}

// Delete in the navigation pane (Delete key, the menu's Delete) is done here rather than
// by the shell's verb, which runs on its own and leaves the pane and the list showing
// a folder that is gone. When the current folder (or one it is in) goes, the folder
// next to it in the pane opens instead, like a list's selection moving on.
static void delete_node(HTREEITEM h, bool permanent) {
    NodeData* d = node_data(g_tree, h);
    if (!d || d->kind != NodeKind::Folder || pidl_is_desktop(d->abs)) return;
    com_ptr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(bind_parent(d->abs, parent.put(), &child)) || !parent || !child) return;
    SFGAOF a = SFGAO_CANDELETE;
    if (FAILED(parent->GetAttributesOf(1, &child, &a)) || !(a & SFGAO_CANDELETE)) { MessageBeep(MB_ICONASTERISK); return; }
    PITEMID_CHILD abs = pidl_clone(d->abs);
    PITEMID_CHILD parentAbs = pidl_clone(d->abs);
    ILRemoveLastID(parentAbs);
    bool current = pidl_equal(abs, g_current) || pidl_is_ancestor(abs, g_current);
    PITEMID_CHILD next = nullptr;
    if (HTREEITEM n = neighbor_folder(h)) next = tree_get_pidl_of_item(n);
    if (!next) next = pidl_clone(parentAbs);

    PITEMID_CHILD own = pidl_clone(child);           // (the node may be gone after the delete)
    fileops_delete_items(g_main, parent.get(), { own }, permanent, false);
    CoTaskMemFree(own);
    // (cancelled at the confirmation, or failed: the folder is still there)
    if (!pidl_exists(abs) && current) {
        navigate_in_place(next);                     // Back must not lead to the deleted folder
        next = nullptr;
    } else {
        navigation_refresh();
    }
    if (next) CoTaskMemFree(next);
    tree_refresh_node(parentAbs);                    // the node goes; the selection follows the current folder
    CoTaskMemFree(parentAbs);
    CoTaskMemFree(abs);
}

void tree_delete_selected(bool permanent) {
    if (HTREEITEM h = TreeView_GetSelection(g_tree)) delete_node(h, permanent);
}

void tree_delete_menu_node(bool permanent) {
    if (HTREEITEM h = TreeView_GetDropHilight(g_tree)) delete_node(h, permanent);
}

// ------------------------------------------------------------------
// notifications

static LRESULT custom_draw(NMTVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            NodeData* d = (NodeData*)cd->nmcd.lItemlParam;
            if (d && d->kind == NodeKind::Separator) {
                RECT r = cd->nmcd.rc;
                fill_rect(cd->nmcd.hdc, r, pal().content);
                int y = (r.top + r.bottom) / 2;
                RECT line{ r.left + S(12), y, r.right - S(12), y + 1 };
                fill_rect(cd->nmcd.hdc, line, pal().divider);
                return CDRF_SKIPDEFAULT;
            }
            return d && d->pinned ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT;
        }
        case CDDS_ITEMPOSTPAINT: {
            RECT r = cd->nmcd.rc;
            RECT g{ r.right - S(28), r.top, r.right - S(8), r.bottom };
            draw_glyph(cd->nmcd.hdc, L'\xE718', g, pal().textSecondary, 12);   // pin
            return CDRF_DODEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

// the node an arrow key lands on past a separator: selected (and opened) as the key would
struct KeySelect { HTREEITEM item; NodeData* node; UINT serial; };

static void select_by_keyboard(void* payload) {
    std::unique_ptr<KeySelect> ks(static_cast<KeySelect*>(payload));
    if (!g_tree || !node_alive(ks->node, ks->serial) || node_data(g_tree, ks->item) != ks->node) return;
    TreeView_SelectItem(g_tree, ks->item);          // programmatic: TVN_SELCHANGED ignores it
    TreeView_EnsureVisible(g_tree, ks->item);
    if (!pidl_equal(ks->node->abs, g_current)) navigate_to(ks->node->abs);
}

LRESULT tree_notify(HWND, WPARAM, LPARAM lp) {
    NMHDR* hdr = (NMHDR*)lp;
    switch (hdr->code) {
        case NM_CUSTOMDRAW:
            return custom_draw((NMTVCUSTOMDRAW*)lp);
        case TVN_ITEMEXPANDINGW: {
            NM_TREEVIEWW* nm = (NM_TREEVIEWW*)lp;
            if (nm->action != TVE_EXPAND) return 0;
            NodeData* d = node_data(g_tree, nm->itemNew.hItem);
            if (!d || d->kind != NodeKind::Folder || d->listed) return 0;
            d->listFailed = false;            // the user asks again
            start_listing(nm->itemNew.hItem, d);
            return TRUE;                      // expands when the children arrive
        }
        case TVN_SELCHANGINGW: {
            NM_TREEVIEWW* nm = (NM_TREEVIEWW*)lp;
            NodeData* d = node_data(g_tree, nm->itemNew.hItem);
            if (!d || d->kind != NodeKind::Separator) return FALSE;
            // separators can't be selected; the arrow keys step over them
            if (nm->action == TVC_BYKEYBOARD) {
                HTREEITEM sep = nm->itemNew.hItem, from = nm->itemOld.hItem;
                bool down = false;
                for (HTREEITEM c = TreeView_GetPrevVisible(g_tree, sep); c; c = TreeView_GetPrevVisible(g_tree, c))
                    if (c == from) { down = true; break; }
                HTREEITEM to = down ? TreeView_GetNextVisible(g_tree, sep) : TreeView_GetPrevVisible(g_tree, sep);
                if (NodeData* td = node_data(g_tree, to); td && td->kind == NodeKind::Folder)
                    post_async(select_by_keyboard, new KeySelect{ to, td, td->serial });
            }
            return TRUE;
        }
        case TVN_SELCHANGEDW: {
            NM_TREEVIEWW* nm = (NM_TREEVIEWW*)lp;
            // only user selection navigates; programmatic syncs report TVC_UNKNOWN
            if (nm->action != TVC_BYMOUSE && nm->action != TVC_BYKEYBOARD) return 0;
            NodeData* d = node_data(g_tree, nm->itemNew.hItem);
            if (d && d->kind == NodeKind::Folder && !pidl_equal(d->abs, g_current)) navigate_to(d->abs);
            return 0;
        }
        case TVN_BEGINLABELEDITW: {
            auto* nm = (NMTVDISPINFOW*)lp;
            NodeData* d = node_data(g_tree, nm->item.hItem);
            if (!d || d->kind != NodeKind::Folder || pidl_is_desktop(d->abs)) return TRUE;
            com_ptr<IShellItem> si;
            SFGAOF a = 0;
            if (FAILED(shell_item_for(d->abs, si.put())) || !si ||
                FAILED(si->GetAttributes(SFGAO_CANRENAME, &a)) || !(a & SFGAO_CANRENAME))
                return TRUE;
            // edit the in-folder name ("DEV"), not a decorated display name
            com_ptr<IShellFolder> parent;
            PCUITEMID_CHILD child = nullptr;
            if (SUCCEEDED(bind_parent(d->abs, parent.put(), &child)) && parent) {
                std::wstring n = child_name(parent.get(), child, SHGDN_INFOLDER | SHGDN_FOREDITING);
                if (HWND ed = TreeView_GetEditControl(g_tree)) SetWindowTextW(ed, n.c_str());
            }
            return FALSE;
        }
        case TVN_ENDLABELEDITW: {
            auto* nm = (NMTVDISPINFOW*)lp;
            if (!nm->item.pszText || !*nm->item.pszText) return FALSE;
            NodeData* d = node_data(g_tree, nm->item.hItem);
            if (!d) return FALSE;
            com_ptr<IShellFolder> parent;
            PCUITEMID_CHILD child = nullptr;
            if (FAILED(bind_parent(d->abs, parent.put(), &child)) || !parent) return FALSE;
            PITEMID_CHILD parentAbs = pidl_clone(d->abs);
            ILRemoveLastID(parentAbs);
            // renaming the current folder or one of its ancestors: follow it (fs paths)
            std::wstring oldPath = pidl_folder_path(d->abs), cur = pidl_folder_path(g_current);
            bool follow = (pidl_equal(d->abs, g_current) || pidl_is_ancestor(d->abs, g_current)) &&
                          PathIsDirectoryW(oldPath.c_str()) && cur.size() >= oldPath.size();
            bool renamed = fileops_rename_item(g_main, parent.get(), child, nm->item.pszText);
            if (follow && renamed) {
                size_t slash = oldPath.find_last_of(L'\\');
                std::wstring moved = oldPath.substr(0, slash + 1) + nm->item.pszText + cur.substr(oldPath.size());
                PIDLIST_ABSOLUTE np = nullptr;
                if (slash != std::wstring::npos && PathIsDirectoryW(moved.c_str()) &&
                    SUCCEEDED(SHParseDisplayName(moved.c_str(), nullptr, &np, 0, nullptr)) && np)
                    navigate_in_place(reinterpret_cast<PITEMID_CHILD>(np));

            }
            tree_refresh_node(parentAbs);          // the node's pidl changed: re-list its parent
            CoTaskMemFree(parentAbs);
            tree_refresh_all_deferred();           // pins / other copies of the folder
            return FALSE;
        }
        case TVN_DELETEITEMW: {
            NM_TREEVIEWW* nm = (NM_TREEVIEWW*)lp;
            free_node((NodeData*)nm->itemOld.lParam);
            return 0;
        }
        case TVN_KEYDOWN: {
            auto* kd = (NMTVKEYDOWN*)lp;
            if (kd->wVKey == VK_DELETE) {
                // (posted: the delete may show its confirmation; not from inside a notification)
                bool shift = GetKeyState(VK_SHIFT) & 0x8000;
                PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(shift ? IDM_TREE_DELETE_PERM : IDM_TREE_DELETE, 0), 0);
            }
            return 0;
        }
    }
    return 0;
}
