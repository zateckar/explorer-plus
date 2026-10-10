#pragma once
#include "appstate.h"

void show_items_context_menu(HWND hwnd, POINT screenPt);    // list selection
void show_bg_context_menu(HWND hwnd, POINT screenPt);       // list background
void show_tree_context_menu(HWND hwnd, POINT screenPt, PCIDLIST_ABSOLUTE folder);
void contextmenu_properties(HWND hwnd);                     // selection, or current folder
bool contextmenu_invoke_verb(HWND hwnd, const char* verb);  // canonical verb on the selection / folder
void show_new_menu(HWND hwnd, POINT screenPt, UINT tpmAlign); // Explorer's New menu for the current folder

// While a shell menu is tracked, its owner-drawn items and lazily-filled submenus
// (Send to, New, Open with) need WM_INITMENUPOPUP / WM_DRAWITEM / WM_MEASUREITEM /
// WM_MENUCHAR routed to the handler. Returns true when the message was consumed.
bool contextmenu_handle_menu_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
bool contextmenu_active();
