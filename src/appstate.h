#pragma once
#include "shell_utils.h"

// Central app state shared by all modules (all defined in appstate.cpp)

// --- view modes (persisted per folder) ---
enum GroupBy : int { GB_AUTO = 0, GB_NONE, GB_NAME, GB_DATE, GB_TYPE, GB_SIZE, GB_MAX = GB_SIZE };

struct View {
    UINT lvView      = LV_VIEW_DETAILS;
    int  thumbsExtra = 0;   // icon views: 0 = shell icons, thumbnails 1 = 96px (Large), 2 = 256px (Extra large), 3 = 48px (Medium)
    int  groupBy     = GB_AUTO;   // AUTO: the folder's own grouping (Home, This PC), else none
};

// view application (defined in main.cpp; uses settings + listview)
void set_view_mode(const View& v, bool persist);

// --- details columns (columns.cpp) ---
// built-in columns read fixed Item fields; the codes double as g_sortColumn values
enum ColKind : int { COL_NAME = 0, COL_SIZE = 1, COL_TYPE = 2, COL_DATE = 3, COL_EXTRA = 4, COL_SYNC = 5 };   // COL_SYNC: Sync status (sync_ui)

struct Column {
    PROPERTYKEY  key{};
    std::wstring title;
    int width96 = 120;          // width at 96 dpi
    int fmt     = LVCFMT_LEFT;
    int kind    = COL_EXTRA;
    int extra   = -1;           // COL_EXTRA: index into Item::extra / extraRaw
};
using ColumnSet = std::vector<Column>;

// --- items of the content pane ---
struct Item {
    std::wstring    name;      // in-folder display name (file name extensions per g_showExtensions)
    std::wstring    fileName;  // file-system items: the real in-folder name ("a.txt")
    std::wstring    path;      // full parsing path where the folder gives one (search results, Home), else empty
    std::wstring    hiddenExt; // the extension `name` leaves out where Windows itself would show it
    bool            extForced = false;      // `name` shows an extension Windows would hide
    std::wstring    size;      // formatted display size; folders: empty until worked out (foldersizes)
    std::wstring    type;
    std::wstring    date;
    ULONGLONG       bytes = 0;
    FILETIME        modified{};
    PITEMID_CHILD   pidl = nullptr;         // child pidl relative to current folder
    // Identity. `key` names the item across listings and changes: selection, focus,
    // overlays and change notifications go by it (lowercased path where the folder gives
    // one, else file name, else display name). `sig` hashes the ID, which also holds size
    // and date: the thumbnail goes by it, so a changed file gets a new one.
    std::wstring    key;
    ULONGLONG       sig  = 0;
    SFGAOF          attrs = 0;
    int             icon = 0;               // system image list index
    bool            ownIcon = false;        // an icon of its own (a program's...), read after the listing
    bool            iconPending = false;    // ownIcon: the type's icon shows until it is read
    int             overlay = 0;            // 1-based overlay index (0 = none)
    bool            isFolder = false;
    bool            homePinned = false;     // Home: a pinned (Favorites) file
    int             seq = 0;                // enumeration order (Home keeps the shell's order)
    int             group = 0;              // group rank when the view is grouped
    UINT            driveType = 0;          // DRIVE_* for drive roots (This PC), else 0
    ULONGLONG       driveTotal = 0, driveFree = 0;
    std::atomic<int> thumbReady{ 0 };       // 0 = not requested, 1 = queued, 2 = ready
    int             thumbIndex = -1;        // index into thumb imagelist (set when ready)
    // values of the folder's extra columns (Column::extra indexes these): text + raw for sorting
    std::vector<std::wstring> extra;
    std::vector<PROPVARIANT>  extraRaw;
    Item() = default;
    Item(const Item&) = delete;
    Item& operator=(const Item&) = delete;
    ~Item() { for (auto& v : extraRaw) PropVariantClear(&v); }
};

// --- file lists ("panes"): the main list, and in a dual-pane tab a second one beside it ---
constexpr int kPanes = 2;
struct Pane {
    int index = 0;                       // 0: the main (left) list, 1: the second one
    HWND list = nullptr;
    std::mutex itemsMx;
    std::vector<Item*> items;            // UI thread owns; workers only read under itemsMx
    PIDLIST_ABSOLUTE current = nullptr;  // folder shown: never nullptr after startup (empty pidl = Desktop)
    std::atomic<UINT> enumId{ 0 };       // listing ids: results carrying a stale one are dropped
    View view{ LV_VIEW_DETAILS, 0 };
    ColumnSet columns;                   // columns of the shown listing
    int sortColumn = COL_NAME;           // ColKind; COL_EXTRA sorts by sortKey
    PROPERTYKEY sortKey{};
    bool sortAsc = true;
};
extern Pane g_panes[kPanes];
// The pane being worked on (UI thread): the active one, or the one a notification, timer
// or worker result is for (PaneScope). Code that reads "the list", its items or its folder
// goes through it; workers carry their own Pane* instead.
extern Pane* g_pane;
extern bool g_dual;                          // the active tab shows the second pane
Pane* active_pane();                         // where the focus and the commands go
void  pane_set_active(Pane* p);              // (navigation_activate_pane: with the UI that follows it)
inline bool pane_shown(const Pane* p) { return p->index == 0 || g_dual; }
Pane* pane_of_list(HWND list);               // nullptr: not a pane's list
inline bool is_pane_list(HWND h) { return h && pane_of_list(h) != nullptr; }
inline bool pane_is_active() { return g_pane == active_pane(); }

