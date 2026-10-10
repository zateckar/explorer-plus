#include "enumerate.h"
#include "listview.h"
#include "thumbs.h"
#include "detailspane.h"
#include "groups.h"
#include "columns.h"
#include <propsys.h>
#include <thread>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cwctype>
#include "perf.h"

// ---------------------------------------------------------------
// item lifecycle

// type names by extension ("Text Document"); the same for every file with that extension
static std::mutex s_typeMx;
static std::unordered_map<std::wstring, std::wstring> s_typeByExt;

static std::wstring fs_type_for(const std::wstring& name, bool folder) {
    if (folder) {
        static std::wstring folderType;
        std::lock_guard<std::mutex> lk(s_typeMx);
        if (folderType.empty()) {
            SHFILEINFOW fi{};
            SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &fi, sizeof(fi),
                           SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES);
            folderType = fi.szTypeName[0] ? fi.szTypeName : L"File folder";
        }
        return folderType;
    }
    std::wstring ext = PathFindExtensionW(name.c_str());
    CharLowerBuffW(ext.data(), (DWORD)ext.size());
    {
        std::lock_guard<std::mutex> lk(s_typeMx);
        auto it = s_typeByExt.find(ext);
        if (it != s_typeByExt.end()) return it->second;
    }
    SHFILEINFOW fi{};
    std::wstring probe = L"x" + ext;
    SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi),
                   SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES);
    std::wstring t = fi.szTypeName;
    std::lock_guard<std::mutex> lk(s_typeMx);
    s_typeByExt[ext] = t;
    return t;
}

// extensions Explorer hides when "File name extensions" is off: those of registered types
static bool registered_ext(const std::wstring& ext) {
    std::wstring key = ext;
    CharLowerBuffW(key.data(), (DWORD)key.size());
    static std::unordered_map<std::wstring, bool> known;
    {
        std::lock_guard<std::mutex> lk(s_typeMx);
        auto it = known.find(key);
        if (it != known.end()) return it->second;
    }
    HKEY h = nullptr;
    bool reg = RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS;
    if (h) RegCloseKey(h);
    std::lock_guard<std::mutex> lk(s_typeMx);
    known[key] = reg;
    return reg;
}

// file name extensions: ours vs. Windows' setting (the shell's names follow Windows')
struct ExtMode { bool show = true; bool windows = true; };

static void apply_ext_choice(Item* it, const std::wstring& fileName, ExtMode ext) {
    if (ext.show == ext.windows || fileName.empty()) return;
    const wchar_t* dot = PathFindExtensionW(fileName.c_str());
    std::wstring e = dot;
    if (e.empty() || e.size() == fileName.size()) return;
    std::wstring base = fileName.substr(0, fileName.size() - e.size());
    if (ext.show) {
        // Windows hid it: show the file name (only where that is all the shell left out)
        if (it->name == base) { it->name = fileName; it->extForced = true; }
    } else if (it->name == fileName && registered_ext(e)) {
        it->name = base;
        it->hiddenExt = e;
    }
}

static bool has_extra_columns(const ColumnSet& cols) {
    for (auto& c : cols)
        if (c.kind == COL_EXTRA && c.extra >= 0) return true;
    return false;
}

