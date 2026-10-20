#pragma once
#include "appstate.h"
#include "tree.h"

// --- per-tab navigation state model (tabs.cpp owns it) ---
// one file list's place in a tab
struct PaneNav {
    PIDLIST_ABSOLUTE current = nullptr;           // absolute pidl (empty = Desktop); null: never shown
    std::vector<PIDLIST_ABSOLUTE> back;
    std::vector<PIDLIST_ABSOLUTE> fwd;
    View view{ LV_VIEW_DETAILS, 0 };
    std::wstring name;                            // display name of the folder
};

struct TabState {
    int id = 0;                                   // stable identity across reorders
    PaneNav side[kPanes];                         // [0]: the main list, [1]: the second one
    bool dual = false;                            // both lists shown, side by side
    int activeSide = 0;                           // the list the commands go to
    bool pinned = false;                          // pinned: no close button, no Ctrl+W, sorted first,
                                                  // its (first list's) folder fixed (folders open in new tabs)
    TreeState tree;                               // the navigation pane as this tab left it
    PaneNav& nav() { return side[g_pane->index]; }   // the side g_pane shows
    // the side the tab is named after: the active list (a pinned tab: its fixed folder)
    const PaneNav& shown() const { return side[dual && !pinned ? activeSide : 0]; }
};

extern int g_activeTab;

// model ops (navigation.cpp calls into these)
// restored tabs (+ openPath as active tab): their folders parsed on a thread from _prepare (as
// early as it can be), the tabs made by _init (the UI thread, where the folders are needed)
void   tabmodel_prepare(const std::wstring& openPath, bool restore);
void   tabmodel_init();
void   tabmodel_new_tab();                         // the active list's folder, one list
void   tabmodel_open_in_new_tab(PIDLIST_ABSOLUTE abs);   // adopts; new active tab, Back = the folder left
void   tabmodel_close_tab(int idx);                // -1 = active; closes window when last
void   tabmodel_switch_to(int idx);
void   tabmodel_show_active();                     // (again: views reset)
void   tabmodel_next_tab(int delta);               // Ctrl+PgUp/Dn / Ctrl+Tab
void   tabmodel_switch_index_1_based(int one);     // Ctrl+1..9
void   tabmodel_toggle_pin(int idx);
void   tabmodel_duplicate_tab(int idx);
void   tabmodel_close_others(int idx);
int    tabmodel_move_tab(int from, int to);        // drag reorder; returns new index
bool   tabmodel_is_pinned(int idx);

// called by navigation when g_pane navigates (in the active tab)
void   tabmodel_on_navigate(PIDLIST_ABSOLUTE newAbs /*adopted clone*/, bool pushHistory);
// info
TabState* tabmodel_active();
std::wstring  tabmodel_active_name();
void tabmodel_all_folders(std::vector<PCIDLIST_ABSOLUTE>& out);   // every tab's folders and history (borrowed)
bool tabmodel_can_back();                          // (g_pane's history)
bool tabmodel_can_forward();

// back/forward within g_pane's side of the active tab; false when there is nowhere to go
bool tabmodel_go_back();
bool tabmodel_go_forward();

// persistence: one line per tab,
// "path\x1name\x1view\x1pinned\x1dual\x1path2\x1name2\x1view2\x1activeSide\x1tree" (the second
// list's fields empty when it never had one; tree: "top,expanded,..." hex keys, empty: never shown)
void tabs_serialize_lines(std::vector<std::wstring>& lines, int& outActive);
void tabs_deserialize(const std::wstring& tabs, int active);

// persist the tab set right now (at exit)
void tabs_persist_now();
// ... or a moment later, once (pin / close / new / dup / reorder / navigation)
void tabs_persist_soon();

// --- tab strip UI window ---
HWND tabstrip_create(HWND parent);   // returns HWND with id IDC_TABSTRIP
void tabstrip_refresh();             // recompute items / repaint
HWND tabstrip_hwnd();
