#include "tabs.h"
#include <algorithm>
using std::max;
using std::min;
#include "navigation.h"
#include "navbar.h"
#include "enumerate.h"
#include "settings.h"
#include "theme.h"
#include "chrome.h"
#include "layout.h"
#include "searchbox.h"
#include <unordered_map>
#include <memory>
#include <atomic>
#include <thread>
#include "workers.h"
#include "acc.h"
#include "kbdrow.h"
#include "perf.h"
#include <cwctype>

// ------------------------------------------------------------------
// model

static std::vector<TabState> s_tabs;
static int s_nextTabId = 1;
static bool s_persist = true;        // this window owns the saved tab set
int g_activeTab = 0;

static void free_nav(PaneNav& n) {
    if (n.current) { CoTaskMemFree(n.current); n.current = nullptr; }
    for (auto p : n.back) CoTaskMemFree(p);
    for (auto p : n.fwd) CoTaskMemFree(p);
    n.back.clear(); n.fwd.clear();
}

static void free_tab(TabState& t) {
    for (auto& n : t.side) free_nav(n);
}

static std::wstring name_of(PCIDLIST_ABSOLUTE p) {
    std::wstring n = pidl_abs_dispname(p);
    if (n.empty()) n = pidl_folder_path(p);
    if (n.empty()) n = L"?";
    return n;
}

static TabState make_tab(PIDLIST_ABSOLUTE adoptCurrent) {
    TabState t;
    t.id = s_nextTabId++;
    t.side[0].current = adoptCurrent ? adoptCurrent : pidl_desktop();
    t.side[0].name = name_of(t.side[0].current);
    return t;
}

static TabState make_tab_named(PIDLIST_ABSOLUTE adoptCurrent, const std::wstring& name) {
    TabState t;
    t.id = s_nextTabId++;
    t.side[0].current = adoptCurrent ? adoptCurrent : pidl_desktop();
    t.side[0].name = name;
    return t;
}

static int index_of_id(int id) {
    for (int i = 0; i < (int)s_tabs.size(); ++i) if (s_tabs[i].id == id) return i;
    return -1;
}

TabState* tabmodel_active() {
    if (s_tabs.empty()) return nullptr;
    if (g_activeTab < 0 || g_activeTab >= (int)s_tabs.size()) g_activeTab = 0;
    return &s_tabs[g_activeTab];
}

void tabmodel_on_navigate(PIDLIST_ABSOLUTE newAbs, bool pushHistory) {
    TabState* t = tabmodel_active();
    if (!t) { CoTaskMemFree(newAbs); return; }
    if (!newAbs) newAbs = pidl_desktop();
    PaneNav& n = t->nav();
    if (n.current) {
        if (pushHistory) n.back.push_back(n.current);       // adopt old
        else CoTaskMemFree(n.current);
    }
    n.current = newAbs;                                     // adopt new
    if (pushHistory) { for (auto p : n.fwd) CoTaskMemFree(p); n.fwd.clear(); }
    // in place onto the folder right behind (a deleted folder's parent): Back skips it
    if (!pushHistory && !n.back.empty() && pidl_equal(n.back.back(), newAbs)) {
        CoTaskMemFree(n.back.back());
        n.back.pop_back();
    }
    n.name = name_of(newAbs);
    if (n.back.size() > 200) { CoTaskMemFree(n.back.front()); n.back.erase(n.back.begin()); }
    tabs_persist_soon();
}

// a pinned tab stays on its folder: no Back / Forward in its first list (the history
// comes back on unpin)
static bool history_fixed(const TabState* t) { return t->pinned && g_pane->index == 0; }
bool tabmodel_can_back() { TabState* t = tabmodel_active(); return t && !history_fixed(t) && !t->nav().back.empty(); }
bool tabmodel_can_forward() { TabState* t = tabmodel_active(); return t && !history_fixed(t) && !t->nav().fwd.empty(); }

bool tabmodel_go_back() {
    TabState* t = tabmodel_active();
    if (!tabmodel_can_back()) return false;
    PaneNav& n = t->nav();
    n.fwd.push_back(n.current);
    n.current = n.back.back();
    n.back.pop_back();
    n.name = name_of(n.current);
    tabs_persist_soon();
    return true;
}
bool tabmodel_go_forward() {
    TabState* t = tabmodel_active();
    if (!tabmodel_can_forward()) return false;
    PaneNav& n = t->nav();
    n.back.push_back(n.current);
    n.current = n.fwd.back();
    n.fwd.pop_back();
    n.name = name_of(n.current);
    tabs_persist_soon();
    return true;
}

// Put the active tab on screen. The tab that was there keeps its navigation pane's state.
static int s_shownId = 0;

static void show_active() {
    TabState* t = tabmodel_active();
    if (!t) return;
    int was = index_of_id(s_shownId);
    bool another = t->id != s_shownId;
    if (another && was >= 0) tree_save_state(s_tabs[was].tree);
    s_shownId = t->id;
    navigation_show_tab(t, another);
}