// Overlays (shortcut arrow, OneDrive / Git states...) ask every registered handler about
// every item: about half of a big folder's listing time. The listing goes out without
// them; the worker then works them out and they follow in batches, like in Explorer.
static Item* make_item(IShellFolder* psf, IShellIcon* icons, PCIDLIST_ABSOLUTE folderAbs,
                       PITEMID_CHILD child, SFGAOF attrs, bool drives, const ColumnSet& cols, ExtMode ext) {
    Item* it = new Item;
    it->pidl = child;        // adopted
    it->sig  = pidl_sig(child);
    it->attrs = attrs;
    it->isFolder = (attrs & SFGAO_FOLDER) != 0;
    it->name = child_name(psf, child, SHGDN_INFOLDER);

    // file-system items carry their size and dates in the ID: no property store (which
    // costs as much again as everything else per item) unless a column needs one
    bool plainFolder = it->isFolder && !(attrs & SFGAO_STREAM);
    WIN32_FIND_DATAW fd{};
    bool haveFd = (attrs & SFGAO_FILESYSTEM) &&
                  SUCCEEDED(SHGetDataFromIDListW(psf, child, SHGDFIL_FINDDATA, &fd, sizeof(fd)));
    com_ptr<IShellItem2> si;
    if (!haveFd || has_extra_columns(cols)) create_item(psf, child, IID_PPV_ARGS(si.put()));
    if (haveFd) {
        if (fd.ftLastWriteTime.dwLowDateTime || fd.ftLastWriteTime.dwHighDateTime) {
            it->modified = fd.ftLastWriteTime;
            it->date = fmt_filetime(fd.ftLastWriteTime);
        }
        if (!plainFolder) {
            it->bytes = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            it->size = fmt_size(it->bytes);
        }
    } else if (si) {
        FILETIME ft{};
        if (SUCCEEDED(si->GetFileTime(PKEY_DateModified, &ft))) {
            it->modified = ft;
            it->date = fmt_filetime(ft);
        }
        if (!it->isFolder) {
            ULONGLONG bytes = 0;
            if (SUCCEEDED(si->GetUInt64(PKEY_Size, &bytes))) {
                it->bytes = bytes;
                it->size = fmt_size(bytes);
            }
        }
    }
    // zip/cab archives are SFGAO_FOLDER|SFGAO_STREAM: they list as files with a type name
    bool fsItem = (attrs & SFGAO_FILESYSTEM) != 0;
    WCHAR root[MAX_PATH]{};
    if (drives && fsItem) {
        // This PC: drive roots get their type + space for the tiles. Network drives are
        // skipped for space (a dead server would stall the listing)
        PIDLIST_ABSOLUTE full = ILCombine(folderAbs, child);
        if (full && SHGetPathFromIDListW(full, root) && PathIsRootW(root) && root[1] == L':') {
            it->driveType = GetDriveTypeW(root);
            ULARGE_INTEGER freeToCaller{}, total{}, freeTotal{};
            if (it->driveType != DRIVE_REMOTE &&
                SHGetDiskFreeSpaceExW(root, &freeToCaller, &total, &freeTotal)) {
                it->driveTotal = total.QuadPart;
                it->driveFree = freeToCaller.QuadPart;
                it->size = fmt_size(it->driveTotal);     // Size column: total size
            }
        }
        if (full) CoTaskMemFree(full);
    }
    if (fsItem && !it->driveType) {
        std::wstring parsing = child_name(psf, child, SHGDN_INFOLDER | SHGDN_FORPARSING);
        it->type = fs_type_for(parsing.empty() ? it->name : parsing, plainFolder);
        it->fileName = PathFindFileNameW(parsing.c_str());     // (search results: a full path)
        // shortcuts keep their extension hidden whatever the setting, like Explorer
        if (!plainFolder && !(attrs & SFGAO_LINK)) apply_ext_choice(it, it->fileName, ext);
    }
    if (it->type.empty() && si) {
        PWSTR s = nullptr;
        if (SUCCEEDED(si->GetString(PKEY_ItemTypeText, &s)) && s) { it->type = s; CoTaskMemFree(s); }
    }
    if (attrs & SFGAO_STREAM) it->isFolder = false;   // archives sort/size like files

    int idx = -1;
    if (!icons || icons->GetIconOf(child, GIL_FORSHELL, &idx) != S_OK) {
        PITEMID_CHILD full = static_cast<PITEMID_CHILD>(ILCombine(folderAbs, child));
        idx = full ? sys_icon_index(full) : 0;
        if (full) CoTaskMemFree(full);
    }
    it->icon = idx;
    columns_fill(si.get(), cols, it);      // the folder's extra columns (after driveType)
    return it;
}

static void free_item(Item* it) {
    if (!it) return;
    if (it->pidl) CoTaskMemFree(it->pidl);
    delete it;
}

static void free_items(std::vector<Item*>& v) {
    for (Item* it : v) free_item(it);
    v.clear();
}

// ---------------------------------------------------------------
// sort

