#pragma once
#include "appstate.h"

// Grouped content view (Group by, Home's Quick access / Recent, This PC's drives).
// The list is owner-data: items are sorted group-first so every group is a contiguous
// index range, and an IOwnerDataCallback maps group-relative indices to list indices.

void groups_install(HWND list);              // once, after the list view exists
bool is_home_folder(PCIDLIST_ABSOLUTE abs);  // thread-safe (COM initialized)
bool is_this_pc_folder(PCIDLIST_ABSOLUTE abs);

int  groups_effective();                     // GB_* in force for the current folder
bool groups_keep_enum_order();               // Home: the shell's own order inside groups

// UI thread, under g_pane->itemsMx: give each item its group rank (Item::group), then the
// caller sorts by (group, ...) and records the ranges
void groups_assign(std::vector<Item*>& items);
void groups_record(const std::vector<Item*>& items);
void groups_apply();                         // push group headers/ranges to the list view
void groups_clear();                         // the list was emptied
const wchar_t* groups_name(int id);          // header text of a shown group
void groups_theme_changed();                 // header colors follow light/dark
