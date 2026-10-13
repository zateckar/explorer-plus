#include "appstate.h"
#include "perf.h"
#include "workers.h"

// globals
HWND g_main = nullptr, g_address = nullptr, g_search = nullptr,
     g_tree = nullptr, g_status = nullptr, g_navbar = nullptr;
HFONT g_font = nullptr;
HFONT g_iconFont = nullptr;   // Segoe Fluent Icons
HFONT g_mdl2Font = nullptr;   // Segoe MDL2 Assets
HINSTANCE g_hinst = nullptr;
int  g_splitterX = 280, g_statusH = 24;
int  g_dualSplit = 500;
std::atomic<bool> g_showHidden{ false };
std::atomic<int> g_showExtensions{ -1 };
bool g_checkBoxes = false;
bool g_compactView = false;
bool g_cmdBarVisible = false;
bool g_treeVisible = true;

// the file lists
Pane g_panes[kPanes];
static const bool s_indexed = [] {
    for (int i = 0; i < kPanes; ++i) g_panes[i].index = i;
    return true;
}();
static Pane* s_active = &g_panes[0];
Pane* g_pane = &g_panes[0];
bool g_dual = false;
static int s_scopeDepth = 0;             // PaneScopes open

Pane* active_pane() { return s_active; }

void pane_set_active(Pane* p) {
    s_active = p;
    if (!s_scopeDepth) g_pane = p;       // (else the outermost scope ends on it)
}

Pane* pane_of_list(HWND list) {
    for (Pane& p : g_panes)
        if (p.list && p.list == list) return &p;
    return nullptr;
}

PaneScope::PaneScope(Pane* p) : m_saved(g_pane) {
    ++s_scopeDepth;
    if (p) g_pane = p;
}

PaneScope::~PaneScope() { g_pane = --s_scopeDepth ? m_saved : s_active; }

bool windows_shows_extensions() {
    SHELLSTATEW ss{};
    SHGetSetSettings(&ss, SSF_SHOWEXTENSIONS, FALSE);
    return ss.fShowExtensions != 0;
}

// Explorer's own "Compact view" (read only)
bool windows_compact_view() {
    DWORD v = 0, cb = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                        L"UseCompactMode", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS && v != 0;
}

bool show_extensions_effective() {
    int pref = g_showExtensions.load();
    return pref < 0 ? windows_shows_extensions() : pref != 0;
}

// ---- async result queue ----
struct AsyncEntry { AsyncHandler apply; void* payload; };
static std::mutex s_asyncMx;
static std::deque<AsyncEntry> s_async;

static bool s_wakePosted = false;          // a WM_ASYNC_WAKE is on its way (under s_asyncMx)

void post_async(AsyncHandler apply, void* payload) {
    if (workers_stopping()) return;        // exiting: the window is going, results are dropped
    bool wake;
    {
        std::lock_guard<std::mutex> lk(s_asyncMx);
        s_async.push_back({ apply, payload });
        wake = !s_wakePosted;              // one wake message per batch
        s_wakePosted = true;
    }
    // a wake that couldn't be posted (queue full, no window yet) is retried by the next post
    if (wake && !(g_main && PostMessageW(g_main, WM_ASYNC_WAKE, 0, 0))) {
        std::lock_guard<std::mutex> lk(s_asyncMx);
        s_wakePosted = false;
    }
}

void drain_async() {
    std::deque<AsyncEntry> batch;
    {
        std::lock_guard<std::mutex> lk(s_asyncMx);
        batch.swap(s_async);
        s_wakePosted = false;
    }
    if (workers_stopping()) return;
    double tAll = perf_on() ? perf_now() : 0;
    for (auto& e : batch) {
        if (!perf_on()) { e.apply(e.payload); continue; }
        double t0 = perf_now();
        e.apply(e.payload);
        double dur = perf_now() - t0;
        if (dur > 8) {                      // name the handler: see perf.h
            WCHAR d[64]{};
            swprintf_s(d, L"handler=%llx", perf_code_address((const void*)e.apply));
            perf_event("slow.async", dur, -1, d);
        }
    }
    if (perf_on() && perf_now() - tAll > 16) perf_event("async.drain", perf_now() - tAll, (long long)batch.size());
}

void fix_font(HWND h)  { if (g_font) SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE); }

PIDLIST_ABSOLUTE current_clone() { return pidl_clone(g_pane->current); }

void current_replace(PIDLIST_ABSOLUTE newAbs) {
    if (!newAbs) newAbs = pidl_desktop();
    if (g_pane->current) CoTaskMemFree(g_pane->current);
    g_pane->current = newAbs;
}

std::wstring current_display_path() { return pidl_editingname(g_pane->current); }