static int cmp_item(const Item* a, const Item* b) {
    int r = 0;
    switch (g_sortColumn) {
        case COL_SIZE:
            // drives (This PC) sort by capacity
            if (a->bytes + a->driveTotal != b->bytes + b->driveTotal)
                r = a->bytes + a->driveTotal < b->bytes + b->driveTotal ? -1 : 1;
            break;
        case COL_TYPE:
            r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE,
                                a->type.c_str(), -1, b->type.c_str(), -1, nullptr, nullptr, 0) - CSTR_EQUAL;
            break;
        case COL_DATE: {
            LONG c = CompareFileTime(&a->modified, &b->modified);
            r = c < 0 ? -1 : (c > 0 ? 1 : 0);
            break;
        }
        case COL_EXTRA:
            r = columns_compare_extra(a, b);
            break;
        default:  // name
            r = StrCmpLogicalW(a->name.c_str(), b->name.c_str());
            break;
    }
    if (!g_sortAsc) r = -r;
    return r;
}

// grouped views sort by group first, so each group is one contiguous range
static bool item_less(const Item* a, const Item* b) {
    if (a->group != b->group) return a->group < b->group;
    if (groups_keep_enum_order()) return a->seq < b->seq;
    if (a->isFolder != b->isFolder) return a->isFolder;    // folders first
    int r = cmp_item(a, b);
    if (r == 0) r = StrCmpLogicalW(a->name.c_str(), b->name.c_str());
    return r < 0;
}

static void sort_items() {
    columns_prepare_sort();
    groups_assign(g_items);
    std::stable_sort(g_items.begin(), g_items.end(), item_less);
    groups_record(g_items);
}

// a streamed batch joins the sorted listing: sort just the batch and merge it in, so
// a 100k-item folder is not re-sorted for every batch. Grouped views re-sort all: a new
// group (a new type, say) can renumber the others.
static void merge_batch(std::vector<Item*>& batch) {
    if (groups_effective() != GB_NONE || groups_keep_enum_order()) {
        g_items.insert(g_items.end(), batch.begin(), batch.end());
        sort_items();
        return;
    }
    columns_prepare_sort();
    groups_assign(batch);
    std::stable_sort(batch.begin(), batch.end(), item_less);
    size_t mid = g_items.size();
    g_items.insert(g_items.end(), batch.begin(), batch.end());
    std::inplace_merge(g_items.begin(), g_items.begin() + mid, g_items.end(), item_less);
    groups_record(g_items);
}

// ---------------------------------------------------------------
// enumeration worker

// (this MinGW's uuid library lacks the IID)
static const IID kIID_IShellIconOverlay =
    { 0x7D688A70, 0xC613, 0x11D0, { 0x99, 0x9B, 0x00, 0xC0, 0x4F, 0xD6, 0x55, 0xE1 } };

// A new listing streams in: the worker drops its items here and the UI takes what has
// arrived after 60 ms, then every 250 ms (TIMER_ENUM_STREAM), so a huge folder or a slow
// search shows its first rows at once. The remainder comes with WM_ENUM_DONE (a small
// folder usually finishes before the first tick and shows in one step). A refresh does
// not stream: it replaces the shown listing in one step.
static constexpr UINT kFirstStreamMs = 60, kStreamMs = 250;
struct StreamBuf {
    UINT opId = 0;
    std::mutex mx;
    std::vector<Item*> items;
    ColumnSet columns;                // set before the first item
    bool closed = false;              // the worker took the rest for WM_ENUM_DONE
    ~StreamBuf() { free_items(items); }
};

struct EnumJob {
    UINT opId = 0;
    PITEMID_CHILD folder = nullptr;   // absolute pidl, owned
    bool showHidden = false;
    ExtMode ext;
    ColumnSet columns;                // empty: the folder's default columns (read by the worker)
    std::shared_ptr<StreamBuf> stream;   // null: collect everything for WM_ENUM_DONE
};

// overlays worked out after the listing went out: (seq, overlay or 0) per item
struct OverlayBatch {
    UINT opId;
    std::vector<std::pair<int, int>> results;
};

static void overlay_apply(void* payload) {
    std::unique_ptr<OverlayBatch> b(static_cast<OverlayBatch*>(payload));
    if (b->opId != g_enumId.load() || b->results.empty()) return;   // another listing by now
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        std::unordered_map<int, Item*> bySeq;
        bySeq.reserve(g_items.size());
        for (Item* it : g_items) bySeq.emplace(it->seq, it);
        for (auto [seq, ov] : b->results) {
            auto f = bySeq.find(seq);
            if (f != bySeq.end()) f->second->overlay = ov;
        }
    }
    InvalidateRect(g_list, nullptr, FALSE);
}

