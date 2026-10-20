#pragma once
#include "appstate.h"

// Win11 command bar: New | Cut Copy Paste Rename Share Delete | Sort View | ...  Details
HWND cmdbar_create(HWND parent);     // id = IDC_CMDBAR (made once it shows: null until then)
HWND cmdbar_hwnd();
void cmdbar_refresh();               // selection / clipboard / pane state changed
// drop-down menus (also reachable from the keyboard / other UI)
void cmdbar_show_menu(int menuCmd);  // IDM_NEW_MENU / IDM_SORT_MENU / IDM_VIEW_MENU / IDM_MORE_MENU
void cmdbar_show_tools();            // the nav row's "..." flyout (the bar's commands while it is hidden)

// menus shared with the background context menu (caller destroys them)
HMENU menu_view_modes();             // Extra large icons .. Tiles (radio)
HMENU menu_sort_by();                // the folder's columns, Ascending / Descending
HMENU menu_group_by();               // Name / Date modified / Type / Size / (None)
