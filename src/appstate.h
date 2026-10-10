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
enum ColKind : int { COL_NAME = 0, COL_SIZE = 1, COL_TYPE = 2, COL_DATE = 3, COL_EXTRA = 4 };

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
    std::wstring    hiddenExt; // the extension `name` leaves out where Windows itself would show it
    bool            extForced = false;      // `name` shows an extension Windows would hide
    std::wstring    size;      // formatted display size, empty for folders
    std::wstring    type;
    std::wstring    date;
    ULONGLONG       bytes = 0;
    FILETIME        modified{};
    PITEMID_CHILD   pidl = nullptr;         // child pidl relative to current folder
    ULONGLONG       sig  = 0;               // identity signature
    SFGAOF          attrs = 0;
    int             icon = 0;               // system image list index
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

extern std::mutex g_itemsMx;
extern std::vector<Item*> g_items;          // UI thread owns; workers only read under g_itemsMx

// current folder: absolute pidl, never nullptr after startup (empty pidl = Desktop)
extern PITEMID_CHILD g_current;

// operation ids — ops carrying a stale id are dropped
extern std::atomic<UINT> g_enumId;

// globals
extern HWND   g_main, g_address, g_search, g_tree, g_list, g_status;
extern HWND   g_navbar;
extern HFONT  g_font;
extern HFONT  g_iconFont;       // Segoe Fluent Icons (Win11)
extern HFONT  g_mdl2Font;       // Segoe MDL2 Assets (Win10 fallback)
extern HINSTANCE g_hinst;
extern struct View g_view;
extern int    g_splitterX, g_statusH;
extern bool   g_treeVisible;
extern std::atomic<bool> g_showHidden;
extern std::atomic<int> g_showExtensions;   // -1 = as Windows does, 0 = hide, 1 = show
extern bool   g_checkBoxes;                 // item check boxes (they mirror the selection)
extern bool   g_cmdBarVisible;              // the Win11 command bar row (else its commands sit in the nav row's "...")
extern bool   g_compactView;                // Win11 "Compact view": less space between items
bool show_extensions_effective();           // g_showExtensions resolved against Windows' setting
bool windows_shows_extensions();            // Explorer's own "File name extensions" (read only)
bool windows_compact_view();                // Explorer's own "Compact view" (read only)
extern int    g_sortColumn;                 // ColKind; COL_EXTRA sorts by g_sortKey
extern PROPERTYKEY g_sortKey;
extern bool   g_sortAsc;

// --- app messages ---
enum : UINT {
    WM_ENUM_DONE    = WM_APP + 1,   // lp = HeapEnumOp* (receiver owns)
    WM_ASYNC_WAKE,                  // drain the async result queue (post_async)
    WM_TREE_SYNC,                   // expand/select current folder in tree (deferred)
    WM_SELECTION_CHANGED,           // coalesced LVN_ITEMCHANGED (status bar + details pane)
};

// timers on g_main
enum : UINT_PTR {
    TIMER_WATCH_REFRESH = 1,        // debounced change-notify refresh
    TIMER_SEARCH = 2,               // search-as-you-type pause
    TIMER_ENUM_STREAM = 3,          // take the rows a listing has produced so far
};

// heap op passed via WM_ENUM_DONE
struct HeapEnumOp {
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

// --- helpers in appstate.cpp ---
void  fix_font(HWND h);

PITEMID_CHILD current_clone();
void current_replace(PITEMID_CHILD newAbs);    // frees old, adopts new (nullptr -> Desktop)
std::wstring current_display_path();           // what the address bar shows

// status bar update (reads g_items + listview selection)
void status_update();
void search_update_cue();                     // "Search <folder>" placeholder
void exit_app();