void tabmodel_switch_to(int idx) {
    if (idx < 0 || idx >= (int)s_tabs.size() || idx == g_activeTab) return;
    g_activeTab = idx;
    show_active();
    tabs_persist_soon();
}

void tabmodel_new_tab() {
    TabState t = make_tab(current_clone());
    t.side[0].view = g_pane->view;
    s_tabs.push_back(std::move(t));
    g_activeTab = (int)s_tabs.size() - 1;
    show_active();
    tabs_persist_soon();
    tabstrip_refresh();
}

// a folder opened from a pinned tab: its own tab, whose Back returns to the pinned folder
void tabmodel_open_in_new_tab(PIDLIST_ABSOLUTE abs) {
    PIDLIST_ABSOLUTE from = tabmodel_active() ? pidl_clone(tabmodel_active()->nav().current) : nullptr;
    TabState t = make_tab(abs);
    if (from) t.side[0].back.push_back(from);
    t.side[0].view = view_for_folder(t.side[0].current);
    s_tabs.push_back(std::move(t));
    g_activeTab = (int)s_tabs.size() - 1;
    show_active();
    tabs_persist_soon();
}

void tabmodel_show_active() { show_active(); }

bool tabmodel_is_pinned(int idx) {
    return idx >= 0 && idx < (int)s_tabs.size() && s_tabs[idx].pinned;
}

void tabmodel_close_tab(int idx) {
    if (idx < 0) idx = g_activeTab;
    if (idx < 0 || idx >= (int)s_tabs.size()) return;
    if (s_tabs[idx].pinned) return;              // pinned tabs can't be closed (Ctrl+W safe)
    if (s_tabs.size() <= 1) { PostMessageW(g_main, WM_CLOSE, 0, 0); return; }
    bool wasActive = idx == g_activeTab;
    free_tab(s_tabs[idx]);
    s_tabs.erase(s_tabs.begin() + idx);
    if (idx < g_activeTab) --g_activeTab;        // keep the same tab active
    if (g_activeTab >= (int)s_tabs.size()) g_activeTab = (int)s_tabs.size() - 1;
    if (wasActive) show_active();
    tabs_persist_soon();
    tabstrip_refresh();
}

// keep pinned tabs first (stable), keeping the active tab active
static void sort_pinned_first() {
    int activeId = tabmodel_active() ? tabmodel_active()->id : 0;
    std::stable_sort(s_tabs.begin(), s_tabs.end(),
                     [](const TabState& a, const TabState& b) { return a.pinned && !b.pinned; });
    g_activeTab = max(0, index_of_id(activeId));
}

void tabmodel_toggle_pin(int idx) {
    if (idx < 0 || idx >= (int)s_tabs.size()) return;
    s_tabs[idx].pinned = !s_tabs[idx].pinned;
    sort_pinned_first();
    tabstrip_refresh();
    navbar_refresh();                            // Back / Forward (off while pinned)
    tabs_persist_soon();
}

void tabmodel_duplicate_tab(int idx) {
    if (idx < 0 || idx >= (int)s_tabs.size()) return;
    // both lists' folders and views, the layout and the navigation pane as they are
    const TabState& src = s_tabs[idx];
    TabState t = make_tab(pidl_clone(src.side[0].current));
    for (int s = 0; s < kPanes; ++s) {
        if (!src.side[s].current) continue;
        if (s) t.side[s].current = pidl_clone(src.side[s].current);
        t.side[s].view = src.side[s].view;
        t.side[s].name = src.side[s].name;
    }
    t.dual = src.dual;
    t.activeSide = src.activeSide;
    if (idx == g_activeTab) tree_capture_state(s_tabs[idx].tree);   // (the one on screen: as it is now)
    t.tree = s_tabs[idx].tree;
    s_tabs.insert(s_tabs.begin() + idx + 1, std::move(t));
    g_activeTab = idx + 1;
    show_active();
    tabs_persist_soon();
    tabstrip_refresh();
}

void tabmodel_close_others(int idx) {
    if (idx < 0 || idx >= (int)s_tabs.size()) return;
    int keepId = s_tabs[idx].id;
    int activeId = tabmodel_active()->id;
    for (int i = (int)s_tabs.size() - 1; i >= 0; --i) {
        if (i != idx && !s_tabs[i].pinned) {
            free_tab(s_tabs[i]);
            s_tabs.erase(s_tabs.begin() + i);
        }
    }
    int stillActive = index_of_id(activeId);
    g_activeTab = stillActive >= 0 ? stillActive : max(0, index_of_id(keepId));
    if (stillActive < 0) show_active();
    tabs_persist_soon();
    tabstrip_refresh();
}