static void overlay_pass(UINT opId, IShellIconOverlay* overlays, std::vector<std::pair<int, PITEMID_CHILD>>& ids) {
    double t0 = perf_on() ? perf_now() : 0;
    auto batch = std::make_unique<OverlayBatch>();
    batch->opId = opId;
    size_t done = 0, with = 0;
    for (auto& [seq, child] : ids) {
        if (opId != g_enumId.load()) break;          // navigated on: the rest is not needed
        int ov = 0;
        if (FAILED(overlays->GetOverlayIndex(child, &ov)) || ov < 0 || ov > 15) ov = 0;
        batch->results.push_back({ seq, ov });
        with += ov != 0;
        // the first rows' overlays go out quickly, the rest in big batches
        if (++done == 200 || done % 5000 == 0) {
            post_async(overlay_apply, batch.release());
            batch = std::make_unique<OverlayBatch>();
            batch->opId = opId;
        }
    }
    if (!batch->results.empty()) post_async(overlay_apply, batch.release());
    for (auto& [seq, child] : ids) CoTaskMemFree(child);
    ids.clear();
    if (perf_on()) {
        WCHAR d[48];
        swprintf_s(d, L"with=%zu", with);
        perf_event("enum.overlays", perf_now() - t0, (long long)done, d);
    }
}

static void worker(EnumJob* job) {
    ComInit ci;
    HeapEnumOp* op = new HeapEnumOp;
    op->opId = job->opId;
    UINT opId = job->opId;

    com_ptr<IShellFolder> psf;
    com_ptr<IShellIconOverlay> overlays;
    std::vector<std::pair<int, PITEMID_CHILD>> overlayIds;   // (seq, copy of the item's ID)
    HRESULT hr = ci.ok() ? bind_list(job->folder, IID_PPV_ARGS(psf.put())) : ci.hr;
    if (SUCCEEDED(hr) && psf) {
        com_ptr<IShellIcon> icons;
        psf->QueryInterface(IID_PPV_ARGS(icons.put()));
        psf->QueryInterface(kIID_IShellIconOverlay, (void**)overlays.put());
        // Home: pinned files make up the Favorites group
        com_ptr<IShellFolder2> home;
        PROPERTYKEY kPinned{};
        if (is_home_folder(job->folder) &&
            SUCCEEDED(PSGetPropertyKeyFromName(L"System.Home.IsPinned", &kPinned)))
            psf->QueryInterface(IID_PPV_ARGS(home.put()));
        bool thisPC = !home && is_this_pc_folder(job->folder);
        op->columns = job->columns.empty() ? columns_defaults(psf.get(), job->folder) : job->columns;
        if (job->stream) {
            std::lock_guard<std::mutex> lk(job->stream->mx);
            job->stream->columns = op->columns;
        }
        int seq = 0;
        double tEnum = perf_on() ? perf_now() : 0;
        com_ptr<IEnumIDList> e;
        SHCONTF flags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_FASTITEMS;
        if (job->showHidden) flags |= SHCONTF_INCLUDEHIDDEN;   // protected OS files stay hidden
        hr = psf->EnumObjects(g_main, flags, e.put());
        if (hr == S_OK && e) {
            PITEMID_CHILD child = nullptr; ULONG fetched = 0;
            while (e->Next(1, &child, &fetched) == S_OK && fetched) {
                if (job->opId != g_enumId.load()) {  // cancelled
                    CoTaskMemFree(child);
                    break;
                }
                SFGAOF attrs = SFGAO_HIDDEN | SFGAO_SYSTEM | SFGAO_FOLDER | SFGAO_STREAM |
                               SFGAO_FILESYSTEM | SFGAO_LINK | SFGAO_SHARE | SFGAO_GHOSTED |
                               SFGAO_DROPTARGET;
                psf->GetAttributesOf(1, const_cast<PCUITEMID_CHILD*>(&child), &attrs);
                if (overlays)
                    if (PITEMID_CHILD copy = pidl_clone(child)) overlayIds.push_back({ seq, copy });
                Item* it = make_item(psf.get(), icons.get(), job->folder, child, attrs, thisPC, op->columns, job->ext);
                if (seq == 0 && perf_on()) perf_event("enum.first_item", perf_now() - tEnum);
                it->seq = seq++;
                if (home && !it->isFolder) {
                    VARIANT v; VariantInit(&v);
                    it->homePinned = SUCCEEDED(home->GetDetailsEx(child, &kPinned, &v)) &&
                                     v.vt == VT_BOOL && v.boolVal;
                    VariantClear(&v);
                }
                if (job->stream) {
                    std::lock_guard<std::mutex> lk(job->stream->mx);
                    job->stream->items.push_back(it);
                } else {
                    op->items.push_back(it);
                }
            }
            hr = S_OK;
            if (perf_on()) perf_event("enum.worker_done", perf_now() - tEnum, seq);
        } else if (hr == S_FALSE) {
            hr = S_OK;    // user cancelled a prompt (e.g. empty drive) — just empty
        }
    }
    if (job->stream) {                       // whatever the UI has not taken yet
        std::lock_guard<std::mutex> lk(job->stream->mx);
        op->items = std::move(job->stream->items);
        job->stream->items.clear();
        job->stream->closed = true;
    }
    op->enumHr = hr;
    if (job->folder) CoTaskMemFree(job->folder);
    delete job;                              // (drops the worker's share of the stream)
    if (!PostMessageW(g_main, WM_ENUM_DONE, 0, (LPARAM)op)) {
        free_items(op->items);
        delete op;
    }
    // (posted after WM_ENUM_DONE, so the listing is applied before its overlays)
    if (overlays) overlay_pass(opId, overlays.get(), overlayIds);
}

