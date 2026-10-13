#pragma once
#include "appstate.h"

// Win11 status bar: "N items | M items selected  size" + Details / Large icons toggles
HWND statusbar_create(HWND parent);          // sets g_status (id IDC_STATUS)
void statusbar_set(const std::wstring& left, const std::wstring& right);
void statusbar_refresh();                     // view toggles follow g_pane->view