// move tab `from` so it lands before the tab currently at `to` (to == size: append).
// Returns the tab's new index.
int tabmodel_move_tab(int from, int to) {
    if (from < 0 || from >= (int)s_tabs.size()) return from;
    to = max(0, min(to, (int)s_tabs.size()));
    // pinned tabs stay first: a pinned tab moves among the pinned ones, others after them
    int pinnedCount = (int)std::count_if(s_tabs.begin(), s_tabs.end(), [](const TabState& t) { return t.pinned; });
    to = s_tabs[from].pinned ? min(to, pinnedCount) : max(to, pinnedCount);
    if (to == from || to == from + 1) return from;
    int activeId = tabmodel_active()->id;
    TabState t = std::move(s_tabs[from]);
    s_tabs.erase(s_tabs.begin() + from);
    if (to > from) --to;
    s_tabs.insert(s_tabs.begin() + to, std::move(t));
    g_activeTab = max(0, index_of_id(activeId));
    tabs_persist_soon();
    tabstrip_refresh();
    return to;
}

void tabmodel_next_tab(int delta) {
    if (s_tabs.size() < 2) return;
    int n = (int)s_tabs.size();
    int idx = (g_activeTab + delta + n) % n;
    tabmodel_switch_to(idx);
}

void tabmodel_switch_index_1_based(int one) {
    int idx = one - 1;
    if (one == 9 || idx >= (int)s_tabs.size()) idx = (int)s_tabs.size() - 1;   // Ctrl+9 = last
    tabmodel_switch_to(idx);
}

std::wstring tabmodel_active_name() {
    TabState* t = tabmodel_active();
    return t ? t->shown().name : L"";
}

// path used for tab persistence: prefer a plain fs path so it survives across
// runs (round-trips through SHParseDisplayName even for system folders);
// fall back to the shell parsing name for pure shell locations.
static std::wstring tab_persist_path(PCIDLIST_ABSOLUTE p) {
    if (pidl_is_desktop(p)) return L"::desktop";
    std::wstring s = pidl_folder_path(p);      // fs path when possible
    if (s.empty()) s = pidl_parsingname(p);     // shell parsing name
    if (!s.empty() && s.back() == L'\\' && s.size() > 3) s.pop_back();
    return s;
}

// the tab set changed: saved a moment later, once (a tab dragged across the strip, a few
// folders opened in a row), so it survives a crash without a write per step
static int s_persistRetries = 0;     // failed saves retried since the last change

void tabs_persist_soon() {
    s_persistRetries = 0;
    if (g_main && s_persist) SetTimer(g_main, TIMER_TABS_PERSIST, 2000, nullptr);
}

void tabs_persist_now() {
    if (g_main) KillTimer(g_main, TIMER_TABS_PERSIST);
    if (s_tabs.empty() || !s_persist) return;      // extra windows don't own the saved tab set
    PerfScope ps("tabs.persist");
    std::vector<std::wstring> lines;
    int active = 0;
    tabs_serialize_lines(lines, active);
    if (settings_set_tabs(lines, active)) { s_persistRetries = 0; return; }   // writes the ini immediately
    // not written (another window holding the file, a scanner): again after 1, 2, 4 s; then
    // the next change, or the settings saved at exit, write it
    if (g_main && s_persistRetries < 3) SetTimer(g_main, TIMER_TABS_PERSIST, 1000u << s_persistRetries++, nullptr);
}

static PIDLIST_ABSOLUTE parse_persisted(const std::wstring& path) {
    if (path.empty() || path == L"::desktop") return pidl_desktop();
    // network tabs open without asking the server (it may be offline: startup would wait
    // for its timeout); the listing, on a worker, finds out whether it is there
    if (path_is_network(path))
        if (PIDLIST_ABSOLUTE simple = SHSimpleIDListFromPath(path.c_str()))
            return simple;
    PIDLIST_ABSOLUTE p = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr)) && p)
        return p;
    // path no longer exists: walk to parent until it parses, else Desktop
    std::wstring fb = path;
    while (!fb.empty()) {
        size_t s1 = fb.find_last_of(L"\\/");
        if (s1 == std::wstring::npos) break;
        std::wstring up = fb.substr(0, s1);
        if (!up.empty() && up.back() == L':') up += L'\\';
        // a drive's root has no parent ("X:\" -> "X:\"): a removed drive ends here
        if (up.size() >= fb.size()) break;
        fb = std::move(up);
        if (SUCCEEDED(SHParseDisplayName(fb.c_str(), nullptr, &p, 0, nullptr)) && p)
            return p;
    }
    return pidl_desktop();
}

// The saved tabs' folders: each is parsed (the disk asked, ~1.4 ms), a few at a time, so
// eight tabs don't hold the window back for 11 ms. Shares keep their saved name (it needs
// no server); the others get the shell's.
struct SavedFolder {
    std::wstring path, savedName;
    PIDLIST_ABSOLUTE pidl = nullptr;
    std::wstring name;
};

static void parse_saved(std::vector<SavedFolder>& v) {
    std::atomic<size_t> next{ 0 };
    auto run = [&] {
        ComInit ci;
        for (size_t i; (i = next++) < v.size();) {
            SavedFolder& s = v[i];
            s.pidl = parse_persisted(s.path);
            s.name = path_is_network(s.path) && !s.savedName.empty() ? s.savedName : name_of(s.pidl);
        }
    };
    std::vector<std::thread> helpers;
    try {
        for (size_t k = 1; k < std::min<size_t>(4, v.size()); ++k) helpers.emplace_back(run);
    } catch (...) {}                // (no thread now: fewer do it)
    run();
    for (auto& t : helpers) t.join();
}