static ULONGLONG s_shownFolderSig = 0;    // folder whose listing is on screen
static ULONGLONG s_pendingFolderSig = 0;  // folder being enumerated
static bool s_pendingKeepView = false;
static std::shared_ptr<StreamBuf> s_stream;   // the listing streaming in, if any
static UINT s_streamShown = 0;            // listing whose first batch is on screen
static std::wstring s_lastError;
static std::wstring s_pendingSelect;
static bool s_pendingRename = false;
static std::unordered_set<std::wstring> s_pendingSelSnapshot;   // selection when it was requested

static std::wstring item_name_at(int idx) {
    if (idx < 0 || idx >= (int)g_items.size()) return {};
    return g_items[idx]->name;
}

static std::unordered_set<std::wstring> selected_names() {
    std::unordered_set<std::wstring> out;
    std::lock_guard<std::mutex> lk(g_itemsMx);
    int i = -1;
    while ((i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) != -1) out.insert(item_name_at(i));
    return out;
}

// select (and optionally rename) `name` when the next listing arrives — unless the
// user has picked something else by then
void enum_set_pending_select(const std::wstring& name, bool beginRename) {
    s_pendingSelect = name;
    s_pendingRename = beginRename;
    s_pendingSelSnapshot = selected_names();
}

const std::wstring& enum_last_error() { return s_lastError; }

static void stream_stop() {
    KillTimer(g_main, TIMER_ENUM_STREAM);
    s_stream.reset();
    s_streamShown = 0;
}

void enum_clear_items() {
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        free_items(g_items);
    }
    s_shownFolderSig = 0;
    groups_clear();
    ListView_SetItemCount(g_list, 0);
}

void enumerate_start(PCIDLIST_ABSOLUTE folderAbs, bool keepView) {
    UINT id = ++g_enumId;
    if (perf_on()) perf_event("enum.start", -1, id, pidl_folder_path(folderAbs).c_str());
    s_pendingFolderSig = pidl_sig(folderAbs);
    s_pendingKeepView = keepView && s_pendingFolderSig == s_shownFolderSig;
    if (!s_pendingKeepView) enum_clear_items();
    EnumJob* job = new EnumJob;
    job->opId = id;
    job->folder = folderAbs ? pidl_clone(folderAbs) : pidl_desktop();
    job->showHidden = g_showHidden.load();
    job->ext.windows = windows_shows_extensions();
    job->ext.show = g_showExtensions.load() < 0 ? job->ext.windows : g_showExtensions.load() != 0;
    // a refresh keeps the shown columns; a new folder uses its remembered set or defaults
    job->columns = s_pendingKeepView ? g_columns : columns_for_folder(job->folder);
    stream_stop();
    if (!s_pendingKeepView) {
        s_stream = std::make_shared<StreamBuf>();
        s_stream->opId = id;
        job->stream = s_stream;
        SetTimer(g_main, TIMER_ENUM_STREAM, kFirstStreamMs, nullptr);
    }
    std::thread(worker, job).detach();
}

