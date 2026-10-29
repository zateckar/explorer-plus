#pragma once
#include "appstate.h"

struct TabState;
// Apply g_pane's side of a tab to its list (and the bars, for the active list)
void navigate_replace(TabState* t);
// Put a tab on screen: one list or two, the active one, the navigation pane's state
// (anotherTab: the tab wasn't on screen)
void navigation_show_tab(TabState* t, bool anotherTab);
void navigation_activate_pane(Pane* p);        // the other list got the focus
void navigation_toggle_dual();                 // two lists in this tab, or one
void navigate_owned(PIDLIST_ABSOLUTE pidl);    // adopts; nullptr = Desktop
void navigate_to(PCIDLIST_ABSOLUTE pidl);   // clones; nullptr = Desktop
void navigate_in_place(PIDLIST_ABSOLUTE pidl); // adopts; replaces the current folder, no history entry
void navigate_path_text(const std::wstring& text);
void navigation_open_abs(PCIDLIST_ABSOLUTE abs);   // folder -> navigate, file -> default verb
// files (no folders) by their default verb; the types Explorer Plus' Notepad or player opens
// (assoc_own_app) in them, in this process, the files of each together
void navigation_open_files(const std::vector<PCIDLIST_ABSOLUTE>& files);
void navigation_open_new_window(PCIDLIST_ABSOLUTE folder);   // Ctrl+N, "Open in new window"
void navigation_go_back();
void navigation_go_forward();
void navigation_go_up();
bool navigation_can_up();
void navigation_refresh();
bool navigation_leave_if_gone();    // current folder deleted: open the nearest existing parent
View view_for_folder(PCIDLIST_ABSOLUTE abs);       // remembered view, else the default
void views_apply_to_all();      // this folder's view + columns: every file-system folder's default
void views_reset_all();         // every file-system folder back to the standard view
std::wstring folder_settings_key(PCIDLIST_ABSOLUTE abs);   // per-folder view / columns key

std::vector<std::wstring> address_history_list();
void address_history_add(const std::wstring& s);