void tabs_deserialize(const std::wstring& tabs, int active) {
    for (auto& t : s_tabs) free_tab(t);
    s_tabs.clear();
    std::vector<std::vector<std::wstring>> lines;
    size_t pos = 0;
    while (pos < tabs.size()) {
        size_t nl = tabs.find(L'\n', pos);
        std::wstring line = nl == std::wstring::npos ? tabs.substr(pos) : tabs.substr(pos, nl - pos);
        pos = nl == std::wstring::npos ? tabs.size() : nl + 1;
        if (line.empty()) continue;
        std::vector<std::wstring> f;
        size_t p2 = 0;
        while (true) {
            size_t s = line.find(L'\x1', p2);   // \x01 separator
            f.push_back(s == std::wstring::npos ? line.substr(p2) : line.substr(p2, s - p2));
            if (s == std::wstring::npos) break;
            p2 = s + 1;
        }
        while (f.size() < 10) f.push_back(L"");
        lines.push_back(std::move(f));
    }
    // each tab's folder, and the second list's where it had one
    std::vector<SavedFolder> folders;
    for (auto& f : lines) {
        folders.push_back({ f[0], f[1] });
        if (!f[5].empty()) folders.push_back({ f[5], f[6] });
    }
    parse_saved(folders);
    size_t k = 0;
    for (auto& f : lines) {
        SavedFolder& first = folders[k++];
        TabState t = make_tab_named(first.pidl, first.name);
        t.side[0].view = int_to_view(_wtoi(f[2].c_str()));
        t.pinned = (f[3] == L"1");
        // the second list (a tab that had two once): its folder is kept while it is off
        if (!f[5].empty()) {
            SavedFolder& second = folders[k++];
            PaneNav& n = t.side[1];
            n.current = second.pidl;
            n.name = second.name;
            n.view = int_to_view(_wtoi(f[7].c_str()));
            t.dual = f[4] == L"1";
            t.activeSide = t.dual && f[8] == L"1" ? 1 : 0;
        }
        if (!f[9].empty()) {
            const wchar_t* p = f[9].c_str();
            wchar_t* end = nullptr;
            t.tree.top = _wcstoui64(p, &end, 16);
            while (end && *end == L',') {
                ULONGLONG key = _wcstoui64(end + 1, &end, 16);
                if (key) t.tree.expanded.insert(key);
            }
            t.tree.saved = true;
        }
        s_tabs.push_back(std::move(t));
    }    if (s_tabs.empty()) {
        // first run: Home, like Windows 11 (older systems: the user's profile)
        PIDLIST_ABSOLUTE dp = nullptr;
        if (FAILED(SHParseDisplayName(L"shell:::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}", nullptr, &dp, 0, nullptr)) &&
            FAILED(SHGetKnownFolderIDList(FOLDERID_Profile, 0, nullptr, &dp)))
            dp = nullptr;
        s_tabs.push_back(make_tab(dp));
    }
    // `active` indexes the saved order: pick the tab first, then let the sort keep it active
    g_activeTab = max(0, min(active, (int)s_tabs.size() - 1));
    sort_pinned_first();
    current_replace(pidl_clone(tabmodel_active()->side[0].current));
}

void tabs_serialize_lines(std::vector<std::wstring>& lines, int& outActive) {
    lines.clear();
    // search results live only in this session: such a tab reopens on the searched folder
    auto place = [](const PaneNav& n) {
        PCIDLIST_ABSOLUTE scope = search_scope_of(n.current);
        std::wstring name = scope ? pidl_abs_dispname(scope) : n.name;
        return tab_persist_path(scope ? scope : n.current) + L"\x1" + name + L"\x1" + std::to_wstring(view_to_int(n.view));
    };
    // the navigation pane: "top,expanded,expanded..." (hex node keys); the shown tab's as it is now
    auto tree = [](const TabState& t) {
        TreeState live;
        const TreeState* s = &t.tree;
        if (t.id == s_shownId) { tree_capture_state(live); s = &live; }
        if (!s->saved) return std::wstring();
        wchar_t buf[24];
        swprintf_s(buf, L"%llx", s->top);
        std::wstring out = buf;
        for (ULONGLONG k : s->expanded) { swprintf_s(buf, L",%llx", k); out += buf; }
        return out;
    };
    for (auto& t : s_tabs) {
        std::wstring line = place(t.side[0]) + L"\x1" + (t.pinned ? L"1" : L"0");
        if (t.side[1].current)
            line += std::wstring(L"\x1") + (t.dual ? L"1" : L"0") + L"\x1" + place(t.side[1]) + L"\x1" +
                    (t.activeSide == 1 ? L"1" : L"0");
        else
            line += L"\x1\x1\x1\x1\x1";
        line += L"\x1" + tree(t);
        lines.push_back(line);
    }
    outActive = g_activeTab;
}

