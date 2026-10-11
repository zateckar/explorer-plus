#include "appstate.h"
#include "perf.h"
#include "workers.h"

// globals
HWND g_main = nullptr, g_address = nullptr, g_search = nullptr,
     g_tree = nullptr, g_list = nullptr, g_status = nullptr, g_navbar = nullptr;
HFONT g_font = nullptr;
HFONT g_iconFont = nullptr;   // Segoe Fluent Icons
HFONT g_mdl2Font = nullptr;   // Segoe MDL2 Assets
HINSTANCE g_hinst = nullptr;
View g_view{ LV_VIEW_DETAILS, 0 };
int  g_splitterX = 280, g_statusH = 24;
std::atomic<bool> g_showHidden{ false };
std::atomic<int> g_showExtensions{ -1 };
bool g_checkBoxes = false;
bool g_compactView = false;
bool g_cmdBarVisible = false;
bool g_treeVisible = true;
int  g_sortColumn = COL_NAME;
PROPERTYKEY g_sortKey{};
bool g_sortAsc = true;

// items + current folder
std::mutex g_itemsMx;
std::vector<Item*> g_items;
PIDLIST_ABSOLUTE g_current = nullptr;

std::atomic<UINT> g_enumId{ 0 };

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
        if (dur > 8) {                      // name the handler: resolve with `nm -C` on the exe
            WCHAR d[64]{};
            swprintf_s(d, L"handler=%p", (void*)e.apply);
            perf_event("slow.async", dur, -1, d);
        }
    }
    if (perf_on() && perf_now() - tAll > 16) perf_event("async.drain", perf_now() - tAll, (long long)batch.size());
}

void fix_font(HWND h)  { if (g_font) SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE); }

PIDLIST_ABSOLUTE current_clone() { return pidl_clone(g_current); }

void current_replace(PIDLIST_ABSOLUTE newAbs) {
    if (!newAbs) newAbs = pidl_desktop();
    if (g_current) CoTaskMemFree(g_current);
    g_current = newAbs;
}

std::wstring current_display_path() { return pidl_editingname(g_current); }