void enum_on_stream_timer() {
    if (!s_stream || s_stream->opId != g_enumId.load()) { stream_stop(); return; }
    // a rename box sits on a row: let it be until the user is done (the rest still
    // arrives with WM_ENUM_DONE)
    if (ListView_GetEditControl(g_list)) return;
    HeapEnumOp* op = new HeapEnumOp;
    op->opId = s_stream->opId;
    op->final = false;
    {
        std::lock_guard<std::mutex> lk(s_stream->mx);
        if (!s_stream->closed && !s_stream->items.empty()) {
            op->items = std::move(s_stream->items);
            s_stream->items.clear();
            op->columns = s_stream->columns;
        }
    }
    if (op->items.empty()) { delete op; return; }
    bool first = s_streamShown != op->opId;
    enum_apply_op(op);
    if (first && s_stream) SetTimer(g_main, TIMER_ENUM_STREAM, kStreamMs, nullptr);
}

// ---------------------------------------------------------------
// UI apply

static void set_error(HRESULT hr) {
    s_lastError.clear();
    if (SUCCEEDED(hr)) return;
    WCHAR msg[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   (DWORD)hr, 0, msg, ARRAYSIZE(msg), nullptr);
    s_lastError = msg[0] ? msg : L"This location could not be opened.";
    while (!s_lastError.empty() && iswspace(s_lastError.back())) s_lastError.pop_back();
}

// a later batch of the listing on screen: merge it in. The user may already be working
// with the first rows, so selection and focus follow their items, not their row numbers.
static void append_batch(HeapEnumOp* op) {
    PerfScope ps("enum.append");
    ps.n = (long long)op->items.size();
    std::vector<int> reselect;
    int focusIdx = -1, n = 0;
    bool hadSelection = false;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        std::unordered_set<Item*> sel;
        Item* focus = nullptr;
        for (int i = -1; (i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) != -1;)
            if (i < (int)g_items.size()) sel.insert(g_items[i]);
        int f = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED);
        if (f >= 0 && f < (int)g_items.size()) focus = g_items[f];
        hadSelection = !sel.empty() || focus;
        merge_batch(op->items);
        op->items.clear();                         // (now owned by g_items)
        n = (int)g_items.size();
        if (hadSelection)
            for (int i = 0; i < n; ++i) {
                if (sel.count(g_items[i])) reselect.push_back(i);
                if (g_items[i] == focus) focusIdx = i;
            }
    }
    // (not under the lock: these notify handlers that take it)
    if (hadSelection) ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    groups_apply();
    ListView_SetItemCountEx(g_list, n, LVSICF_NOSCROLL);
    for (int i : reselect) ListView_SetItemState(g_list, i, LVIS_SELECTED, LVIS_SELECTED);
    if (focusIdx >= 0) ListView_SetItemState(g_list, focusIdx, LVIS_FOCUSED, LVIS_FOCUSED);
    InvalidateRect(g_list, nullptr, TRUE);
    status_update();
}