void tabmodel_init(const std::wstring& openPath, bool restore) {
    std::wstring saved; int active = 0;
    s_persist = restore;
    if (restore) settings_tabs(saved, active);
    if (restore || openPath.empty()) tabs_deserialize(saved, active);
    if (openPath.empty()) return;
    // "explorer-plus.exe <folder>": open it in a new, active tab
    PIDLIST_ABSOLUTE p = nullptr;
    std::wstring netName;                   // a share's tab name, known without the server
    if (openPath == L"::desktop") {
        p = pidl_desktop();
    } else if (path_is_network(openPath)) {
        // a share or mapped drive opens without asking the server: it may be offline, and
        // the window would wait for its timeout before it even shows
        std::wstring path = openPath;
        while (path.size() > 3 && path.back() == L'\\') path.pop_back();
        p = SHSimpleIDListFromPath(path.c_str());
        netName = PathFindFileNameW(path.c_str());
    } else if (FAILED(parse_typed(openPath, &p))) {
        p = nullptr;
    }
    if (!p) {
        if (s_tabs.empty()) tabs_deserialize(L"", 0);      // fresh window, bad path: Home
        return;
    }
    // a file: its folder, with the file selected (like "Open file location")
    if (netName.empty() && !pidl_is_desktop(p)) {
        com_ptr<IShellItem> si;
        SFGAOF a = 0;
        if (SUCCEEDED(SHCreateItemFromIDList(p, IID_PPV_ARGS(si.put()))) && si &&
            SUCCEEDED(si->GetAttributes(SFGAO_FOLDER, &a)) && !(a & SFGAO_FOLDER)) {
            PWSTR name = nullptr;
            if (SUCCEEDED(si->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)) && name) {
                ILRemoveLastID(p);
                enum_select_on_arrival(name, p);
                CoTaskMemFree(name);
            }
        }
    }
    // already open: just activate it (the tab that was active, if it is one of them)
    for (int k = -1; k < (int)s_tabs.size(); ++k) {
        int i = k < 0 ? g_activeTab : k;
        if (i >= 0 && i < (int)s_tabs.size() && pidl_equal(s_tabs[i].side[0].current, p)) {
            CoTaskMemFree(p);
            g_activeTab = i;
            current_replace(pidl_clone(s_tabs[i].side[0].current));
            return;
        }
    }
    TabState t = netName.empty() ? make_tab(p)
                                 : make_tab_named(p, netName);
    t.side[0].view = view_for_folder(t.side[0].current);
    s_tabs.push_back(std::move(t));
    g_activeTab = (int)s_tabs.size() - 1;
    current_replace(pidl_clone(tabmodel_active()->side[0].current));
}

// ------------------------------------------------------------------
// tab strip window: Win11 tabs in the caption row (custom drawn)

static HWND s_strip = nullptr;
static int s_hoverTab = -1;
static KbdRow s_kbd;                     // keyboard focus: a tab, or "+" (index = tab count)
static bool s_hoverClose = false;
static bool s_hoverPlus = false;
static int s_draggingTab = -1;   // index of tab being drag-reordered
static bool s_dragMoved = false;
static POINT s_dragDownPt{};
// a button acts on release over what was pressed: a tab's close button (left) or the tab
// itself (middle), or "+"
static int s_pressClose = -1, s_pressMiddle = -1;
static bool s_pressPlus = false;
static std::unordered_map<ULONGLONG, int> s_iconCache;   // pidl signature -> system icon

struct TabUI { RECT rect; RECT close; };

// A tab's icon is looked up on a worker (a share's icon can take the server's timeout);
// the plain folder icon stands in until it is known.
struct TabIconJob { ULONGLONG sig; PIDLIST_ABSOLUTE abs; int icon; };
static constexpr int kIconPending = -2;

static void tab_icon_apply(void* payload) {
    std::unique_ptr<TabIconJob> job(static_cast<TabIconJob*>(payload));
    auto it = s_iconCache.find(job->sig);
    if (it == s_iconCache.end() || it->second != kIconPending) return;   // cache was cleared meanwhile
    it->second = job->icon;
    if (s_strip) InvalidateRect(s_strip, nullptr, FALSE);
}

static void tab_icon_worker(TabIconJob* job) {
    ComInit ci;
    job->icon = sys_icon_index(job->abs, false, sys_folder_icon_index(false));
    CoTaskMemFree(job->abs);
    job->abs = nullptr;
    post_async(tab_icon_apply, job);
}

static int tab_icon(PCIDLIST_ABSOLUTE p) {
    ULONGLONG sig = pidl_sig(p);
    auto it = s_iconCache.find(sig);
    if (it != s_iconCache.end()) return it->second == kIconPending ? sys_folder_icon_index(false) : it->second;
    if (s_iconCache.size() > 256) s_iconCache.clear();
    if (PIDLIST_ABSOLUTE abs = pidl_clone(p)) {
        s_iconCache[sig] = kIconPending;
        auto* job = new TabIconJob{ sig, abs, -1 };
        worker_submit([job] { tab_icon_worker(job); });
    }
    return sys_folder_icon_index(false);
}

