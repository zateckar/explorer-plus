#pragma once
#include "appstate.h"

// Apply a tab state to the whole UI (tabs.cpp / tab switching uses this)
struct TabState;
void navigate_replace(TabState* t);
void navigate_owned(PITEMID_CHILD pidl);    // adopts; nullptr = Desktop
void navigate_to(PCIDLIST_ABSOLUTE pidl);   // clones; nullptr = Desktop
void navigate_path_text(const std::wstring& text);
void navigation_open_abs(PCIDLIST_ABSOLUTE abs);   // folder -> navigate, file -> default verb
void navigation_open_new_window(PCIDLIST_ABSOLUTE folder);   // Ctrl+N, "Open in new window"
void navigation_go_back();
void navigation_go_forward();
void navigation_go_up();
bool navigation_can_up();
void navigation_refresh();
View view_for_folder(PCIDLIST_ABSOLUTE abs);       // remembered view, else the default
std::wstring folder_settings_key(PCIDLIST_ABSOLUTE abs);   // per-folder view / columns key

std::vector<std::wstring> address_history_list();
void address_history_add(const std::wstring& s);
