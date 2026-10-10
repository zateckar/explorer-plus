#pragma once
#include "appstate.h"

void list_create();
LRESULT list_notify(HWND hwnd, WPARAM wp, LPARAM lp);
void list_apply_view();                    // switch LV_VIEW_* + imagelists
void list_context_menu(POINT screenPt);    // (-1,-1) = keyboard invocation
void list_on_selection_changed();          // WM_SELECTION_CHANGED handler
int  list_sort_display_column();           // header column showing the sort arrow (-1: none)

std::vector<PCITEMID_CHILD> list_selected_pidls();   // owned by g_items; use synchronously
void list_copy_selection(bool cut);
void list_delete_selection(bool permanent);
void list_rename_selected();
void list_show_properties();
void list_select_all();
void list_select_none();
void list_invert_selection();
bool list_all_selected();                  // every item selected (header check box)
int  list_first_selected();
void navigation_open_selected();