static int tab_top() { return S(8); }

static void layout_tabs(int stripW, std::vector<TabUI>& out, RECT* plus) {
    out.clear();
    HDC hdc = s_strip ? GetDC(s_strip) : nullptr;
    HGDIOBJ of = hdc ? SelectObject(hdc, g_font) : nullptr;
    int h = layout_caption_h();
    int left = S(12);
    int avail = max((int)S(100), stripW - left - S(48) - S(40));   // "+" and a drag area stay visible
    std::vector<int> widths;
    int total = 0;
    for (auto& t : s_tabs) {
        int tw = hdc ? text_width(hdc, t.shown().name.c_str(), (int)t.shown().name.size()) : 0;
        int w = S(12) + S(16) + S(10) + tw + S(8) + (t.pinned ? S(4) : S(28)) + S(4);
        widths.push_back(max((int)S(96), min(w, (int)S(240))));
        total += widths.back();
    }
    if (hdc) { SelectObject(hdc, of); ReleaseDC(s_strip, hdc); }
    int n = (int)s_tabs.size();
    if (total > avail && n > 0) {
        int each = max((int)S(56), avail / n);
        for (auto& w : widths) w = min(w, each);
    }
    int x = left;
    for (int i = 0; i < n; ++i) {
        TabUI ui;
        ui.rect = { x, tab_top(), x + widths[i], h };
        int cs = S(24), cy = tab_top() + (h - tab_top() - cs) / 2;
        ui.close = { ui.rect.right - S(6) - cs, cy, ui.rect.right - S(6), cy + cs };
        out.push_back(ui);
        x += widths[i];
    }
    if (plus) {
        int ps = S(32), py = tab_top() + (h - tab_top() - ps) / 2;
        *plus = { x + S(6), py, x + S(6) + ps, py + ps };
    }
}

static int hit_tab(POINT pt, bool* onClose, bool* onPlus) {
    std::vector<TabUI> uis;
    RECT rc{}; if (s_strip) GetClientRect(s_strip, &rc);
    RECT plus{};
    layout_tabs(rc.right, uis, &plus);
    *onClose = false;
    if (onPlus) *onPlus = PtInRect(&plus, pt) != 0;
    for (size_t i = 0; i < uis.size(); ++i) {
        if (PtInRect(&uis[i].rect, pt)) {
            *onClose = !s_tabs[i].pinned && PtInRect(&uis[i].close, pt);
            return (int)i;
        }
    }
    return -1;
}