// g_pane = p for a while; the outermost scope ends on the active pane
struct PaneScope {
    explicit PaneScope(Pane* p);
    ~PaneScope();
    PaneScope(const PaneScope&) = delete;
    PaneScope& operator=(const PaneScope&) = delete;
private:
    Pane* m_saved;
};

// run f with each pane as g_pane (shownOnly: not the second one in a single-pane tab)
template <class F> void for_each_pane(F f, bool shownOnly = false) {
    for (Pane& p : g_panes) {
        if (!p.list || (shownOnly && !pane_shown(&p))) continue;
        PaneScope ps(&p);
        f();
    }
}

// timers that belong to one pane: the base id + 100 * pane index
inline UINT_PTR pane_timer(UINT_PTR base) { return base + 100 * (UINT_PTR)g_pane->index; }

// globals
extern HWND   g_main, g_address, g_search, g_tree, g_status;
extern HWND   g_navbar;
extern HFONT  g_font;
extern HFONT  g_iconFont;       // Segoe Fluent Icons (Win11)
extern HFONT  g_mdl2Font;       // Segoe MDL2 Assets (Win10 fallback)
extern HINSTANCE g_hinst;
extern int    g_splitterX, g_statusH;
extern int    g_dualSplit;                  // the first list's share of the two lists (per mille)
extern bool   g_treeVisible;
extern std::atomic<bool> g_showHidden;
extern std::atomic<int> g_showExtensions;   // -1 = as Windows does, 0 = hide, 1 = show
extern bool   g_checkBoxes;                 // item check boxes (they mirror the selection)
extern bool   g_cmdBarVisible;              // the Win11 command bar row (else its commands sit in the nav row's "...")
extern bool   g_compactView;                // Win11 "Compact view": less space between items
bool show_extensions_effective();           // g_showExtensions resolved against Windows' setting
bool windows_shows_extensions();            // Explorer's own "File name extensions" (read only)
bool windows_compact_view();                // Explorer's own "Compact view" (read only)

// --- app messages ---
enum : UINT {
    WM_ENUM_DONE    = WM_APP + 1,   // lp = HeapEnumOp* (receiver owns)
    WM_ASYNC_WAKE,                  // drain the async result queue (post_async)
    WM_TREE_SYNC,                   // expand/select current folder in tree (deferred)
    WM_SELECTION_CHANGED,           // coalesced LVN_ITEMCHANGED (status bar + details pane); wp = pane index
    WM_RUN_LATER,                   // wp = AsyncHandler, lp = its payload (post_later)
    WM_FILEOPS_ENDED,               // to the UI thread (no window): the last paste / delete ended
};

// timers on g_main (the per-pane ones: pane_timer)
enum : UINT_PTR {
    TIMER_WATCH_REFRESH = 1,        // debounced change-notify refresh (per pane)
    TIMER_SEARCH = 2,               // search-as-you-type pause
    TIMER_ENUM_STREAM = 3,          // take the rows a listing has produced so far (per pane)
    TIMER_TABS_PERSIST = 4,         // save the tab set (debounced: tabs_persist_soon)
    TIMER_ENUM_HELD = 5,            // listing results held while a rename box is open (per pane)
    TIMER_FOLDER_SIZES = 6,         // folders changed inside, waiting to be walked again (per pane)
    TIMER_SETTINGS_SAVE = 7,        // views, columns, preferences changed: saved every few minutes (not lost in a crash)
};

// heap op passed via WM_ENUM_DONE
struct HeapEnumOp {
    int pane = 0;                   // index of the pane listing
    UINT opId = 0;
    std::vector<Item*> items;
    HRESULT enumHr = S_OK;
    ColumnSet columns;          // the columns the items' extra values were read for
    bool final = true;          // false: a batch of a listing still streaming in
};

// Results produced by worker threads, applied on the UI thread.
using AsyncHandler = void (*)(void* payload);
// Queue a payload for the UI thread; `apply` runs there and owns the payload.
void post_async(AsyncHandler apply, void* payload);
void drain_async();                 // WM_ASYNC_WAKE handler
// UI thread: run `apply` from a message of its own, after the current handler (a modal box
// shown from an async handler would run the rest of its batch inside the box's loop)
void post_later(AsyncHandler apply, void* payload);

// --- helpers in appstate.cpp ---
void  fix_font(HWND h);

PIDLIST_ABSOLUTE current_clone();
void current_replace(PIDLIST_ABSOLUTE newAbs);    // frees old, adopts new (nullptr -> Desktop)
std::wstring current_display_path();           // what the address bar shows

// status bar update (reads the active pane's items + selection; other panes: nothing)
void status_update();
void search_update_cue();                     // "Search <folder>" placeholder
void exit_app();
// the process lives while one of its windows does (the main window, the media player, a
// picture editor): one of them gone, the process ends when it was the last (main.cpp)
void app_window_closed();
HACCEL app_accelerators(UINT* count);         // the main window's shortcuts (count: entries; may be null)
