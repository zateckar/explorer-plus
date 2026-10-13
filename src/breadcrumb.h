#pragma once
#include "appstate.h"

// Win11 address bar: breadcrumb segments with chevron drop-downs; click into the
// empty area (or Ctrl+L / Alt+D / F4) to type a path. The window is g_address
// (IDC_ADDRESS); WM_SETTEXT on it re-syncs the crumbs with g_pane->current.
HWND address_create(HWND parent);
void address_begin_edit();          // switch to the edit box, path selected
bool address_editing();