static void paint_strip(HWND hwnd) {
    BufferedPaint bp(hwnd);
    const Palette& p = pal();
    bool activeWnd = chrome_active();
    COLORREF bg = activeWnd ? p.caption : p.captionInactive;
    fill_rect(bp.dc, bp.rc, bg);
    std::vector<TabUI> uis;
    RECT plus{};
    layout_tabs(bp.rc.right, uis, &plus);
    COLORREF sepColor = blend(bg, p.text, 0.22);
    HIMAGELIST il = sys_imagelist(SHIL_SMALL);
    int iconW = 0, iconH = 0;
    ImageList_GetIconSize(il, &iconW, &iconH);     // (the list's own size, not this monitor's metric)
    for (size_t i = 0; i < uis.size(); ++i) {
        const TabUI& u = uis[i];
        bool active = (int)i == g_activeTab;
        bool hover = (int)i == s_hoverTab;
        if (active) {
            fill_tab_shape(bp.dc, u.rect, S(8), p.layer);
        } else if (hover) {
            RECT hr{ u.rect.left + S(2), u.rect.top + S(2), u.rect.right - S(2), u.rect.bottom - S(4) };
            fill_rrect(bp.dc, hr, S(6), p.tabHover);
        }
        // separator after an inactive tab unless a highlighted tab follows
        bool nextHighlighted = (int)i + 1 == g_activeTab || (int)i + 1 == s_hoverTab;
        if (!active && !hover && !nextHighlighted && i + 1 < uis.size()) {
            int cy = (u.rect.top + u.rect.bottom) / 2;
            RECT s{ u.rect.right - 1, cy - S(8), u.rect.right, cy + S(8) };
            fill_rect(bp.dc, s, sepColor);
        }
        int x = u.rect.left + S(12);
        int cy = (u.rect.top + u.rect.bottom) / 2;
        if (s_tabs[i].pinned) {
            RECT pr{ x, u.rect.top, x + S(16), u.rect.bottom };
            draw_glyph(bp.dc, L'\xE718', pr, active ? p.accent : p.textSecondary, 12);
        } else {
            int icon = tab_icon(s_tabs[i].shown().current);
            if (icon >= 0) ImageList_Draw(il, icon, bp.dc, x, cy - iconH / 2, ILD_TRANSPARENT);
        }
        x += S(16) + S(10);
        RECT tr{ x, u.rect.top, (s_tabs[i].pinned ? u.rect.right - S(8) : u.close.left - S(4)), u.rect.bottom };
        SetTextColor(bp.dc, activeWnd ? p.text : p.textSecondary);
        DrawTextW(bp.dc, s_tabs[i].shown().name.c_str(), -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (s_kbd.focus == (int)i) {
            RECT fr{ u.rect.left + S(2), u.rect.top + S(2), u.rect.right - S(2), u.rect.bottom - S(4) };
            kbdrow_ring(bp.dc, fr, S(6));
        }
        if (!s_tabs[i].pinned) {
            if (hover && s_hoverClose) fill_rrect(bp.dc, u.close, S(4), blend(active ? p.layer : p.tabHover, p.text, 0.08));
            draw_glyph(bp.dc, L'\xE711', u.close, p.text, 10);
        }
    }
    if (s_hoverPlus) fill_rrect(bp.dc, plus, S(4), blend(bg, p.text, 0.07));
    draw_glyph(bp.dc, L'\xE710', plus, p.text, 12);
    if (s_kbd.focus == (int)uis.size()) kbdrow_ring(bp.dc, plus, S(4));
}

// screen readers: each tab (selected = the active one), its close button, then "New tab"
struct AccTabsItem { int tab; bool close; bool plus; };
static std::vector<AccTabsItem> acc_items() {
    std::vector<AccTabsItem> v;
    for (int i = 0; i < (int)s_tabs.size(); ++i) {
        v.push_back({ i, false, false });
        if (!s_tabs[i].pinned) v.push_back({ i, true, false });
    }
    v.push_back({ -1, false, true });
    return v;
}
static const AccSource s_acc = [] {
    AccSource a;
    a.name = L"Tabs";
    a.role = ROLE_SYSTEM_PAGETABLIST;
    a.count = [] { return (int)acc_items().size(); };
    a.child = [](int i) {
        AccChild c;
        AccTabsItem it = acc_items()[i];
        std::vector<TabUI> uis;
        RECT rc{}, plus{};
        GetClientRect(s_strip, &rc);
        layout_tabs(rc.right, uis, &plus);
        if (it.plus) {
            c.name = L"New tab"; c.shortcut = L"Ctrl+T"; c.action = L"Press"; c.rc = plus;
        } else if (it.close) {
            c.name = L"Close tab " + s_tabs[it.tab].shown().name; c.shortcut = L"Ctrl+W"; c.action = L"Press";
            c.rc = uis[it.tab].close;
        } else {
            c.name = s_tabs[it.tab].shown().name + (s_tabs[it.tab].pinned ? L" (pinned)" : L"");
            c.role = ROLE_SYSTEM_PAGETAB;
            c.state = STATE_SYSTEM_SELECTABLE | (it.tab == g_activeTab ? STATE_SYSTEM_SELECTED : 0);
            c.action = L"Switch";
            c.rc = uis[it.tab].rect;
        }
        return c;
    };
    a.focus = [] {
        auto v = acc_items();
        for (int n = 0; n < (int)v.size(); ++n)
            if ((v[n].plus && s_kbd.focus == (int)s_tabs.size()) || (!v[n].close && !v[n].plus && v[n].tab == s_kbd.focus))
                return n;
        return -1;
    };
    a.action = [](int i) {
        AccTabsItem it = acc_items()[i];
        if (it.plus) tabmodel_new_tab();
        else if (it.close) tabmodel_close_tab(it.tab);
        else tabmodel_switch_to(it.tab);
    };
    return a;
}();

// keyboard: Left / Right through the tabs and "+", Enter / Space switches to the tab (or
// opens one), Delete closes it
static void strip_press(int i) {
    if (i >= (int)s_tabs.size()) tabmodel_new_tab();
    else if (i != g_activeTab) tabmodel_switch_to(i);
}

static LRESULT CALLBACK strip_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETFOCUS) s_kbd.last = g_activeTab;           // (the ring starts on the active tab)
    LRESULT kr = 0;
    if (s_kbd.handle(hwnd, msg, wp, &kr, (int)s_tabs.size() + 1, [](int) { return true; }, strip_press))
        return kr;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_DELETE && s_kbd.focus >= 0 && s_kbd.focus < (int)s_tabs.size()) {
                tabmodel_close_tab(s_kbd.focus);                // (pinned: kept)
                if (s_kbd.focus >= (int)s_tabs.size() + 1) s_kbd.focus = (int)s_tabs.size();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_GETOBJECT: return acc_get_object(hwnd, wp, lp, &s_acc);
        case WM_NCHITTEST: {
            // empty strip area belongs to the title bar (drag, double-click, snap)
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            bool oc = false, op = false;
            if (s_draggingTab < 0 && hit_tab(pt, &oc, &op) < 0 && !op) return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint_strip(hwnd); return 0;
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (s_draggingTab >= 0) {
                // small threshold so a plain click doesn't reorder
                if (!s_dragMoved) {
                    if (abs(pt.x - s_dragDownPt.x) < GetSystemMetrics(SM_CXDRAG) &&
                        abs(pt.y - s_dragDownPt.y) < GetSystemMetrics(SM_CYDRAG))
                        return 0;
                    s_dragMoved = true;
                }
                RECT rc{}; GetClientRect(hwnd, &rc);
                std::vector<TabUI> uis;
                layout_tabs(rc.right, uis, nullptr);
                int to = s_draggingTab;
                for (size_t i = 0; i < uis.size(); ++i) {
                    if (pt.x < (uis[i].rect.left + uis[i].rect.right) / 2) { to = (int)i; break; }
                    to = (int)i + 1;
                }
                if (to != s_draggingTab && to != s_draggingTab + 1)
                    s_draggingTab = tabmodel_move_tab(s_draggingTab, to);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            bool oc = false, op = false;
            int t = hit_tab(pt, &oc, &op);
            if (t != s_hoverTab || oc != s_hoverClose || op != s_hoverPlus) {
                s_hoverTab = t; s_hoverClose = oc; s_hoverPlus = op;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (s_draggingTab < 0) {
                s_hoverTab = -1; s_hoverClose = false; s_hoverPlus = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool oc = false, op = false;
            int t = hit_tab(pt, &oc, &op);
            if (op || (t >= 0 && oc)) {
                s_pressPlus = op;
                s_pressClose = op ? -1 : t;
                SetCapture(hwnd);
                return 0;
            }
            if (t >= 0) {
                if (t != g_activeTab) tabmodel_switch_to(t);
                s_draggingTab = t;         // potential drag-reorder; released on LBUTTONUP without move
                s_dragMoved = false;
                s_dragDownPt = pt;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (s_draggingTab >= 0) {
                s_draggingTab = -1;
                ReleaseCapture();
                return 0;
            }
            int pc = s_pressClose;
            bool pp = s_pressPlus;
            if (pc < 0 && !pp) return 0;
            ReleaseCapture();                                   // (clears the press)
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool oc = false, op = false;
            int t = hit_tab(pt, &oc, &op);
            if (pp && op) tabmodel_new_tab();
            else if (pc >= 0 && oc && t == pc) tabmodel_close_tab(t);
            return 0;
        }
        case WM_CAPTURECHANGED:
            s_draggingTab = -1;
            s_pressClose = s_pressMiddle = -1;
            s_pressPlus = false;
            return 0;
        case WM_RBUTTONUP: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool oc = false;
            int t = hit_tab(pt, &oc, nullptr);
            if (t < 0) return 0;
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"New tab\tCtrl+T");
            AppendMenuW(m, MF_STRING, 2, L"Duplicate tab");
            AppendMenuW(m, MF_STRING, 3, s_tabs[t].pinned ? L"Unpin tab" : L"Pin tab");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING | (s_tabs[t].pinned ? MF_GRAYED : 0), 4, L"Close tab\tCtrl+W");
            AppendMenuW(m, MF_STRING, 5, L"Close other tabs");
            POINT sp = pt; ClientToScreen(hwnd, &sp);
            UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON, sp.x, sp.y, hwnd, nullptr);
            DestroyMenu(m);
            switch (cmd) {
                case 1: tabmodel_new_tab(); break;
                case 2: tabmodel_duplicate_tab(t); break;
                case 3: tabmodel_toggle_pin(t); break;
                case 4: tabmodel_close_tab(t); break;
                case 5: tabmodel_close_others(t); break;
            }
            return 0;
        }
        case WM_MBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool oc = false;
            int t = hit_tab(pt, &oc, nullptr);
            if (t >= 0 && s_draggingTab < 0 && s_pressClose < 0 && !s_pressPlus) {
                s_pressMiddle = t;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MBUTTONUP: {
            int pm = s_pressMiddle;
            if (pm < 0) return 0;
            ReleaseCapture();
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool oc = false;
            int t = hit_tab(pt, &oc, nullptr);
            if (t == pm) tabmodel_close_tab(t);   // close_tab itself ignores pinned
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND tabstrip_create(HWND parent) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = strip_proc;
    wc.lpszClassName = L"ExplorerPlusTabStrip";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    s_strip = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                              0, 0, 100, layout_caption_h(), parent, (HMENU)IDC_TABSTRIP, g_hinst, nullptr);
    return s_strip;
}

HWND tabstrip_hwnd() { return s_strip; }

void tabstrip_refresh() {
    if (!s_strip) return;
    InvalidateRect(s_strip, nullptr, FALSE);
    // the tab list or the active tab may have changed
    acc_notify(s_strip, EVENT_OBJECT_REORDER, -1);
    for (int i = 0, n = 0; i < (int)s_tabs.size(); ++i, ++n) {
        if (i == g_activeTab) { acc_notify(s_strip, EVENT_OBJECT_SELECTION, n); break; }
        if (!s_tabs[i].pinned) ++n;                     // (its close button)
    }
}