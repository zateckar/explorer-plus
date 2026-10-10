#pragma once
#include "appstate.h"

// --- per-tab navigation state model (tabs.cpp owns it) ---
struct TabState {
    int id = 0;                                   // stable identity across reorders
    PITEMID_CHILD current = nullptr;              // absolute pidl (empty = Desktop)
    std::vector<PITEMID_CHILD> back;
    std::vector<PITEMID_CHILD> fwd;
    View view{ LV_VIEW_DETAILS, 0 };
    std::wstring name;                            // display name shown in the tab
    bool pinned = false;                          // pinned: no close button, no Ctrl+W, sorted first
};

extern int g_activeTab;

// model ops (navigation.cpp calls into these)
void   tabmodel_init(const std::wstring& openPath, bool restore);   // restored tabs (+ openPath as active tab)
void   tabmodel_new_tab();                         // duplicate current folder
void   tabmodel_close_tab(int idx);                // -1 = active; closes window when last
void   tabmodel_switch_to(int idx);
void   tabmodel_next_tab(int delta);               // Ctrl+PgUp/Dn / Ctrl+Tab
void   tabmodel_switch_index_1_based(int one);     // Ctrl+1..9
void   tabmodel_toggle_pin(int idx);
void   tabmodel_duplicate_tab(int idx);
void   tabmodel_close_others(int idx);
int    tabmodel_move_tab(int from, int to);        // drag reorder; returns new index
bool   tabmodel_is_pinned(int idx);

// called by navigation when the active tab navigates
void   tabmodel_on_navigate(PITEMID_CHILD newAbs /*adopted clone*/, bool pushHistory);
// info
TabState* tabmodel_active();
std::wstring  tabmodel_active_name();
bool tabmodel_can_back();
bool tabmodel_can_forward();

// back/forward within active tab; returns false when there is nowhere to go
bool tabmodel_go_back();
bool tabmodel_go_forward();

// persistence: one line per tab, "path\x1name\x1view\x1pinned"
void tabs_serialize_lines(std::vector<std::wstring>& lines, int& outActive);
void tabs_deserialize(const std::wstring& tabs, int active);

// persist the tab set right now (called on pin / close / new / dup / reorder)
void tabs_persist_now();

// --- tab strip UI window ---
HWND tabstrip_create(HWND parent);   // returns HWND with id IDC_TABSTRIP
void tabstrip_refresh();             // recompute items / repaint
HWND tabstrip_hwnd();