void enum_apply_op(HeapEnumOp* op) {
    if (!op) return;
    if (op->opId != g_enumId.load()) {   // superseded listing: drop
        free_items(op->items);
        delete op;
        return;
    }
    if (op->opId == s_streamShown) {     // the rest of a listing already on screen
        append_batch(op);
        if (op->final) {
            if (perf_on()) perf_event("enum.complete", -1, (long long)g_items.size());
            stream_stop();
            set_error(op->enumHr);
            detailspane_update();
        }
        delete op;
        return;
    }
    PerfScope ps(op->final ? "enum.apply_all" : "enum.apply_first");
    ps.n = (long long)op->items.size();
    if (op->final) stream_stop();
    else s_streamShown = op->opId;       // first rows of a streaming listing
    // capture selection to restore on refresh (LVSICF_NOSCROLL keeps the scroll position)
    std::unordered_set<std::wstring> selected;
    std::wstring focused;
    bool keep = s_pendingKeepView;
    if (keep) {
        selected = selected_names();
        std::lock_guard<std::mutex> lk(g_itemsMx);
        focused = item_name_at(ListView_GetNextItem(g_list, -1, LVNI_FOCUSED));
    }
    // a pending select is for the first listing after the request; the user choosing
    // other items meanwhile (or navigating away) cancels it
    std::wstring pendingName;
    bool pendingRename = false;
    if (!s_pendingSelect.empty() && keep && selected == s_pendingSelSnapshot) {
        pendingName = s_pendingSelect;
        pendingRename = s_pendingRename;
    }
    s_pendingSelect.clear();
    s_pendingRename = false;
    s_pendingSelSnapshot.clear();

    // the header first (not under g_itemsMx: it repaints through LVN_GETDISPINFO), then
    // the items it describes
    columns_apply(op->columns);
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        if (keep) {     // a refresh: overlays stay until the overlay pass has them (no blinking arrows)
            std::unordered_map<ULONGLONG, int> ov;
            for (Item* it : g_items)
                if (it->overlay) ov.emplace(it->sig, it->overlay);
            if (!ov.empty())
                for (Item* it : op->items)
                    if (auto f = ov.find(it->sig); f != ov.end()) it->overlay = f->second;
        }
        free_items(g_items);
        g_items = std::move(op->items);
        sort_items();
    }
    s_shownFolderSig = s_pendingFolderSig;
    set_error(op->enumHr);
    thumb_reset_for_new_folder();
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    groups_apply();
    ListView_SetItemCountEx(g_list, (int)g_items.size(), keep ? LVSICF_NOSCROLL : 0);
    enum_update_header_arrows();

    // restore selection / focus, then apply a pending "select this new item"
    int focusIdx = -1, pendingIdx = -1;
    std::vector<int> reselect;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        for (int i = 0; i < (int)g_items.size(); ++i) {
            const std::wstring& n = g_items[i]->name;
            // (the request may name the file, "a.txt", where the list shows "a")
            if (!pendingName.empty() && (StrCmpIW(n.c_str(), pendingName.c_str()) == 0 ||
                                         StrCmpIW(g_items[i]->fileName.c_str(), pendingName.c_str()) == 0))
                pendingIdx = i;
            if (!keep) continue;
            if (selected.count(n)) reselect.push_back(i);
            if (n == focused) focusIdx = i;
        }
    }
    // (state changes notify LVN_ITEMCHANGED, whose handlers take g_itemsMx — so not under the lock)
    for (int i : reselect) ListView_SetItemState(g_list, i, LVIS_SELECTED, LVIS_SELECTED);
    if (focusIdx >= 0) ListView_SetItemState(g_list, focusIdx, LVIS_FOCUSED, LVIS_FOCUSED);
    if (pendingIdx >= 0) {
        ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
        ListView_SetItemState(g_list, pendingIdx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_list, pendingIdx, FALSE);
        if (pendingRename) {
            SetFocus(g_list);
            ListView_EditLabel(g_list, pendingIdx);
        }
    }
    InvalidateRect(g_list, nullptr, TRUE);
    status_update();
    detailspane_update();
    if (op->final && perf_on()) perf_event("enum.complete", -1, (long long)g_items.size());
    delete op;
}

void enum_sort_items() {
    PerfScope ps("sort");
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_itemsMx);
        sort_items();
        n = (int)g_items.size();
    }
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    groups_apply();                                  // group ranges move with the order
    ListView_SetItemCountEx(g_list, n, LVSICF_NOSCROLL);
    enum_update_header_arrows();
    InvalidateRect(g_list, nullptr, TRUE);
}

void enum_update_header_arrows() {
    HWND hdr = ListView_GetHeader(g_list);
    if (!hdr) return;
    int n = Header_GetItemCount(hdr);
    int sortCol = list_sort_display_column();
    bool all = g_checkBoxes && list_all_selected();
    for (int i = 0; i < n; ++i) {
        HDITEMW hdi{};
        hdi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hdi);
        int fmt = hdi.fmt & ~(HDF_SORTUP | HDF_SORTDOWN | HDF_CHECKBOX | HDF_CHECKED);
        if (i == sortCol) fmt |= g_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        if (i == 0 && g_checkBoxes) fmt |= HDF_CHECKBOX | (all ? HDF_CHECKED : 0);   // select all
        if (fmt == hdi.fmt) continue;
        hdi.fmt = fmt;
        Header_SetItem(hdr, i, &hdi);
    }
}
